//===----------------------------------------------------------------------===//
//  dashboard_ui_message_handler_listener.cc
//===----------------------------------------------------------------------===//

#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "build/build_config.h"
#include "base/base64.h"
#include "base/command_line.h"
#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/containers/span.h"
#include "base/feature_list.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/threading/thread_restrictions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "chrome/browser/ui/webui/dashboard/headless_investigation_service.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_prewarm.h"
#include "chrome/browser/ui/webui/dashboard/listener_manager.h"
#include "chrome/browser/ui/webui/dashboard/run_history_store.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/codec/png_codec.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_POSIX)
#include <unistd.h>

#include "base/posix/eintr_wrapper.h"
#endif

namespace dashboard {

// Allows headless-host spawn/wait helpers to join the child process.
// Friended in base/threading/thread_restrictions.h.
class [[maybe_unused, nodiscard]] HeadlessHostScopedAllowBaseSyncPrimitives
    : public base::ScopedAllowBaseSyncPrimitives {};

}  // namespace dashboard

namespace {

BASE_FEATURE(kDashboardNativeHeadlessListeners,
             "DashboardNativeHeadlessListeners",
             base::FEATURE_DISABLED_BY_DEFAULT);

constexpr int kHeadlessListenerStartWaitMs = 15000;

const base::Value::Dict* ReadScriptSettingsOverrides(
    const base::Value::Dict& script) {
  return script.FindDict("settingsOverrides");
}

base::Value::List FlattenTimelineForListenerScript(
    const base::Value::Dict& script) {
  base::Value::List flat;
  bool appended_from_actions = false;
  if (const base::Value::List* acts = script.FindList("actions")) {
    for (const auto& a : *acts) {
      flat.Append(a.Clone());
      appended_from_actions = true;
    }
  }
  if (!appended_from_actions) {
    if (const base::Value::List* ctxs = script.FindList("url_contexts")) {
      for (const auto& c : *ctxs) {
        if (!c.is_dict()) {
          continue;
        }
        if (const base::Value::List* acts = c.GetDict().FindList("actions")) {
          for (const auto& a : *acts) {
            flat.Append(a.Clone());
          }
        }
      }
    }
  }
  return flat;
}

std::optional<GURL> ExtractScriptStartUrl(const base::Value::Dict& script) {
  auto try_url = [](const std::string& raw) -> std::optional<GURL> {
    std::string trimmed = raw;
    base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
    // Token placeholders like {listener.match.url} are not start URLs.
    if (trimmed.empty() || trimmed.find('{') != std::string::npos) {
      return std::nullopt;
    }
    GURL gurl(trimmed);
    if (gurl.is_valid() &&
        (gurl.SchemeIsHTTPOrHTTPS() || gurl.SchemeIs("chrome") ||
         gurl.SchemeIs("about"))) {
      return gurl;
    }
    return std::nullopt;
  };

  if (const std::string* explicit_start = script.FindString("startUrl")) {
    if (auto g = try_url(*explicit_start)) {
      return g;
    }
  }

  if (const base::Value::List* contexts = script.FindList("url_contexts")) {
    for (const auto& ctx : *contexts) {
      const base::Value::Dict* d = ctx.GetIfDict();
      if (!d) {
        continue;
      }
      const std::string* url = d->FindString("url");
      if (!url) {
        continue;
      }
      if (auto g = try_url(*url)) {
        return g;
      }
    }
  }

  // Fall back to the first concrete navigate URL in the flattened timeline.
  if (const base::Value::List* actions = script.FindList("actions")) {
    for (const auto& entry : *actions) {
      const base::Value::Dict* action = entry.GetIfDict();
      if (!action) {
        continue;
      }
      const std::string* action_name = action->FindString("action");
      if (!action_name ||
          !base::EqualsCaseInsensitiveASCII(*action_name, "navigate")) {
        continue;
      }
      const base::Value::Dict* params = action->FindDict("params");
      if (!params) {
        continue;
      }
      const std::string* navigate_url = params->FindString("url");
      if (!navigate_url) {
        continue;
      }
      if (auto g = try_url(*navigate_url)) {
        return g;
      }
    }
  }
  return std::nullopt;
}

std::string ReadKeyScriptIdFromScript(const base::Value::Dict& script) {
  if (const std::string* ks = script.FindString("keyScriptId")) {
    return *ks;
  }
  return std::string();
}

bool LoadScriptFromStore(Profile* profile,
                         const std::string& script_id,
                         base::Value::Dict* out_script) {
  if (!profile || !out_script || script_id.empty()) {
    return false;
  }
  return dashboard::ScriptStore::Read(profile, script_id, out_script);
}

std::string NormalizeDataKey(std::string key) {
  base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  if (key.size() >= 2 && key.front() == '{' && key.back() == '}') {
    key = key.substr(1, key.size() - 2);
    base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  }
  return key;
}

std::string ExtractFirstHttpUrl(const std::string& source) {
  if (source.empty()) {
    return std::string();
  }
  size_t start = source.find("https://");
  if (start == std::string::npos) {
    start = source.find("http://");
  }
  if (start == std::string::npos) {
    return std::string();
  }
  const std::string kTerminators = " \t\r\n\"'<>)]}";
  size_t end = source.find_first_of(kTerminators, start);
  if (end == std::string::npos) {
    end = source.size();
  }
  if (end <= start) {
    return std::string();
  }
  std::string candidate = source.substr(start, end - start);
  while (!candidate.empty() &&
         (candidate.back() == '.' || candidate.back() == ',' ||
          candidate.back() == ';' || candidate.back() == ':' ||
          candidate.back() == '!')) {
    candidate.pop_back();
  }
  GURL url(candidate);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return std::string();
  }
  return url.spec();
}

std::string ExpandTokensInString(
    std::string input,
    const base::flat_map<std::string, std::string>& token_map) {
  if (token_map.empty() || input.find('{') == std::string::npos) {
    return input;
  }
  std::string out;
  out.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    const size_t open = input.find('{', i);
    if (open == std::string::npos) {
      out.append(input.substr(i));
      break;
    }
    out.append(input.substr(i, open - i));
    const size_t close = input.find('}', open + 1);
    if (close == std::string::npos) {
      out.append(input.substr(open));
      break;
    }
    std::string token =
        NormalizeDataKey(input.substr(open + 1, close - open - 1));
    auto it = token_map.find(token);
    if (!token.empty() && it != token_map.end()) {
      out.append(it->second);
    } else {
      out.append(input.substr(open, close - open + 1));
    }
    i = close + 1;
  }
  return out;
}

void ExpandTokensInValue(
    base::Value* value,
    const base::flat_map<std::string, std::string>& token_map) {
  if (!value) {
    return;
  }
  if (value->is_string()) {
    *value = base::Value(ExpandTokensInString(value->GetString(), token_map));
    return;
  }
  if (value->is_list()) {
    for (auto& item : value->GetList()) {
      ExpandTokensInValue(&item, token_map);
    }
    return;
  }
  if (value->is_dict()) {
    for (auto it = value->GetDict().begin(); it != value->GetDict().end();
         ++it) {
      ExpandTokensInValue(&it->second, token_map);
    }
  }
}

void SetListenerMatchToken(base::flat_map<std::string, std::string>* token_map,
                           const std::string& key,
                           const std::string& value) {
  if (!token_map || key.empty() || value.empty()) {
    return;
  }
  token_map->insert_or_assign(key, value);
}

base::flat_map<std::string, std::string> BuildListenerMatchTokenMapFromEvent(
    const base::Value::Dict& event) {
  base::flat_map<std::string, std::string> token_map;
  const std::string listener_script_id =
      event.FindString("listener_script_id")
          ? *event.FindString("listener_script_id")
          : std::string();
  const std::string match_value =
      event.FindString("matchValue") ? *event.FindString("matchValue")
                                     : std::string();
  const std::string text =
      event.FindString("text") ? *event.FindString("text") : std::string();
  const std::string attr =
      event.FindString("attr") ? *event.FindString("attr") : std::string();
  const std::string page_url = event.FindString("pageUrl")
                                   ? *event.FindString("pageUrl")
                                   : std::string();

  std::string matched_url = ExtractFirstHttpUrl(match_value);
  if (matched_url.empty()) {
    matched_url = ExtractFirstHttpUrl(text);
  }
  if (matched_url.empty()) {
    matched_url = ExtractFirstHttpUrl(attr);
  }
  std::string best_value = !matched_url.empty() ? matched_url : match_value;
  if (best_value.empty()) {
    best_value = !text.empty() ? text : attr;
  }

  SetListenerMatchToken(&token_map, "listener.match", best_value);
  SetListenerMatchToken(&token_map, "listener.match.value", best_value);
  SetListenerMatchToken(&token_map, "listener.match.text", text);
  SetListenerMatchToken(&token_map, "listener.match.attr", attr);
  SetListenerMatchToken(&token_map, "listener.match.url", matched_url);
  SetListenerMatchToken(&token_map, "listener.match.page_url", page_url);

  const std::string normalized_listener_id =
      NormalizeDataKey(listener_script_id);
  if (!normalized_listener_id.empty()) {
    const std::string prefix = "listener." + normalized_listener_id;
    SetListenerMatchToken(&token_map, prefix + ".match", best_value);
    SetListenerMatchToken(&token_map, prefix + ".match.value", best_value);
    SetListenerMatchToken(&token_map, prefix + ".match.text", text);
    SetListenerMatchToken(&token_map, prefix + ".match.attr", attr);
    SetListenerMatchToken(&token_map, prefix + ".match.url", matched_url);
    SetListenerMatchToken(&token_map, prefix + ".match.page_url", page_url);
  }

  std::string capture_variable = NormalizeDataKey(
      event.FindString("captureVariable") ? *event.FindString("captureVariable")
                                          : std::string());
  if (!capture_variable.empty()) {
    SetListenerMatchToken(&token_map, capture_variable, best_value);
  }
  return token_map;
}

struct ParsedTaggedKey {
  struct TagRef {
    std::string raw_tag;
    std::string grouped_tag;
    std::optional<int> order;
  };

  std::string clean_key;
  std::string base_key;
  std::vector<TagRef> tags;
};

struct TagTokenValue {
  std::string value;
  std::optional<int> order;
  size_t sequence = 0;
  bool no_delimiter = false;
};

ParsedTaggedKey::TagRef ParseTagRef(const std::string& raw_tag) {
  ParsedTaggedKey::TagRef ref;
  ref.raw_tag = raw_tag;
  ref.grouped_tag = raw_tag;
  if (raw_tag.empty()) {
    return ref;
  }

  size_t suffix_begin = raw_tag.size();
  while (suffix_begin > 0 && base::IsAsciiDigit(raw_tag[suffix_begin - 1])) {
    --suffix_begin;
  }
  if (suffix_begin == raw_tag.size() || suffix_begin == 0) {
    return ref;
  }

  int parsed_order = 0;
  const std::string suffix = raw_tag.substr(suffix_begin);
  if (!base::StringToInt(suffix, &parsed_order) || parsed_order < 0) {
    return ref;
  }
  ref.grouped_tag = raw_tag.substr(0, suffix_begin);
  if (ref.grouped_tag.empty()) {
    ref.grouped_tag = raw_tag;
  } else {
    ref.order = parsed_order;
  }
  return ref;
}

TagTokenValue MakeTagTokenValue(const std::string& raw_value,
                                std::optional<int> order,
                                size_t sequence) {
  TagTokenValue out;
  out.value = raw_value;
  out.order = order;
  out.sequence = sequence;
  if (raw_value.size() >= 2) {
    const char first = raw_value.front();
    const char last = raw_value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      out.no_delimiter = true;
      out.value = raw_value.substr(1, raw_value.size() - 2);
    }
  }
  return out;
}

std::vector<TagTokenValue> SortedTagValues(
    const std::vector<TagTokenValue>& values) {
  std::vector<TagTokenValue> out = values;
  std::stable_sort(out.begin(), out.end(),
                   [](const TagTokenValue& a, const TagTokenValue& b) {
                     const bool a_has_order = a.order.has_value();
                     const bool b_has_order = b.order.has_value();
                     if (a_has_order != b_has_order) {
                       return a_has_order && !b_has_order;
                     }
                     if (a_has_order && b_has_order && a.order != b.order) {
                       return a.order.value() < b.order.value();
                     }
                     return a.sequence < b.sequence;
                   });
  return out;
}

std::string JoinTagScalarValue(const std::vector<TagTokenValue>& values) {
  if (values.empty()) {
    return std::string();
  }
  const std::vector<TagTokenValue> sorted = SortedTagValues(values);
  bool use_concat = false;
  std::vector<std::string> parts;
  parts.reserve(sorted.size());
  for (const auto& item : sorted) {
    parts.push_back(item.value);
    use_concat = use_concat || item.no_delimiter;
  }
  return base::JoinString(parts, use_concat ? "" : ",");
}

std::optional<ParsedTaggedKey> ParseTaggedKey(const std::string& raw_key) {
  ParsedTaggedKey out;
  out.clean_key = NormalizeDataKey(raw_key);
  if (out.clean_key.empty()) {
    return std::nullopt;
  }

  std::vector<std::string> parts = base::SplitString(
      out.clean_key, "#", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (parts.empty()) {
    return std::nullopt;
  }

  out.base_key = parts.front();
  if (out.base_key.empty()) {
    return std::nullopt;
  }

  for (size_t i = 1; i < parts.size(); ++i) {
    const std::string& tag = parts[i];
    if (tag.empty()) {
      continue;
    }
    const bool already_present =
        std::any_of(out.tags.begin(), out.tags.end(),
                    [&tag](const ParsedTaggedKey::TagRef& ref) {
                      return ref.raw_tag == tag;
                    });
    if (!already_present) {
      out.tags.push_back(ParseTagRef(tag));
    }
  }
  return out;
}

void AppendUniqueTagValue(
    base::flat_map<std::string, std::vector<TagTokenValue>>* out,
    const std::string& tag,
    const std::string& value,
    std::optional<int> order,
    size_t sequence) {
  if (!out || tag.empty()) {
    return;
  }
  std::vector<TagTokenValue>& values = (*out)[tag];
  const TagTokenValue candidate = MakeTagTokenValue(value, order, sequence);
  const bool duplicate = std::any_of(
      values.begin(), values.end(),
      [&candidate](const TagTokenValue& existing) {
        return existing.value == candidate.value;
      });
  if (!duplicate) {
    values.push_back(candidate);
  }
}

base::Value::Dict ReadKeyValueIniBlocking(const base::FilePath& path) {
  base::Value::Dict out;
  if (!base::PathExists(path)) {
    return out;
  }
  std::string content;
  if (!base::ReadFileToString(path, &content)) {
    PLOG(ERROR) << "[Dashboard] Failed to read ini: " << path.AsUTF8Unsafe();
    return out;
  }

  for (const auto& raw : base::SplitStringPiece(content, "\n",
                                                base::KEEP_WHITESPACE,
                                                base::SPLIT_WANT_ALL)) {
    std::string line(raw);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::string trimmed = line;
    base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
    if (trimmed.empty()) {
      continue;
    }
    if (trimmed[0] == '#' || trimmed[0] == ';') {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = NormalizeDataKey(line.substr(0, eq));
    std::string value = line.substr(eq + 1);
    if (key.empty()) {
      continue;
    }
    out.Set(key, value);
  }
  return out;
}

base::flat_map<std::string, std::string> BuildListenerTokenMapBlocking(
    const Profile* profile) {
  constexpr size_t kMaxTokenEntries = 50000;
  constexpr size_t kMaxTokenValueBytes = 256 * 1024;
  constexpr size_t kMaxTokenAggregateBytes = 8 * 1024 * 1024;

  base::flat_map<std::string, std::string> out;
  if (!profile) {
    return out;
  }

  const base::FilePath data_dir =
      profile->GetPath().AppendASCII("SimulachromeData");
  const base::Value::Dict vars =
      ReadKeyValueIniBlocking(data_dir.AppendASCII("variables.ini"));
  const base::Value::Dict secrets =
      ReadKeyValueIniBlocking(data_dir.AppendASCII("secrets.ini"));

  base::flat_map<std::string, std::vector<TagTokenValue>> tag_values;
  size_t aggregate_bytes = 0;
  size_t tag_sequence = 0;
  bool logged_entry_cap = false;
  bool logged_aggregate_cap = false;

  auto store_token = [&](const std::string& key,
                         std::string value,
                         bool allow_overwrite) {
    if (key.empty()) {
      return;
    }
    auto it = out.find(key);
    if (!allow_overwrite && it != out.end()) {
      return;
    }

    if (it == out.end() && out.size() >= kMaxTokenEntries) {
      if (!logged_entry_cap) {
        logged_entry_cap = true;
        LOG(WARNING) << "[Dashboard] Listener token-map entry cap reached ("
                     << kMaxTokenEntries << "); extra entries skipped.";
      }
      return;
    }

    if (value.size() > kMaxTokenValueBytes) {
      LOG(WARNING) << "[Dashboard] Listener token value truncated for key='"
                   << key << "' from " << value.size() << " to "
                   << kMaxTokenValueBytes << " bytes.";
      value.resize(kMaxTokenValueBytes);
    }

    const size_t existing_value_size =
        (it != out.end()) ? it->second.size() : 0u;
    const size_t existing_key_size = (it != out.end()) ? 0u : key.size();
    const size_t candidate_total =
        aggregate_bytes - existing_value_size + existing_key_size +
        value.size();
    if (candidate_total > kMaxTokenAggregateBytes) {
      if (!logged_aggregate_cap) {
        logged_aggregate_cap = true;
        LOG(WARNING) << "[Dashboard] Listener token-map aggregate cap reached ("
                     << kMaxTokenAggregateBytes
                     << " bytes); extra entries skipped.";
      }
      return;
    }

    aggregate_bytes = candidate_total;
    out[key] = std::move(value);
  };

  auto ingest = [&](const base::Value::Dict& src, bool allow_overwrite) {
    for (auto it : src) {
      const std::string* value = it.second.GetIfString();
      if (!value) {
        continue;
      }

      std::optional<ParsedTaggedKey> parsed = ParseTaggedKey(it.first);
      if (!parsed.has_value()) {
        continue;
      }

      store_token(parsed->clean_key, *value, allow_overwrite);
      store_token(parsed->base_key, *value, allow_overwrite);

      for (const ParsedTaggedKey::TagRef& tag : parsed->tags) {
        if (tag.raw_tag.empty()) {
          continue;
        }
        AppendUniqueTagValue(&tag_values, tag.raw_tag, *value, std::nullopt,
                             tag_sequence++);
        if (!tag.grouped_tag.empty() && tag.grouped_tag != tag.raw_tag) {
          AppendUniqueTagValue(&tag_values, tag.grouped_tag, *value, tag.order,
                               tag_sequence++);
        }
      }
    }
  };

  ingest(vars, /*allow_overwrite=*/true);
  ingest(secrets, /*allow_overwrite=*/false);

  for (const auto& [tag, values] : tag_values) {
    if (tag.empty() || values.empty()) {
      continue;
    }
    store_token(tag, JoinTagScalarValue(values), /*allow_overwrite=*/true);
  }
  return out;
}

std::string ReadOnMatchScriptIdFromListenerScript(Profile* profile,
                                                  const std::string& listener_id) {
  base::Value::Dict script;
  if (!LoadScriptFromStore(profile, listener_id, &script)) {
    return std::string();
  }
  base::Value::List timeline = FlattenTimelineForListenerScript(script);
  for (const auto& v : timeline) {
    const base::Value::Dict* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    const std::string* act = d->FindString("action");
    const std::string kind = act ? *act : std::string();
    if (kind != "listen") {
      continue;
    }
    const base::Value::Dict* params = d->FindDict("params");
    if (!params) {
      continue;
    }
    if (const std::string* s = params->FindString("onMatchScriptId")) {
      return *s;
    }
  }
  return std::string();
}

struct TriggerDispatchTarget {
  class Observer : public content::WebContentsObserver {
   public:
    explicit Observer(TriggerDispatchTarget* owner) : owner_(owner) {}
    ~Observer() override = default;

    void Watch(content::WebContents* contents) { Observe(contents); }

    void WebContentsDestroyed() override {
      if (owner_) {
        owner_->web_contents = nullptr;
      }
      Observe(nullptr);
    }

   private:
    raw_ptr<TriggerDispatchTarget> owner_ = nullptr;
  };

  TriggerDispatchTarget() : observer(std::make_unique<Observer>(this)) {}
  ~TriggerDispatchTarget() = default;

  raw_ptr<Browser> browser = nullptr;
  raw_ptr<content::WebContents> web_contents = nullptr;
  std::unique_ptr<Observer> observer;
};

using TriggerDispatchPerListener =
    std::map<std::string, std::unique_ptr<TriggerDispatchTarget>>;
using TriggerDispatchByProfile = std::map<Profile*, TriggerDispatchPerListener>;

TriggerDispatchByProfile& HeadlessTriggerDispatchTargets() {
  static base::NoDestructor<TriggerDispatchByProfile> targets;
  return *targets;
}

content::WebContents* ResolveReusableTriggerTab(Profile* profile,
                                                const std::string& listener_id) {
  auto& by_profile = HeadlessTriggerDispatchTargets();
  auto profile_it = by_profile.find(profile);
  if (profile_it == by_profile.end()) {
    return nullptr;
  }
  auto listener_it = profile_it->second.find(listener_id);
  if (listener_it == profile_it->second.end() || !listener_it->second) {
    return nullptr;
  }
  TriggerDispatchTarget* slot = listener_it->second.get();
  if (!slot->browser || !slot->web_contents) {
    return nullptr;
  }
  TabStripModel* tabs = slot->browser->tab_strip_model();
  if (!tabs) {
    return nullptr;
  }
  if (tabs->GetIndexOfWebContents(slot->web_contents) < 0) {
    return nullptr;
  }
  return slot->web_contents;
}

void StoreReusableTriggerTab(Profile* profile,
                             const std::string& listener_id,
                             Browser* browser,
                             content::WebContents* web_contents) {
  if (!profile || listener_id.empty() || !browser || !web_contents) {
    return;
  }
  auto& by_profile = HeadlessTriggerDispatchTargets();
  auto& per_listener = by_profile[profile];
  auto& slot_ptr = per_listener[listener_id];
  if (!slot_ptr) {
    slot_ptr = std::make_unique<TriggerDispatchTarget>();
  }
  slot_ptr->browser = browser;
  slot_ptr->web_contents = web_contents;
  slot_ptr->observer->Watch(web_contents);
}

struct TriggerDispatchRequest {
  std::string trigger_script_id;
  std::string listener_script_id;
  std::string preferred_start_url;
  base::flat_map<std::string, std::string> token_map;
};

struct PreparedTriggerDispatch {
  std::string trigger_script_id;
  std::string listener_script_id;
  GURL start_url = GURL("about:blank");
  base::Value::List timeline;
  base::Value::List key_timeline;
  base::flat_map<std::string, std::string> token_map;
  std::string error;
};

PreparedTriggerDispatch PrepareTriggerDispatchBlocking(
    Profile* profile,
    TriggerDispatchRequest request) {
  PreparedTriggerDispatch out;
  out.listener_script_id = request.listener_script_id;
  out.trigger_script_id = request.trigger_script_id;
  out.token_map = std::move(request.token_map);

  if (out.trigger_script_id.empty() && !out.listener_script_id.empty()) {
    out.trigger_script_id =
        ReadOnMatchScriptIdFromListenerScript(profile, out.listener_script_id);
  }
  if (out.trigger_script_id.empty()) {
    out.error = out.listener_script_id.empty()
                    ? "Empty trigger script id."
                    : "No onMatchScriptId configured for listener script: " +
                          out.listener_script_id;
    return out;
  }

  base::Value::Dict script;
  if (dashboard::ScriptStore::IsCollectionRef(out.trigger_script_id)) {
    const std::string collection =
        dashboard::ScriptStore::ParseCollectionRef(out.trigger_script_id);
    if (!dashboard::ScriptStore::IsValidCollectionName(collection)) {
      out.error = "Invalid collection name: " + collection;
      return out;
    }
    base::Value::Dict all = dashboard::ScriptStore::LoadAll(profile);
    std::vector<std::pair<std::string, std::string>> ranked;
    for (auto [id, value] : all) {
      if (!value.is_dict()) {
        continue;
      }
      if (dashboard::ScriptStore::CollectionOfScriptId(id) != collection) {
        continue;
      }
      const base::Value::Dict& member = value.GetDict();
      const std::string* kind = member.FindString("kind");
      const std::string kind_l =
          kind ? base::ToLowerASCII(*kind) : std::string("main");
      if (kind_l != "main") {
        continue;
      }
      const std::string* name = member.FindString("name");
      ranked.emplace_back(name ? *name : id, id);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (ranked.empty()) {
      out.error = "Collection has no main scripts: " + collection;
      return out;
    }
    base::Value::List actions;
    for (const auto& item : ranked) {
      base::Value::Dict step;
      step.Set("action", "call_script");
      base::Value::Dict params;
      params.Set("scriptId", item.second);
      step.Set("params", std::move(params));
      actions.Append(std::move(step));
    }
    script.Set("id", out.trigger_script_id);
    script.Set("name", "Collection: " + collection);
    script.Set("kind", "main");
    script.Set("actions", std::move(actions));
  } else if (!LoadScriptFromStore(profile, out.trigger_script_id, &script) ||
             script.empty()) {
    out.error = "Trigger script not found: " + out.trigger_script_id;
    return out;
  }

  out.timeline = FlattenTimelineForListenerScript(script);
  if (out.timeline.empty()) {
    out.error = "Trigger script has no actions: " + out.trigger_script_id;
    return out;
  }

  // Prefer the matched link from the listener event over any stale
  // url_contexts / contextUrl baked into the trigger script (common when a
  // navigate step was recorded against a previous destination).
  auto prefer_token_start = [&](const std::string& key) -> bool {
    auto it = out.token_map.find(key);
    if (it == out.token_map.end() || it->second.empty()) {
      return false;
    }
    GURL gurl(it->second);
    if (!gurl.is_valid() || !gurl.SchemeIsHTTPOrHTTPS()) {
      return false;
    }
    out.start_url = gurl;
    return true;
  };
  if (!prefer_token_start("listener.match.url") &&
      !prefer_token_start("listener.match") &&
      !prefer_token_start("discordURLtest1")) {
    // If the trigger timeline still references listener tokens, never open a
    // baked recording URL (e.g. an old stake.com contextUrl). Wait for tokens.
    bool timeline_uses_listener_token = false;
    for (const auto& step : out.timeline) {
      const base::Value::Dict* action = step.GetIfDict();
      if (!action) {
        continue;
      }
      const base::Value::Dict* params = action->FindDict("params");
      if (!params) {
        continue;
      }
      if (const std::string* url = params->FindString("url")) {
        if (url->find("{listener.") != std::string::npos ||
            url->find("{discordURLtest1}") != std::string::npos) {
          timeline_uses_listener_token = true;
          break;
        }
      }
    }
    if (!timeline_uses_listener_token) {
      std::optional<GURL> script_start = ExtractScriptStartUrl(script);
      if (script_start.has_value()) {
        out.start_url = *script_start;
      }
    }
    if (out.start_url.is_empty() && !request.preferred_start_url.empty()) {
      GURL preferred(request.preferred_start_url);
      if (preferred.is_valid() && preferred.SchemeIsHTTPOrHTTPS()) {
        out.start_url = preferred;
      }
    }
  }
  const std::string key_script_id = ReadKeyScriptIdFromScript(script);
  if (!key_script_id.empty()) {
    base::Value::Dict key_script;
    if (LoadScriptFromStore(profile, key_script_id, &key_script) &&
        !key_script.empty()) {
      out.key_timeline = FlattenTimelineForListenerScript(key_script);
    }
  }
  return out;
}

std::vector<PreparedTriggerDispatch> PrepareTriggerDispatchesBlocking(
    Profile* profile,
    std::vector<TriggerDispatchRequest> requests) {
  std::vector<PreparedTriggerDispatch> out;
  out.reserve(requests.size());
  for (auto& request : requests) {
    out.push_back(
        PrepareTriggerDispatchBlocking(profile, std::move(request)));
  }
  return out;
}

std::string StartPreparedScriptInBackgroundTab(
    Profile* profile,
    const PreparedTriggerDispatch& prepared,
    std::string* error_out) {
  if (!profile) {
    if (error_out) {
      *error_out = "Missing profile.";
    }
    return std::string();
  }
  if (prepared.trigger_script_id.empty()) {
    if (error_out) {
      *error_out = "Empty trigger script id.";
    }
    return std::string();
  }
  if (!prepared.error.empty()) {
    if (error_out) {
      *error_out = prepared.error;
    }
    return std::string();
  }
  if (prepared.timeline.empty()) {
    if (error_out) {
      *error_out =
          "Trigger script has no actions: " + prepared.trigger_script_id;
    }
    return std::string();
  }

  Browser* browser = chrome::FindLastActiveWithProfile(profile);
  if (!browser) {
    if (error_out) {
      *error_out = "No active browser to open background tab.";
    }
    return std::string();
  }

  content::WebContents* target_wc = nullptr;
  if (!prepared.listener_script_id.empty()) {
    target_wc =
        ResolveReusableTriggerTab(profile, prepared.listener_script_id);
  }
  if (!target_wc) {
    const GURL start_url =
        prepared.start_url.is_valid() ? prepared.start_url : GURL("about:blank");
    NavigateParams np(browser, start_url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    np.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
    Navigate(&np);
    target_wc = np.navigated_or_inserted_contents;
    if (target_wc && !prepared.listener_script_id.empty()) {
      StoreReusableTriggerTab(profile, prepared.listener_script_id, browser,
                              target_wc);
    }
  }
  if (!target_wc) {
    if (error_out) {
      *error_out = "Failed to create background tab for trigger script.";
    }
    return std::string();
  }

  dashboard::ScriptExecutionService* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile);
  if (!exec_service) {
    if (error_out) {
      *error_out = "No ScriptExecutionService.";
    }
    return std::string();
  }

  base::Value::List run_timeline = prepared.timeline.Clone();
  base::Value::List run_key_timeline = prepared.key_timeline.Clone();
  if (!prepared.token_map.empty()) {
    for (auto& step : run_timeline) {
      ExpandTokensInValue(&step, prepared.token_map);
    }
    for (auto& step : run_key_timeline) {
      ExpandTokensInValue(&step, prepared.token_map);
    }
  }

  const base::Value::List* key_ptr =
      run_key_timeline.empty() ? nullptr : &run_key_timeline;
  const std::string run_id = exec_service->StartScriptRun(
      prepared.trigger_script_id, run_timeline, target_wc, key_ptr);
  if (run_id.empty() && error_out) {
    *error_out = "Failed to start trigger script run.";
  }
  return run_id;
}

void DispatchPreparedTriggerScripts(Profile* profile,
                                    std::vector<PreparedTriggerDispatch> prepared,
                                    std::string log_prefix) {
  if (!profile) {
    return;
  }
  for (const auto& dispatch : prepared) {
    if (!dispatch.error.empty()) {
      LOG(ERROR) << "[Dashboard] " << log_prefix << " failed for "
                 << (dispatch.trigger_script_id.empty()
                         ? std::string("<empty_trigger_script_id>")
                         : dispatch.trigger_script_id)
                 << (dispatch.listener_script_id.empty()
                         ? std::string()
                         : std::string(" listener=") +
                               dispatch.listener_script_id)
                 << ": " << dispatch.error;
      continue;
    }
    std::string trigger_error;
    const std::string run_id =
        StartPreparedScriptInBackgroundTab(profile, dispatch, &trigger_error);
    if (run_id.empty()) {
      LOG(ERROR) << "[Dashboard] " << log_prefix << " failed for "
                 << dispatch.trigger_script_id
                 << (dispatch.listener_script_id.empty()
                         ? std::string()
                         : std::string(" listener=") +
                               dispatch.listener_script_id)
                 << ": " << trigger_error;
    } else {
      LOG(INFO) << "[Dashboard] " << log_prefix << " "
                << dispatch.trigger_script_id
                << (dispatch.listener_script_id.empty()
                        ? std::string()
                        : std::string(" listener=") +
                              dispatch.listener_script_id)
                << " run_id=" << run_id;
    }
  }
}

void DispatchTriggerScriptsAsync(Profile* profile,
                                 std::vector<TriggerDispatchRequest> dispatches,
                                 std::string log_prefix) {
  if (!profile || dispatches.empty()) {
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&PrepareTriggerDispatchesBlocking,
                     base::Unretained(profile), std::move(dispatches)),
      base::BindOnce(
          [](Profile* profile, std::string log_prefix,
             std::vector<PreparedTriggerDispatch> prepared) {
            DispatchPreparedTriggerScripts(profile, std::move(prepared),
                                           std::move(log_prefix));
          },
          base::Unretained(profile), std::move(log_prefix)));
}

bool ResolveRunListenersHeadlessForScript(const base::Value::Dict& script) {
  bool global_default = true;
  if (const base::Value::Dict* listener_mode =
          dashboard::GlobalSettingsStore().FindDict("listenerMode")) {
    if (auto v = listener_mode->FindBool("runListenersInHeadlessMode")) {
      global_default = *v;
    }
  }

  if (!global_default) {
    return false;
  }

  const base::Value::Dict* overrides = ReadScriptSettingsOverrides(script);
  if (!overrides) {
    return true;
  }
  if (auto v = overrides->FindBool("runListenersInHeadlessMode")) {
    return *v;
  }
  return true;
}

bool AllowHeadlessFallbackToBackgroundTab() {
  if (const base::Value::Dict* listener_mode =
          dashboard::GlobalSettingsStore().FindDict("listenerMode")) {
    if (auto v = listener_mode->FindBool(
            "fallbackToBackgroundTabOnHeadlessFailure")) {
      return *v;
    }
  }
  // Prefer failing closed so "headless" never silently means a headed tab.
  return false;
}

base::FilePath InstalledHeadlessHostScriptPath() {
  if (const char* home = std::getenv("SIMULACHROME_HOME"); home && *home) {
    return base::FilePath::FromUTF8Unsafe(home)
        .AppendASCII("Headless")
        .AppendASCII("headless_listener_host.py");
  }
  base::FilePath app_data_dir;
  if (base::PathService::Get(base::DIR_APP_DATA, &app_data_dir) &&
      !app_data_dir.empty()) {
    return app_data_dir.AppendASCII("Simulachrome")
        .AppendASCII("Headless")
        .AppendASCII("headless_listener_host.py");
  }
  return base::FilePath();
}

void MaybeInstallHeadlessHostScript(const base::FilePath& source) {
  if (source.empty() || !base::PathExists(source)) {
    return;
  }
  const base::FilePath installed = InstalledHeadlessHostScriptPath();
  if (installed.empty() || installed == source) {
    return;
  }
  if (base::PathExists(installed)) {
    return;
  }
  if (!base::CreateDirectory(installed.DirName())) {
    return;
  }
  if (!base::CopyFile(source, installed)) {
    LOG(WARNING) << "[Dashboard] Failed to install headless host script to "
                 << installed;
  } else {
    LOG(INFO) << "[Dashboard] Installed headless host script at " << installed;
  }
}

std::optional<base::FilePath> ResolveHeadlessHostScriptPath() {
  const char* env = std::getenv("SIMULACHROME_HEADLESS_HOST_SCRIPT");
  if (env && *env) {
    base::FilePath path = base::FilePath::FromUTF8Unsafe(env);
    if (base::PathExists(path)) {
      return path;
    }
  }

  const base::FilePath installed = InstalledHeadlessHostScriptPath();
  if (!installed.empty() && base::PathExists(installed)) {
    return installed;
  }

  const base::FilePath relative_host_path =
      base::FilePath(FILE_PATH_LITERAL("chrome"))
          .AppendASCII("browser")
          .AppendASCII("resources")
          .AppendASCII("dashboard")
          .AppendASCII("headless_listener_host.py");

  std::vector<base::FilePath> roots;
  base::FilePath cwd;
  if (base::GetCurrentDirectory(&cwd)) {
    roots.push_back(cwd);
  }
  roots.push_back(base::CommandLine::ForCurrentProcess()->GetProgram().DirName());
  base::FilePath exe = base::CommandLine::ForCurrentProcess()->GetProgram();
  if (!exe.IsAbsolute()) {
    base::FilePath abs_exe = base::MakeAbsoluteFilePath(exe);
    if (!abs_exe.empty()) {
      roots.push_back(abs_exe.DirName());
    }
  }

  for (const auto& start : roots) {
    if (start.empty()) {
      continue;
    }
    base::FilePath cur = start;
    while (true) {
      const base::FilePath candidate = cur.Append(relative_host_path);
      if (base::PathExists(candidate)) {
        MaybeInstallHeadlessHostScript(candidate);
        return candidate;
      }
      const base::FilePath parent = cur.DirName();
      if (parent == cur || parent.empty()) {
        break;
      }
      cur = parent;
    }
  }
  return std::nullopt;
}

base::FilePath ResolvePython3Binary() {
  const char* env = std::getenv("SIMULACHROME_PYTHON_BIN");
  if (env && *env) {
    base::FilePath from_env = base::FilePath::FromUTF8Unsafe(env);
    if (base::PathExists(from_env)) {
      return from_env;
    }
  }

  const base::FilePath candidates[] = {
      base::FilePath(FILE_PATH_LITERAL("/usr/bin/python3")),
      base::FilePath(FILE_PATH_LITERAL("/opt/homebrew/bin/python3")),
      base::FilePath(FILE_PATH_LITERAL("/usr/local/bin/python3")),
  };
  for (const auto& candidate : candidates) {
    if (base::PathExists(candidate)) {
      return candidate;
    }
  }

  return base::FilePath(FILE_PATH_LITERAL("python3"));
}

std::optional<base::Value> ParseFirstJsonObjectFromOutput(
    const std::string& output) {
  const size_t start = output.find('{');
  if (start == std::string::npos) {
    return std::nullopt;
  }

  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (size_t i = start; i < output.size(); ++i) {
    const char ch = output[i];
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (ch == '\\') {
        escape = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '{') {
      ++depth;
      continue;
    }
    if (ch == '}') {
      --depth;
      if (depth == 0) {
        std::string candidate = output.substr(start, i - start + 1);
        return base::JSONReader::Read(candidate,
                                      base::JSON_PARSE_CHROMIUM_EXTENSIONS);
      }
    }
  }
  return std::nullopt;
}

struct HeadlessHostCommandResult {
  bool ok = false;
  base::Value::Dict response;
  std::string error;
};

struct ListenerEventsFetchResult {
  base::Value::List events;
  std::vector<TriggerDispatchRequest> headless_trigger_dispatches;
  std::string headless_warning;
};

struct HeadlessRelayState {
  bool enabled = false;
  bool in_flight = false;
};

std::map<Profile*, HeadlessRelayState>& HeadlessRelayStates() {
  static base::NoDestructor<std::map<Profile*, HeadlessRelayState>> states;
  return *states;
}

constexpr int kHeadlessRelayWaitMs = 10000;
constexpr base::TimeDelta kHeadlessRelayImmediateRescheduleDelay =
    base::Milliseconds(1);
constexpr base::TimeDelta kHeadlessRelayErrorBackoff = base::Seconds(2);

void ScheduleHeadlessRelayPoll(Profile* profile, base::TimeDelta delay);

HeadlessHostCommandResult InvokeHeadlessListenerHostForResultBlocking(
    const std::string& command,
    const std::string& script_id,
    const base::FilePath& profile_path,
    const base::FilePath& scripts_dir,
    const base::FilePath& browser_program,
    int wait_ms = 0);

HeadlessHostCommandResult InvokeHeadlessListenerHostForResultWithRequestBlocking(
    const std::string& command,
    const std::string& script_id,
    const base::FilePath& profile_path,
    const base::FilePath& scripts_dir,
    const base::FilePath& browser_program,
    int wait_ms,
    const std::string& request_json);

void AppendListenerEventAsync(Profile* profile, base::Value::Dict event) {
  if (!profile) {
    return;
  }
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, base::Value::Dict event) {
            (void)dashboard::RunHistoryStore::AppendListenerEvent(profile, event);
          },
          base::Unretained(profile), std::move(event)));
}

void ProcessHeadlessEvents(Profile* profile,
                           const base::Value::List& host_events) {
  if (!profile) {
    return;
  }
  std::vector<TriggerDispatchRequest> dispatches;
  for (const auto& ev : host_events) {
    const base::Value::Dict* d = ev.GetIfDict();
    if (!d) {
      continue;
    }
    base::Value::Dict normalized = d->Clone();
    if (!normalized.Find("ts_ms")) {
      normalized.Set(
          "ts_ms",
          static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
    }
    const std::string action =
        normalized.FindString("action")
            ? base::ToLowerASCII(*normalized.FindString("action"))
            : std::string();
    if (action == "trigger") {
      const std::string listener_script_id =
          normalized.FindString("listener_script_id")
              ? *normalized.FindString("listener_script_id")
              : std::string();
      const std::string trigger_script_id =
          normalized.FindString("trigger_script_id")
              ? *normalized.FindString("trigger_script_id")
              : std::string();
      std::string preferred_start_url;
      base::flat_map<std::string, std::string> token_map =
          BuildListenerMatchTokenMapFromEvent(normalized);
      auto token_it = token_map.find("listener.match.url");
      if (token_it != token_map.end() && !token_it->second.empty()) {
        preferred_start_url = token_it->second;
      } else if (const std::string* match_url =
                     normalized.FindString("match_url")) {
        preferred_start_url = *match_url;
      }
      // Never fall back to the listener pageUrl (e.g. discord.com/channels/...).
      // An empty start URL opens about:blank, then navigate uses the token.
      dispatches.push_back(TriggerDispatchRequest{
          trigger_script_id, listener_script_id, preferred_start_url,
          std::move(token_map)});
    }
    AppendListenerEventAsync(profile, std::move(normalized));
  }

  DispatchTriggerScriptsAsync(profile, std::move(dispatches),
                              "Headless listener trigger dispatch");
  if (!host_events.empty()) {
    DashboardUIMessageHandler::NotifyListenersChanged();
  }
}

void PollHeadlessRelayNow(Profile* profile) {
  if (!profile) {
    return;
  }
  auto& states = HeadlessRelayStates();
  auto it = states.find(profile);
  if (it == states.end() || !it->second.enabled || it->second.in_flight) {
    return;
  }
  it->second.in_flight = true;

  const base::FilePath profile_path = profile->GetPath();
  const base::FilePath scripts_dir = dashboard::ScriptStore::ScriptsDir(profile);
  const base::FilePath browser_program =
      base::CommandLine::ForCurrentProcess()->GetProgram();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&InvokeHeadlessListenerHostForResultBlocking, "poll_events",
                     /*script_id=*/"", profile_path, scripts_dir, browser_program,
                     kHeadlessRelayWaitMs),
      base::BindOnce(
          [](Profile* profile, HeadlessHostCommandResult host_result) {
            auto& states = HeadlessRelayStates();
            auto it = states.find(profile);
            if (it == states.end()) {
              return;
            }
            it->second.in_flight = false;
            if (!it->second.enabled) {
              return;
            }
            if (host_result.ok) {
              if (const base::Value::List* host_events =
                      host_result.response.FindList("events")) {
                ProcessHeadlessEvents(profile, *host_events);
              }
              ScheduleHeadlessRelayPoll(
                  profile, kHeadlessRelayImmediateRescheduleDelay);
            } else if (!host_result.error.empty()) {
              VLOG(1) << "[Dashboard] headless relay poll warning: "
                      << host_result.error;
              ScheduleHeadlessRelayPoll(profile, kHeadlessRelayErrorBackoff);
            }
          },
          base::Unretained(profile)));
}

void ScheduleHeadlessRelayPoll(Profile* profile, base::TimeDelta delay) {
  if (!profile) {
    return;
  }
  auto& states = HeadlessRelayStates();
  auto it = states.find(profile);
  if (it == states.end() || !it->second.enabled) {
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&PollHeadlessRelayNow, base::Unretained(profile)),
      delay);
}

void EnsureHeadlessRelayEnabled(Profile* profile) {
  if (!profile) {
    return;
  }
  auto& states = HeadlessRelayStates();
  HeadlessRelayState& state = states[profile];
  if (state.enabled) {
    return;
  }
  state.enabled = true;
  state.in_flight = false;
  PollHeadlessRelayNow(profile);
}

#if BUILDFLAG(IS_POSIX)
bool WriteAllToFd(int fd, base::span<const uint8_t> bytes) {
  while (!bytes.empty()) {
    const ssize_t rv = HANDLE_EINTR(write(fd, bytes.data(), bytes.size()));
    if (rv <= 0) {
      return false;
    }
    bytes = bytes.subspan(static_cast<size_t>(rv));
  }
  return true;
}

bool ReadAllFromFd(int fd, base::span<uint8_t> bytes) {
  while (!bytes.empty()) {
    const ssize_t rv = HANDLE_EINTR(read(fd, bytes.data(), bytes.size()));
    if (rv <= 0) {
      return false;
    }
    bytes = bytes.subspan(static_cast<size_t>(rv));
  }
  return true;
}

bool ReadFramedJsonFromFd(int fd, std::string* payload_json) {
  if (!payload_json) {
    return false;
  }

  std::array<uint8_t, 4> header{};
  if (!ReadAllFromFd(fd, base::span<uint8_t>(header))) {
    return false;
  }

  const uint32_t payload_len =
      static_cast<uint32_t>(header[0]) |
      (static_cast<uint32_t>(header[1]) << 8) |
      (static_cast<uint32_t>(header[2]) << 16) |
      (static_cast<uint32_t>(header[3]) << 24);

  constexpr uint32_t kMaxNativeResponseBytes = 16u * 1024u * 1024u;
  if (payload_len > kMaxNativeResponseBytes) {
    return false;
  }

  std::string response(payload_len, '\0');
  if (payload_len > 0 &&
      !ReadAllFromFd(fd, base::as_writable_byte_span(response))) {
    return false;
  }

  *payload_json = std::move(response);
  return true;
}

HeadlessHostCommandResult InvokeHeadlessListenerHostViaNativeMessagingBlocking(
    const base::FilePath& python_bin,
    const base::FilePath& host_script,
    const std::string& command,
    const std::string& script_id,
    const base::FilePath& profile_path,
    const base::FilePath& scripts_dir,
    const base::FilePath& browser_program,
    int wait_ms,
    const std::string& request_json) {
  // These helpers always run on ThreadPool MayBlock tasks and must wait on the
  // child host process. Allow base sync waits explicitly (MayBlock alone is not
  // enough for WaitForExit / GetAppOutputAndError).
  dashboard::HeadlessHostScopedAllowBaseSyncPrimitives allow_wait;
  HeadlessHostCommandResult out;

  base::Value::Dict native_msg;
  native_msg.Set("type", command);
  native_msg.Set("profile_dir", profile_path.AsUTF8Unsafe());
  native_msg.Set("browser_bin", browser_program.AsUTF8Unsafe());
  if (wait_ms > 0) {
    native_msg.Set("wait_ms", wait_ms);
  }
  if (!script_id.empty()) {
    native_msg.Set("script_id", script_id);
    base::FilePath script_path =
        scripts_dir.AppendASCII(script_id + ".json");
    const size_t slash = script_id.find('/');
    if (slash != std::string::npos && slash + 1 < script_id.size()) {
      script_path = scripts_dir.AppendASCII(script_id.substr(0, slash))
                        .AppendASCII(script_id.substr(slash + 1) + ".json");
    }
    native_msg.Set("script_path", script_path.AsUTF8Unsafe());
  }

  if (!request_json.empty()) {
    std::optional<base::Value> parsed_request =
        base::JSONReader::Read(request_json,
                               base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!parsed_request || !parsed_request->is_dict()) {
      out.error = "Failed to parse headless investigation request payload.";
      return out;
    }
    native_msg.Set("request", parsed_request->GetDict().Clone());
  }

  std::string native_msg_json;
  if (!base::JSONWriter::Write(base::Value(std::move(native_msg)),
                               &native_msg_json)) {
    out.error = "Failed to serialize headless host native request.";
    return out;
  }
  if (native_msg_json.size() > std::numeric_limits<uint32_t>::max()) {
    out.error = "Headless host native request is too large.";
    return out;
  }

  int stdin_pipe[2];
  if (HANDLE_EINTR(pipe(stdin_pipe)) != 0) {
    out.error =
        "Failed to create native messaging stdin pipe for headless host.";
    return out;
  }
  int stdout_pipe[2];
  if (HANDLE_EINTR(pipe(stdout_pipe)) != 0) {
    IGNORE_EINTR(close(stdin_pipe[0]));
    IGNORE_EINTR(close(stdin_pipe[1]));
    out.error =
        "Failed to create native messaging stdout pipe for headless host.";
    return out;
  }

  base::ScopedFD child_stdin_read(stdin_pipe[0]);
  base::ScopedFD parent_stdin_write(stdin_pipe[1]);
  base::ScopedFD parent_stdout_read(stdout_pipe[0]);
  base::ScopedFD child_stdout_write(stdout_pipe[1]);

  base::LaunchOptions options;
  options.fds_to_remap.emplace_back(child_stdin_read.get(), STDIN_FILENO);
  options.fds_to_remap.emplace_back(child_stdout_write.get(), STDOUT_FILENO);

  std::vector<std::string> argv = {
      python_bin.AsUTF8Unsafe(),
      host_script.AsUTF8Unsafe(),
  };

  base::Process process = base::LaunchProcess(argv, options);
  if (!process.IsValid()) {
    out.error = "Failed to launch headless host process for native request.";
    return out;
  }

  child_stdin_read.reset();
  child_stdout_write.reset();

  const uint32_t msg_len = static_cast<uint32_t>(native_msg_json.size());
  std::array<uint8_t, 4> header = {
      static_cast<uint8_t>(msg_len & 0xffu),
      static_cast<uint8_t>((msg_len >> 8) & 0xffu),
      static_cast<uint8_t>((msg_len >> 16) & 0xffu),
      static_cast<uint8_t>((msg_len >> 24) & 0xffu),
  };

  if (!WriteAllToFd(parent_stdin_write.get(), base::span<const uint8_t>(header)) ||
      !WriteAllToFd(parent_stdin_write.get(),
                    base::as_byte_span(native_msg_json))) {
    out.error = "Failed to write native request to headless host process.";
    process.Terminate(0, false);
    return out;
  }
  parent_stdin_write.reset();

  std::string response_json;
  if (!ReadFramedJsonFromFd(parent_stdout_read.get(), &response_json)) {
    out.error = "Headless host native response was invalid or incomplete.";
    process.Terminate(0, false);
    return out;
  }
  parent_stdout_read.reset();

  int exit_code = 0;
  process.WaitForExit(&exit_code);

  auto parsed = base::JSONReader::Read(response_json,
                                       base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    out.error = "Headless host native response JSON parsing failed.";
    return out;
  }

  base::Value::Dict res = parsed->GetDict().Clone();
  out.ok = res.FindBool("ok").value_or(false);
  if (!out.ok) {
    out.error = res.FindString("error")
                    ? *res.FindString("error")
                    : "Headless host native command failed.";
  } else if (exit_code != 0) {
    out.ok = false;
    out.error = "Headless host process exited with non-zero status.";
  }
  out.response = std::move(res);
  return out;
}
#endif  // BUILDFLAG(IS_POSIX)

HeadlessHostCommandResult InvokeHeadlessListenerHostForResultBlocking(
    const std::string& command,
    const std::string& script_id,
    const base::FilePath& profile_path,
    const base::FilePath& scripts_dir,
    const base::FilePath& browser_program,
    int wait_ms) {
  return InvokeHeadlessListenerHostForResultWithRequestBlocking(
      command, script_id, profile_path, scripts_dir, browser_program, wait_ms,
      std::string());
}

HeadlessHostCommandResult InvokeHeadlessListenerHostForResultWithRequestBlocking(
    const std::string& command,
    const std::string& script_id,
    const base::FilePath& profile_path,
    const base::FilePath& scripts_dir,
    const base::FilePath& browser_program,
    int wait_ms,
    const std::string& request_json) {
  // Same as native-messaging path: blocking host spawn/wait is intentional.
  dashboard::HeadlessHostScopedAllowBaseSyncPrimitives allow_wait;
  HeadlessHostCommandResult out;
  const std::optional<base::FilePath> host_script =
      ResolveHeadlessHostScriptPath();
  if (!host_script) {
    out.error =
        "Headless listener host script not found. Set "
        "SIMULACHROME_HEADLESS_HOST_SCRIPT to enable headless listeners.";
    return out;
  }

  const base::FilePath python_bin = ResolvePython3Binary();
  if (!request_json.empty()) {
#if BUILDFLAG(IS_POSIX)
    return InvokeHeadlessListenerHostViaNativeMessagingBlocking(
        python_bin, *host_script, command, script_id, profile_path, scripts_dir,
        browser_program, wait_ms, request_json);
#else
    out.error =
        "Secure headless request transport is unsupported on this platform.";
    return out;
#endif
  }

  base::FilePath absolute_browser = browser_program;
  if (!absolute_browser.IsAbsolute()) {
    base::FilePath resolved = base::MakeAbsoluteFilePath(absolute_browser);
    if (!resolved.empty()) {
      absolute_browser = std::move(resolved);
    }
  }

  std::vector<std::string> argv = {
      python_bin.AsUTF8Unsafe(),
      host_script->AsUTF8Unsafe(),
      "--command",
      command,
      "--profile-dir",
      profile_path.AsUTF8Unsafe(),
      "--browser-bin",
      absolute_browser.AsUTF8Unsafe(),
  };
  if (wait_ms > 0) {
    argv.push_back("--wait-ms");
    argv.push_back(base::NumberToString(wait_ms));
  }
  if (!script_id.empty()) {
    base::FilePath script_path =
        scripts_dir.AppendASCII(script_id + ".json");
    // Collection ids use Scripts/<collection>/<id>.json.
    const size_t slash = script_id.find('/');
    if (slash != std::string::npos && slash + 1 < script_id.size()) {
      script_path = scripts_dir.AppendASCII(script_id.substr(0, slash))
                        .AppendASCII(script_id.substr(slash + 1) + ".json");
    }
    argv.push_back("--script-id");
    argv.push_back(script_id);
    argv.push_back("--script-path");
    argv.push_back(script_path.AsUTF8Unsafe());
  }
  std::string output;
  const bool process_ok = base::GetAppOutputAndError(argv, &output);

  auto parsed =
      base::JSONReader::Read(output, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    std::vector<std::string> lines = base::SplitString(
        output, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (!lines.empty()) {
      parsed = base::JSONReader::Read(lines.back(),
                                      base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    }
  }
  if ((!parsed || !parsed->is_dict()) && !output.empty()) {
    parsed = ParseFirstJsonObjectFromOutput(output);
  }
  // Host returns exit code 1 on {"ok":false}; still prefer the structured error.
  if (parsed && parsed->is_dict()) {
    base::Value::Dict res = parsed->GetDict().Clone();
    out.ok = res.FindBool("ok").value_or(false);
    if (!out.ok) {
      out.error = res.FindString("error")
                      ? *res.FindString("error")
                      : "Headless listener host command failed.";
    }
    out.response = std::move(res);
    return out;
  }

  if (!process_ok) {
    out.error = base::StrCat(
        {"Failed to execute headless listener host command (python=",
         python_bin.AsUTF8Unsafe(), ")."});
    if (!output.empty()) {
      out.error = base::StrCat(
          {out.error, " Host output: ",
           output.substr(0, std::min<size_t>(output.size(), 512))});
    }
    return out;
  }

  out.error = "Headless listener host returned invalid response.";
  if (!output.empty()) {
    out.error = base::StrCat(
        {out.error, " Host output: ",
         output.substr(0, std::min<size_t>(output.size(), 512))});
  }
  return out;
}

void PrewarmHeadlessListenerHostBlocking(
    const base::FilePath& profile_path,
    const base::FilePath& scripts_dir,
    const base::FilePath& browser_program) {
  HeadlessHostCommandResult result = InvokeHeadlessListenerHostForResultBlocking(
      "warm_headless", /*script_id=*/"", profile_path, scripts_dir,
      browser_program, /*wait_ms=*/0);
  if (result.ok) {
    LOG(INFO) << "[Dashboard] Headless listener host prewarmed";
  } else {
    LOG(WARNING) << "[Dashboard] Headless listener host prewarm failed: "
                 << result.error;
  }
}

}  // namespace

namespace dashboard {

void MaybePrewarmHeadlessListenerHost(Profile* profile) {
  if (!profile) {
    return;
  }
  bool run_headless = true;
  if (const base::Value::Dict* listener_mode =
          GlobalSettingsStore().FindDict("listenerMode")) {
    if (auto v = listener_mode->FindBool("runListenersInHeadlessMode")) {
      run_headless = *v;
    }
  }
  if (!run_headless) {
    return;
  }

  const base::FilePath profile_path = profile->GetPath();
  const base::FilePath scripts_dir = ScriptStore::ScriptsDir(profile);
  base::FilePath browser_program =
      base::CommandLine::ForCurrentProcess()->GetProgram();
  if (!browser_program.IsAbsolute()) {
    base::FilePath resolved = base::MakeAbsoluteFilePath(browser_program);
    if (!resolved.empty()) {
      browser_program = std::move(resolved);
    }
  }

  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&PrewarmHeadlessListenerHostBlocking, profile_path,
                     scripts_dir, browser_program));
}

void InvestigatePageOnHeadlessListener(
    Profile* profile,
    base::Value::Dict payload,
    base::OnceCallback<void(base::Value::Dict)> callback) {
  if (!profile) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "Missing profile.");
    out.Set("command", "investigate_page");
    out.Set("transport", "headless_listener");
    std::move(callback).Run(std::move(out));
    return;
  }

  // Default to closing the headless CDP target when the probe finishes.
  // Callers may keep it warm with close_target/closeTarget=false.
  if (!payload.FindBool("close_target").has_value() &&
      !payload.FindBool("closeTarget").has_value()) {
    payload.Set("close_target", true);
    payload.Set("closeTarget", true);
  }

  int wait_ms = 0;
  if (auto wait = payload.FindInt("wait_ms")) {
    wait_ms = *wait;
  } else if (auto wait_alt = payload.FindInt("waitMs")) {
    wait_ms = *wait_alt;
  }
  // Investigation often includes navigate + settle + snapshot; allow up to 2m.
  if (wait_ms <= 0) {
    wait_ms = 120000;
  }
  wait_ms = std::clamp(wait_ms, 1, 180000);

  std::string request_json;
  if (!base::JSONWriter::Write(base::Value(payload.Clone()), &request_json) ||
      request_json.empty()) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "Failed to serialize investigation payload.");
    out.Set("command", "investigate_page");
    out.Set("transport", "headless_listener");
    std::move(callback).Run(std::move(out));
    return;
  }

  const base::FilePath profile_path = profile->GetPath();
  const base::FilePath scripts_dir = ScriptStore::ScriptsDir(profile);
  base::FilePath browser_program =
      base::CommandLine::ForCurrentProcess()->GetProgram();
  if (!browser_program.IsAbsolute()) {
    base::FilePath resolved = base::MakeAbsoluteFilePath(browser_program);
    if (!resolved.empty()) {
      browser_program = std::move(resolved);
    }
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&InvokeHeadlessListenerHostForResultWithRequestBlocking,
                     "investigate_page", /*script_id=*/"", profile_path,
                     scripts_dir, browser_program, wait_ms, request_json),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value::Dict)> callback,
             HeadlessHostCommandResult host_result) {
            base::Value::Dict out = host_result.response.Clone();
            out.Set("ok", host_result.ok);
            out.Set("command", "investigate_page");
            out.Set("transport", "headless_listener");
            if (!host_result.ok) {
              out.Set("error", host_result.error.empty()
                                   ? "Headless investigation failed."
                                   : host_result.error);
            }
            std::move(callback).Run(std::move(out));
          },
          std::move(callback)));
}

}  // namespace dashboard

void DashboardUIMessageHandler::HandleGetListenerEvents(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string from_iso =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                               : std::string();
  const std::string to_iso =
      (args.size() > 2 && args[2].is_string()) ? args[2].GetString()
                                               : std::string();
  uint32_t limit = 2000;
  if (args.size() > 3 && args[3].is_int()) {
    limit = static_cast<uint32_t>(std::max(0, args[3].GetInt()));
  } else if (args.size() > 3 && args[3].is_double()) {
    limit = static_cast<uint32_t>(std::max(0.0, args[3].GetDouble()));
  }

  const base::Time now = base::Time::Now();
  base::Time from = now - base::Days(7);
  base::Time to = now + base::Days(1);
  if (!from_iso.empty()) {
    base::Time parsed;
    if (base::Time::FromString(from_iso.c_str(), &parsed)) {
      from = parsed;
    }
  }
  if (!to_iso.empty()) {
    base::Time parsed;
    if (base::Time::FromString(to_iso.c_str(), &parsed)) {
      to = parsed;
    }
  }

  const base::FilePath profile_path = profile_->GetPath();
  const base::FilePath scripts_dir = dashboard::ScriptStore::ScriptsDir(profile_);
  const base::FilePath browser_program =
      base::CommandLine::ForCurrentProcess()->GetProgram();
  const bool use_native_headless =
      base::FeatureList::IsEnabled(kDashboardNativeHeadlessListeners);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, const base::FilePath& profile_path,
             const base::FilePath& scripts_dir,
             const base::FilePath& browser_program, bool use_native_headless,
             base::Time from, base::Time to,
             uint32_t limit) -> ListenerEventsFetchResult {
            ListenerEventsFetchResult out;
            if (!use_native_headless) {
              const HeadlessHostCommandResult host_result =
                  InvokeHeadlessListenerHostForResultBlocking(
                      "poll_events", /*script_id=*/"", profile_path, scripts_dir,
                      browser_program, /*wait_ms=*/0);
              if (host_result.ok) {
                if (const base::Value::List* host_events =
                        host_result.response.FindList("events")) {
                  for (const auto& ev : *host_events) {
                    const base::Value::Dict* d = ev.GetIfDict();
                    if (!d) {
                      continue;
                    }
                    const std::string action =
                        d->FindString("action")
                            ? base::ToLowerASCII(*d->FindString("action"))
                            : std::string();
                    if (action == "trigger") {
                      const std::string listener_script_id =
                          d->FindString("listener_script_id")
                              ? *d->FindString("listener_script_id")
                              : std::string();
                      std::string trigger_script_id =
                          d->FindString("trigger_script_id")
                              ? *d->FindString("trigger_script_id")
                              : std::string();
                      if (trigger_script_id.empty() &&
                          !listener_script_id.empty()) {
                        trigger_script_id = ReadOnMatchScriptIdFromListenerScript(
                            profile, listener_script_id);
                      }
                      if (!trigger_script_id.empty()) {
                        base::flat_map<std::string, std::string> token_map =
                            BuildListenerMatchTokenMapFromEvent(*d);
                        std::string preferred_start_url;
                        // Prefer the captured outbound link, not the listener
                        // page (e.g. discord.com/channels/...).
                        auto token_it = token_map.find("listener.match.url");
                        if (token_it != token_map.end() &&
                            !token_it->second.empty()) {
                          preferred_start_url = token_it->second;
                        } else if (const std::string* match_url =
                                       d->FindString("match_url")) {
                          preferred_start_url = *match_url;
                        }
                        // Do not fall back to pageUrl (listener channel page).
                        out.headless_trigger_dispatches.push_back(
                            TriggerDispatchRequest{trigger_script_id,
                                                   listener_script_id,
                                                   preferred_start_url,
                                                   std::move(token_map)});
                      }
                    }
                    base::Value::Dict normalized = d->Clone();
                    if (!normalized.Find("ts_ms")) {
                      normalized.Set(
                          "ts_ms",
                          static_cast<double>(
                              base::Time::Now().InMillisecondsSinceUnixEpoch()));
                    }
                    (void)dashboard::RunHistoryStore::AppendListenerEvent(
                        profile, normalized);
                  }
                }
              } else if (!host_result.error.empty()) {
                out.headless_warning = host_result.error;
              }
            }
            out.events = dashboard::RunHistoryStore::ReadListenerEvents(
                profile, from, to, limit);
            return out;
          },
          base::Unretained(profile_), profile_path, scripts_dir, browser_program,
          use_native_headless, from, to, limit),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             ListenerEventsFetchResult fetch_result) {
            if (!self) {
              return;
            }
            if (!fetch_result.headless_trigger_dispatches.empty()) {
              DispatchTriggerScriptsAsync(
                  self->profile_,
                  std::move(fetch_result.headless_trigger_dispatches),
                  "Headless listener trigger dispatch");
            }
            for (auto& ev : fetch_result.events) {
              base::Value::Dict* d = ev.GetIfDict();
              if (!d) {
                continue;
              }
              const std::string* listener_id =
                  d->FindString("listener_script_id");
              if (listener_id && !d->FindString("listener_script_name")) {
                base::Value::Dict script =
                    self->dashboard_->LoadScript(*listener_id);
                const std::string* name = script.FindString("name");
                if (name && !name->empty()) {
                  d->Set("listener_script_name", *name);
                }
              }
              const std::string* trigger_id =
                  d->FindString("trigger_script_id");
              if (trigger_id && !d->FindString("trigger_script_name")) {
                base::Value::Dict script =
                    self->dashboard_->LoadScript(*trigger_id);
                const std::string* name = script.FindString("name");
                if (name && !name->empty()) {
                  d->Set("trigger_script_name", *name);
                }
              }
            }
            base::Value::Dict payload;
            payload.Set("events", std::move(fetch_result.events));
            if (!fetch_result.headless_warning.empty()) {
              payload.Set("headless_warning", fetch_result.headless_warning);
            }
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::StartBackgroundTabListenerWithTokenMapAsync(
    const std::string& cb,
    const std::string& script_id,
    const std::string& warning,
    bool report_headless_mode) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&BuildListenerTokenMapBlocking, profile_),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             std::string script_id, std::string warning,
             bool report_headless_mode,
             base::flat_map<std::string, std::string> token_map) {
            if (!self) {
              return;
            }

            if (!self->listener_manager_) {
              self->listener_manager_ =
                  std::make_unique<dashboard::ListenerManager>(self->profile_,
                                                               self->dashboard_);
            }

            std::string error;
            const bool ok = self->listener_manager_->StartListener(
                script_id, token_map, &error,
                /*force_cdp_execution=*/false);
            if (ok && report_headless_mode) {
              self->headless_listener_started_at_[script_id] = base::Time::Now();
            }
            LOG(INFO) << "[Dashboard] Listener start id=" << script_id
                      << " mode="
                      << (report_headless_mode ? "native_headless"
                                               : "background_tab")
                      << " success=" << (ok ? 1 : 0)
                      << (error.empty() ? std::string()
                                        : std::string(" error=") + error);

            base::Value::Dict res;
            res.Set("success", ok);
            res.Set("error", error);
            res.Set("mode",
                    report_headless_mode ? "headless" : "background_tab");
            if (!warning.empty()) {
              res.Set("warning", warning);
            }
            self->ResolveCallback(cb, res);
            DashboardUIMessageHandler::NotifyListenersChanged();
          },
          weak_ptr_factory_.GetWeakPtr(), cb, script_id, warning,
          report_headless_mode));
}

void DashboardUIMessageHandler::HandleStartListener(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string script_id =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                               : std::string();

  base::Value::Dict res;
  if (script_id.empty()) {
    res.Set("success", false);
    res.Set("error", "Empty listener script id.");
    ResolveCallback(cb, res);
    return;
  }

  const base::Value::Dict script = dashboard_->LoadScript(script_id);
  bool run_headless = ResolveRunListenersHeadlessForScript(script);
  const bool use_native_headless =
      run_headless &&
      base::FeatureList::IsEnabled(kDashboardNativeHeadlessListeners);
  if (use_native_headless) {
    StartBackgroundTabListenerWithTokenMapAsync(
        cb, script_id, "Native listener engine active (in-process).",
        /*report_headless_mode=*/true);
    return;
  }
  if (run_headless) {
    const base::FilePath profile_path = profile_->GetPath();
    const base::FilePath scripts_dir =
        dashboard::ScriptStore::ScriptsDir(profile_);
    const base::FilePath browser_program =
        base::CommandLine::ForCurrentProcess()->GetProgram();
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce(&InvokeHeadlessListenerHostForResultBlocking,
                       "start_headless_listener", script_id, profile_path,
                       scripts_dir, browser_program,
                       /*wait_ms=*/kHeadlessListenerStartWaitMs),
        base::BindOnce(
            [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
               std::string script_id, HeadlessHostCommandResult host_result) {
              if (!self) {
                return;
              }
              base::Value::Dict res;
              if (host_result.ok) {
                self->headless_listener_started_at_[script_id] =
                    base::Time::Now();
                EnsureHeadlessRelayEnabled(self->profile_);
                LOG(INFO) << "[Dashboard] Listener started id=" << script_id
                          << " mode=headless";
                res.Set("success", true);
                res.Set("error", "");
                res.Set("mode", "headless");
                self->ResolveCallback(cb, res);
                DashboardUIMessageHandler::NotifyListenersChanged();
                return;
              }

              if (!host_result.error.empty()) {
                LOG(WARNING) << "[Dashboard] Headless listener start failed for "
                             << script_id << ": " << host_result.error;
              }
              if (!AllowHeadlessFallbackToBackgroundTab()) {
                res.Set("success", false);
                res.Set(
                    "error",
                    host_result.error.empty()
                        ? "Headless listener failed to start."
                        : host_result.error);
                res.Set("mode", "headless");
                self->ResolveCallback(cb, res);
                return;
              }
              std::string warning =
                  host_result.error.empty()
                      ? "Headless listener failed to start after waiting."
                      : base::StrCat(
                            {"Headless listener failed to start after waiting: ",
                             host_result.error,
                             ". Falling back to background tab."});
              self->StartBackgroundTabListenerWithTokenMapAsync(
                  cb, script_id, warning,
                  /*report_headless_mode=*/false);
            },
            weak_ptr_factory_.GetWeakPtr(), cb, script_id));
    return;
  }

  StartBackgroundTabListenerWithTokenMapAsync(cb, script_id, std::string(),
                                              /*report_headless_mode=*/false);
}

void DashboardUIMessageHandler::HandleStopListener(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string script_id =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                               : std::string();

  base::Value::Dict res;
  if (script_id.empty()) {
    res.Set("success", false);
    res.Set("error", "Empty listener script id.");
    ResolveCallback(cb, res);
    return;
  }

  const bool was_tracked_headless =
      headless_listener_started_at_.find(script_id) !=
      headless_listener_started_at_.end();
  if (base::FeatureList::IsEnabled(kDashboardNativeHeadlessListeners)) {
    if (!listener_manager_) {
      listener_manager_ =
          std::make_unique<dashboard::ListenerManager>(profile_, dashboard_);
    }
    std::string native_error;
    const bool native_ok =
        listener_manager_->StopListener(script_id, &native_error);
    if (native_ok) {
      headless_listener_started_at_.erase(script_id);
      const std::string mode =
          was_tracked_headless ? "headless" : "background_tab";
      LOG(INFO) << "[Dashboard] Listener stopped id=" << script_id
                << " mode=native_" << mode;
      base::Value::Dict native_res;
      native_res.Set("success", true);
      native_res.Set("error", "");
      native_res.Set("mode", mode);
      ResolveCallback(cb, native_res);
      NotifyListenersChanged();
      return;
    }
    LOG(WARNING) << "[Dashboard] Native listener stop failed for " << script_id
                 << ": " << native_error;
    base::Value::Dict native_res;
    native_res.Set("success", false);
    native_res.Set("error", native_error.empty() ? "Listener is not active."
                                                 : native_error);
    native_res.Set("mode",
                   was_tracked_headless ? "headless" : "background_tab");
    ResolveCallback(cb, native_res);
    return;
  }
  const base::FilePath profile_path = profile_->GetPath();
  const base::FilePath scripts_dir = dashboard::ScriptStore::ScriptsDir(profile_);
  const base::FilePath browser_program =
      base::CommandLine::ForCurrentProcess()->GetProgram();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&InvokeHeadlessListenerHostForResultBlocking,
                     "stop_headless_listener", script_id, profile_path,
                     scripts_dir, browser_program, /*wait_ms=*/0),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             std::string script_id, bool was_tracked_headless,
             HeadlessHostCommandResult host_result) {
            if (!self) {
              return;
            }
            base::Value::Dict res;
            if (host_result.ok) {
              self->headless_listener_started_at_.erase(script_id);
              const bool already_stopped =
                  host_result.response.FindBool("already_stopped")
                      .value_or(false);
              if (!already_stopped || was_tracked_headless) {
                LOG(INFO) << "[Dashboard] Listener stopped id=" << script_id
                          << " mode=headless";
                res.Set("success", true);
                res.Set("error", "");
                res.Set("mode", "headless");
                self->ResolveCallback(cb, res);
                DashboardUIMessageHandler::NotifyListenersChanged();
                return;
              }
            } else if (was_tracked_headless) {
              LOG(WARNING) << "[Dashboard] Headless listener stop failed for "
                           << script_id << ": " << host_result.error;
              res.Set("success", false);
              res.Set("error", host_result.error);
              res.Set("mode", "headless");
              self->ResolveCallback(cb, res);
              return;
            } else if (!host_result.error.empty()) {
              res.Set("warning", host_result.error);
            }

            if (!self->listener_manager_) {
              res.Set("success", false);
              res.Set("error", "No listeners are active.");
              self->ResolveCallback(cb, res);
              return;
            }

            std::string error;
            const bool ok =
                self->listener_manager_->StopListener(script_id, &error);
            LOG(INFO) << "[Dashboard] Listener stop id=" << script_id
                      << " mode=background_tab success=" << (ok ? 1 : 0)
                      << (error.empty() ? std::string()
                                        : std::string(" error=") + error);
            res.Set("success", ok);
            res.Set("error", error);
            res.Set("mode", "background_tab");
            self->ResolveCallback(cb, res);
            DashboardUIMessageHandler::NotifyListenersChanged();
          },
          weak_ptr_factory_.GetWeakPtr(), cb, script_id, was_tracked_headless));
}

void DashboardUIMessageHandler::HandleStopHeadlessTask(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string task_id =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                               : std::string();

  base::Value::Dict out;
  if (task_id.empty()) {
    out.Set("success", false);
    out.Set("error", "Empty headless task id.");
    ResolveCallback(cb, out);
    return;
  }

  base::Value::Dict stop_result = dashboard::StopHeadlessInvestigateTask(
      profile_, task_id, /*close_target=*/true,
      "user closed headless investigation tab");
  const bool ok = stop_result.FindBool("ok").value_or(false);
  out.Set("success", ok);
  out.Set("task_id", task_id);
  out.Set("taskId", task_id);
  if (!ok) {
    const std::string* stop_error = stop_result.FindString("error");
    out.Set("error", stop_error ? *stop_error
                                : "Failed to stop headless task.");
  }
  out.Set("result", std::move(stop_result));
  ResolveCallback(cb, out);
  if (ok) {
    NotifyListenersChanged();
  }
}

void DashboardUIMessageHandler::HandleRefreshHeadlessTab(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string owner_kind =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                               : std::string();
  const std::string owner_id =
      (args.size() > 2 && args[2].is_string()) ? args[2].GetString()
                                               : std::string();

  base::Value::Dict out;
  if (owner_id.empty()) {
    out.Set("success", false);
    out.Set("error", "Empty headless tab id.");
    ResolveCallback(cb, out);
    return;
  }

  const std::string kind = base::ToLowerASCII(owner_kind);
  const bool is_listener =
      kind.empty() || kind == "listener" || kind == "stop_listener";
  const std::string script_id = is_listener ? owner_id : std::string();
  base::Value::Dict request;
  if (!is_listener) {
    request.Set("target_id", owner_id);
  }
  std::string request_json;
  if (!request.empty()) {
    if (!base::JSONWriter::Write(base::Value(request.Clone()), &request_json) ||
        request_json.empty()) {
      out.Set("success", false);
      out.Set("error", "Failed to encode headless refresh request.");
      ResolveCallback(cb, out);
      return;
    }
  }

  const base::FilePath profile_path = profile_->GetPath();
  const base::FilePath scripts_dir =
      dashboard::ScriptStore::ScriptsDir(profile_);
  const base::FilePath browser_program =
      base::CommandLine::ForCurrentProcess()->GetProgram();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&InvokeHeadlessListenerHostForResultWithRequestBlocking,
                     "refresh_headless_tab", script_id, profile_path,
                     scripts_dir, browser_program, /*wait_ms=*/0,
                     request_json),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             std::string owner_id, HeadlessHostCommandResult host_result) {
            if (!self) {
              return;
            }
            base::Value::Dict res;
            res.Set("success", host_result.ok);
            res.Set("owner_id", owner_id);
            res.Set("ownerId", owner_id);
            if (!host_result.ok) {
              res.Set("error", host_result.error.empty()
                                   ? "Failed to refresh headless tab."
                                   : host_result.error);
            }
            if (!host_result.response.empty()) {
              res.Set("result", host_result.response.Clone());
            }
            self->ResolveCallback(cb, res);
          },
          weak_ptr_factory_.GetWeakPtr(), cb, owner_id));
}

void DashboardUIMessageHandler::HandleGetActiveListeners(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();

  base::Value::List listeners;
  dashboard::ScriptExecutionService* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (listener_manager_) {
    for (const auto& info : listener_manager_->GetActiveListeners()) {
      base::Value::Dict d;
      d.Set("script_id", info.script_id);
      d.Set("scriptId", info.script_id);
      d.Set("script_name", info.script_name);
      d.Set("scriptName", info.script_name);
      d.Set("is_active", info.is_active);
      d.Set("isActive", info.is_active);
      const std::string iso = base::TimeFormatAsIso8601(info.started_at);
      d.Set("started_at_iso", iso);
      d.Set("startedAtIso", iso);
      d.Set("status", info.status);
      d.Set("status_detail", info.status_detail);
      d.Set("last_error", info.last_error);
      d.Set("running_kind", info.running_kind);
      d.Set("execution_id", info.execution_id);
      d.Set("current_url", info.current_url);
      d.Set("currentUrl", info.current_url);
      const bool tracked_headless =
          headless_listener_started_at_.find(info.script_id) !=
          headless_listener_started_at_.end();
      d.Set("mode", tracked_headless ? "headless" : "background_tab");
      d.Set("mode_kind", tracked_headless ? "headless" : "background_tab");
      d.Set("modeKind", tracked_headless ? "headless" : "background_tab");
      if (exec_service && !info.execution_id.empty()) {
        base::Value::Dict st = exec_service->GetRunStatusDict(info.execution_id);
        d.Set("execution_status", std::move(st));
      }
      listeners.Append(std::move(d));
    }
  }

  if (base::FeatureList::IsEnabled(kDashboardNativeHeadlessListeners)) {
    base::Value::Dict payload;
    payload.Set("listeners", std::move(listeners));
    base::Value::List headless_tabs;
    const base::Value::List* listeners_out = payload.FindList("listeners");
    base::flat_set<std::string> seen_tab_ids;
    auto site_name_from_url = [](const std::string& raw_url) {
      if (raw_url.empty()) {
        return std::string("about:blank");
      }
      GURL gurl(raw_url);
      if (gurl.is_valid()) {
        if (!gurl.host().empty()) {
          return std::string(gurl.host());
        }
        if (!gurl.spec().empty()) {
          return gurl.spec();
        }
      }
      return raw_url;
    };
    auto append_headless_tab = [&headless_tabs, &seen_tab_ids](
                                   base::Value::Dict tab) {
      const std::string tab_id =
          tab.FindString("tab_id") ? *tab.FindString("tab_id") : std::string();
      if (tab_id.empty() || seen_tab_ids.contains(tab_id)) {
        return;
      }
      seen_tab_ids.insert(tab_id);
      headless_tabs.Append(std::move(tab));
    };
    if (listeners_out) {
      for (const auto& item : *listeners_out) {
        const base::Value::Dict* d = item.GetIfDict();
        if (!d) {
          continue;
        }
        const std::string mode_kind =
            d->FindString("mode_kind") ? *d->FindString("mode_kind")
                                       : (d->FindString("mode")
                                              ? *d->FindString("mode")
                                              : std::string());
        if (mode_kind != "headless") {
          continue;
        }
        const std::string script_id =
            d->FindString("script_id") ? *d->FindString("script_id")
                                       : std::string();
        if (script_id.empty()) {
          continue;
        }
        const std::string current_url =
            d->FindString("current_url") ? *d->FindString("current_url")
                                         : std::string();
        const std::string script_name =
            d->FindString("script_name") ? *d->FindString("script_name")
                                         : script_id;
        const std::string site_name = site_name_from_url(current_url);
        base::Value::Dict tab;
        tab.Set("tab_id", "listener:" + script_id);
        tab.Set("tabId", "listener:" + script_id);
        tab.Set("owner_kind", "listener");
        tab.Set("ownerKind", "listener");
        tab.Set("owner_id", script_id);
        tab.Set("ownerId", script_id);
        tab.Set("label", script_name);
        tab.Set("open_reason", "listener");
        tab.Set("openReason", "listener");
        tab.Set("current_url", current_url);
        tab.Set("currentUrl", current_url);
        tab.Set("site_name", site_name);
        tab.Set("siteName", site_name);
        tab.Set("closable", true);
        tab.Set("close_action", "stop_listener");
        tab.Set("closeAction", "stop_listener");
        tab.Set("close_id", script_id);
        tab.Set("closeId", script_id);
        append_headless_tab(std::move(tab));
      }
    }
    for (const auto& task :
         dashboard::GetHeadlessInvestigateTaskSnapshots(profile_)) {
      const std::string& task_id = task.task_id;
      if (task_id.empty()) {
        continue;
      }
      base::Value::Dict tab;
      tab.Set("tab_id", "investigation:" + task_id);
      tab.Set("tabId", "investigation:" + task_id);
      tab.Set("owner_kind", "investigation");
      tab.Set("ownerKind", "investigation");
      tab.Set("owner_id", task_id);
      tab.Set("ownerId", task_id);
      tab.Set("label", task.display_label);
      tab.Set("open_reason", task.open_reason);
      tab.Set("openReason", task.open_reason);
      tab.Set("current_url", task.current_url);
      tab.Set("currentUrl", task.current_url);
      tab.Set("site_name", site_name_from_url(task.current_url));
      tab.Set("siteName", site_name_from_url(task.current_url));
      tab.Set("closable", true);
      tab.Set("close_action", "stop_headless_task");
      tab.Set("closeAction", "stop_headless_task");
      tab.Set("close_id", task_id);
      tab.Set("closeId", task_id);
      append_headless_tab(std::move(tab));
    }
    payload.Set("headless_tabs", std::move(headless_tabs));
    ResolveCallback(cb, payload);
    return;
  }

  const base::FilePath profile_path = profile_->GetPath();
  const base::FilePath scripts_dir = dashboard::ScriptStore::ScriptsDir(profile_);
  const base::FilePath browser_program =
      base::CommandLine::ForCurrentProcess()->GetProgram();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&InvokeHeadlessListenerHostForResultBlocking, "list_active",
                     /*script_id=*/"", profile_path, scripts_dir,
                     browser_program, /*wait_ms=*/0),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::List listeners, HeadlessHostCommandResult host_result) {
            if (!self) {
              return;
            }
            if (const base::Value::Dict* active =
                    host_result.response.FindDict("listeners")) {
              for (auto it = active->begin(); it != active->end(); ++it) {
                const std::string& id = it->first;
                const base::Value& v = it->second;
                if (!v.is_dict()) {
                  continue;
                }
                bool already_added = false;
                for (const auto& item : listeners) {
                  if (!item.is_dict()) {
                    continue;
                  }
                  const base::Value::Dict& d = item.GetDict();
                  if (d.FindString("script_id") &&
                      *d.FindString("script_id") == id) {
                    already_added = true;
                    break;
                  }
                }
                if (already_added) {
                  continue;
                }
                const base::Value::Dict& entry = v.GetDict();
                const base::Value::Dict script =
                    self->dashboard_->LoadScript(id);
                const std::string name = script.FindString("name")
                                             ? *script.FindString("name")
                                             : id;
                base::Value::Dict d;
                d.Set("script_id", id);
                d.Set("scriptId", id);
                d.Set("script_name", name);
                d.Set("scriptName", name);
                d.Set("is_active", true);
                d.Set("isActive", true);
                const int started_ms = entry.FindInt("started_ms").value_or(0);
                if (started_ms > 0) {
                  const base::Time started =
                      base::Time::FromMillisecondsSinceUnixEpoch(started_ms);
                  const std::string iso = base::TimeFormatAsIso8601(started);
                  d.Set("started_at_iso", iso);
                  d.Set("startedAtIso", iso);
                }
                d.Set("status", "listening");
                d.Set("status_detail", "Headless listener active");
                d.Set("running_kind", "headless");
                d.Set("mode", "headless");
                d.Set("mode_kind", "headless");
                d.Set("modeKind", "headless");
                std::string headless_url;
                if (const std::string* current =
                        entry.FindString("current_url")) {
                  headless_url = *current;
                } else if (const std::string* start =
                               entry.FindString("start_url")) {
                  headless_url = *start;
                }
                d.Set("current_url", headless_url);
                d.Set("currentUrl", headless_url);
                listeners.Append(std::move(d));
              }
            }

            for (const auto& [id, started_at] :
                 self->headless_listener_started_at_) {
              bool already_added = false;
              for (const auto& item : listeners) {
                if (!item.is_dict()) {
                  continue;
                }
                const base::Value::Dict& existing = item.GetDict();
                if (existing.FindString("script_id") &&
                    *existing.FindString("script_id") == id) {
                  already_added = true;
                  break;
                }
              }
              if (already_added) {
                continue;
              }

              base::Value::Dict d;
              const base::Value::Dict script = self->dashboard_->LoadScript(id);
              const std::string name = script.FindString("name")
                                           ? *script.FindString("name")
                                           : id;
              d.Set("script_id", id);
              d.Set("scriptId", id);
              d.Set("script_name", name);
              d.Set("scriptName", name);
              d.Set("is_active", true);
              d.Set("isActive", true);
              const std::string iso = base::TimeFormatAsIso8601(started_at);
              d.Set("started_at_iso", iso);
              d.Set("startedAtIso", iso);
              d.Set("status", "listening");
              d.Set("status_detail", "Headless listener active");
              d.Set("running_kind", "headless");
              d.Set("mode", "headless");
              d.Set("mode_kind", "headless");
              d.Set("modeKind", "headless");
              d.Set("current_url", std::string());
              d.Set("currentUrl", std::string());
              listeners.Append(std::move(d));
            }

            base::Value::Dict payload;
            payload.Set("listeners", std::move(listeners));
            if (!host_result.ok && !host_result.error.empty()) {
              payload.Set("headless_warning", host_result.error);
            }
            base::Value::List headless_tabs;
            base::flat_set<std::string> seen_tab_ids;
            auto site_name_from_url = [](const std::string& raw_url) {
              if (raw_url.empty()) {
                return std::string("about:blank");
              }
              GURL gurl(raw_url);
              if (gurl.is_valid()) {
                if (!gurl.host().empty()) {
                  return std::string(gurl.host());
                }
                if (!gurl.spec().empty()) {
                  return gurl.spec();
                }
              }
              return raw_url;
            };
            auto append_headless_tab = [&headless_tabs, &seen_tab_ids](
                                           base::Value::Dict tab) {
              const std::string tab_id =
                  tab.FindString("tab_id")
                      ? *tab.FindString("tab_id")
                      : std::string();
              if (tab_id.empty() || seen_tab_ids.contains(tab_id)) {
                return;
              }
              seen_tab_ids.insert(tab_id);
              headless_tabs.Append(std::move(tab));
            };
            const base::Value::List* listeners_out =
                payload.FindList("listeners");
            if (listeners_out) {
              bool has_headless = false;
              for (const auto& item : *listeners_out) {
                const base::Value::Dict* d = item.GetIfDict();
                if (!d) {
                  continue;
                }
                const std::string* running_kind =
                    d->FindString("running_kind");
                const std::string mode_kind =
                    d->FindString("mode_kind")
                        ? *d->FindString("mode_kind")
                        : (d->FindString("mode")
                               ? *d->FindString("mode")
                               : std::string());
                if ((running_kind && *running_kind == "headless") ||
                    mode_kind == "headless") {
                  has_headless = true;
                  const std::string script_id =
                      d->FindString("script_id")
                          ? *d->FindString("script_id")
                          : std::string();
                  if (!script_id.empty()) {
                    const std::string current_url =
                        d->FindString("current_url")
                            ? *d->FindString("current_url")
                            : std::string();
                    const std::string script_name =
                        d->FindString("script_name")
                            ? *d->FindString("script_name")
                            : script_id;
                    const std::string site_name =
                        site_name_from_url(current_url);
                    base::Value::Dict tab;
                    tab.Set("tab_id", "listener:" + script_id);
                    tab.Set("tabId", "listener:" + script_id);
                    tab.Set("owner_kind", "listener");
                    tab.Set("ownerKind", "listener");
                    tab.Set("owner_id", script_id);
                    tab.Set("ownerId", script_id);
                    tab.Set("label", script_name);
                    tab.Set("open_reason", "listener");
                    tab.Set("openReason", "listener");
                    tab.Set("current_url", current_url);
                    tab.Set("currentUrl", current_url);
                    tab.Set("site_name", site_name);
                    tab.Set("siteName", site_name);
                    tab.Set("closable", true);
                    tab.Set("close_action", "stop_listener");
                    tab.Set("closeAction", "stop_listener");
                    tab.Set("close_id", script_id);
                    tab.Set("closeId", script_id);
                    append_headless_tab(std::move(tab));
                  }
                }
              }
              if (has_headless) {
                EnsureHeadlessRelayEnabled(self->profile_);
              }
            }
            for (const auto& task :
                 dashboard::GetHeadlessInvestigateTaskSnapshots(
                     self->profile_)) {
              const std::string& task_id = task.task_id;
              if (task_id.empty()) {
                continue;
              }
              base::Value::Dict tab;
              tab.Set("tab_id", "investigation:" + task_id);
              tab.Set("tabId", "investigation:" + task_id);
              tab.Set("owner_kind", "investigation");
              tab.Set("ownerKind", "investigation");
              tab.Set("owner_id", task_id);
              tab.Set("ownerId", task_id);
              tab.Set("label", task.display_label);
              tab.Set("open_reason", task.open_reason);
              tab.Set("openReason", task.open_reason);
              tab.Set("current_url", task.current_url);
              tab.Set("currentUrl", task.current_url);
              tab.Set("site_name", site_name_from_url(task.current_url));
              tab.Set("siteName", site_name_from_url(task.current_url));
              tab.Set("closable", true);
              tab.Set("close_action", "stop_headless_task");
              tab.Set("closeAction", "stop_headless_task");
              tab.Set("close_id", task_id);
              tab.Set("closeId", task_id);
              append_headless_tab(std::move(tab));
            }
            payload.Set("headless_tabs", std::move(headless_tabs));
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb, std::move(listeners)));
}

void DashboardUIMessageHandler::HandleRequestHeadlessScreenshot(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string script_id =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                               : std::string();

  base::Value::Dict early;
  if (script_id.empty()) {
    early.Set("success", false);
    early.Set("error", "Missing listener script id.");
    ResolveCallback(cb, early);
    return;
  }

  if (base::FeatureList::IsEnabled(kDashboardNativeHeadlessListeners)) {
    if (!listener_manager_) {
      listener_manager_ =
          std::make_unique<dashboard::ListenerManager>(profile_, dashboard_);
    }
    content::WebContents* wc =
        listener_manager_->GetListenerWebContents(script_id);
    if (!wc) {
      base::Value::Dict res;
      res.Set("script_id", script_id);
      res.Set("scriptId", script_id);
      res.Set("success", false);
      res.Set("error", "Listener is not active.");
      ResolveCallback(cb, res);
      return;
    }
    content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
    if (!view || !view->IsSurfaceAvailableForCopy()) {
      base::Value::Dict res;
      res.Set("script_id", script_id);
      res.Set("scriptId", script_id);
      res.Set("success", false);
      res.Set("error", "Listener surface unavailable for screenshot.");
      ResolveCallback(cb, res);
      return;
    }
    const std::string current_url = wc->GetLastCommittedURL().spec();
    view->CopyFromSurface(
        gfx::Rect(), gfx::Size(),
        base::BindOnce(
            [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
               std::string script_id, std::string current_url,
               const viz::CopyOutputBitmapWithMetadata& result) {
              if (!self) {
                return;
              }
              base::Value::Dict res;
              res.Set("script_id", script_id);
              res.Set("scriptId", script_id);
              if (result.bitmap.drawsNothing()) {
                res.Set("success", false);
                res.Set("error", "Captured empty screenshot.");
                self->ResolveCallback(cb, res);
                return;
              }
              std::optional<std::vector<uint8_t>> png =
                  gfx::PNGCodec::EncodeBGRASkBitmap(
                      result.bitmap, /*discard_transparency=*/false);
              if (!png || png->empty()) {
                res.Set("success", false);
                res.Set("error", "Failed to encode screenshot.");
                self->ResolveCallback(cb, res);
                return;
              }
              const std::string image_base64 =
                  base::Base64Encode(base::span<const uint8_t>(*png));
              if (image_base64.empty()) {
                res.Set("success", false);
                res.Set("error", "Failed to serialize screenshot.");
                self->ResolveCallback(cb, res);
                return;
              }
              res.Set("success", true);
              res.Set("image_base64", image_base64);
              res.Set("current_url", current_url);
              res.Set("currentUrl", current_url);
              const double ts_ms = static_cast<double>(
                  base::Time::Now().InMillisecondsSinceUnixEpoch());
              res.Set("ts_ms", ts_ms);
              res.Set("tsMs", ts_ms);
              self->ResolveCallback(cb, res);
            },
            weak_ptr_factory_.GetWeakPtr(), cb, script_id, current_url));
    return;
  }

  const base::FilePath profile_path = profile_->GetPath();
  const base::FilePath scripts_dir = dashboard::ScriptStore::ScriptsDir(profile_);
  const base::FilePath browser_program =
      base::CommandLine::ForCurrentProcess()->GetProgram();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&InvokeHeadlessListenerHostForResultBlocking,
                     "capture_screenshot", script_id, profile_path, scripts_dir,
                     browser_program, /*wait_ms=*/0),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             std::string script_id, HeadlessHostCommandResult host_result) {
            if (!self) {
              return;
            }
            base::Value::Dict res;
            res.Set("script_id", script_id);
            res.Set("scriptId", script_id);
            res.Set("success", host_result.ok);
            if (!host_result.ok) {
              res.Set("error", host_result.error.empty()
                                   ? "Failed to capture headless screenshot."
                                   : host_result.error);
              self->ResolveCallback(cb, res);
              return;
            }

            const std::string* image_base64 =
                host_result.response.FindString("image_base64");
            if (image_base64 && !image_base64->empty()) {
              res.Set("image_base64", *image_base64);
            }
            if (const std::string* path =
                    host_result.response.FindString("path")) {
              res.Set("path", *path);
            }
            if (const std::string* current_url =
                    host_result.response.FindString("current_url")) {
              res.Set("current_url", *current_url);
              res.Set("currentUrl", *current_url);
            }
            if (const std::string* detail =
                    host_result.response.FindString("detail")) {
              res.Set("detail", *detail);
            }
            if (auto ts_ms = host_result.response.FindInt("ts_ms")) {
              res.Set("ts_ms", *ts_ms);
              res.Set("tsMs", *ts_ms);
            } else if (auto ts_ms_double =
                           host_result.response.FindDouble("ts_ms")) {
              res.Set("ts_ms", *ts_ms_double);
              res.Set("tsMs", *ts_ms_double);
            }

            self->ResolveCallback(cb, res);
          },
          weak_ptr_factory_.GetWeakPtr(), cb, script_id));
}

void DashboardUIMessageHandler::HandleAgentInvestigatePage(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  if (!payload) {
    base::Value::Dict early;
    early.Set("ok", false);
    early.Set("error", "Missing investigation payload.");
    ResolveCallback(cb, early);
    return;
  }

  // Always investigate on the isolated headless instance (:9222). Never open
  // headed background tabs in the main browser for this path.
  dashboard::InvestigatePageOnHeadlessListener(
      profile_, payload->Clone(),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::Dict out) {
            if (!self) {
              return;
            }
            self->ResolveCallback(cb, out);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

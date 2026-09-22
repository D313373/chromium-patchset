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
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
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
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "chrome/browser/ui/webui/dashboard/execution_run_context.h"
#include "chrome/browser/ui/webui/dashboard/headless_investigation_service.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_agent.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_controller.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_ipc.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_prewarm.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/browser/ui/webui/dashboard/listener_manager.h"
#include "chrome/browser/ui/webui/dashboard/run_history_store.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/common/chrome_paths.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/navigation_controller.h"
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

namespace {

constexpr int kHeadlessListenerStartWaitMs [[maybe_unused]] = 15000;

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

  if (const base::Value::Dict* tokens = event.FindDict("tokens")) {
    for (auto it : *tokens) {
      if (it.second.is_string()) {
        SetListenerMatchToken(&token_map, it.first, it.second.GetString());
      }
    }
    if (matched_url.empty()) {
      if (const std::string* url = tokens->FindString("listener.match.url")) {
        SetListenerMatchToken(&token_map, "listener.match.url", *url);
      }
    }
  }
  if (const std::string* match_url = event.FindString("match_url");
      match_url && !match_url->empty()) {
    SetListenerMatchToken(&token_map, "listener.match.url", *match_url);
  }

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

struct ListenerEventsFetchResult {
  base::Value::List events;
  std::string headless_warning;
  std::vector<TriggerDispatchRequest> headless_trigger_dispatches;
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

void AppendListenerEventAsync(Profile* profile, base::Value::Dict event) {
  if (!profile) {
    return;
  }
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, base::Value::Dict event) {
            (void)dashboard::RunHistoryStore::AppendListenerEvent(profile,
                                                                  event);
          },
          base::Unretained(profile), std::move(event)));
}

// Convert pushed headless trigger events into headed onMatch dispatches.
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


}  // namespace

namespace dashboard {

namespace {

// When the headed browser crashes/quits, its --headless-listener child can be
// left running (--no-startup-window + LSUIElement). On macOS, Launch Services
// then treats that invisible process as Chromium.app, so the next launch shows
// nothing. Reap orphans on headed startup (MayBlock worker only).
void ReapOrphanedHeadlessListenerProcessesBlocking() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kHeadlessListener)) {
    return;
  }
#if BUILDFLAG(IS_POSIX)
  std::vector<std::string> argv = {"/usr/bin/pkill", "-f",
                                   "--headless-listener"};
  base::Process p = base::LaunchProcess(argv, base::LaunchOptions());
  if (p.IsValid()) {
    int exit = 0;
    p.WaitForExit(&exit);
    if (exit == 0) {
      LOG(WARNING) << "[Headless] Reaped orphaned --headless-listener "
                      "process(es) so headed Chromium can show a window";
    }
  }
  const base::FilePath sock = DefaultHeadlessControlSocketPath();
  if (!sock.empty()) {
    base::DeleteFile(sock);
  }
#endif
}

}  // namespace

void MaybePrewarmHeadlessListenerHost(Profile* profile) {
  // Do not pre-launch headless (dock-tile issue). Instead, clear orphans left
  // behind after a headed crash so the next open -a Chromium is visible.
  (void)profile;
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&ReapOrphanedHeadlessListenerProcessesBlocking));
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
    out.Set("transport", "native_headless");
    std::move(callback).Run(std::move(out));
    return;
  }

  // Ensure the C++-owned headless Chromium is up, then run investigate_page
  // over the length-prefixed JSON control channel.
  auto* existing = HeadlessListenerController::GetForProfile(profile);
  HeadlessListenerController* controller = existing;
  std::unique_ptr<HeadlessListenerController> owned;
  if (!controller) {
    owned = std::make_unique<HeadlessListenerController>(profile);
    controller = owned.get();
    owned.release();
  }
  controller->InvestigatePage(
      std::move(payload),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value::Dict)> callback, bool ok,
             base::Value::Dict response) {
            base::Value::Dict out = std::move(response);
            out.Set("command", "investigate_page");
            out.Set("transport", "native_headless");
            out.Set("ok", ok);
            if (!ok && !out.FindString("error")) {
              out.Set("error", "investigate_page failed.");
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

  // Triggers are pushed live over the native control channel; this API only
  // reads persisted listener event history for the Debugging UI.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, base::Time from, base::Time to,
             uint32_t limit) -> ListenerEventsFetchResult {
            ListenerEventsFetchResult out;
            out.events = dashboard::RunHistoryStore::ReadListenerEvents(
                profile, from, to, limit);
            return out;
          },
          base::Unretained(profile_), from, to, limit),
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
  // C++-owned isolated headless process (no Python). Background-tab path when
  // headless mode is off.
  if (run_headless) {
    dashboard::HeadlessListenerController* controller =
        dashboard::HeadlessListenerController::GetForProfile(profile_);
    if (!controller) {
      headless_listener_controller_ =
          std::make_unique<dashboard::HeadlessListenerController>(profile_);
      controller = headless_listener_controller_.get();
    }
    controller->SetTriggerCallback(base::BindRepeating(
        [](base::WeakPtr<DashboardUIMessageHandler> self,
           base::Value::Dict trigger_event) {
          if (!self || !self->profile_) {
            return;
          }
          base::Value::List events;
          events.Append(std::move(trigger_event));
          ProcessHeadlessEvents(self->profile_, events);
          DashboardUIMessageHandler::NotifyListenersChanged();
        },
        weak_ptr_factory_.GetWeakPtr()));
    controller->StartListener(
        script_id,
        base::BindOnce(
            [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
               std::string script_id, bool ok, base::Value::Dict response) {
              if (!self) {
                return;
              }
              base::Value::Dict res;
              if (ok) {
                self->headless_listener_started_at_[script_id] =
                    base::Time::Now();
                LOG(INFO) << "[Dashboard] Listener started id=" << script_id
                          << " mode=headless_native";
                res.Set("success", true);
                res.Set("error", "");
                res.Set("mode", "headless");
                self->ResolveCallback(cb, res);
                DashboardUIMessageHandler::NotifyListenersChanged();
                return;
              }
              const std::string error =
                  response.FindString("error") ? *response.FindString("error")
                                               : "Headless listener failed.";
              LOG(WARNING) << "[Dashboard] Native headless start failed for "
                           << script_id << ": " << error;
              if (!AllowHeadlessFallbackToBackgroundTab()) {
                res.Set("success", false);
                res.Set("error", error);
                res.Set("mode", "headless");
                self->ResolveCallback(cb, res);
                return;
              }
              self->StartBackgroundTabListenerWithTokenMapAsync(
                  cb, script_id,
                  "Native headless failed; using background tab. " + error,
                  /*report_headless_mode=*/false);
            },
            weak_ptr_factory_.GetWeakPtr(), cb, script_id));
    return;
  }

  StartBackgroundTabListenerWithTokenMapAsync(cb, script_id, std::string(),
                                              /*report_headless_mode=*/false);
}

void DashboardUIMessageHandler::HandleStopAllScripts(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();

  size_t stopped = 0;
  dashboard::ScriptExecutionService* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (exec_service) {
    stopped = exec_service->StopAllRuns("stop all scripts");
  }
  // Break nested listen→onMatch chains that may still be mid-dispatch.
  dashboard::ExecutionRunContext::Get().set_listen_oneshot_depth(0);

  // Best-effort: clear pending triggers on the native headless control channel.
  if (auto* controller =
          dashboard::HeadlessListenerController::GetForProfile(profile_)) {
    controller->ClearPendingTriggers(base::DoNothing());
  }

  LOG(INFO) << "[Dashboard] HandleStopAllScripts stopped=" << stopped;
  base::Value::Dict out;
  out.Set("ok", true);
  out.Set("stopped", static_cast<int>(stopped));
  ResolveCallback(cb, out);
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
  if (auto* controller =
          dashboard::HeadlessListenerController::GetForProfile(profile_);
      controller && was_tracked_headless) {
    controller->StopListener(
        script_id,
        base::BindOnce(
            [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
               std::string script_id, bool ok, base::Value::Dict response) {
              if (!self) {
                return;
              }
              base::Value::Dict res;
              self->headless_listener_started_at_.erase(script_id);
              if (ok) {
                LOG(INFO) << "[Dashboard] Listener stopped id=" << script_id
                          << " mode=headless_native";
                res.Set("success", true);
                res.Set("error", "");
                res.Set("mode", "headless");
              } else {
                const std::string error =
                    response.FindString("error") ? *response.FindString("error")
                                                 : "Stop failed.";
                res.Set("success", false);
                res.Set("error", error);
                res.Set("mode", "headless");
              }
              self->ResolveCallback(cb, res);
              DashboardUIMessageHandler::NotifyListenersChanged();
            },
            weak_ptr_factory_.GetWeakPtr(), cb, script_id));
    return;
  }

  if (!listener_manager_) {
    listener_manager_ =
        std::make_unique<dashboard::ListenerManager>(profile_, dashboard_);
  }
  std::string native_error;
  const bool native_ok =
      listener_manager_->StopListener(script_id, &native_error);
  headless_listener_started_at_.erase(script_id);
  const std::string mode =
      was_tracked_headless ? "headless" : "background_tab";
  LOG(INFO) << "[Dashboard] Listener stop id=" << script_id
            << " mode=native_" << mode << " success=" << (native_ok ? 1 : 0)
            << (native_error.empty() ? std::string()
                                     : std::string(" error=") + native_error);
  res.Set("success", native_ok);
  res.Set("error", native_ok ? ""
                             : (native_error.empty() ? "Listener is not active."
                                                     : native_error));
  res.Set("mode", mode);
  ResolveCallback(cb, res);
  NotifyListenersChanged();
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

  // Background-tab listeners: reload in-process WebContents.
  if (listener_manager_) {
    if (content::WebContents* wc =
            listener_manager_->GetListenerWebContents(owner_id)) {
      wc->GetController().Reload(content::ReloadType::NORMAL,
                                 /*check_for_repost=*/false);
      out.Set("success", true);
      out.Set("owner_id", owner_id);
      out.Set("ownerId", owner_id);
      out.Set("mode", "background_tab");
      ResolveCallback(cb, out);
      return;
    }
  }

  out.Set("success", false);
  out.Set("owner_id", owner_id);
  out.Set("ownerId", owner_id);
  out.Set("error",
          "Native headless refresh over the control channel is not wired yet.");
  ResolveCallback(cb, out);
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
      d.Set("mode", "background_tab");
      d.Set("mode_kind", "background_tab");
      d.Set("modeKind", "background_tab");
      if (exec_service && !info.execution_id.empty()) {
        base::Value::Dict st = exec_service->GetRunStatusDict(info.execution_id);
        d.Set("execution_status", std::move(st));
      }
      listeners.Append(std::move(d));
    }
  }

  auto finish =
      [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
         base::Value::List listeners) {
        if (!self) {
          return;
        }
        base::Value::Dict payload;
        payload.Set("listeners", listeners.Clone());
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
          const std::string tab_id = tab.FindString("tab_id")
                                         ? *tab.FindString("tab_id")
                                         : std::string();
          if (tab_id.empty() || seen_tab_ids.contains(tab_id)) {
            return;
          }
          seen_tab_ids.insert(tab_id);
          headless_tabs.Append(std::move(tab));
        };
        for (const auto& item : listeners) {
          const base::Value::Dict* d = item.GetIfDict();
          if (!d) {
            continue;
          }
          const std::string mode_kind =
              d->FindString("mode_kind")
                  ? *d->FindString("mode_kind")
                  : (d->FindString("mode") ? *d->FindString("mode")
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
        for (const auto& task :
             dashboard::GetHeadlessInvestigateTaskSnapshots(self->profile_)) {
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
        payload.Set("headless_tabs", headless_tabs.Clone());
        payload.Set("headlessTabs", std::move(headless_tabs));
        self->ResolveCallback(cb, payload);
      };

  if (auto* controller =
          dashboard::HeadlessListenerController::GetForProfile(profile_)) {
    // Inline merge+finish so BindOnce does not need to pass the local lambda.
    controller->ListActive(base::BindOnce(
        [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
           base::Value::List background_listeners, bool ok,
           base::Value::Dict response) {
          base::Value::List listeners = std::move(background_listeners);
          if (ok) {
            if (const base::Value::List* remote =
                    response.FindList("listeners")) {
              for (const auto& entry : *remote) {
                if (!entry.is_dict()) {
                  continue;
                }
                base::Value::Dict d = entry.GetDict().Clone();
                d.Set("mode", "headless");
                d.Set("mode_kind", "headless");
                d.Set("modeKind", "headless");
                if (const std::string* sid = d.FindString("script_id")) {
                  d.Set("scriptId", *sid);
                }
                if (const std::string* name = d.FindString("script_name")) {
                  d.Set("scriptName", *name);
                }
                if (const std::string* url = d.FindString("current_url")) {
                  d.Set("currentUrl", *url);
                }
                listeners.Append(std::move(d));
              }
            }
          }
          if (!self) {
            return;
          }
          base::Value::Dict payload;
          payload.Set("listeners", listeners.Clone());
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
            const std::string tab_id = tab.FindString("tab_id")
                                           ? *tab.FindString("tab_id")
                                           : std::string();
            if (tab_id.empty() || seen_tab_ids.contains(tab_id)) {
              return;
            }
            seen_tab_ids.insert(tab_id);
            headless_tabs.Append(std::move(tab));
          };
          for (const auto& item : listeners) {
            const base::Value::Dict* d = item.GetIfDict();
            if (!d) {
              continue;
            }
            const std::string mode_kind =
                d->FindString("mode_kind")
                    ? *d->FindString("mode_kind")
                    : (d->FindString("mode") ? *d->FindString("mode")
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
          for (const auto& task :
               dashboard::GetHeadlessInvestigateTaskSnapshots(self->profile_)) {
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
          payload.Set("headless_tabs", headless_tabs.Clone());
          payload.Set("headlessTabs", std::move(headless_tabs));
          self->ResolveCallback(cb, payload);
        },
        weak_ptr_factory_.GetWeakPtr(), cb, std::move(listeners)));
    return;
  }

  finish(weak_ptr_factory_.GetWeakPtr(), cb, std::move(listeners));
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
    res.Set("error",
            "Listener is not active in this process. Isolated headless "
            "screenshots over the control channel are not wired yet.");
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

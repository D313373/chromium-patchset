#include "chrome/browser/ui/webui/dashboard/listener_manager.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <string_view>

#include "base/command_line.h"
#include "base/hash/hash.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/containers/flat_set.h"
#include "base/containers/flat_map.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_factory.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_metadata_promotion.h"
#include "chrome/browser/ui/webui/dashboard/actions/native_page_map_matcher.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "chrome/browser/ui/webui/dashboard/run_history_store.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/common/referrer.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/re2/src/re2/re2.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace dashboard {

namespace {

constexpr base::TimeDelta kDefaultAutoRefreshInterval = base::Minutes(1);
constexpr base::TimeDelta kDefaultAutoRefreshListenWindow = base::Minutes(1);
constexpr base::TimeDelta kPostRefreshSettleDelay = base::Seconds(2);
constexpr base::TimeDelta kDefaultListenerTriggerThrottle = base::Seconds(5);
constexpr int kHumanJitterMinMs = 1000;
constexpr int kHumanJitterMaxMs = 3000;
constexpr int kLongPauseStartMinHours = 8;
constexpr int kLongPauseStartMaxHours = 12;
constexpr int kLongPauseDurationMinHours = 4;
constexpr int kLongPauseDurationMaxHours = 6;
constexpr base::TimeDelta kTriggerStormWindow = base::Minutes(1);
constexpr size_t kTriggerStormLimit = 12;
constexpr char kRecentMatchSignaturesKey[] = "listenerRecentMatchSignatures";
constexpr size_t kMaxRecentMatchSignatures = 3;
// Temporary debug switch: disable persisted cross-run match dedupe so listener
// restarts always start from a clean signature state.
constexpr bool kEnableCrossRunMatchDedupe = false;
// Temporary debug switch: disable in-session duplicate match suppression too.
// When false, every satisfied listen match can trigger (subject to throttle).
constexpr bool kEnableInSessionMatchDedupe = true;

std::string TruncateForLog(std::string input, size_t max_len) {
  if (input.size() <= max_len)
    return input;
  return input.substr(0, max_len) + "...";
}

std::string ValueToCompactLog(const base::Value& value, size_t max_len = 2000) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  return TruncateForLog(std::move(json), max_len);
}

std::string NormalizeMatchText(std::string text) {
  return base::CollapseWhitespaceASCII(std::move(text), true);
}

bool ContainsCaseInsensitive(std::string_view haystack,
                             std::string_view needle) {
  return base::ToLowerASCII(haystack).find(base::ToLowerASCII(needle)) !=
         std::string::npos;
}

std::string AttributeValueForCandidate(
    const dashboard::NativePageMapCandidate& candidate,
    const std::string& attribute) {
  const std::string attr = base::ToLowerASCII(attribute);
  if (attr == "class" || attr == "classname")
    return candidate.class_attr;
  if (attr == "id")
    return candidate.id_attr;
  if (attr == "name")
    return candidate.name_attr;
  if (attr == "role")
    return candidate.role;
  if (attr == "aria-label" || attr == "accessibility_name" ||
      attr == "accessibilityname") {
    return candidate.accessibility_name;
  }
  if (attr == "text")
    return candidate.text;
  if (attr == "tag")
    return candidate.tag;
  if (attr == "frame" || attr == "frame_id" || attr == "frameid")
    return candidate.frame_id;
  if (attr == "css" || attr == "css_selector" || attr == "cssselector")
    return candidate.css_selector;
  if (attr == "xpath")
    return candidate.xpath;
  if (attr == "resource_path" || attr == "resourcepath")
    return candidate.resource_path;
  return std::string();
}

std::string RegexMatchValue(const std::string& pattern,
                            const std::string& haystack) {
  if (pattern.empty() || haystack.empty())
    return std::string();
  RE2 re(pattern);
  if (!re.ok())
    return std::string();
  re2::StringPiece input(haystack);
  re2::StringPiece groups[2];
  const int group_count = re.NumberOfCapturingGroups() > 0 ? 2 : 1;
  if (!re.Match(input, 0, input.size(), RE2::UNANCHORED, groups,
                group_count)) {
    return std::string();
  }
  const re2::StringPiece& match = group_count > 1 ? groups[1] : groups[0];
  return std::string(match.data(), match.size());
}

bool CandidateMatchesSnapshotCondition(
    const dashboard::NativePageMapCandidate& candidate,
    const base::Value::Dict& listen_params) {
  const std::string condition = base::ToLowerASCII(
      listen_params.FindString("condition")
          ? *listen_params.FindString("condition")
          : std::string("visible"));
  const std::string want_text = NormalizeMatchText(
      listen_params.FindString("text") ? *listen_params.FindString("text")
                                        : std::string());
  const std::string attribute =
      listen_params.FindString("attribute")
          ? *listen_params.FindString("attribute")
          : std::string();
  const bool text_use_regex =
      listen_params.FindBool("textUseRegex").value_or(false);
  const bool attribute_use_regex =
      listen_params.FindBool("attributeUseRegex").value_or(false);
  const std::string text = NormalizeMatchText(candidate.text);
  const std::string attr = AttributeValueForCandidate(candidate, attribute);

  if (condition == "visible")
    return candidate.visible;
  if (condition == "hidden")
    return !candidate.visible;
  if (condition == "text_contains" || condition == "contains_text") {
    if (want_text.empty())
      return false;
    if (text_use_regex)
      return !RegexMatchValue(want_text, text).empty();
    return ContainsCaseInsensitive(text, want_text);
  }
  if (condition == "text_equals" || condition == "text_is") {
    if (want_text.empty())
      return false;
    if (text_use_regex)
      return !RegexMatchValue(want_text, text).empty();
    return base::EqualsCaseInsensitiveASCII(text, want_text);
  }
  if (condition == "attribute_contains") {
    if (attribute.empty() || want_text.empty())
      return false;
    if (attribute_use_regex)
      return !RegexMatchValue(want_text, attr).empty();
    return ContainsCaseInsensitive(attr, want_text);
  }
  if (condition == "attribute_equals") {
    if (attribute.empty() || want_text.empty())
      return false;
    if (attribute_use_regex)
      return !RegexMatchValue(want_text, attr).empty();
    return base::EqualsCaseInsensitiveASCII(attr, want_text);
  }
  // Transition conditions are resolved by pre/post snapshot diffing, so keep
  // all matched candidates in the snapshot key set.
  return true;
}

std::string SnapshotKeyForCandidate(
    const dashboard::NativePageMapCandidate& candidate) {
  const std::string text = NormalizeMatchText(candidate.text);
  const std::string ax_name = NormalizeMatchText(candidate.accessibility_name);
  const std::string frame_id = candidate.frame_id;
  const std::string resource_path = candidate.resource_path;
  const std::string css = candidate.css_selector;
  const std::string xpath = candidate.xpath;
  const std::string id_attr = candidate.id_attr;
  const std::string class_attr = NormalizeMatchText(candidate.class_attr);
  const std::string name_attr = candidate.name_attr;
  const std::string tag = base::ToLowerASCII(candidate.tag);
  const std::string role = base::ToLowerASCII(candidate.role);
  return base::StringPrintf(
      "e|%s|%s|%s|%s|%s|%s|%s|%s|%s|%d|%d|%d|%d|%d|%d", tag.c_str(),
      id_attr.c_str(), class_attr.c_str(), name_attr.c_str(), css.c_str(),
      xpath.c_str(), resource_path.c_str(), ax_name.c_str(), text.c_str(),
      candidate.x, candidate.y, candidate.width, candidate.height,
      candidate.visible ? 1 : 0, candidate.disabled ? 1 : 0);
}

std::string ListenLocatorsSummary(const base::Value::Dict& listen_params,
                                  size_t max_len = 1000) {
  const base::Value::List* locators = listen_params.FindList("locators");
  if (!locators)
    return "[]";
  base::Value::List out;
  for (const auto& entry : *locators) {
    const base::Value::Dict* d = entry.GetIfDict();
    if (!d)
      continue;
    base::Value::Dict one;
    if (const std::string* t = d->FindString("type"))
      one.Set("type", *t);
    if (const std::string* v = d->FindString("value"))
      one.Set("value", TruncateForLog(*v, 180));
    out.Append(std::move(one));
  }
  return ValueToCompactLog(base::Value(out.Clone()), max_len);
}

base::TimeDelta RandomHoursBetween(int min_hours, int max_hours) {
  return base::Hours(base::RandInt(min_hours, max_hours));
}

std::optional<GURL> NormalizeListenerStartUrl(const std::string& raw) {
  std::string s;
  base::TrimWhitespaceASCII(raw, base::TRIM_ALL, &s);
  if (s.empty())
    return std::nullopt;

  GURL url(s);
  if (url.is_valid() && url.SchemeIsHTTPOrHTTPS())
    return url;

  // If the user typed a bare host (e.g. "example.com"), try https://.
  if (!base::Contains(s, "://")) {
    const bool looks_like_host =
        base::Contains(s, ".") || base::Contains(s, ":") ||
        base::StartsWith(s, "localhost", base::CompareCase::INSENSITIVE_ASCII) ||
        base::StartsWith(s, "127.0.0.1",
                         base::CompareCase::INSENSITIVE_ASCII);
    if (!looks_like_host)
      return std::nullopt;
    GURL with_scheme("https://" + s);
    if (with_scheme.is_valid() && with_scheme.SchemeIsHTTPOrHTTPS())
      return with_scheme;
  }

  return std::nullopt;
}

std::string NormalizeDataKey(std::string key) {
  base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  if (key.size() >= 2 && key.front() == '{' && key.back() == '}') {
    key = key.substr(1, key.size() - 2);
    base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  }
  return key;
}

std::string ExpandTokensInString(
    std::string input,
    const base::flat_map<std::string, std::string>& token_map) {
  if (token_map.empty() || input.find('{') == std::string::npos)
    return input;
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
    std::string token_key = NormalizeDataKey(input.substr(open + 1, close - open - 1));
    auto it = token_map.find(token_key);
    if (!token_key.empty() && it != token_map.end()) {
      out.append(it->second);
    } else {
      out.append(input.substr(open, close - open + 1));
    }
    i = close + 1;
  }
  return out;
}

void ExpandTokensInValue(base::Value* value,
                         const base::flat_map<std::string, std::string>& token_map) {
  if (!value)
    return;
  if (value->is_string()) {
    *value = base::Value(ExpandTokensInString(value->GetString(), token_map));
    return;
  }
  if (value->is_list()) {
    for (auto& item : value->GetList())
      ExpandTokensInValue(&item, token_map);
    return;
  }
  if (value->is_dict()) {
    for (auto it = value->GetDict().begin(); it != value->GetDict().end(); ++it)
      ExpandTokensInValue(&it->second, token_map);
  }
}

bool HasUnresolvedToken(const std::string& text) {
  const size_t open = text.find('{');
  if (open == std::string::npos)
    return false;
  const size_t close = text.find('}', open + 1);
  if (close == std::string::npos || close <= open + 1)
    return false;
  std::string token = text.substr(open + 1, close - open - 1);
  base::TrimWhitespaceASCII(token, base::TRIM_ALL, &token);
  return !token.empty();
}

bool ValidateListenLocatorParams(const base::Value::Dict& listen_params,
                                 std::string* error) {
  const base::Value::List* locators = listen_params.FindList("locators");
  if (!locators || locators->empty()) {
    if (error)
      *error = "Listen action requires at least one locator.";
    return false;
  }
  for (const auto& v : *locators) {
    const base::Value::Dict* loc = v.GetIfDict();
    if (!loc)
      continue;
    const std::string type = loc->FindString("type") ? *loc->FindString("type")
                                                     : std::string();
    const std::string value = loc->FindString("value") ? *loc->FindString("value")
                                                       : std::string();
    std::string trimmed = value;
    base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
    if (trimmed.empty()) {
      if (error)
        *error = "Listen locator contains an empty value.";
      return false;
    }
    const bool is_visual_coords =
        base::EqualsCaseInsensitiveASCII(type, "VISUAL_COORDS");
    if (HasUnresolvedToken(trimmed)) {
      // VISUAL_COORDS values are commonly stored as JSON like {"x":12,"y":34}
      // which uses braces but is not token syntax.
      if (!is_visual_coords) {
        if (error) {
          *error =
              "Listen locator contains unresolved {token}. Set the token in Data "
              "variables/secrets, or use a literal locator value.";
        }
        return false;
      }
    }
    if (base::EqualsCaseInsensitiveASCII(type, "CSS_SELECTOR") &&
        trimmed == "#") {
      if (error)
        *error = "Listen locator CSS selector '#' is invalid.";
      return false;
    }
  }
  return true;
}

base::Value::List FlattenTimeline(const base::Value::Dict& script) {
  base::Value::List flat;
  bool appended_from_actions = false;
  if (const base::Value::List* acts = script.FindList("actions")) {
    for (const auto& a : *acts) {
      flat.Append(a.Clone());
      appended_from_actions = true;
    }
  }
  // Backward compatibility: older scripts may still only use url_contexts.
  if (!appended_from_actions) {
    if (const base::Value::List* ctxs = script.FindList("url_contexts")) {
      for (const auto& c : *ctxs) {
        if (!c.is_dict())
          continue;
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

base::Value::List ExpandCallScripts(Dashboard* dashboard,
                                    const base::Value::List& in,
                                    base::flat_set<std::string>* visiting) {
  base::Value::List out;
  for (const auto& v : in) {
    if (!v.is_dict())
      continue;
    const base::Value::Dict& d = v.GetDict();
    const std::string* act = d.FindString("action");
    const std::string kind = act ? *act : std::string();
    if (base::EqualsCaseInsensitiveASCII(kind, "call_script")) {
      const base::Value::Dict* params = d.FindDict("params");
      std::string sid;
      if (params) {
        if (const std::string* s = params->FindString("scriptId"))
          sid = *s;
      }
      if (!sid.empty() && dashboard && visiting && !visiting->contains(sid)) {
        visiting->insert(sid);
        base::Value::Dict callee = dashboard->LoadScript(sid);
        const std::string callee_kind =
            callee.FindString("kind") ? *callee.FindString("kind")
                                      : std::string("main");
        if (base::EqualsCaseInsensitiveASCII(callee_kind, "listener")) {
          LOG(WARNING) << "[ListenerManager] call_script target is listener kind; "
                          "skipping sid="
                       << sid;
          visiting->erase(sid);
          continue;
        }
        base::Value::List flat = FlattenTimeline(callee);
        base::Value::List expanded = ExpandCallScripts(dashboard, flat, visiting);
        for (auto& e : expanded)
          out.Append(std::move(e));
        visiting->erase(sid);
        continue;
      }
    }
    out.Append(v.Clone());
  }
  return out;
}

std::string TimeToIso8601(base::Time time) {
  base::Time::Exploded exploded;
  time.UTCExplode(&exploded);
  return base::StringPrintf("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                            exploded.year, exploded.month, exploded.day_of_month,
                            exploded.hour, exploded.minute, exploded.second,
                            exploded.millisecond);
}

std::string ReadInitialUrlFromScript(const base::Value::Dict& script) {
  if (const std::string* explicit_start = script.FindString("startUrl")) {
    if (explicit_start && !explicit_start->empty() &&
        explicit_start->find('{') == std::string::npos) {
      std::optional<GURL> start = NormalizeListenerStartUrl(*explicit_start);
      if (start)
        return start->spec();
    }
  }

  if (const base::Value::List* ctxs = script.FindList("url_contexts")) {
    for (const auto& v : *ctxs) {
      if (!v.is_dict())
        continue;
      const std::string* url = v.GetDict().FindString("url");
      if (!url || url->empty() || url->find('{') != std::string::npos)
        continue;
      std::optional<GURL> normalized = NormalizeListenerStartUrl(*url);
      if (normalized)
        return normalized->spec();
    }
  }

  return std::string();
}

std::string FindBestStartUrl(Browser* browser) {
  if (!browser || !browser->tab_strip_model())
    return std::string();
  TabStripModel* tabs = browser->tab_strip_model();
  const int active = tabs->active_index();
  auto ok_url = [](const GURL& url) {
    if (!url.is_valid())
      return false;
    if (!url.SchemeIsHTTPOrHTTPS())
      return false;
    return true;
  };

  if (active >= 0) {
    const GURL url = tabs->GetWebContentsAt(active)->GetLastCommittedURL();
    if (ok_url(url))
      return url.spec();
  }
  for (int i = 0; i < tabs->count(); ++i) {
    if (i == active)
      continue;
    const GURL url = tabs->GetWebContentsAt(i)->GetLastCommittedURL();
    if (ok_url(url))
      return url.spec();
  }
  return std::string();
}

bool InHeadlessListenerProcess() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kHeadlessListener);
}

content::WebContents* FindAnyWebContents(Browser* browser) {
  if (!browser || !browser->tab_strip_model()) {
    return nullptr;
  }
  TabStripModel* tabs = browser->tab_strip_model();
  if (content::WebContents* active = tabs->GetActiveWebContents()) {
    return active;
  }
  if (tabs->count() > 0) {
    return tabs->GetWebContentsAt(0);
  }
  return nullptr;
}

void LoadUrlInExistingTab(content::WebContents* wc, const GURL& url) {
  if (!wc || !url.is_valid()) {
    return;
  }
  wc->GetController().LoadURL(url, content::Referrer(),
                              ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                              std::string());
}

}  // namespace

// ListenerInfo implementation

ListenerInfo::ListenerInfo() = default;
ListenerInfo::~ListenerInfo() = default;
ListenerInfo::ListenerInfo(const ListenerInfo&) = default;
ListenerInfo& ListenerInfo::operator=(const ListenerInfo&) = default;
ListenerInfo::ListenerInfo(ListenerInfo&&) = default;
ListenerInfo& ListenerInfo::operator=(ListenerInfo&&) = default;

dashboard::mojom::ListenerInfoPtr ListenerInfo::ToMojo() const {
  auto mojo = dashboard::mojom::ListenerInfo::New();
  mojo->script_id = script_id;
  mojo->script_name = script_name;
  mojo->is_active = is_active;
  mojo->started_at_iso = TimeToIso8601(started_at);
  return mojo;
}

namespace {

std::optional<bool> ReadOptionalBoolFromParams(const base::Value::Dict& params,
                                               const char* key) {
  if (auto v = params.FindBool(key))
    return *v;
  return std::nullopt;
}

std::optional<base::TimeDelta> ParseExecutionFrequencyFromListenParams(
    const base::Value::Dict& listen_params) {
  std::optional<int> ms = listen_params.FindInt("executionFrequencyMs");
  if (!ms || *ms <= 0)
    return std::nullopt;
  return base::Milliseconds(*ms);
}

std::optional<base::TimeDelta> ParseListenTimeoutFromListenParams(
    const base::Value::Dict& listen_params) {
  std::optional<int> ms = listen_params.FindInt("timeout_ms");
  if (!ms)
    ms = listen_params.FindInt("timeoutMs");
  if (!ms || *ms <= 0)
    return std::nullopt;
  return base::Milliseconds(*ms);
}

std::optional<base::TimeDelta> ParseTriggerThrottleFromListenParams(
    const base::Value::Dict& listen_params) {
  std::optional<int> ms = listen_params.FindInt("triggerThrottleMs");
  if (!ms)
    ms = listen_params.FindInt("trigger_throttle_ms");
  if (!ms || *ms <= 0)
    return std::nullopt;
  return base::Milliseconds(*ms);
}

base::TimeDelta ComputeListenerSampleInterval(
    const base::Value::Dict& listen_params) {
  std::optional<int> ms = listen_params.FindInt("sampleIntervalMs");
  if (!ms)
    ms = listen_params.FindInt("sample_interval_ms");
  constexpr int kDefaultMs = 5000;
  constexpr int kMinMs = 250;
  constexpr int kMaxMs = 60000;
  const int value = ms && *ms > 0 ? *ms : kDefaultMs;
  return base::Milliseconds(std::clamp(value, kMinMs, kMaxMs));
}

int ComputeAutoRefreshListenTimeoutMs(const base::Value::Dict& listen_params) {
  const base::TimeDelta timeout =
      ParseListenTimeoutFromListenParams(listen_params)
          .value_or(kDefaultAutoRefreshListenWindow);
  const int64_t ms = timeout.InMilliseconds();
  constexpr int64_t kMinMs = 1000;
  constexpr int64_t kMaxMs = 2147483647;  // JS setTimeout int32 upper bound.
  return static_cast<int>(std::clamp<int64_t>(ms, kMinMs, kMaxMs));
}

base::TimeDelta ComputeTriggerThrottle(const base::Value::Dict& listen_params) {
  return ParseTriggerThrottleFromListenParams(listen_params)
      .value_or(kDefaultListenerTriggerThrottle);
}

std::string NormalizeSignatureToken(std::string s) {
  base::TrimWhitespaceASCII(s, base::TRIM_ALL, &s);
  s = base::CollapseWhitespaceASCII(s, true);
  if (s.size() > 512)
    s.resize(512);
  return base::ToLowerASCII(s);
}

std::string NormalizeElementKeyToken(std::string s) {
  base::TrimWhitespaceASCII(s, base::TRIM_ALL, &s);
  s = base::CollapseWhitespaceASCII(s, true);
  // Keep substantially more source entropy than generic signatures so the
  // element-key hash remains unique across long dynamic payloads.
  if (s.size() > 4096)
    s.resize(4096);
  return base::ToLowerASCII(s);
}

void AppendCandidateSignature(std::vector<std::string>* out,
                              const std::string& candidate) {
  if (!out)
    return;
  std::string normalized = NormalizeSignatureToken(candidate);
  if (normalized.empty())
    return;
  if (std::find(out->begin(), out->end(), normalized) != out->end())
    return;
  out->push_back(std::move(normalized));
}

std::string SignatureFromElementKey(const std::string& raw_key) {
  const std::string normalized = NormalizeElementKeyToken(raw_key);
  if (normalized.empty())
    return std::string();
  return "k:" + base::NumberToString(base::FastHash(normalized));
}

std::string BuildFallbackMatchSignature(const base::Value::Dict& d) {
  const std::string chosen =
      NormalizeSignatureToken(d.FindString("chosenValue")
                                  ? *d.FindString("chosenValue")
                                  : std::string());
  const std::string text =
      NormalizeSignatureToken(d.FindString("text")
                                  ? *d.FindString("text")
                                  : std::string());
  const std::string attr =
      NormalizeSignatureToken(d.FindString("attr")
                                  ? *d.FindString("attr")
                                  : std::string());
  const std::string tag =
      NormalizeSignatureToken(d.FindString("chosenTag")
                                  ? *d.FindString("chosenTag")
                                  : std::string());
  const std::string author =
      NormalizeSignatureToken(d.FindString("author")
                                  ? *d.FindString("author")
                                  : std::string());
  const std::string timestamp =
      NormalizeSignatureToken(d.FindString("timestamp")
                                  ? *d.FindString("timestamp")
                                  : std::string());
  const std::string article_text =
      NormalizeSignatureToken(d.FindString("articleText")
                                  ? *d.FindString("articleText")
                                  : std::string());
  const int idx = d.FindInt("chosenIndex").value_or(-1);

  if (!author.empty() || !timestamp.empty() || !article_text.empty()) {
    std::string stable_key = author;
    stable_key.append("|");
    stable_key.append(timestamp);
    stable_key.append("|");
    stable_key.append(article_text);
    return "k:" + base::NumberToString(base::FastHash(stable_key));
  }

  std::string sig;
  if (!chosen.empty())
    sig = "v:" + chosen;
  else if (!text.empty())
    sig = "t:" + text;
  else if (!attr.empty())
    sig = "a:" + attr;
  if (!tag.empty())
    sig += "|tag:" + tag;
  if (idx >= 0)
    sig += "|idx:" + std::to_string(idx);
  return sig;
}

std::vector<std::string> BuildMatchSignatures(const std::string& payload) {
  std::vector<std::string> out;
  if (payload.empty())
    return out;

  std::optional<base::Value> parsed = base::JSONReader::Read(
      payload, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    AppendCandidateSignature(&out, payload);
    return out;
  }

  const base::Value::Dict& d = parsed->GetDict();
  if (const base::Value::List* keys = d.FindList("matchKeys")) {
    for (const auto& value : *keys) {
      if (!value.is_string())
        continue;
      AppendCandidateSignature(
          &out, SignatureFromElementKey(value.GetString()));
      if (out.size() >= 24u)
        break;
    }
  }
  if (const std::string* key = d.FindString("matchKey")) {
    AppendCandidateSignature(&out, SignatureFromElementKey(*key));
  }

  AppendCandidateSignature(&out, BuildFallbackMatchSignature(d));
  return out;
}

std::string ExtractFirstHttpUrl(const std::string& source) {
  if (source.empty())
    return std::string();
  size_t start = source.find("https://");
  if (start == std::string::npos)
    start = source.find("http://");
  if (start == std::string::npos)
    return std::string();

  const std::string kTerminators = " \t\r\n\"'<>)]}";
  size_t end = source.find_first_of(kTerminators, start);
  if (end == std::string::npos)
    end = source.size();
  if (end <= start)
    return std::string();

  std::string candidate = source.substr(start, end - start);
  while (!candidate.empty() &&
         (candidate.back() == '.' || candidate.back() == ',' ||
          candidate.back() == ';' || candidate.back() == ':' ||
          candidate.back() == '!')) {
    candidate.pop_back();
  }

  const GURL url(candidate);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS())
    return std::string();
  return url.spec();
}

void SetMatchToken(base::flat_map<std::string, std::string>* tokens,
                   const std::string& key,
                   const std::string& value) {
  if (!tokens || key.empty() || value.empty())
    return;
  tokens->insert_or_assign(key, value);
}

base::flat_map<std::string, std::string> BuildListenerMatchTokenMap(
    const std::string& listener_script_id,
    const base::Value::Dict& listen_params,
    const std::string& payload_json) {
  base::flat_map<std::string, std::string> out;
  if (payload_json.empty())
    return out;

  std::optional<base::Value> parsed = base::JSONReader::Read(
      payload_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict())
    return out;
  const base::Value::Dict& d = parsed->GetDict();

  const std::string match_value =
      d.FindString("matchValue") ? *d.FindString("matchValue") : std::string();
  const std::string chosen_value = d.FindString("chosenValue")
                                       ? *d.FindString("chosenValue")
                                       : std::string();
  const std::string text =
      d.FindString("text") ? *d.FindString("text") : std::string();
  const std::string attr =
      d.FindString("attr") ? *d.FindString("attr") : std::string();
  const std::string page_url = d.FindString("pageUrl")
                                   ? *d.FindString("pageUrl")
                                   : std::string();

  std::string matched_url = ExtractFirstHttpUrl(match_value);
  if (matched_url.empty())
    matched_url = ExtractFirstHttpUrl(chosen_value);
  if (matched_url.empty())
    matched_url = ExtractFirstHttpUrl(text);
  if (matched_url.empty())
    matched_url = ExtractFirstHttpUrl(attr);

  std::string best_value = matched_url;
  if (best_value.empty())
    best_value = !match_value.empty() ? match_value : chosen_value;
  if (best_value.empty())
    best_value = !text.empty() ? text : attr;

  SetMatchToken(&out, "listener.match", best_value);
  SetMatchToken(&out, "listener.match.value", best_value);
  SetMatchToken(&out, "listener.match.text", text);
  SetMatchToken(&out, "listener.match.attr", attr);
  SetMatchToken(&out, "listener.match.url", matched_url);
  SetMatchToken(&out, "listener.match.page_url", page_url);

  const std::string normalized_listener_id =
      NormalizeDataKey(listener_script_id);
  if (!normalized_listener_id.empty()) {
    const std::string prefix = "listener." + normalized_listener_id;
    SetMatchToken(&out, prefix + ".match", best_value);
    SetMatchToken(&out, prefix + ".match.value", best_value);
    SetMatchToken(&out, prefix + ".match.text", text);
    SetMatchToken(&out, prefix + ".match.attr", attr);
    SetMatchToken(&out, prefix + ".match.url", matched_url);
    SetMatchToken(&out, prefix + ".match.page_url", page_url);
  }

  const std::string capture_variable = NormalizeDataKey(
      listen_params.FindString("captureVariable")
          ? *listen_params.FindString("captureVariable")
          : std::string());
  if (!capture_variable.empty()) {
    SetMatchToken(&out, capture_variable, best_value);
  }
  return out;
}

std::vector<std::string> ReadRecentMatchSignatures(
    const base::Value::Dict& script) {
  std::vector<std::string> out;
  const base::Value::List* values = script.FindList(kRecentMatchSignaturesKey);
  if (!values)
    return out;
  out.reserve(values->size());
  for (const auto& value : *values) {
    if (!value.is_string())
      continue;
    std::string s = NormalizeSignatureToken(value.GetString());
    if (s.empty())
      continue;
    if (std::find(out.begin(), out.end(), s) != out.end())
      continue;
    out.push_back(std::move(s));
    if (out.size() >= kMaxRecentMatchSignatures)
      break;
  }
  return out;
}

void WriteRecentMatchSignatures(base::Value::Dict* script,
                                const std::vector<std::string>& signatures) {
  if (!script)
    return;
  base::Value::List out;
  size_t count = 0;
  for (const auto& signature : signatures) {
    if (signature.empty())
      continue;
    out.Append(signature);
    if (++count >= kMaxRecentMatchSignatures)
      break;
  }
  script->Set(kRecentMatchSignaturesKey, std::move(out));
}

void RememberRecentMatchSignature(std::vector<std::string>* signatures,
                                  const std::string& signature) {
  if (!signatures)
    return;
  std::string normalized = NormalizeSignatureToken(signature);
  if (normalized.empty())
    return;
  auto& vec = *signatures;
  vec.erase(std::remove(vec.begin(), vec.end(), normalized), vec.end());
  vec.insert(vec.begin(), std::move(normalized));
  if (vec.size() > kMaxRecentMatchSignatures)
    vec.resize(kMaxRecentMatchSignatures);
}

void PersistRecentMatchSignaturesAsync(Profile* profile,
                                       Dashboard* dashboard,
                                       const std::string& script_id,
                                       const std::vector<std::string>& signatures) {
  if (!profile || !dashboard || script_id.empty())
    return;
  base::Value::Dict script = dashboard->LoadScript(script_id);
  if (script.empty())
    return;
  WriteRecentMatchSignatures(&script, signatures);
  dashboard->SaveScript(script_id, script.Clone());
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, std::string script_id, base::Value::Dict script) {
            (void)dashboard::ScriptStore::Write(profile, script_id, script);
          },
          base::Unretained(profile), script_id, std::move(script)));
}

std::string ReadKeyScriptId(const base::Value::Dict& script) {
  if (const std::string* ks = script.FindString("keyScriptId"))
    return *ks;
  return std::string();
}

bool FindFirstListenParams(const base::Value::List& timeline,
                           base::Value::Dict* out_params) {
  if (!out_params)
    return false;
  for (const auto& v : timeline) {
    if (!v.is_dict())
      continue;
    const base::Value::Dict& d = v.GetDict();
    const std::string* act = d.FindString("action");
    const std::string kind = act ? *act : std::string();
    if (!base::EqualsCaseInsensitiveASCII(kind, "listen"))
      continue;
    const base::Value::Dict* params = d.FindDict("params");
    if (params) {
      *out_params = params->Clone();
      return true;
    }
  }
  return false;
}

std::string ReadOnMatchScriptId(const base::Value::Dict& listen_params) {
  if (const std::string* s = listen_params.FindString("onMatchScriptId"))
    return *s;
  return std::string();
}

std::vector<std::string> ListMainScriptsInCollection(
    dashboard::Dashboard* dashboard,
    const std::string& collection_name) {
  std::vector<std::pair<std::string, std::string>> ranked;
  if (!dashboard || collection_name.empty()) {
    return {};
  }
  base::Value::Dict all = dashboard->LoadAllScripts();
  for (auto [id, value] : all) {
    if (!value.is_dict()) {
      continue;
    }
    if (dashboard::ScriptStore::CollectionOfScriptId(id) != collection_name) {
      continue;
    }
    const base::Value::Dict& script = value.GetDict();
    const std::string* kind = script.FindString("kind");
    const std::string kind_l =
        kind ? base::ToLowerASCII(*kind) : std::string("main");
    if (kind_l != "main") {
      continue;
    }
    const std::string* name = script.FindString("name");
    ranked.emplace_back(name ? *name : id, id);
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::string> out;
  out.reserve(ranked.size());
  for (const auto& item : ranked) {
    out.push_back(item.second);
  }
  return out;
}

base::Value::Dict BuildCollectionTriggerScript(
    const std::string& collection_ref,
    const std::string& collection_name,
    const std::vector<std::string>& script_ids) {
  base::Value::Dict script;
  script.Set("id", collection_ref);
  script.Set("name", "Collection: " + collection_name);
  script.Set("kind", "main");
  script.Set("collection", collection_name);
  base::Value::List actions;
  for (const std::string& id : script_ids) {
    base::Value::Dict step;
    step.Set("action", "call_script");
    base::Value::Dict params;
    params.Set("scriptId", id);
    step.Set("params", std::move(params));
    actions.Append(std::move(step));
  }
  script.Set("actions", std::move(actions));
  return script;
}

bool ResolveOnMatchTriggerScript(dashboard::Dashboard* dashboard,
                                 const std::string& on_match,
                                 base::Value::Dict* out_script,
                                 std::string* error) {
  if (!dashboard || !out_script) {
    if (error)
      *error = "Internal error resolving on-match target.";
    return false;
  }
  if (dashboard::ScriptStore::IsCollectionRef(on_match)) {
    const std::string collection =
        dashboard::ScriptStore::ParseCollectionRef(on_match);
    if (!dashboard::ScriptStore::IsValidCollectionName(collection)) {
      if (error)
        *error = "Invalid collection name: " + collection;
      return false;
    }
    const std::vector<std::string> members =
        ListMainScriptsInCollection(dashboard, collection);
    if (members.empty()) {
      if (error) {
        *error = "Collection has no main scripts: " + collection;
      }
      return false;
    }
    *out_script = BuildCollectionTriggerScript(on_match, collection, members);
    return true;
  }
  base::Value::Dict script = dashboard->LoadScript(on_match);
  if (script.empty()) {
    if (error)
      *error = "On-match script not found: " + on_match;
    return false;
  }
  *out_script = std::move(script);
  return true;
}

bool ReplaceFirstListenActionRecursive(base::Value::Dict* action,
                                       const base::Value::Dict& promoted) {
  if (!action) {
    return false;
  }
  const std::string* action_name = action->FindString("action");
  if (action_name && base::EqualsCaseInsensitiveASCII(*action_name, "listen")) {
    action->Set("action", "listen");
    if (const base::Value::Dict* promoted_params = promoted.FindDict("params")) {
      action->Set("params", promoted_params->Clone());
    }
    return true;
  }

  constexpr const char* kChildKeys[] = {"steps", "then", "else", "actions"};
  for (const char* key : kChildKeys) {
    if (base::Value::List* list = action->FindList(key)) {
      for (auto& item : *list) {
        if (base::Value::Dict* child = item.GetIfDict();
            child && ReplaceFirstListenActionRecursive(child, promoted)) {
          return true;
        }
      }
    }
  }
  if (base::Value::Dict* params = action->FindDict("params")) {
    for (const char* key : kChildKeys) {
      if (base::Value::List* list = params->FindList(key)) {
        for (auto& item : *list) {
          if (base::Value::Dict* child = item.GetIfDict();
              child && ReplaceFirstListenActionRecursive(child, promoted)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

base::Value::List ExtractSetupActionsBeforeFirstListen(
    const base::Value::List& actions) {
  base::Value::List out;
  for (const auto& v : actions) {
    const base::Value::Dict* d = v.GetIfDict();
    if (!d)
      continue;
    const std::string* act = d->FindString("action");
    const std::string kind = act ? *act : std::string();
    if (base::EqualsCaseInsensitiveASCII(kind, "listen"))
      break;
    out.Append(v.Clone());
  }
  return out;
}

}  // namespace

// ListenerManager::ActiveListener implementation

struct ListenerManager::ActiveListener : public content::WebContentsObserver {
  struct TriggerTabObserver : public content::WebContentsObserver {
    explicit TriggerTabObserver(ActiveListener* owner) : owner_(owner) {}
    ~TriggerTabObserver() override = default;

    void Watch(content::WebContents* contents) { Observe(contents); }

    void WebContentsDestroyed() override {
      if (owner_)
        owner_->trigger_web_contents = nullptr;
      Observe(nullptr);
    }

   private:
    raw_ptr<ActiveListener> owner_ = nullptr;
  };

  ActiveListener(ListenerManager* owner,
                 std::string script_id,
                 content::WebContents* wc)
      : content::WebContentsObserver(wc),
        trigger_tab_observer(this),
        owner(owner),
        script_id_for_cb(std::move(script_id)) {}

  ListenerInfo info;
  uint64_t seq = 0;
  raw_ptr<Browser> browser = nullptr;
  raw_ptr<content::WebContents> web_contents = nullptr;
  raw_ptr<content::WebContents> trigger_web_contents = nullptr;
  TriggerTabObserver trigger_tab_observer;

  std::string key_script_id;
  base::Value::Dict listen_params;
  std::string on_match_script_id;

  std::string current_execution_id;
  base::Time last_triggered;
  std::deque<base::Time> recent_trigger_times;
  bool stop_requested = false;
  bool auto_refresh = false;
  base::TimeDelta refresh_interval;
  base::Time next_refresh_at;
  bool refresh_jitter_enabled = true;
  bool extreme_evasion_enabled = true;
  bool force_cdp_execution = false;
  std::vector<std::string> recent_match_signatures;
  std::vector<std::string> pre_refresh_snapshot_keys;
  bool awaiting_post_refresh_snapshot = false;
  base::Time next_long_pause_at;
  base::Time long_pause_until;
  bool jitter_due_after_refresh = false;
  bool owns_web_contents = false;
  base::Value::List pending_setup_actions;

  // Owns the in-flight listen action for the currently selected execution path.
  std::unique_ptr<dashboard::actions::Action> listen_action;

  raw_ptr<ListenerManager> owner = nullptr;
  std::string script_id_for_cb;

  void WebContentsDestroyed() override {
    // The user can close the background tab manually. Stop the listener and
    // cancel any pending work to avoid UAFs/crashes.
    web_contents = nullptr;
    info.current_url.clear();
    listen_action.reset();
    DashboardUIMessageHandler::NotifyListenersChanged();
    if (owner) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&ListenerManager::OnListenerTabDestroyed,
                         owner->weak_ptr_factory_.GetWeakPtr(),
                         script_id_for_cb, seq));
    }
  }

  void DidFinishNavigation(content::NavigationHandle* navigation_handle) override {
    if (!navigation_handle || !navigation_handle->HasCommitted() ||
        !navigation_handle->IsInPrimaryMainFrame()) {
      return;
    }
    if (!web_contents)
      return;
    const GURL url = web_contents->GetLastCommittedURL();
    info.current_url = url.is_valid() ? url.spec() : std::string();
    DashboardUIMessageHandler::NotifyListenersChanged();
  }
};

// ListenerManager implementation

ListenerManager::ListenerManager(Profile* profile, Dashboard* dashboard)
    : profile_(profile), dashboard_(dashboard) {}

ListenerManager::~ListenerManager() {
  // Best-effort stop all active listeners (stop runs + close background tabs).
  while (!active_listeners_.empty()) {
    const std::string script_id = active_listeners_.begin()->first;
    StopListener(script_id, /*error=*/nullptr);
  }
}

void ListenerManager::SetRemoteTriggerDispatchCallback(
    RemoteTriggerDispatchCallback callback) {
  remote_trigger_dispatch_ = std::move(callback);
}

bool ListenerManager::StartListener(const std::string& script_id,
                                    std::string* error) {
  static const base::flat_map<std::string, std::string> kEmptyTokenMap;
  return StartListener(script_id, kEmptyTokenMap, error,
                       /*force_cdp_execution=*/false);
}

bool ListenerManager::StartListener(
    const std::string& script_id,
    const base::flat_map<std::string, std::string>& token_map,
    std::string* error,
    bool force_cdp_execution) {
  LOG(WARNING) << "[ListenerManager] StartListener requested script="
               << script_id << " force_cdp_execution="
               << (force_cdp_execution ? 1 : 0);
  // Check if already active
  if (IsListenerActive(script_id)) {
    if (error) {
      *error = "Listener already active: " + script_id;
    }
    LOG(WARNING) << "[ListenerManager] StartListener rejected already_active "
                 << "script=" << script_id;
    return false;
  }

  // Validate listener script
  if (!ValidateListenerScript(script_id, error))
    return false;

  base::Value::Dict listener_script = dashboard_->LoadScript(script_id);
  if (listener_script.empty()) {
    if (error)
      *error = "Script not found: " + script_id;
    LOG(WARNING) << "[ListenerManager] StartListener missing_script script="
                 << script_id;
    return false;
  }

  // Extract listen params + trigger script id.
  base::Value::List flat = FlattenTimeline(listener_script);
  base::flat_set<std::string> visiting;
  base::Value::List expanded = ExpandCallScripts(dashboard_, flat, &visiting);
  base::Value::Dict listen_params;
  if (!FindFirstListenParams(expanded, &listen_params)) {
    if (error)
      *error = "Listener script has no listen action.";
    LOG(WARNING) << "[ListenerManager] StartListener no_listen_action script="
                 << script_id;
    return false;
  }
  std::string on_match = ReadOnMatchScriptId(listen_params);
  if (on_match.empty()) {
    if (error)
      *error = "Listen action is missing 'On Match: Run Script'.";
    LOG(WARNING) << "[ListenerManager] StartListener missing_on_match script="
                 << script_id;
    return false;
  }
  base::Value::List setup_actions =
      ExtractSetupActionsBeforeFirstListen(expanded);
  const std::string key_script_id_early = ReadKeyScriptId(listener_script);
  if (!key_script_id_early.empty() && !setup_actions.empty()) {
    const base::Value::Dict* first = setup_actions.front().GetIfDict();
    const std::string* act = first ? first->FindString("action") : nullptr;
    if (act && base::EqualsCaseInsensitiveASCII(*act, "navigate")) {
      base::Value::List trimmed;
      bool skipped = false;
      for (auto& step : setup_actions) {
        if (!skipped) {
          skipped = true;
          continue;
        }
        trimmed.Append(std::move(step));
      }
      setup_actions = std::move(trimmed);
      LOG(WARNING) << "[ListenerManager] Dropped leading setup navigate; "
                      "key script owns navigation script="
                   << script_id;
    }
  }

  base::Value listen_params_value(listen_params.Clone());
  ExpandTokensInValue(&listen_params_value, token_map);
  if (const base::Value::Dict* expanded_dict = listen_params_value.GetIfDict())
    listen_params = expanded_dict->Clone();

  std::string locator_error;
  if (!ValidateListenLocatorParams(listen_params, &locator_error)) {
    if (error)
      *error = locator_error;
    LOG(WARNING) << "[ListenerManager] StartListener invalid_locators script="
                 << script_id << " error=" << locator_error;
    return false;
  }

  Browser* browser = chrome::FindLastActiveWithProfile(profile_);
  if (!browser) {
    if (error)
      *error = "No active browser window for listener.";
    LOG(WARNING) << "[ListenerManager] StartListener no_browser script="
                 << script_id;
    return false;
  }

  const std::string key_script_id = ReadKeyScriptId(listener_script);
  const std::string initial_url = ReadInitialUrlFromScript(listener_script);
  const bool auto_refresh =
      ReadOptionalBoolFromParams(listen_params, "autoRefresh")
          .value_or(false);
  const bool refresh_jitter_enabled =
      ReadOptionalBoolFromParams(listen_params, "refreshJitter")
          .value_or(true);
  const bool extreme_evasion_enabled =
      ReadOptionalBoolFromParams(listen_params, "extremeEvasion")
          .value_or(true);
  const base::TimeDelta refresh_interval =
      ParseExecutionFrequencyFromListenParams(listen_params)
          .value_or(kDefaultAutoRefreshInterval);
  std::string start_url;
  std::optional<GURL> start_gurl;
  bool owns_web_contents = true;
  content::WebContents* wc = nullptr;
  const bool headless_proc = InHeadlessListenerProcess();
  if (key_script_id.empty()) {
    start_url = initial_url;
    start_gurl = NormalizeListenerStartUrl(start_url);
    if (!start_gurl)
      start_url.clear();
    if (start_url.empty()) {
      start_url = FindBestStartUrl(browser);
      start_gurl = NormalizeListenerStartUrl(start_url);
    }
    if (start_url.empty()) {
      if (error) {
        *error =
            "Listener has no start URL and no Key Script. Set a URL in the first "
            "url_contexts entry, or set a Key Script that navigates.";
      }
      LOG(WARNING) << "[ListenerManager] StartListener no_start_url script="
                   << script_id;
      return false;
    }
  } else {
    // Key script owns navigation/login. Do not load startUrl into this tab
    // (that would wipe the session the key script is about to build).
    start_gurl = GURL("about:blank");
    start_url = "about:blank";
  }

  const GURL target_url = start_gurl.value_or(GURL("about:blank"));
  if (headless_proc) {
    // Dedicated headless process already has a launch tab. Reuse it so a
    // key-script listener does not open a second tab, then listen on the
    // page the key script ends on.
    wc = FindAnyWebContents(browser);
  }
  if (!wc) {
    NavigateParams np(browser, target_url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    np.disposition = headless_proc
                         ? WindowOpenDisposition::NEW_FOREGROUND_TAB
                         : WindowOpenDisposition::NEW_BACKGROUND_TAB;
    Navigate(&np);
    wc = np.navigated_or_inserted_contents;
    LOG(WARNING) << "[ListenerManager] StartListener created_tab script="
                 << script_id << " target_url=" << target_url.spec()
                 << " headless=" << (headless_proc ? 1 : 0);
  } else if (key_script_id.empty()) {
    LoadUrlInExistingTab(wc, target_url);
    LOG(WARNING) << "[ListenerManager] StartListener reused_tab script="
                 << script_id << " target_url=" << target_url.spec();
  } else {
    LOG(WARNING)
        << "[ListenerManager] StartListener reused_tab_for_key_script script="
        << script_id << " url=" << wc->GetLastCommittedURL().spec();
  }
  if (!wc) {
    if (error)
      *error = "Failed to create listener tab.";
    LOG(WARNING) << "[ListenerManager] StartListener create_tab_failed script="
                 << script_id;
    return false;
  }

  auto state = std::make_unique<ActiveListener>(this, script_id, wc);
  state->seq = next_listener_seq_++;
  state->browser = browser;
  state->web_contents = wc;
  state->owns_web_contents = owns_web_contents;
  state->listen_params = std::move(listen_params);
  if (auto_refresh) {
    state->listen_params.Set(
        "timeout_ms", ComputeAutoRefreshListenTimeoutMs(state->listen_params));
  }
  state->on_match_script_id = std::move(on_match);
  if (kEnableCrossRunMatchDedupe) {
    state->recent_match_signatures = ReadRecentMatchSignatures(listener_script);
  } else {
    state->recent_match_signatures.clear();
  }
  state->key_script_id = key_script_id;
  state->auto_refresh = auto_refresh;
  state->refresh_interval = refresh_interval;
  if (auto_refresh && refresh_interval.is_positive()) {
    state->next_refresh_at = base::Time::Now() + refresh_interval;
  }
  state->refresh_jitter_enabled = refresh_jitter_enabled;
  state->extreme_evasion_enabled = extreme_evasion_enabled;
  state->force_cdp_execution = force_cdp_execution;
  state->next_long_pause_at =
      base::Time::Now() +
      RandomHoursBetween(kLongPauseStartMinHours, kLongPauseStartMaxHours);
  state->info.script_id = script_id;
  state->info.script_name = GetScriptName(script_id);
  state->info.is_active = true;
  state->info.started_at = base::Time::Now();
  state->info.current_url =
      wc->GetLastCommittedURL().is_valid() ? wc->GetLastCommittedURL().spec()
                                           : std::string();
  state->info.status = "starting";
  state->info.status_detail = "Starting listener…";
  state->info.running_kind.clear();
  state->info.execution_id.clear();
  const uint64_t seq = state->seq;

  active_listeners_.emplace(script_id, std::move(state));
  DashboardUIMessageHandler::NotifyListenersChanged();

  auto* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (!exec_service) {
    if (error)
      *error = "No ScriptExecutionService for listener.";
    LOG(WARNING) << "[ListenerManager] StartListener no_exec_service script="
                 << script_id;
    StopListener(script_id, /*error=*/nullptr);
    return false;
  }

  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second) {
    if (error)
      *error = "Listener state missing after start.";
    return false;
  }
  ActiveListener* started = it->second.get();
  const bool run_key_on_start = !started->key_script_id.empty();

  if (run_key_on_start) {
    started->pending_setup_actions = std::move(setup_actions);
    started->info.status = "key_script";
    started->info.status_detail = "Running key script...";
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ListenerManager::RunKeyScriptForListener,
                       weak_ptr_factory_.GetWeakPtr(), script_id, seq),
        base::Milliseconds(250));
    LOG(WARNING) << "[ListenerManager] StartListener scheduling_key_script "
                 << "script=" << script_id
                 << " key_script=" << started->key_script_id;
    return true;
  }

  // Key script already left a page: leftover setup (if any) runs in place,
  // then listen. Do not replay the key script or open another tab.
  if (!setup_actions.empty()) {
    started->info.status = "setup";
    started->info.status_detail = "Running setup actions...";
    started->info.running_kind = "setup";
    dashboard::ScriptExecutionService::RunOptions run_options;
    run_options.force_cdp_execution = started->force_cdp_execution;
    started->current_execution_id = exec_service->StartScriptRunWithOptions(
        script_id, setup_actions, started->web_contents,
        /*key_timeline=*/nullptr, run_options,
        base::BindOnce(&ListenerManager::OnSetupActionsCompleted,
                       weak_ptr_factory_.GetWeakPtr(), script_id, seq));
    started->info.execution_id = started->current_execution_id;
    if (started->current_execution_id.empty()) {
      started->info.status = "error";
      started->info.status_detail = "Failed to start setup actions.";
      started->info.last_error = started->info.status_detail;
      started->info.running_kind.clear();
      started->info.execution_id.clear();
      LOG(WARNING) << "[ListenerManager] StartListener setup_start_failed "
                   << "script=" << script_id;
      return true;
    }
    LOG(WARNING) << "[ListenerManager] StartListener running_setup script="
                 << script_id;
    return true;
  }

  LOG(WARNING) << "[ListenerManager] StartListener arming_listen script="
               << script_id;
  ArmListen(script_id, seq);
  return true;
}

bool ListenerManager::StartListener(
    const std::string& script_id,
    const base::flat_map<std::string, std::string>& token_map,
    std::string* error) {
  return StartListener(script_id, token_map, error,
                       /*force_cdp_execution=*/false);
}

void ListenerManager::RunKeyScriptForListener(const std::string& script_id,
                                              uint64_t seq) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq || st->stop_requested)
    return;
  if (st->key_script_id.empty()) {
    ArmListen(script_id, seq);
    return;
  }
  if (!st->web_contents) {
    st->info.status = "error";
    st->info.status_detail = "Listener tab was closed.";
    st->info.last_error = st->info.status_detail;
    return;
  }

  base::Value::Dict key_script = dashboard_->LoadScript(st->key_script_id);
  if (key_script.empty()) {
    LOG(ERROR) << "[ListenerManager] key script not found id="
               << st->key_script_id;
    st->info.status = "error";
    st->info.status_detail = "Key script missing.";
    st->info.last_error = st->info.status_detail;
    return;
  }
  base::Value::List kflat = FlattenTimeline(key_script);
  base::flat_set<std::string> kvisiting;
  base::Value::List key_seq = ExpandCallScripts(dashboard_, kflat, &kvisiting);

  auto* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (!exec_service) {
    st->info.status = "error";
    st->info.status_detail = "No ScriptExecutionService.";
    st->info.last_error = st->info.status_detail;
    return;
  }

  st->info.status = "key_script";
  st->info.status_detail = "Running key script…";
  st->info.running_kind = "key_script";
  dashboard::ScriptExecutionService::RunOptions run_options;
  run_options.force_cdp_execution = st->force_cdp_execution;
  st->current_execution_id = exec_service->StartScriptRunWithOptions(
      st->key_script_id, key_seq, st->web_contents, /*key_timeline=*/nullptr,
      run_options,
      base::BindOnce(&ListenerManager::OnKeyScriptCompleted,
                     weak_ptr_factory_.GetWeakPtr(), script_id, seq));
  st->info.execution_id = st->current_execution_id;
  if (st->current_execution_id.empty()) {
    st->info.status = "error";
    st->info.status_detail = "Failed to start key script.";
    st->info.last_error = st->info.status_detail;
    st->info.execution_id.clear();
    st->info.running_kind.clear();
  }
}

bool ListenerManager::StopListener(const std::string& script_id,
                                    std::string* error) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second) {
    if (error) {
      *error = "Listener not active: " + script_id;
    }
    return false;
  }

  std::unique_ptr<ActiveListener> state = std::move(it->second);
  active_listeners_.erase(it);
  if (!state)
    return true;

  state->stop_requested = true;
  state->listen_action.reset();

  auto* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (exec_service && !state->current_execution_id.empty()) {
    exec_service->StopRun(state->current_execution_id, "listener stopped");
  }

  auto close_tab_if_present = [&](content::WebContents* contents) {
    if (!contents || !state->browser)
      return;
    TabStripModel* tabs = state->browser->tab_strip_model();
    const int idx = tabs ? tabs->GetIndexOfWebContents(contents) : -1;
    if (tabs && idx >= 0)
      tabs->CloseWebContentsAt(idx, TabCloseTypes::CLOSE_NONE);
  };
  if (state->owns_web_contents)
    close_tab_if_present(state->web_contents);
  if (state->trigger_web_contents &&
      state->trigger_web_contents != state->web_contents)
    close_tab_if_present(state->trigger_web_contents);

  LOG(INFO) << "[ListenerManager] Stopped listener: " << script_id;
  DashboardUIMessageHandler::NotifyListenersChanged();
  return true;
}

std::vector<ListenerInfo> ListenerManager::GetActiveListeners() const {
  std::vector<ListenerInfo> result;
  for (const auto& [script_id, listener] : active_listeners_) {
    if (!listener)
      continue;
    ListenerInfo info = listener->info;
    if (listener->web_contents) {
      const GURL url = listener->web_contents->GetLastCommittedURL();
      if (url.is_valid())
        info.current_url = url.spec();
    }
    result.push_back(std::move(info));
  }
  return result;
}

content::WebContents* ListenerManager::GetListenerWebContents(
    const std::string& script_id) const {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return nullptr;
  return it->second->web_contents;
}

bool ListenerManager::IsListenerActive(const std::string& script_id) const {
  auto it = active_listeners_.find(script_id);
  return it != active_listeners_.end() && it->second && it->second->info.is_active;
}

std::string ListenerManager::GetScriptName(const std::string& script_id) const {
  base::Value::Dict script = dashboard_->LoadScript(script_id);
  const std::string* name = script.FindString("name");
  return name ? *name : "";
}

bool ListenerManager::ValidateListenerScript(const std::string& script_id,
                                              std::string* error) const {
  // Load script
  base::Value::Dict script = dashboard_->LoadScript(script_id);
  if (script.empty()) {
    if (error) {
      *error = "Script not found: " + script_id;
    }
    return false;
  }

  const std::string kind =
      script.FindString("kind") ? *script.FindString("kind") : std::string();
  if (!kind.empty() && kind != "listener") {
    if (error) {
      *error = "Script is not a listener script (kind=" + kind + ").";
    }
    return false;
  }

  // Validate the first listen action's required params (locators + condition).
  base::Value::List flat = FlattenTimeline(script);
  base::flat_set<std::string> visiting;
  base::Value::List expanded = ExpandCallScripts(dashboard_, flat, &visiting);
  base::Value::Dict listen_params;
  if (!FindFirstListenParams(expanded, &listen_params)) {
    if (error)
      *error = "Listener script has no listen action.";
    return false;
  }
  const base::Value::List* locs = listen_params.FindList("locators");
  if (!locs || locs->empty()) {
    if (error)
      *error = "Listen action must include at least one locator.";
    return false;
  }
  const std::string cond =
      listen_params.FindString("condition")
          ? *listen_params.FindString("condition")
          : std::string();
  if (cond.empty()) {
    if (error)
      *error = "Listen action must include a condition.";
    return false;
  }
  if (cond.find("text_") != std::string::npos ||
      cond == "new_child_contains") {
    const std::string text =
        listen_params.FindString("text") ? *listen_params.FindString("text")
                                         : std::string();
    if (base::TrimWhitespaceASCII(text, base::TRIM_ALL).empty()) {
      if (error)
        *error = "Listen condition requires non-empty text.";
      return false;
    }
  }
  if (cond.find("attribute_") != std::string::npos) {
    const std::string attr =
        listen_params.FindString("attribute")
            ? *listen_params.FindString("attribute")
            : std::string();
    if (base::TrimWhitespaceASCII(attr, base::TRIM_ALL).empty()) {
      if (error)
        *error = "Listen condition requires an attribute name.";
      return false;
    }
  }

  const std::string on_match = ReadOnMatchScriptId(listen_params);
  if (on_match.empty()) {
    if (error)
      *error = "Listen action is missing 'On Match: Run Script'.";
    return false;
  }
  base::Value::Dict resolved;
  std::string resolve_error;
  if (!ResolveOnMatchTriggerScript(dashboard_, on_match, &resolved,
                                   &resolve_error)) {
    if (error)
      *error = resolve_error.empty() ? ("On-match target not found: " + on_match)
                                     : resolve_error;
    return false;
  }

  return true;
}

void ListenerManager::OnSetupActionsCompleted(const std::string& script_id,
                                              uint64_t seq,
                                              bool success,
                                              int /*failed_step*/,
                                              std::string message) {
  LOG(INFO) << "[ListenerManager] setup completed: listener=" << script_id
            << " success=" << (success ? 1 : 0)
            << (message.empty() ? std::string()
                                : std::string(" message=") + message);
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq)
    return;

  st->current_execution_id.clear();
  st->info.execution_id.clear();
  st->info.running_kind.clear();

  if (st->stop_requested)
    return;

  if (!success) {
    LOG(WARNING) << "[ListenerManager] setup actions failed for listener="
                 << script_id << " message=" << message;
    st->info.status = "error";
    st->info.status_detail =
        message.empty() ? "Setup actions failed." : message;
    st->info.last_error = st->info.status_detail;
    LogListenerEvent(script_id, *st, "error", st->info.last_error);
    return;
  }

  // Key script, when needed, already ran on this tab (or we attached to the
  // page it left). Leftover setup is done; start listening in place.
  st->info.status = "listening";
  st->info.status_detail = "Listening...";
  ArmListen(script_id, seq);
}

void ListenerManager::OnKeyScriptCompleted(const std::string& script_id,
                                          uint64_t seq,
                                          bool success,
                                          int /*failed_step*/,
                                          std::string message) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq)
    return;
  st->current_execution_id.clear();
  st->info.execution_id.clear();
  st->info.running_kind.clear();

  if (st->stop_requested)
    return;
  if (!success) {
    LOG(WARNING) << "[ListenerManager] key script failed for listener=" << script_id
                 << " message=" << message;
    st->info.status = "error";
    st->info.status_detail = message.empty() ? "Key script failed" : message;
    st->info.last_error = st->info.status_detail;
    return;
  }

  if (!st->pending_setup_actions.empty()) {
    auto* exec_service =
        dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
    if (!exec_service) {
      st->info.status = "error";
      st->info.status_detail = "No ScriptExecutionService.";
      st->info.last_error = st->info.status_detail;
      return;
    }
    if (!st->web_contents) {
      st->info.status = "error";
      st->info.status_detail = "Listener tab was closed.";
      st->info.last_error = st->info.status_detail;
      return;
    }
    st->info.status = "setup";
    st->info.status_detail = "Running setup actions...";
    st->info.running_kind = "setup";
    dashboard::ScriptExecutionService::RunOptions run_options;
    run_options.force_cdp_execution = st->force_cdp_execution;
    base::Value::List leftover = std::move(st->pending_setup_actions);
    st->pending_setup_actions.clear();
    st->current_execution_id = exec_service->StartScriptRunWithOptions(
        script_id, leftover, st->web_contents, /*key_timeline=*/nullptr,
        run_options,
        base::BindOnce(&ListenerManager::OnSetupActionsCompleted,
                       weak_ptr_factory_.GetWeakPtr(), script_id, seq));
    st->info.execution_id = st->current_execution_id;
    if (st->current_execution_id.empty()) {
      st->info.status = "error";
      st->info.status_detail = "Failed to start setup actions.";
      st->info.last_error = st->info.status_detail;
      st->info.running_kind.clear();
      st->info.execution_id.clear();
    }
    return;
  }

  st->info.status = "listening";
  st->info.status_detail = "Listening…";
  ArmListen(script_id, seq);
}

bool ListenerManager::MaybeDelayForCadence(const std::string& script_id,
                                           uint64_t seq) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return false;
  ActiveListener* st = it->second.get();
  if (st->seq != seq || st->stop_requested)
    return false;
  if (!st->web_contents) {
    LOG(ERROR) << "[ListenerManager] no WebContents for listener=" << script_id;
    st->info.status = "error";
    st->info.status_detail = "Listener tab was closed.";
    st->info.last_error = st->info.status_detail;
    return true;
  }

  const base::Time now = base::Time::Now();
  if (st->auto_refresh && st->extreme_evasion_enabled) {
    if (!st->long_pause_until.is_null() && now < st->long_pause_until) {
      const base::TimeDelta remaining = st->long_pause_until - now;
      st->info.status = "listening";
      st->info.status_detail = "Status: Sleeping";
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&ListenerManager::ArmListen,
                         weak_ptr_factory_.GetWeakPtr(), script_id, seq),
          remaining);
      return true;
    }

    if (st->next_long_pause_at.is_null()) {
      st->next_long_pause_at =
          now + RandomHoursBetween(kLongPauseStartMinHours,
                                   kLongPauseStartMaxHours);
    }
    if (now >= st->next_long_pause_at) {
      const base::TimeDelta pause =
          RandomHoursBetween(kLongPauseDurationMinHours,
                             kLongPauseDurationMaxHours);
      st->long_pause_until = now + pause;
      st->next_long_pause_at =
          st->long_pause_until +
          RandomHoursBetween(kLongPauseStartMinHours, kLongPauseStartMaxHours);
      st->info.status = "listening";
      st->info.status_detail = "Status: Sleeping";
      LogListenerEvent(script_id, *st, "long_pause",
                       "sleep=" + base::NumberToString(pause.InMinutes()) +
                           "m");
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&ListenerManager::ArmListen,
                         weak_ptr_factory_.GetWeakPtr(), script_id, seq),
          pause);
      return true;
    }
  }

  // Apply evasive jitter exactly once after each refresh cycle.
  if (st->auto_refresh && st->refresh_jitter_enabled &&
      st->jitter_due_after_refresh) {
    const int jitter_ms = base::RandInt(kHumanJitterMinMs, kHumanJitterMaxMs);
    st->jitter_due_after_refresh = false;
    st->info.status = "listening";
    st->info.status_detail =
        base::StringPrintf("Applying jitter (%dms)…", jitter_ms);
    PerformEvasiveInteraction(st->web_contents);
    LogListenerEvent(script_id, *st, "jitter",
                     base::StringPrintf("jitter=%dms", jitter_ms));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ListenerManager::ArmListen,
                       weak_ptr_factory_.GetWeakPtr(), script_id, seq),
        base::Milliseconds(jitter_ms));
    return true;
  }

  return false;
}

void ListenerManager::ArmListen(const std::string& script_id, uint64_t seq) {
  if (MaybeDelayForCadence(script_id, seq))
    return;
  ArmListenNow(script_id, seq);
}

void ListenerManager::ArmListenNow(const std::string& script_id, uint64_t seq) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq || st->stop_requested)
    return;
  if (!st->web_contents) {
    LOG(ERROR) << "[ListenerManager] no WebContents for listener=" << script_id;
    st->info.status = "error";
    st->info.status_detail = "Listener tab was closed.";
    st->info.last_error = st->info.status_detail;
    return;
  }

  if (st->auto_refresh && st->awaiting_post_refresh_snapshot) {
    CollectSnapshotKeys(
        st->web_contents, st->listen_params,
        base::BindOnce(&ListenerManager::OnPostRefreshSnapshotCollected,
                       weak_ptr_factory_.GetWeakPtr(), script_id, seq));
    return;
  }

  ArmListenNowImpl(script_id, seq);
}

void ListenerManager::ArmListenNowImpl(const std::string& script_id,
                                       uint64_t seq) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq || st->stop_requested)
    return;
  if (!st->web_contents) {
    LOG(ERROR) << "[ListenerManager] no WebContents for listener=" << script_id;
    st->info.status = "error";
    st->info.status_detail = "Listener tab was closed.";
    st->info.last_error = st->info.status_detail;
    return;
  }

  st->info.status = "listening";
  st->info.status_detail = "Listening…";
  st->info.running_kind.clear();
  st->info.execution_id.clear();
  LOG(WARNING) << "[ListenerManager] ArmListenNow script=" << script_id
               << " seq=" << seq
               << " url="
               << (st->web_contents &&
                           st->web_contents->GetLastCommittedURL().is_valid()
                       ? st->web_contents->GetLastCommittedURL().spec()
                       : std::string("<invalid>"))
               << " condition="
               << (st->listen_params.FindString("condition")
                       ? *st->listen_params.FindString("condition")
                       : std::string("visible"));
  LOG(INFO) << "[ListenerManager] Arm listen: script=" << script_id
            << " condition="
            << (st->listen_params.FindString("condition")
                    ? *st->listen_params.FindString("condition")
                    : std::string("visible"))
            << " timeout_ms="
            << st->listen_params.FindInt("timeout_ms").value_or(60000)
            << " url="
            << (st->web_contents && st->web_contents->GetLastCommittedURL().is_valid()
                    ? st->web_contents->GetLastCommittedURL().spec()
                    : std::string("<invalid>"))
            << " locators="
            << ListenLocatorsSummary(st->listen_params, 1200);
  if (dashboard::IsVerboseLoggingEnabled()) {
    base::Value::Dict diag;
    diag.Set("script_id", script_id);
    diag.Set("seq", static_cast<double>(seq));
    diag.Set("current_url",
             st->web_contents && st->web_contents->GetLastCommittedURL().is_valid()
                 ? st->web_contents->GetLastCommittedURL().spec()
                 : std::string("<invalid>"));
    diag.Set("listen_params", st->listen_params.Clone());
    diag.Set("on_match_script_id", st->on_match_script_id);
    diag.Set("key_script_id", st->key_script_id);
    diag.Set("auto_refresh", st->auto_refresh);
    diag.Set("refresh_interval_ms",
             static_cast<double>(st->refresh_interval.InMilliseconds()));
    LOG(INFO) << "[ListenerManager] Arm listen verbose: "
              << ValueToCompactLog(base::Value(diag.Clone()), 2200);
  }

  dashboard::actions::ActionFactory factory(profile_);
  factory.SetForceCdpExecution(st->force_cdp_execution);
  base::Value::Dict action_data;
  action_data.Set("action", "listen");
  // Persistent listeners own onMatch + rearm. Strip onMatch so ListenAction
  // does not also one-shot-dispatch the trigger when the condition matches.
  base::Value::Dict listen_params_for_action = st->listen_params.Clone();
  listen_params_for_action.Remove("onMatchScriptId");
  action_data.Set("params", std::move(listen_params_for_action));
  st->listen_action = factory.CreateAction(action_data);
  if (!st->listen_action) {
    LOG(ERROR) << "[ListenerManager] failed to create routed listen action for "
               << "listener=" << script_id;
    st->info.status = "error";
    st->info.status_detail = "Failed to configure listen action.";
    st->info.last_error = st->info.status_detail;
    StopListener(script_id, /*error=*/nullptr);
    return;
  }

  st->listen_action->Execute(
      st->web_contents,
      base::BindOnce(&ListenerManager::OnListenCompleted,
                     weak_ptr_factory_.GetWeakPtr(), script_id, seq));
  LOG(WARNING) << "[ListenerManager] ListenAction dispatched script="
               << script_id << " seq=" << seq;
  LOG(INFO) << "[ListenerManager] ListenAction dispatched: script=" << script_id
            << " seq=" << seq;
}

void ListenerManager::OnPostRefreshSnapshotCollected(
    const std::string& script_id,
    uint64_t seq,
    std::vector<std::string> post_keys) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq || st->stop_requested)
    return;

  const bool expected_post_compare = st->awaiting_post_refresh_snapshot;
  st->awaiting_post_refresh_snapshot = false;
  if (!expected_post_compare) {
    ArmListenNowImpl(script_id, seq);
    return;
  }

  const bool had_snapshot_data =
      !st->pre_refresh_snapshot_keys.empty() || !post_keys.empty();
  bool has_new_matching_elements = false;
  if (had_snapshot_data) {
    base::flat_set<std::string> pre;
    for (const auto& k : st->pre_refresh_snapshot_keys)
      pre.insert(k);
    for (const auto& k : post_keys) {
      if (!pre.contains(k)) {
        has_new_matching_elements = true;
        break;
      }
    }
  }
  st->pre_refresh_snapshot_keys = post_keys;

  if (had_snapshot_data && !has_new_matching_elements) {
    st->info.status = "listening";
    st->info.status_detail = "No new matching elements; waiting for refresh…";
    ScheduleRefreshAndRearm(script_id, seq, st->refresh_interval);
    return;
  }

  ArmListenNowImpl(script_id, seq);
}

void ListenerManager::OnListenCompleted(const std::string& script_id,
                                        uint64_t seq,
                                        bool success,
                                        const std::string& message) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq)
    return;

  base::Value::Dict promoted_listen_action;
  if (st->listen_action) {
    st->listen_action->SerializeInto(&promoted_listen_action);
  }
  st->listen_action.reset();
  if (st->stop_requested)
    return;

  if (success) {
    if (const base::Value::Dict* promoted_params =
            promoted_listen_action.FindDict("params")) {
      if (*promoted_params != st->listen_params) {
        const base::Value::Dict prior_listen_params = st->listen_params.Clone();
        st->listen_params = promoted_params->Clone();

        base::Value::Dict script = dashboard_->LoadScript(script_id);
        if (!script.empty()) {
          if (base::Value::List* actions = script.FindList("actions")) {
            std::string persistence_reason;
            const bool should_persist = dashboard::ShouldPersistPromotedActionParams(
                prior_listen_params, *promoted_params, &persistence_reason);
            if (!should_persist) {
              LOG(INFO) << "[ListenerManager] Skipping persisted listen param "
                           "promotion for "
                        << script_id << ": " << persistence_reason;
            }
            if (should_persist) {
              bool updated = false;
              for (auto& item : *actions) {
                if (base::Value::Dict* action = item.GetIfDict();
                    action &&
                    ReplaceFirstListenActionRecursive(action,
                                                     promoted_listen_action)) {
                  updated = true;
                  break;
                }
              }
              if (updated) {
                script = dashboard::ScriptSchema::CanonicalizeForSave(
                    std::move(script), script_id, base::Time::Now());
                scoped_refptr<base::SequencedTaskRunner> ui_task_runner =
                    base::SequencedTaskRunner::GetCurrentDefault();
                base::ThreadPool::PostTask(
                    FROM_HERE, {base::MayBlock()},
                    base::BindOnce(
                        [](Profile* profile, std::string script_id,
                           base::Value::Dict script,
                           scoped_refptr<base::SequencedTaskRunner>
                               ui_task_runner) {
                          if (dashboard::ScriptStore::Write(profile, script_id,
                                                            script) &&
                              ui_task_runner) {
                            ui_task_runner->PostTask(
                                FROM_HERE,
                                base::BindOnce(&DashboardUIMessageHandler::
                                                   UpdateRuntimeScriptCache,
                                               profile, script_id,
                                               script.Clone()));
                          }
                        },
                        profile_, script_id, std::move(script),
                        std::move(ui_task_runner)));
                LOG(INFO)
                    << "[ListenerManager] Persisted honed listen params for "
                    << script_id;
              }
            }
          }
        }
      }
    }
  }

  if (!success) {
    // Timeout or transient failure: keep monitoring.
    LOG(INFO) << "[ListenerManager] listen not satisfied: script=" << script_id
              << " message=" << message;
    if (dashboard::IsVerboseLoggingEnabled()) {
      LOG(INFO) << "[ListenerManager] listen not satisfied verbose: script="
                << script_id << " auto_refresh=" << (st->auto_refresh ? 1 : 0)
                << " will_rearm=1 detail="
                << TruncateForLog(message, 1800);
    }
    st->info.status = "listening";
    if (!message.empty())
      st->info.status_detail = message;
    if (!message.empty())
      st->info.last_error = message;
    if (st->auto_refresh) {
      const base::Time now = base::Time::Now();
      if (!st->next_refresh_at.is_null() && now < st->next_refresh_at) {
        ScheduleRefreshAndRearm(script_id, seq, st->next_refresh_at - now);
      } else {
        // Only refresh when the configured cadence is actually due.
        LogListenerEvent(script_id, *st, "refresh",
                         "listen-timeout-refresh");
        if (st->refresh_interval.is_positive())
          st->next_refresh_at = now + st->refresh_interval;
        RefreshPageAndRearm(script_id, seq);
      }
    } else {
      const base::TimeDelta sample_interval =
          ComputeListenerSampleInterval(st->listen_params);
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&ListenerManager::ArmListen,
                         weak_ptr_factory_.GetWeakPtr(), script_id, seq),
          sample_interval);
    }
    return;
  }

  const std::vector<std::string> match_signatures = BuildMatchSignatures(message);
  std::string selected_match_signature;
  if (kEnableInSessionMatchDedupe) {
    for (const auto& candidate : match_signatures) {
      if (std::find(st->recent_match_signatures.begin(),
                    st->recent_match_signatures.end(),
                    candidate) == st->recent_match_signatures.end()) {
        selected_match_signature = candidate;
        break;
      }
    }
    if (selected_match_signature.empty() && !match_signatures.empty()) {
      LOG(INFO) << "[ListenerManager] duplicate match skipped for listener="
                << script_id << " signatures="
                << base::JoinString(match_signatures, ",");
      st->info.status = "listening";
      st->info.status_detail = "Duplicate match skipped.";
      if (st->auto_refresh) {
        ScheduleRefreshAndRearm(script_id, seq, st->refresh_interval);
      } else {
        ArmListen(script_id, seq);
      }
      return;
    }
  } else if (!match_signatures.empty()) {
    selected_match_signature = match_signatures.front();
  }
  LOG(INFO) << "[ListenerManager] listen matched: script=" << script_id
            << " trigger_script=" << st->on_match_script_id
            << " detail=" << message;
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[ListenerManager] listen matched verbose: script=" << script_id
              << " trigger_script=" << st->on_match_script_id
              << " auto_refresh=" << (st->auto_refresh ? 1 : 0)
              << " detail=" << TruncateForLog(message, 1800);
  }

  // Condition satisfied: run trigger script, but throttle by the listener's
  // own execution frequency setting.
  const std::string trigger_id = st->on_match_script_id;
  if (trigger_id.empty()) {
    if (st->auto_refresh) {
      ScheduleRefreshAndRearm(script_id, seq, st->refresh_interval);
    } else {
      ArmListen(script_id, seq);
    }
    return;
  }

  const base::Time now = base::Time::Now();
  const base::TimeDelta throttle = ComputeTriggerThrottle(st->listen_params);
  if (!st->last_triggered.is_null()) {
    const base::TimeDelta elapsed = now - st->last_triggered;
    if (elapsed < throttle) {
      const base::TimeDelta remaining = throttle - elapsed;
      st->info.status = "listening";
      st->info.status_detail = "Listener throttle active…";
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&ListenerManager::ArmListen,
                         weak_ptr_factory_.GetWeakPtr(), script_id, seq),
          remaining);
      return;
    }
  }

  while (!st->recent_trigger_times.empty() &&
         now - st->recent_trigger_times.front() > kTriggerStormWindow) {
    st->recent_trigger_times.pop_front();
  }
  if (st->recent_trigger_times.size() >= kTriggerStormLimit) {
    st->info.status = "error";
    st->info.status_detail =
        "Listener disabled after repeated rapid triggers.";
    st->info.last_error = st->info.status_detail;
    LogListenerEvent(script_id, *st, "error", "trigger_storm_circuit_breaker");
    LOG(ERROR) << "[ListenerManager] Trigger storm circuit breaker tripped for "
               << script_id << " count=" << st->recent_trigger_times.size()
               << " window_sec=" << kTriggerStormWindow.InSeconds();
    return;
  }
  st->recent_trigger_times.push_back(now);

  base::Value::Dict trigger_script;
  std::string resolve_error;
  if (!ResolveOnMatchTriggerScript(dashboard_, trigger_id, &trigger_script,
                                   &resolve_error)) {
    LOG(ERROR) << "[ListenerManager] trigger target missing id=" << trigger_id
               << " error=" << resolve_error;
    st->info.status = "error";
    st->info.status_detail = resolve_error.empty() ? "Trigger script missing."
                                                   : resolve_error;
    st->info.last_error = st->info.status_detail;
    if (st->auto_refresh) {
      ScheduleRefreshAndRearm(script_id, seq, st->refresh_interval);
    } else {
      const base::TimeDelta sample_interval =
          ComputeListenerSampleInterval(st->listen_params);
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&ListenerManager::ArmListen,
                         weak_ptr_factory_.GetWeakPtr(), script_id, seq),
          sample_interval);
    }
    return;
  }

  base::Value::List flat = FlattenTimeline(trigger_script);
  base::flat_set<std::string> visiting;
  base::Value::List expanded = ExpandCallScripts(dashboard_, flat, &visiting);
  const std::string trigger_initial_url = ReadInitialUrlFromScript(trigger_script);
  std::optional<GURL> trigger_start_url =
      NormalizeListenerStartUrl(trigger_initial_url);
  base::Value::List trigger_key_timeline;
  const std::string trigger_key_script_id = ReadKeyScriptId(trigger_script);
  if (!trigger_key_script_id.empty()) {
    base::Value::Dict trigger_key_script =
        dashboard_->LoadScript(trigger_key_script_id);
    if (!trigger_key_script.empty()) {
      base::Value::List key_flat = FlattenTimeline(trigger_key_script);
      base::flat_set<std::string> key_visiting;
      trigger_key_timeline =
          ExpandCallScripts(dashboard_, key_flat, &key_visiting);
    }
  }
  const base::flat_map<std::string, std::string> listener_match_tokens =
      BuildListenerMatchTokenMap(script_id, st->listen_params, message);

  // Isolated headless agent: push trigger to headed browser; do not run
  // onMatch in this process.
  if (remote_trigger_dispatch_) {
    std::string preferred_start_url;
    auto token_it = listener_match_tokens.find("listener.match.url");
    if (token_it != listener_match_tokens.end() && !token_it->second.empty()) {
      preferred_start_url = token_it->second;
    }
    st->last_triggered = now;
    st->info.status = "listening";
    st->info.status_detail = "Trigger dispatched to headed browser.";
    st->info.running_kind.clear();
    st->info.execution_id.clear();
    LogListenerEvent(script_id, *st, "trigger",
                     preferred_start_url.empty() ? message
                                                 : preferred_start_url);
    remote_trigger_dispatch_.Run(script_id, trigger_id, listener_match_tokens,
                                 preferred_start_url, message);
    if (!selected_match_signature.empty()) {
      RememberRecentMatchSignature(&st->recent_match_signatures,
                                   selected_match_signature);
    }
    if (st->auto_refresh) {
      ScheduleRefreshAndRearm(script_id, seq, st->refresh_interval);
    } else {
      ArmListen(script_id, seq);
    }
    return;
  }

  base::Value::List run_timeline = expanded.Clone();
  base::Value::List run_key_timeline = trigger_key_timeline.Clone();
  if (!listener_match_tokens.empty()) {
    for (auto& step : run_timeline)
      ExpandTokensInValue(&step, listener_match_tokens);
    for (auto& step : run_key_timeline)
      ExpandTokensInValue(&step, listener_match_tokens);
  }

  auto* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (!exec_service) {
    LOG(ERROR) << "[ListenerManager] no ScriptExecutionService for trigger";
    st->info.status = "error";
    st->info.status_detail = "No ScriptExecutionService.";
    st->info.last_error = st->info.status_detail;
    ArmListen(script_id, seq);
    return;
  }

  auto ensure_trigger_web_contents =
      [&](ActiveListener* listener, std::optional<GURL> preferred_start_url)
      -> content::WebContents* {
    if (!listener)
      return nullptr;
    if (listener->trigger_web_contents)
      return listener->trigger_web_contents;

    Browser* trigger_browser = listener->browser;
    if (!trigger_browser)
      trigger_browser = chrome::FindLastActiveWithProfile(profile_);
    if (!trigger_browser)
      return nullptr;

    const GURL initial_url = preferred_start_url.value_or(GURL("about:blank"));
    NavigateParams trigger_np(trigger_browser, initial_url,
                              ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    trigger_np.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
    Navigate(&trigger_np);
    content::WebContents* trigger_wc = trigger_np.navigated_or_inserted_contents;
    if (!trigger_wc)
      return nullptr;

    listener->browser = trigger_browser;
    listener->trigger_web_contents = trigger_wc;
    listener->trigger_tab_observer.Watch(trigger_wc);
    return trigger_wc;
  };

  content::WebContents* trigger_wc =
      ensure_trigger_web_contents(st, trigger_start_url);
  if (!trigger_wc) {
    LOG(ERROR) << "[ListenerManager] failed to create/reuse trigger tab for listener="
               << script_id;
    st->info.status = "error";
    st->info.status_detail = "Failed to create trigger background tab.";
    st->info.last_error = st->info.status_detail;
    LogListenerEvent(script_id, *st, "error", st->info.last_error);
    if (st->auto_refresh) {
      ScheduleRefreshAndRearm(script_id, seq, st->refresh_interval);
    } else {
      ArmListen(script_id, seq);
    }
    return;
  }

  st->last_triggered = now;
  st->info.status = "trigger";
  st->info.status_detail = "Running trigger script…";
  st->info.running_kind = "trigger";

  dashboard::ScriptExecutionService::RunOptions run_options;
  run_options.force_cdp_execution = st->force_cdp_execution;
  st->current_execution_id = exec_service->StartScriptRunWithOptions(
      trigger_id, run_timeline, trigger_wc,
      run_key_timeline.empty() ? nullptr : &run_key_timeline, run_options,
      base::BindOnce(&ListenerManager::OnTriggerScriptCompleted,
                     weak_ptr_factory_.GetWeakPtr(), script_id, seq));
  st->info.execution_id = st->current_execution_id;
  if (st->current_execution_id.empty()) {
    st->info.status = "error";
    st->info.status_detail = "Failed to start trigger script.";
    st->info.last_error = st->info.status_detail;
    st->info.running_kind.clear();
    LogListenerEvent(script_id, *st, "error", st->info.last_error);
    if (st->auto_refresh) {
      ScheduleRefreshAndRearm(script_id, seq, st->refresh_interval);
    } else {
      ArmListen(script_id, seq);
    }
    return;
  }
  if (kEnableInSessionMatchDedupe && !selected_match_signature.empty()) {
    RememberRecentMatchSignature(&st->recent_match_signatures,
                                 selected_match_signature);
    if (kEnableCrossRunMatchDedupe) {
      PersistRecentMatchSignaturesAsync(profile_, dashboard_, script_id,
                                        st->recent_match_signatures);
    }
  }

  // Persist a marker event for the Debugging timeline.
  {
    base::Value::Dict ev;
    ev.Set("ts_ms", static_cast<double>(now.InMillisecondsSinceUnixEpoch()));
    ev.Set("action", "trigger");
    ev.Set("listener_script_id", script_id);
    ev.Set("listener_script_name", st->info.script_name);
    ev.Set("trigger_script_id", trigger_id);
    ev.Set("trigger_script_name", GetScriptName(trigger_id));
    ev.Set("run_id", st->current_execution_id);
    const std::string* cond = st->listen_params.FindString("condition");
    if (cond)
      ev.Set("condition", *cond);
    if (!listener_match_tokens.empty()) {
      auto token_it = listener_match_tokens.find("listener.match");
      if (token_it != listener_match_tokens.end() && !token_it->second.empty())
        ev.Set("match_value", token_it->second);
      token_it = listener_match_tokens.find("listener.match.url");
      if (token_it != listener_match_tokens.end() && !token_it->second.empty())
        ev.Set("match_url", token_it->second);
      token_it = listener_match_tokens.find("listener.match.text");
      if (token_it != listener_match_tokens.end() && !token_it->second.empty())
        ev.Set("match_text", token_it->second);
      token_it = listener_match_tokens.find("listener.match.attr");
      if (token_it != listener_match_tokens.end() && !token_it->second.empty())
        ev.Set("match_attr", token_it->second);
    }
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce(
            [](Profile* profile, base::Value::Dict ev) {
              (void)dashboard::RunHistoryStore::AppendListenerEvent(profile, ev);
            },
            base::Unretained(profile_), ev.Clone()));
  }
}

void ListenerManager::ScheduleRefreshAndRearm(const std::string& script_id,
                                              uint64_t seq,
                                              base::TimeDelta delay) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq || st->stop_requested)
    return;
  const base::TimeDelta wait =
      delay.is_positive() ? delay : kDefaultAutoRefreshInterval;
  if (st->auto_refresh)
    st->next_refresh_at = base::Time::Now() + wait;
  st->info.status = "listening";
  st->info.status_detail = "Waiting for refresh…";
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ListenerManager::RefreshPageAndRearm,
                     weak_ptr_factory_.GetWeakPtr(), script_id, seq),
      wait);
}

void ListenerManager::RefreshPageAndRearm(const std::string& script_id,
                                          uint64_t seq) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq || st->stop_requested)
    return;
  if (!st->web_contents) {
    st->info.status = "error";
    st->info.status_detail = "Listener tab was closed.";
    st->info.last_error = st->info.status_detail;
    return;
  }

  CollectSnapshotKeys(
      st->web_contents, st->listen_params,
      base::BindOnce(&ListenerManager::OnPreRefreshSnapshotCollected,
                     weak_ptr_factory_.GetWeakPtr(), script_id, seq));
}

void ListenerManager::OnPreRefreshSnapshotCollected(
    const std::string& script_id,
    uint64_t seq,
    std::vector<std::string> pre_keys) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq || st->stop_requested)
    return;
  if (!st->web_contents) {
    st->info.status = "error";
    st->info.status_detail = "Listener tab was closed.";
    st->info.last_error = st->info.status_detail;
    return;
  }

  st->pre_refresh_snapshot_keys = std::move(pre_keys);
  st->awaiting_post_refresh_snapshot = true;
  st->jitter_due_after_refresh = st->refresh_jitter_enabled;
  if (st->auto_refresh && st->refresh_interval.is_positive())
    st->next_refresh_at = base::Time::Now() + st->refresh_interval;
  st->info.status = "listening";
  st->info.status_detail = "Refreshing listener page…";
  LogListenerEvent(script_id, *st, "refresh", "reload");
  st->web_contents->GetController().Reload(content::ReloadType::NORMAL, false);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ListenerManager::ArmListen,
                     weak_ptr_factory_.GetWeakPtr(), script_id, seq),
      kPostRefreshSettleDelay);
}

void ListenerManager::CollectSnapshotKeys(
    content::WebContents* web_contents,
    const base::Value::Dict& listen_params,
    base::OnceCallback<void(std::vector<std::string>)> callback) {
  if (!web_contents) {
    std::move(callback).Run({});
    return;
  }
  const base::Value::List* locators = listen_params.FindList("locators");
  if (!locators || locators->empty()) {
    std::move(callback).Run({});
    return;
  }

  const std::vector<Locator> locator_bundle = ConvertLocators(*locators);
  FetchUnifiedPageMapSnapshot(
      web_contents,
      base::BindOnce(
          [](base::Value::Dict listen_params,
             std::vector<Locator> locator_bundle,
             base::OnceCallback<void(std::vector<std::string>)> callback,
             dashboard::UnifiedPageMapResult result) {
            std::vector<std::string> keys;
            if (!result.ok) {
              std::move(callback).Run(std::move(keys));
              return;
            }
            const std::vector<dashboard::NativePageMapCandidate> matches =
                dashboard::FindMatchingPageMapCandidates(result.page_map,
                                                         locator_bundle);
            base::flat_set<std::string> dedupe;
            for (const auto& candidate : matches) {
              if (!CandidateMatchesSnapshotCondition(candidate, listen_params))
                continue;
              const std::string key = SnapshotKeyForCandidate(candidate);
              if (key.empty() || dedupe.contains(key))
                continue;
              dedupe.insert(key);
              keys.push_back(key);
              if (keys.size() >= 512u)
                break;
            }
            std::move(callback).Run(std::move(keys));
          },
          listen_params.Clone(), locator_bundle, std::move(callback)),
      dashboard::mojom::SearchScope::kAll);
  return;

}

void ListenerManager::OnTriggerScriptCompleted(const std::string& script_id,
                                               uint64_t seq,
                                               bool success,
                                               int /*failed_step*/,
                                               std::string message) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  ActiveListener* st = it->second.get();
  if (st->seq != seq)
    return;
  const std::string completed_execution_id = st->current_execution_id;
  st->current_execution_id.clear();
  if (st->stop_requested)
    return;
  if (!success) {
    LOG(WARNING) << "[ListenerManager] triggered script failed for listener="
                 << script_id << " message=" << message;
    st->info.last_error = message.empty() ? "Triggered script failed" : message;
    st->info.status_detail = st->info.last_error;
    LogListenerEvent(script_id, *st, "error", st->info.last_error,
                     completed_execution_id);
  }
  st->info.execution_id.clear();
  st->info.running_kind.clear();
  if (st->auto_refresh) {
    ScheduleRefreshAndRearm(script_id, seq, st->refresh_interval);
  } else {
    ArmListen(script_id, seq);
  }
}

void ListenerManager::OnListenerTabDestroyed(const std::string& script_id,
                                             uint64_t seq) {
  auto it = active_listeners_.find(script_id);
  if (it == active_listeners_.end() || !it->second)
    return;
  if (it->second->seq != seq)
    return;
  std::string ignored;
  StopListener(script_id, &ignored);
}

void ListenerManager::PerformEvasiveInteraction(content::WebContents* web_contents) {
  if (!web_contents)
    return;
  VLOG(1) << "[ListenerManager] Evasive interaction disabled in native-only mode";
  return;

}

void ListenerManager::LogListenerEvent(const std::string& script_id,
                                       const ActiveListener& listener,
                                       const std::string& action,
                                       const std::string& detail,
                                       const std::string& execution_id) {
  base::Value::Dict ev;
  const base::Time now = base::Time::Now();
  ev.Set("ts_ms", static_cast<double>(now.InMillisecondsSinceUnixEpoch()));
  ev.Set("listener_script_id", script_id);
  ev.Set("listener_script_name", listener.info.script_name);
  ev.Set("action", action);
  if (!execution_id.empty())
    ev.Set("run_id", execution_id);
  if (!detail.empty())
    ev.Set("detail", detail);
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, base::Value::Dict ev) {
            (void)dashboard::RunHistoryStore::AppendListenerEvent(profile, ev);
          },
          base::Unretained(profile_), std::move(ev)));
  DashboardUIMessageHandler::NotifyListenersChanged();
}

}  // namespace dashboard

// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/recorder_controller.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/json/json_writer.h"
#include "base/json/json_reader.h"
#include "base/json/string_escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/containers/span.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "base/types/token_type.h"

#include "chrome/common/chrome_features.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sessions/content/session_tab_helper.h"
#include "chrome/browser/ui/webui/dashboard/recorder_devtools_client.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "chrome/browser/ui/webui/dashboard/page_map_js.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "chrome/browser/ui/webui/dashboard/recording_suppression.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/page_content_annotations/multi_source_page_context_fetcher.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "components/optimization_guide/content/browser/page_content_proto_provider.h"
#include "components/optimization_guide/content/browser/page_content_proto_util.h"
#include "components/optimization_guide/proto/features/common_quality_data.pb.h"
#include "ui/gfx/geometry/rect_f.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "ui/base/page_transition_types.h"
#include "ui/accessibility/ax_enums.mojom-forward.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/gfx/geometry/rect.h"
#include "third_party/skia/include/core/SkColor.h"
#include "url/gurl.h"
#include "content/public/browser/render_widget_host_view.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/mojom/content_extraction/ai_page_content.mojom.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents_delegate.h"
#include "chrome/browser/ui/webui/dashboard/recorder_highlight_overlay.h"
#include "components/input/input_event_source.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"



namespace {
constexpr base::TimeDelta kTypeBufferIdleFlushDelay = base::Milliseconds(1500);
constexpr base::TimeDelta kHoverProbeDebounceDelay = base::Milliseconds(200);
constexpr base::TimeDelta kDuplicateClickSuppressWindow =
    base::Milliseconds(2000);
constexpr base::TimeDelta kSameSourceSyntheticDuplicateSuppressWindow =
    base::Milliseconds(150);
constexpr int kDuplicateClickMaxManhattanDistancePx = 24;
constexpr int kHoverProbeMinMovePx = 12;
constexpr base::TimeDelta kHoverProbeMinInterval = base::Milliseconds(250);
constexpr base::TimeDelta kLoopPreviewDebounceDelay = base::Milliseconds(120);
constexpr base::TimeDelta kGuidedLoopPreviewIdleDelay = base::Milliseconds(500);
constexpr int kLoopPreviewMinMovePx = 8;
constexpr base::TimeDelta kHoverAfterClickSuppressWindow =
    base::Milliseconds(900);
constexpr base::TimeDelta kPostLocatorCaptureDuplicateSuppressWindow =
    base::Milliseconds(900);
constexpr int kPostLocatorCaptureSuppressDistancePx = 8;
constexpr int kHoverAfterClickSuppressDistancePx = 28;
constexpr int kDragGestureThresholdPx = 18;
constexpr char kEnableDashboardAutoPageMapCaptureSwitch[] =
    "enable-dashboard-auto-pagemap-capture";
constexpr char kDisableDashboardAutoPageMapCaptureSwitch[] =
    "disable-dashboard-auto-pagemap-capture";
constexpr base::TimeDelta kRecentPageMapSnapshotReuseWindow =
    base::Milliseconds(1200);

bool MaybeBool(const base::Value::Dict& dict, const std::string& key) {
  return dict.FindBool(key).value_or(false);
}

std::optional<int> ReadIntLike(const base::Value::Dict& dict,
                               const std::string& key) {
  if (auto v = dict.FindInt(key))
    return *v;
  if (auto d = dict.FindDouble(key))
    return static_cast<int>(std::lround(*d));
  return std::nullopt;
}

bool NearPointManhattan(const gfx::Point& a, const gfx::Point& b, int max_dist) {
  const int dx = std::abs(a.x() - b.x());
  const int dy = std::abs(a.y() - b.y());
  return (dx + dy) <= max_dist;
}

std::string NormalizeAxRoleString(std::string role) {
  base::TrimWhitespaceASCII(role, base::TRIM_ALL, &role);
  if (role.empty()) {
    return role;
  }
  role = base::ToLowerASCII(role);
  std::replace(role.begin(), role.end(), ' ', '_');
  return role;
}

bool IsInteractiveAxRole(std::string_view role) {
  return role == "button" || role == "link" || role == "checkbox" ||
         role == "radio_button" || role == "radio" || role == "switch" ||
         role == "menu_item" || role == "menuitem" || role == "tab" ||
         role == "tree_item" || role == "treeitem" || role == "textbox" ||
         role == "text_field" || role == "textfield" || role == "combobox";
}

std::string SummarizeLocatorTypesForLog(const base::Value::List* locators,
                                        size_t max_items = 8) {
  if (!locators || locators->empty()) {
    return "<none>";
  }
  std::string out;
  size_t shown = 0;
  for (const auto& value : *locators) {
    const base::Value::Dict* loc = value.GetIfDict();
    if (!loc) {
      continue;
    }
    const std::string type =
        loc->FindString("type") ? *loc->FindString("type") : std::string("?");
    const std::string raw_value =
        loc->FindString("value") ? *loc->FindString("value") : std::string();
    if (!out.empty()) {
      out += ", ";
    }
    out += type;
    if (!raw_value.empty()) {
      out += "=";
      out += base::CollapseWhitespaceASCII(raw_value, true).substr(0, 60);
    }
    ++shown;
    if (shown >= max_items) {
      if (locators->size() > shown) {
        out += ", ...";
      }
      break;
    }
  }
  return out.empty() ? std::string("<none>") : out;
}

bool IsSyntheticClickSource(std::string_view source) {
  return source == "fallback" || source == "renderer_resolver" ||
         source == "devtools_probe" || source == "devtools_attributes" ||
         source == "renderer_agent_mojo" || source == "capture_xy_fallback";
}

bool IsGuidedReturnUrlMatch(const std::string& observed,
                            const std::string& origin) {
  if (observed.empty() || origin.empty()) {
    return false;
  }
  const GURL observed_url(observed);
  const GURL origin_url(origin);
  if (!observed_url.is_valid() || !origin_url.is_valid()) {
    return observed == origin;
  }
  return observed_url.GetWithoutRef() == origin_url.GetWithoutRef();
}

bool IsExcludedLocatorCaptureUrl(const GURL& url) {
  if (!url.is_valid() || url.is_empty()) {
    return true;
  }
  if (url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile()) {
    return false;
  }
  if (!url.SchemeIs("chrome")) {
    return true;
  }
  const std::string host = base::ToLowerASCII(url.host());
  return host == "dashboard" || host == "glic";
}

bool IsCapturableWebContents(content::WebContents* web_contents,
                             content::WebContents* avoid_contents) {
  if (!web_contents || web_contents == avoid_contents) {
    return false;
  }
  GURL url = web_contents->GetVisibleURL();
  if (url.is_empty()) {
    url = web_contents->GetLastCommittedURL();
  }
  return !IsExcludedLocatorCaptureUrl(url);
}

content::WebContents* FindWebContentsBySessionTabId(TabStripModel* tab_strip_model,
                                                    int tab_id) {
  if (!tab_strip_model || tab_id <= 0) {
    return nullptr;
  }
  for (int i = 0; i < tab_strip_model->count(); ++i) {
    content::WebContents* candidate = tab_strip_model->GetWebContentsAt(i);
    if (!candidate) {
      continue;
    }
    if (sessions::SessionTabHelper::IdForTab(candidate).id() == tab_id) {
      return candidate;
    }
  }
  return nullptr;
}

bool ContextUrlsMatchExactlyOrByHost(const GURL& candidate_url,
                                     const GURL& preferred_url) {
  if (!candidate_url.is_valid() || !preferred_url.is_valid()) {
    return false;
  }
  if (!candidate_url.SchemeIsHTTPOrHTTPS() || !preferred_url.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  if (candidate_url.GetWithoutRef() == preferred_url.GetWithoutRef()) {
    return true;
  }
  return base::EqualsCaseInsensitiveASCII(candidate_url.host(), preferred_url.host());
}

content::WebContents* FindCapturableMatchingContext(
    TabStripModel* tab_strip_model,
    content::WebContents* avoid_contents,
    const GURL& preferred_context_url) {
  if (!tab_strip_model || !preferred_context_url.is_valid()) {
    return nullptr;
  }
  content::WebContents* host_match = nullptr;
  for (int i = 0; i < tab_strip_model->count(); ++i) {
    content::WebContents* candidate = tab_strip_model->GetWebContentsAt(i);
    if (!IsCapturableWebContents(candidate, avoid_contents)) {
      continue;
    }
    GURL candidate_url = candidate->GetVisibleURL();
    if (candidate_url.is_empty()) {
      candidate_url = candidate->GetLastCommittedURL();
    }
    if (!candidate_url.is_valid()) {
      continue;
    }
    if (candidate_url.GetWithoutRef() == preferred_context_url.GetWithoutRef()) {
      return candidate;
    }
    if (!host_match &&
        ContextUrlsMatchExactlyOrByHost(candidate_url, preferred_context_url)) {
      host_match = candidate;
    }
  }
  return host_match;
}

content::WebContents* FindCapturableInTabStrip(TabStripModel* tab_strip_model,
                                               content::WebContents* avoid) {
  if (!tab_strip_model || tab_strip_model->count() <= 0) {
    return nullptr;
  }
  const int active = tab_strip_model->active_index();
  if (active >= 0) {
    if (content::WebContents* active_contents =
            tab_strip_model->GetWebContentsAt(active);
        IsCapturableWebContents(active_contents, avoid)) {
      return active_contents;
    }
  }
  for (int distance = 1; distance < tab_strip_model->count(); ++distance) {
    const int left = active - distance;
    if (left >= 0) {
      if (content::WebContents* candidate =
              tab_strip_model->GetWebContentsAt(left);
          IsCapturableWebContents(candidate, avoid)) {
        return candidate;
      }
    }
    const int right = active + distance;
    if (right >= 0 && right < tab_strip_model->count()) {
      if (content::WebContents* candidate =
              tab_strip_model->GetWebContentsAt(right);
          IsCapturableWebContents(candidate, avoid)) {
        return candidate;
      }
    }
  }
  return nullptr;
}

content::WebContents* FindBestCapturableWebContents(
    BrowserView* preferred_browser_view,
    content::WebContents* avoid_contents,
    const std::string& preferred_context_url) {
  GURL preferred_context_gurl(preferred_context_url);
  if (preferred_browser_view && preferred_browser_view->browser()) {
    if (preferred_context_gurl.is_valid()) {
      if (content::WebContents* preferred_match =
              FindCapturableMatchingContext(
                  preferred_browser_view->browser()->tab_strip_model(),
                  avoid_contents, preferred_context_gurl)) {
        return preferred_match;
      }
    }
    if (content::WebContents* preferred =
            FindCapturableInTabStrip(
                preferred_browser_view->browser()->tab_strip_model(),
                avoid_contents)) {
      return preferred;
    }
  }

  Browser* preferred_browser =
      preferred_browser_view ? preferred_browser_view->browser() : nullptr;
  Browser* fallback_browser = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser_interface) {
        if (!browser_interface || browser_interface == preferred_browser) {
          return true;
        }
        TabStripModel* tab_strip_model = browser_interface->GetTabStripModel();
        if (preferred_context_gurl.is_valid()) {
          if (content::WebContents* candidate =
                  FindCapturableMatchingContext(tab_strip_model, avoid_contents,
                                                preferred_context_gurl)) {
            fallback_browser = chrome::FindBrowserWithTab(candidate);
            return false;
          }
        }
        if (content::WebContents* candidate =
                FindCapturableInTabStrip(tab_strip_model, avoid_contents)) {
          fallback_browser = chrome::FindBrowserWithTab(candidate);
          return false;
        }
        return true;
      });
  if (fallback_browser && fallback_browser->tab_strip_model()) {
    return FindCapturableInTabStrip(fallback_browser->tab_strip_model(),
                                    avoid_contents);
  }
  return nullptr;
}

void StampActionContextUrl(base::Value::Dict* action,
                           const std::string& current_url) {
  if (!action) {
    return;
  }
  std::string context = current_url;
  base::TrimWhitespaceASCII(context, base::TRIM_ALL, &context);
  if (context.empty()) {
    return;
  }
  const GURL url(context);
  if (url.is_valid()) {
    context = url.GetWithoutRef().spec();
  }
  if (context.empty()) {
    return;
  }
  action->Set("contextUrl", context);
  action->Remove("context_url");
}
// (moved active-pointer and methods out of anonymous namespace below)

bool ShouldAutoCapturePageMapSnapshot(const std::string& reason) {
  const base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch(kEnableDashboardAutoPageMapCaptureSwitch))
    return true;
  if (cmd->HasSwitch(kDisableDashboardAutoPageMapCaptureSwitch))
    return false;

  // Explicit user-driven capture requests should always be allowed.
  if (reason == "ui_capture" || reason == "locator_capture" ||
      reason == "agent_snapshot") {
    return true;
  }

  // In the common Glic workflow, local agent context comes from native APC
  // context capture. Keep auto-capture off unless explicitly requested, to
  // avoid unnecessary repeated extraction overhead.
  if (base::FeatureList::IsEnabled(features::kGlic))
    return false;

  // Otherwise, keep PageMap available (including explicit ui_capture).
  return true;
}

bool ShouldUseLegacyRendererAgentPipeline() {
  // Full legacy pipeline (hover probes, continuous renderer agent work) stays
  // off — that is the expensive Mojo census path.
  return false;
}

// Recording click/locator capture: one hit-test + single-node locator harvest
// via StealthyElementResolver::ResolveAtPoint. Cheap vs full PageMap walks.
bool ShouldUseRendererHitTestLocatorCapture() {
  return true;
}

base::Value::Dict BuildEmptyPageMapSnapshot(const std::string& url,
                                           const std::string& reason) {
  base::Value::Dict empty;
  empty.Set("url", url);
  empty.Set("timestamp",
            static_cast<double>(
                base::Time::Now().InMillisecondsSinceUnixEpoch()));
  empty.Set("frames", base::Value::List());
  base::Value::Dict meta;
  meta.Set("skipped", reason);
  meta.Set("source", "empty");
  empty.Set("metadata", std::move(meta));
  return empty;
}

std::string TrimForLocatorValue(std::string in, size_t max_len) {
  if (in.size() <= max_len)
    return in;
  in.resize(max_len);
  in.append("…");
  return in;
}

std::string NormalizeUtf8BestEffort(std::string value) {
  if (value.empty() || base::IsStringUTF8AllowingNoncharacters(value)) {
    return value;
  }
  return base::UTF16ToUTF8(base::UTF8ToUTF16(value));
}

void AppendUniqueLocator(base::Value::List& locators,
                         std::set<std::pair<std::string, std::string>>& seen,
                         const char* type,
                         std::string value) {
  const std::string type_str = type ? std::string(type) : std::string();
  value = NormalizeUtf8BestEffort(std::move(value));
  if (type_str.empty() || value.empty()) {
    return;
  }
  std::pair<std::string, std::string> key{type_str, std::move(value)};
  if (!seen.insert(key).second) {
    return;
  }
  base::Value::Dict d;
  d.Set("type", key.first);
  d.Set("value", key.second);
  locators.Append(std::move(d));
}

bool IsLinkLikeTag(const std::string& tag) {
  const std::string lowered = base::ToLowerASCII(tag);
  return lowered == "a";
}

bool IsBareUselessTagLocator(const std::string& type,
                             const std::string& lowered_value) {
  static constexpr const char* kBare[] = {
      "div", "span", "p", "li", "ul", "ol", "section", "main", "article",
      "header", "footer", "nav", "i", "b", "em", "strong", "body", "html"};
  if (type == "CSS_SELECTOR" || type == "TAG_NAME") {
    for (const char* bare : kBare) {
      if (lowered_value == bare) {
        return true;
      }
    }
    return false;
  }
  if (type == "XPATH") {
    for (const char* bare : kBare) {
      if (lowered_value == std::string("//") + bare ||
          lowered_value == std::string("/") + bare) {
        return true;
      }
    }
    return lowered_value == "/html/body" || lowered_value == "/html";
  }
  if (type == "SHADOW_DOM") {
    return lowered_value == "input" || lowered_value == "textarea" ||
           lowered_value == "select" || lowered_value == "button";
  }
  return false;
}

bool IsRootLikeRecordedLocator(const std::string& type,
                               const std::string& value) {
  const std::string lowered = base::ToLowerASCII(value);
  if (IsBareUselessTagLocator(type, lowered)) {
    return true;
  }
  if (type == "CSS_SELECTOR" || type == "TAG_NAME") {
    return lowered == "body" || lowered == "html";
  }
  if (type == "XPATH") {
    return lowered == "//body" || lowered == "//html" ||
           lowered == "/html/body" || lowered == "/html";
  }
  if (type == "JS_EVAL") {
    return lowered == "document.queryselector(\"html\")" ||
           lowered == "document.queryselector('html')" ||
           lowered == "document.queryselector(\"body\")" ||
           lowered == "document.queryselector('body')";
  }
  if (type == "RESOURCE_TREE") {
    return lowered.empty() || lowered == "/html[1]" ||
           lowered == "/html[1]/body[1]";
  }
  return false;
}

bool IsUnknownAxRoleValue(std::string_view value) {
  const std::string lowered = base::ToLowerASCII(std::string(value));
  return lowered.empty() || lowered == "ax_role_unknown" ||
         lowered == "unknown" || lowered == "generic_container";
}

bool IsRootLikeResourcePath(std::string_view value) {
  const std::string lowered = base::ToLowerASCII(std::string(value));
  return lowered.empty() || lowered == "/html[1]" ||
         lowered == "/html[1]/body[1]";
}

bool IsReservedLoopLocatorToken(std::string_view value) {
  const std::string lowered = base::ToLowerASCII(value);
  return lowered == "for_each" || lowered == "loop" || lowered == "count" ||
         lowered == "while_element_exists" || lowered == "recorded_repeat";
}

bool IsUsableForEachLocatorValue(const std::string& type,
                                 const std::string& value) {
  if (value.empty() || IsReservedLoopLocatorToken(value) ||
      IsRootLikeRecordedLocator(type, value)) {
    return false;
  }
  if (type == "CSS_SELECTOR") {
    const std::string lowered = base::ToLowerASCII(value);
    if (lowered == "*" || lowered == ":root") {
      return false;
    }
  }
  return true;
}

bool IsVisualCoordsLocator(const base::Value::Dict& locator) {
  const std::string type = locator.FindString("type")
      ? base::ToUpperASCII(*locator.FindString("type"))
      : std::string();
  return type == "VISUAL_COORDS";
}

bool HasNonVisualRecordedLocator(const base::Value::List& locators) {
  for (const auto& entry : locators) {
    const base::Value::Dict* dict = entry.GetIfDict();
    if (!dict || IsVisualCoordsLocator(*dict)) {
      continue;
    }
    const std::string* value = dict->FindString("value");
    if (value && !value->empty()) {
      return true;
    }
  }
  return false;
}

bool IsMeaningfulRecordedLocator(const base::Value::Dict& locator) {
  const std::string type = locator.FindString("type")
      ? base::ToUpperASCII(*locator.FindString("type"))
      : std::string();
  const std::string value =
      locator.FindString("value") ? *locator.FindString("value") : std::string();
  if (value.empty()) {
    return false;
  }
  if (type == "VISUAL_COORDS" || type == "FRAME_ID" || type == "SHADOW_DOM") {
    return false;
  }
  return !IsRootLikeRecordedLocator(type, value);
}

bool HasMeaningfulRecordedLocator(const base::Value::List& locators) {
  for (const auto& entry : locators) {
    const base::Value::Dict* dict = entry.GetIfDict();
    if (!dict) {
      continue;
    }
    if (IsMeaningfulRecordedLocator(*dict)) {
      return true;
    }
  }
  return false;
}

bool LocatorBundleContainsCss(const base::Value::List& locators,
                              std::string_view css_selector) {
  if (css_selector.empty()) {
    return false;
  }
  for (const auto& entry : locators) {
    const base::Value::Dict* dict = entry.GetIfDict();
    if (!dict) {
      continue;
    }
    const std::string type = dict->FindString("type")
                                 ? base::ToUpperASCII(*dict->FindString("type"))
                                 : std::string();
    const std::string value =
        dict->FindString("value") ? *dict->FindString("value") : std::string();
    if (type == "CSS_SELECTOR" && value == css_selector) {
      return true;
    }
  }
  return false;
}

base::Value::List SanitizeLoopTargetLocators(
    const base::Value::List& locators,
    std::string_view preferred_css_selector) {
  base::Value::List sanitized;
  std::set<std::pair<std::string, std::string>> seen;
  auto append_locator = [&](const std::string& type, const std::string& value) {
    if (!IsUsableForEachLocatorValue(type, value)) {
      return;
    }
    const std::string lowered = base::ToLowerASCII(value);
    if (lowered.find("__dash_") != std::string::npos) {
      return;
    }
    const auto dedupe_key = std::make_pair(type, value);
    if (!seen.insert(dedupe_key).second) {
      return;
    }
    base::Value::Dict locator;
    locator.Set("type", type);
    locator.Set("value", value);
    sanitized.Append(std::move(locator));
  };

  if (!preferred_css_selector.empty()) {
    append_locator("CSS_SELECTOR", std::string(preferred_css_selector));
  }

  const std::string preferred_lower =
      base::ToLowerASCII(preferred_css_selector);
  const bool preferred_is_table_row =
      preferred_lower.find("> tr") != std::string::npos ||
      preferred_lower.ends_with(" tr") || preferred_lower == "tr";

  for (const auto& value : locators) {
    const base::Value::Dict* locator = value.GetIfDict();
    const std::string* type = locator ? locator->FindString("type") : nullptr;
    const std::string* raw_value =
        locator ? locator->FindString("value") : nullptr;
    if (!type || !raw_value || raw_value->empty()) {
      continue;
    }
    const std::string upper_type = base::ToUpperASCII(*type);
    if (upper_type == "VISUAL_COORDS" || upper_type == "VISUAL_AI" ||
        upper_type == "JS_EVAL" || upper_type == "FRAME_ID" ||
        upper_type == "SHADOW_DOM" || upper_type == "TAG_NAME" ||
        upper_type == "CLASS_NAME") {
      continue;
    }
    // When the peer resolver already chose body rows, drop header-cell CSS
    // (tr > th.header) so playback does not iterate the wrong sibling group.
    if (preferred_is_table_row && upper_type == "CSS_SELECTOR") {
      const std::string lowered = base::ToLowerASCII(*raw_value);
      if (lowered.find("> th") != std::string::npos ||
          lowered.find(" th.") != std::string::npos) {
        continue;
      }
    }
    append_locator(upper_type, *raw_value);
  }

  return sanitized;
}

base::Value::List MaybeReuseFreshLoopTargetLocators(const std::string& current_url,
                                                    std::string_view css_selector) {
  uint64_t latest_sequence = 0;
  std::optional<base::Value::Dict> latest_capture =
      RecorderController::GetLatestLocatorCaptureResult(
          /*min_sequence_exclusive=*/0, &latest_sequence);
  if (!latest_capture) {
    return base::Value::List();
  }
  const double captured_at_ms =
      latest_capture->FindDouble("captured_at_ms").value_or(0.0);
  const std::string captured_page_url =
      latest_capture->FindString("page_url")
          ? *latest_capture->FindString("page_url")
          : std::string();
  const base::Value::List* locators = latest_capture->FindList("locators");
  if (!locators || !HasNonVisualRecordedLocator(*locators) ||
      !HasMeaningfulRecordedLocator(*locators)) {
    return base::Value::List();
  }
  if (captured_at_ms <= 0.0 || captured_page_url.empty() || current_url.empty() ||
      captured_page_url != current_url) {
    return base::Value::List();
  }
  const double now_ms =
      static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch());
  if ((now_ms - captured_at_ms) > 30000.0) {
    return base::Value::List();
  }
  if (!css_selector.empty() &&
      !LocatorBundleContainsCss(*locators, css_selector)) {
    return base::Value::List();
  }
  return SanitizeLoopTargetLocators(*locators, css_selector);
}

bool IsWindowScrollAction(const base::Value::Dict& action, int* distance_out) {
  const std::string action_name =
      action.FindString("action") ? base::ToLowerASCII(*action.FindString("action"))
                                  : std::string();
  if (action_name != "scroll") {
    return false;
  }
  const base::Value::Dict* params = action.FindDict("params");
  if (!params) {
    return false;
  }
  const std::string element =
      params->FindString("element") ? base::ToLowerASCII(*params->FindString("element"))
                                    : std::string();
  if (!element.empty() && element != "window") {
    return false;
  }
  if (const base::Value::List* locators = params->FindList("locators");
      locators && !locators->empty()) {
    return false;
  }
  if (distance_out) {
    *distance_out = params->FindInt("distance").value_or(0);
  }
  return true;
}

void AppendConsolidatedScrollIfMeaningful(base::Value::List* out_steps,
                                          int accumulated_distance) {
  constexpr int kGuidedLoopSignificantScrollPx = 48;
  if (!out_steps || std::abs(accumulated_distance) < kGuidedLoopSignificantScrollPx) {
    return;
  }
  base::Value::Dict action;
  action.Set("action", "scroll");
  base::Value::Dict params;
  params.Set("element", "window");
  params.Set("distance", accumulated_distance);
  action.Set("params", std::move(params));
  out_steps->Append(std::move(action));
}

void NormalizeManualLoopCaptureSteps(base::Value::List* steps) {
  if (!steps || steps->empty()) {
    return;
  }
  base::Value::List normalized;
  int pending_scroll_distance = 0;
  bool has_pending_scroll = false;
  for (const auto& entry : *steps) {
    const base::Value::Dict* action = entry.GetIfDict();
    if (!action) {
      continue;
    }
    int distance = 0;
    if (IsWindowScrollAction(*action, &distance)) {
      pending_scroll_distance += distance;
      has_pending_scroll = true;
      continue;
    }
    if (has_pending_scroll) {
      AppendConsolidatedScrollIfMeaningful(&normalized, pending_scroll_distance);
      pending_scroll_distance = 0;
      has_pending_scroll = false;
    }
    normalized.Append(entry.Clone());
  }
  if (has_pending_scroll) {
    AppendConsolidatedScrollIfMeaningful(&normalized, pending_scroll_distance);
  }
  *steps = std::move(normalized);
}

std::string SummarizeMetadataFields(const base::Value::Dict& dict) {
  constexpr const char* kKeys[] = {
      "frame_id", "shadow_chain", "resource_path", "ax_role",
      "ax_name", "interactive_ancestor_role", "context_url"};
  std::vector<std::string> parts;
  parts.reserve(std::size(kKeys));
  for (const char* key : kKeys) {
    const std::string* value = dict.FindString(key);
    parts.push_back(std::string(key) + "=" +
                    ((value && !value->empty()) ? "1" : "0"));
  }
  return base::JoinString(parts, " ");
}

base::Value::Dict BuildCaptureDebugState(const base::Value::Dict& dict,
                                         const base::Value::List& locators,
                                         int retry_count,
                                         bool has_agent_snapshot,
                                         bool has_page_map_snapshot,
                                         std::string_view source) {
  base::Value::Dict out;
  out.Set("source", std::string(source));
  out.Set("retry_count", retry_count);
  out.Set("has_agent_snapshot", has_agent_snapshot);
  out.Set("has_page_map_snapshot", has_page_map_snapshot);
  out.Set("has_non_visual_locator", HasNonVisualRecordedLocator(locators));
  out.Set("has_meaningful_locator", HasMeaningfulRecordedLocator(locators));
  out.Set("locator_types", SummarizeLocatorTypesForLog(&locators));
  out.Set("metadata_presence", SummarizeMetadataFields(dict));
  return out;
}

bool IsStableTestCssSelector(std::string_view value) {
  const std::string lowered = base::ToLowerASCII(value);
  return lowered.find("[data-testid") != std::string::npos ||
         lowered.find("[data-test") != std::string::npos ||
         lowered.find("[data-cy") != std::string::npos ||
         lowered.find("[data-qa") != std::string::npos;
}

std::vector<std::string> SplitNonEmptyAsciiTokens(std::string value,
                                                  const char* separators) {
  std::vector<std::string> tokens = base::SplitString(
      value, separators, base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (std::string& token : tokens) {
    base::TrimWhitespaceASCII(token, base::TRIM_ALL, &token);
  }
  tokens.erase(
      std::remove_if(tokens.begin(), tokens.end(),
                     [](const std::string& token) { return token.empty(); }),
      tokens.end());
  return tokens;
}

bool IsLikelyGeneratedIdentifierToken(std::string_view token) {
  if (token.empty()) {
    return false;
  }
  const std::string lower = base::ToLowerASCII(token);
  if (base::StartsWith(lower, "css-") || base::StartsWith(lower, "jsx-") ||
      base::StartsWith(lower, "sc-") || base::StartsWith(lower, "r-") ||
      base::StartsWith(lower, "id__") || base::StartsWith(lower, "_")) {
    return true;
  }

  int digits = 0;
  int letters = 0;
  int hyphens = 0;
  int other = 0;
  for (char c : token) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isdigit(uc)) {
      ++digits;
    } else if (std::isalpha(uc)) {
      ++letters;
    } else if (c == '-' || c == '_') {
      ++hyphens;
    } else {
      ++other;
    }
  }
  if (other > 0) {
    return true;
  }
  // Google (and similar) mint short opaque ids like "ti6dpd" / "APjFqb".
  // Those used to rank as top-tier ID locators and steal clicks from NAME=q.
  if (hyphens == 0 && token.size() >= 5 && token.size() <= 12 && digits >= 1 &&
      letters >= 2 && digits + letters == static_cast<int>(token.size())) {
    return true;
  }
  if (token.size() >= 8 && digits >= 3) {
    return true;
  }
  if (token.size() >= 10 && digits >= 2 && letters >= 4 && hyphens >= 1) {
    return true;
  }
  return false;
}

bool IsVolatileClassLocatorValue(std::string_view value) {
  std::vector<std::string> tokens =
      SplitNonEmptyAsciiTokens(std::string(value), " .");
  if (tokens.empty()) {
    return false;
  }
  int volatile_tokens = 0;
  for (const std::string& token : tokens) {
    if (IsLikelyGeneratedIdentifierToken(token)) {
      ++volatile_tokens;
    }
  }
  if (volatile_tokens == 0) {
    return false;
  }
  if (tokens.size() == 1) {
    return true;
  }
  if (tokens.size() >= 4 && volatile_tokens * 2 >= static_cast<int>(tokens.size())) {
    return true;
  }
  return volatile_tokens >= 3;
}

bool IsVolatileCssSelector(std::string_view value) {
  const std::string lowered = base::ToLowerASCII(value);
  if (value.empty() || IsStableTestCssSelector(value)) {
    return false;
  }
  if (lowered.find(":nth-child(") != std::string::npos ||
      lowered.find(":nth-of-type(") != std::string::npos ||
      lowered.find(":nth-last-child(") != std::string::npos ||
      lowered.find(":nth-last-of-type(") != std::string::npos) {
    return true;
  }
  if (!lowered.empty() && lowered.front() == '.') {
    return IsVolatileClassLocatorValue(lowered);
  }
  if (lowered.find(".css-") != std::string::npos ||
      lowered.find(".jsx-") != std::string::npos ||
      lowered.find(".sc-") != std::string::npos ||
      lowered.find(".r-") != std::string::npos) {
    return true;
  }
  if (!lowered.empty() && lowered.front() == '#') {
    return IsLikelyGeneratedIdentifierToken(
        lowered.size() > 1 ? lowered.substr(1) : std::string());
  }
  return false;
}

int LocatorPreferenceRank(std::string_view type, std::string_view value) {
  const std::string upper = base::ToUpperASCII(type);
  // Generated / ephemeral ids must not outrank stable NAME/CSS locators.
  if (upper == "ID") {
    return IsLikelyGeneratedIdentifierToken(value) ? 12 : 0;
  }
  if (upper == "NAME") return 1;
  if (upper == "ACCESSIBILITY_NAME") return 2;
  if (upper == "LINK_TEXT") return 3;
  if (upper == "CSS_SELECTOR" && IsStableTestCssSelector(value)) return 4;
  if (upper == "CSS_SELECTOR" && IsVolatileCssSelector(value)) return 9;
  if (upper == "CSS_SELECTOR") return 5;
  if (upper == "TEXT_CONTENT") return 7;
  if (upper == "PARTIAL_LINK_TEXT") return 8;
  if (upper == "XPATH") {
    // //*[@id='ti6dpd'] is just a generated-id wrapper.
    if (value.find("@id=") != std::string_view::npos) {
      return 12;
    }
    return 9;
  }
  if (upper == "CLASS_NAME" && IsVolatileClassLocatorValue(value)) return 11;
  if (upper == "CLASS_NAME") return 10;
  if (upper == "TAG_NAME") return 11;
  if (upper == "JS_EVAL") return 40;
  if (upper == "SHADOW_DOM") return 91;
  if (upper == "VISUAL_COORDS") return 100;
  return 50;
}

base::Value::List ReorderRecordedLocatorBundle(base::Value::List locators) {
  std::stable_sort(
      locators.begin(), locators.end(),
      [](const base::Value& lhs, const base::Value& rhs) {
        const base::Value::Dict* lhs_dict = lhs.GetIfDict();
        const base::Value::Dict* rhs_dict = rhs.GetIfDict();
        const std::string lhs_type = lhs_dict && lhs_dict->FindString("type")
            ? *lhs_dict->FindString("type")
            : std::string();
        const std::string lhs_value = lhs_dict && lhs_dict->FindString("value")
            ? *lhs_dict->FindString("value")
            : std::string();
        const std::string rhs_type = rhs_dict && rhs_dict->FindString("type")
            ? *rhs_dict->FindString("type")
            : std::string();
        const std::string rhs_value = rhs_dict && rhs_dict->FindString("value")
            ? *rhs_dict->FindString("value")
            : std::string();
        return LocatorPreferenceRank(lhs_type, lhs_value) <
               LocatorPreferenceRank(rhs_type, rhs_value);
      });
  return locators;
}

std::string SanitizeRecordedLocatorValue(const std::string& type,
                                         std::string value) {
  value = NormalizeUtf8BestEffort(std::move(value));
  base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
  if (value.empty()) {
    return std::string();
  }

  if (type == "ACCESSIBILITY_NAME" || type == "TEXT_CONTENT" ||
      type == "LINK_TEXT") {
    value = TrimForLocatorValue(
        base::CollapseWhitespaceASCII(value, /*trim_sequences_with_line_breaks=*/true),
        120u);
  } else if (type == "PARTIAL_LINK_TEXT") {
    value = TrimForLocatorValue(
        base::CollapseWhitespaceASCII(value, /*trim_sequences_with_line_breaks=*/true),
        40u);
  }

  if (IsRootLikeRecordedLocator(type, value)) {
    return std::string();
  }

  // Drop ephemeral ids at record time — they win resolve scoring and then
  // click the wrong node (or nothing) on the next page load.
  if (type == "ID" && IsLikelyGeneratedIdentifierToken(value)) {
    return std::string();
  }
  if (type == "CSS_SELECTOR" && !value.empty() && value.front() == '#' &&
      IsLikelyGeneratedIdentifierToken(value.substr(1))) {
    return std::string();
  }
  if (type == "XPATH") {
    // Match //*[@id='ti6dpd'] / //*[@id="ti6dpd"].
    const size_t id_pos = value.find("@id=");
    if (id_pos != std::string::npos) {
      size_t q = value.find_first_of("'\"", id_pos);
      if (q != std::string::npos && q + 1 < value.size()) {
        const char quote = value[q];
        const size_t end = value.find(quote, q + 1);
        if (end != std::string::npos) {
          const std::string id_token = value.substr(q + 1, end - q - 1);
          if (IsLikelyGeneratedIdentifierToken(id_token)) {
            return std::string();
          }
        }
      }
    }
  }
  if (type == "TAG_NAME") {
    const std::string lower = base::ToLowerASCII(value);
    // Bare tags are never unique on real pages; recording still harvested them
    // and the executor would happily click the first <textarea>/<div>.
    if (lower == "div" || lower == "span" || lower == "a" || lower == "p" ||
        lower == "input" || lower == "textarea" || lower == "button" ||
        lower == "li" || lower == "ul" || lower == "section" ||
        lower == "main" || lower == "body" || lower == "html") {
      return std::string();
    }
  }

  return value;
}

void AppendRecordedLocator(base::Value::List& locators,
                           std::set<std::pair<std::string, std::string>>& seen,
                           const char* type,
                           std::string value) {
  const std::string type_str = type ? std::string(type) : std::string();
  if (type_str.empty()) {
    return;
  }
  std::string sanitized = SanitizeRecordedLocatorValue(type_str, std::move(value));
  if (sanitized.empty()) {
    return;
  }
  AppendUniqueLocator(locators, seen, type, std::move(sanitized));
}

void AppendRecordedLocatorsOfType(
    base::Value::List& out,
    std::set<std::pair<std::string, std::string>>& seen,
    const base::Value::List* locators,
    const char* desired_type) {
  if (!locators || !desired_type || !*desired_type) {
    return;
  }
  const std::string desired = base::ToUpperASCII(desired_type);
  for (const auto& entry : *locators) {
    const base::Value::Dict* dict = entry.GetIfDict();
    if (!dict) {
      continue;
    }
    const std::string* type = dict->FindString("type");
    const std::string* value = dict->FindString("value");
    if (!type || !value) {
      continue;
    }
    if (base::ToUpperASCII(*type) != desired) {
      continue;
    }
    AppendRecordedLocator(out, seen, desired_type, *value);
  }
}

bool IsAutoLoopEligibleActionKind(const std::string& kind) {
  return kind != "loop" && kind != "if_condition" && kind != "listen" &&
         kind != "call_script";
}

std::string StripIndexedXPathPredicates(std::string xpath) {
  size_t cursor = 0;
  while (cursor < xpath.size()) {
    const size_t open = xpath.find('[', cursor);
    if (open == std::string::npos) {
      break;
    }
    const size_t close = xpath.find(']', open + 1);
    if (close == std::string::npos) {
      break;
    }
    bool numeric_only = true;
    for (size_t i = open + 1; i < close; ++i) {
      unsigned char c = static_cast<unsigned char>(xpath[i]);
      if (!std::isspace(c) && !std::isdigit(c)) {
        numeric_only = false;
        break;
      }
    }
    if (!numeric_only) {
      cursor = close + 1;
      continue;
    }
    xpath.erase(open, close - open + 1);
    cursor = open;
  }
  return xpath;
}

void StripCssPseudoFunction(std::string& css, const std::string& pseudo) {
  size_t cursor = 0;
  while (cursor < css.size()) {
    const size_t start = css.find(pseudo, cursor);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = css.find(')', start + pseudo.size());
    if (end == std::string::npos) {
      break;
    }
    css.erase(start, end - start + 1);
    cursor = start;
  }
}

std::string NormalizeIndexedLocatorValue(const std::string& type,
                                         const std::string& value) {
  const std::string locator_type = base::ToUpperASCII(type);
  if (locator_type == "XPATH") {
    return StripIndexedXPathPredicates(value);
  }
  if (locator_type == "CSS_SELECTOR") {
    std::string css = value;
    StripCssPseudoFunction(css, ":nth-child(");
    StripCssPseudoFunction(css, ":nth-of-type(");
    StripCssPseudoFunction(css, ":nth-last-child(");
    StripCssPseudoFunction(css, ":nth-last-of-type(");
    return css;
  }
  return value;
}

struct ForEachLocatorCandidate {
  std::string type;
  std::string raw_value;
  std::string normalized_value;
  bool normalized_changed = false;
};

std::optional<ForEachLocatorCandidate> ExtractForEachLocatorCandidate(
    const base::Value::Dict& action) {
  const base::Value::Dict* params = action.FindDict("params");
  if (!params) {
    return std::nullopt;
  }
  const base::Value::List* locators = params->FindList("locators");
  if (!locators) {
    return std::nullopt;
  }
  for (const auto& locator_value : *locators) {
    const base::Value::Dict* locator = locator_value.GetIfDict();
    if (!locator) {
      continue;
    }
    const std::string* type = locator->FindString("type");
    const std::string* value = locator->FindString("value");
    if (!type || !value) {
      continue;
    }
    const std::string locator_type = base::ToUpperASCII(*type);
    if (locator_type != "CSS_SELECTOR" && locator_type != "XPATH") {
      continue;
    }
    if (!IsUsableForEachLocatorValue(locator_type, *value)) {
      continue;
    }
    const std::string normalized =
        NormalizeIndexedLocatorValue(locator_type, *value);
    if (!IsUsableForEachLocatorValue(locator_type, normalized)) {
      continue;
    }
    ForEachLocatorCandidate candidate;
    candidate.type = locator_type;
    candidate.raw_value = *value;
    candidate.normalized_value = normalized;
    candidate.normalized_changed = (normalized != *value);
    return candidate;
  }
  return std::nullopt;
}

std::string BuildAutoLoopSignature(const base::Value::Dict& action) {
  const std::string* action_name = action.FindString("action");
  const std::string kind =
      base::ToLowerASCII(action_name ? *action_name : std::string());
  if (kind.empty() || !IsAutoLoopEligibleActionKind(kind)) {
    return std::string();
  }

  base::Value::Dict normalized;
  normalized.Set("action", kind);
  if (const base::Value::Dict* params = action.FindDict("params")) {
    base::Value::Dict p = params->Clone();
    p.Remove("frame_id");
    p.Remove("frameId");
    p.Remove("source");
    p.Remove("x");
    p.Remove("y");
    p.Remove("timestamp");
    p.Remove("captured_at");
    p.Remove("url");
    p.Remove("target_url");
    p.Remove("targetUrl");
    p.Remove("referrer");
    if (base::Value::List* locators = p.FindList("locators")) {
      base::Value::List normalized_locators;
      normalized_locators.reserve(locators->size());
      for (const auto& loc : *locators) {
        const base::Value::Dict* loc_dict = loc.GetIfDict();
        if (!loc_dict) {
          continue;
        }
        const std::string* type = loc_dict->FindString("type");
        if (!type) {
          continue;
        }
        const std::string locator_type = base::ToUpperASCII(*type);
        if (locator_type == "VISUAL_COORDS") {
          continue;
        }
        base::Value::Dict normalized_locator = loc_dict->Clone();
        normalized_locator.Remove("priority");
        normalized_locator.Remove("score");
        normalized_locator.Remove("confidence");
        if (const std::string* value = normalized_locator.FindString("value")) {
          normalized_locator.Set(
              "value", NormalizeIndexedLocatorValue(locator_type, *value));
        }
        normalized_locators.Append(std::move(normalized_locator));
      }
      p.Set("locators", std::move(normalized_locators));
    }
    normalized.Set("params", std::move(p));
  }
  std::string signature;
  base::JSONWriter::Write(normalized, &signature);
  return signature;
}

bool TryReadPageMapNumber(const base::Value::Dict& dict,
                          const char* key,
                          double* out) {
  if (!out) {
    return false;
  }
  if (const std::optional<double> value = dict.FindDouble(key)) {
    *out = *value;
    return true;
  }
  if (const std::optional<int> value = dict.FindInt(key)) {
    *out = static_cast<double>(*value);
    return true;
  }
  return false;
}

double ScorePointAgainstPageMapElement(const gfx::Point& point,
                                       const base::Value::Dict& element,
                                       bool require_containment = false) {
  const base::Value::Dict* bounds = element.FindDict("bounds");
  if (!bounds) {
    return 0.0;
  }
  double x = 0.0;
  double y = 0.0;
  double w = 0.0;
  double h = 0.0;
  if (!TryReadPageMapNumber(*bounds, "x", &x) ||
      !TryReadPageMapNumber(*bounds, "y", &y) ||
      !TryReadPageMapNumber(*bounds, "w", &w) ||
      !TryReadPageMapNumber(*bounds, "h", &h) || w <= 0.0 || h <= 0.0) {
    return 0.0;
  }
  const double px = point.x();
  const double py = point.y();
  const bool inside = px >= x && px <= (x + w) && py >= y && py <= (y + h);
  if (inside) {
    const double area = std::max(1.0, w * h);
    return 2.0 - std::min(1.0, area / 200000.0);
  }
  // Nearby fuzzy matches routinely pick the wrong sibling button (Log in vs
  // Continue with Apple). Only allow them for soft assist paths.
  if (require_containment) {
    return 0.0;
  }
  const double cx = x + (w * 0.5);
  const double cy = y + (h * 0.5);
  const double dx = px - cx;
  const double dy = py - cy;
  const double distance = std::sqrt(dx * dx + dy * dy);
  if (distance <= 24.0) {
    return 0.5 - std::min(0.25, distance / 96.0);
  }
  return 0.0;
}

void AppendRecordedLocatorsFromPageMapSelectors(
    base::Value::List& out,
    std::set<std::pair<std::string, std::string>>& seen,
    const base::Value::Dict& selectors) {
  if (const std::string* css = selectors.FindString("css")) {
    AppendRecordedLocator(out, seen, "CSS_SELECTOR", *css);
  }
  if (const std::string* xpath = selectors.FindString("xpath")) {
    AppendRecordedLocator(out, seen, "XPATH", *xpath);
  }
  if (const base::Value::List* locators = selectors.FindList("locators")) {
    constexpr const char* kPreferredTypes[] = {
        "SHADOW_DOM",         "ACCESSIBILITY_NAME",
        "LINK_TEXT",         "PARTIAL_LINK_TEXT",  "TEXT_CONTENT",
        "ID",                "CLASS_NAME",         "NAME",
        "CSS_SELECTOR",      "XPATH",
        "TAG_NAME",
    };
    for (const char* type : kPreferredTypes) {
      AppendRecordedLocatorsOfType(out, seen, locators, type);
    }
  }
}

float MaybeFloat(const base::Value::Dict& dict, const std::string& key) {
  return static_cast<float>(dict.FindDouble(key).value_or(0.0));
}

[[maybe_unused]] std::string NormalizeApcText(std::string text,
                                              size_t max_len = 160u) {
  text = base::CollapseWhitespaceASCII(text, true);
  if (text.size() <= max_len)
    return text;
  text.resize(max_len);
  text.append("...");
  return text;
}

[[maybe_unused]] std::string ApcTagHintForAttributeType(
    optimization_guide::proto::ContentAttributeType type) {
  using T = optimization_guide::proto::ContentAttributeType;
  switch (type) {
    case T::CONTENT_ATTRIBUTE_ANCHOR:
      return "a";
    case T::CONTENT_ATTRIBUTE_FORM:
      return "form";
    case T::CONTENT_ATTRIBUTE_FORM_CONTROL:
      return "input";
    case T::CONTENT_ATTRIBUTE_TABLE:
      return "table";
    case T::CONTENT_ATTRIBUTE_TABLE_ROW:
      return "tr";
    case T::CONTENT_ATTRIBUTE_IMAGE:
      return "img";
    case T::CONTENT_ATTRIBUTE_VIDEO:
      return "video";
    case T::CONTENT_ATTRIBUTE_CANVAS:
      return "canvas";
    case T::CONTENT_ATTRIBUTE_SVG:
      return "svg";
    default:
      return std::string();
  }
}

[[maybe_unused]] base::Value::List BuildLocatorBundleFromApcNode(
    const optimization_guide::proto::ContentAttributes& attrs,
    const std::string& primary_text,
    const std::string& field_name,
    const std::string& placeholder,
    std::optional<gfx::Point> center,
    std::string_view document_identifier) {
  base::Value::List out;
  std::set<std::pair<std::string, std::string>> seen;

  if (!primary_text.empty()) {
    AppendUniqueLocator(out, seen, "ACCESSIBILITY_NAME", primary_text);
    AppendUniqueLocator(out, seen, "TEXT_CONTENT", primary_text);
  }
  if (!placeholder.empty()) {
    AppendUniqueLocator(out, seen, "TEXT_CONTENT", placeholder);
  }
  if (!field_name.empty()) {
    AppendUniqueLocator(out, seen, "NAME", field_name);
  }

  const std::string tag_hint = ApcTagHintForAttributeType(attrs.attribute_type());
  if (!tag_hint.empty()) {
    AppendUniqueLocator(out, seen, "TAG_NAME", tag_hint);
  }

  if (attrs.attribute_type() ==
          optimization_guide::proto::CONTENT_ATTRIBUTE_ANCHOR &&
      !primary_text.empty()) {
    AppendUniqueLocator(out, seen, "LINK_TEXT", primary_text);
  }

  if (center.has_value()) {
    AppendUniqueLocator(out, seen, "VISUAL_COORDS",
                        base::StringPrintf("{\"x\":%d,\"y\":%d}",
                                           center->x(), center->y()));
  }

  return out;
}

[[maybe_unused]] base::Value::Dict BuildPageMapFromAnnotatedPageContent(
    const optimization_guide::proto::AnnotatedPageContent& apc,
    const GURL& fallback_url) {
  dashboard::PageMapExtractor extractor;
  extractor.set_timestamp_ms(base::Time::Now().InMillisecondsSinceUnixEpoch());

  GURL page_url = fallback_url;
  std::string main_document_id;
  if (apc.has_main_frame_data()) {
    const auto& frame_data = apc.main_frame_data();
    if (!frame_data.url().empty()) {
      GURL candidate(frame_data.url());
      if (candidate.is_valid()) {
        page_url = candidate;
      }
    }
    if (frame_data.has_document_identifier()) {
      main_document_id = frame_data.document_identifier().serialized_token();
    }
  }
  extractor.set_url(page_url);

  if (!apc.has_root_node()) {
    base::Value::Dict empty = extractor.BuildPageMap();
    if (base::Value::Dict* meta = empty.FindDict("metadata")) {
      meta->Set("source", "glic_apc");
      meta->Set("apcNodeCount", 0);
      meta->Set("interactiveCount", 0);
      if (!main_document_id.empty()) {
        meta->Set("documentId", main_document_id);
      }
    }
    return empty;
  }

  int visited_nodes = 0;
  optimization_guide::VisitContentNodes(
      apc.root_node(), main_document_id,
      [&](const optimization_guide::proto::ContentNode& node,
          std::string_view document_identifier) {
        ++visited_nodes;
        if (!node.has_content_attributes()) {
          return;
        }

        const auto& attrs = node.content_attributes();
        const int dom_node_id = attrs.common_ancestor_dom_node_id();
        if (dom_node_id <= 0) {
          return;
        }

        bool clickable = false;
        bool focusable = false;
        bool editable = false;
        bool disabled = false;
        if (attrs.has_interaction_info()) {
          const auto& interaction = attrs.interaction_info();
          clickable = interaction.is_clickable() ||
                      interaction.clickability_reasons_size() > 0;
          focusable = interaction.is_focusable();
          editable = interaction.is_editable();
          disabled = interaction.is_disabled();
        }

        std::string field_name;
        std::string placeholder;
        std::string type;
        if (attrs.has_form_control_data()) {
          const auto& form_control = attrs.form_control_data();
          field_name = NormalizeApcText(form_control.field_name(), 80u);
          placeholder = NormalizeApcText(form_control.placeholder(), 120u);
          type = optimization_guide::proto::FormControlType_Name(
              form_control.form_control_type());
        }

        std::string primary_text = NormalizeApcText(attrs.label(), 160u);
        if (primary_text.empty() && attrs.has_text_data()) {
          primary_text = NormalizeApcText(attrs.text_data().text_content(), 160u);
        }
        if (primary_text.empty() && !placeholder.empty()) {
          primary_text = placeholder;
        } else if (primary_text.empty() && !field_name.empty()) {
          primary_text = field_name;
        }

        std::string role;
        if (attrs.has_aria_role()) {
          role = optimization_guide::proto::AXRole_Name(attrs.aria_role());
        }

        const bool actionable =
            clickable || focusable || editable || attrs.has_form_control_data();
        if (!actionable && primary_text.empty() && role.empty()) {
          return;
        }

        gfx::RectF bounds;
        std::optional<gfx::Point> center;
        if (attrs.has_geometry()) {
          const auto& geometry = attrs.geometry();
          const auto* box = geometry.has_visible_bounding_box()
                                ? &geometry.visible_bounding_box()
                                : (geometry.has_outer_bounding_box()
                                       ? &geometry.outer_bounding_box()
                                       : nullptr);
          if (box) {
            bounds = gfx::RectF(static_cast<float>(box->x()),
                                static_cast<float>(box->y()),
                                static_cast<float>(box->width()),
                                static_cast<float>(box->height()));
            if (box->width() > 0 && box->height() > 0) {
              center = gfx::Point(box->x() + box->width() / 2,
                                  box->y() + box->height() / 2);
            }
          }
        }

        dashboard::PageElementSnapshot snap;
        snap.element_id = base::NumberToString(dom_node_id);
        snap.frame_id = std::string(document_identifier);
        snap.tag = ApcTagHintForAttributeType(attrs.attribute_type());
        snap.role = role;
        snap.type = !type.empty()
                        ? type
                        : optimization_guide::proto::ContentAttributeType_Name(
                              attrs.attribute_type());
        snap.text = primary_text;
        snap.accessibility_name = primary_text;
        snap.locator_bundle = BuildLocatorBundleFromApcNode(
            attrs, primary_text, field_name, placeholder, center,
            document_identifier);
        snap.clickable = clickable;
        snap.focusable = focusable;
        snap.disabled = disabled;
        snap.bounds = bounds;
        extractor.AddElement(std::move(snap));
      });

  base::Value::Dict pagemap = extractor.BuildPageMap();
  if (base::Value::Dict* meta = pagemap.FindDict("metadata")) {
    meta->Set("source", "glic_apc");
    meta->Set("apcNodeCount", visited_nodes);
    if (!main_document_id.empty()) {
      meta->Set("documentId", main_document_id);
    }
  }
  return pagemap;
}

}  // namespace

// Static singleton pointer for the active recorder.
static RecorderController* g_active_recorder = nullptr;
static uint64_t g_locator_capture_sequence = 0;
static bool g_has_locator_capture_result = false;

base::Value::Dict& LatestLocatorCaptureResult() {
  static base::NoDestructor<base::Value::Dict> result;
  return *result;
}

RecorderController::RecorderController() = default;
RecorderController::~RecorderController() {
  // Ensure CDP/input hooks are detached before object teardown. DevTools can
  // still dispatch notifications briefly during shutdown; leaving a client
  // attached here risks use-after-free crashes.
  weak_factory_.InvalidateWeakPtrs();
  if (observed_tab_strip_model_) {
    observed_tab_strip_model_->RemoveObserver(this);
    observed_tab_strip_model_ = nullptr;
  }
  DetachInputObserver();
  DetachFromTarget();
  if (RecorderController::GetActive() == this) {
    RecorderController::SetActive(nullptr);
  }
}

void RecorderController::ObservePreferredTabStrip(BrowserView* browser_view) {
  Browser* browser = browser_view ? browser_view->browser() : nullptr;
  TabStripModel* tab_strip_model = browser ? browser->tab_strip_model() : nullptr;
  if (!tab_strip_model) {
    if (observed_tab_strip_model_) {
      observed_tab_strip_model_->RemoveObserver(this);
      observed_tab_strip_model_ = nullptr;
    }
    observed_browser_ = nullptr;
    last_capturable_tab_id_ = -1;
    return;
  }
  if (observed_tab_strip_model_ == tab_strip_model) {
    return;
  }
  if (observed_tab_strip_model_) {
    observed_tab_strip_model_->RemoveObserver(this);
  }
  observed_tab_strip_model_ = tab_strip_model;
  observed_tab_strip_model_->AddObserver(this);
  observed_browser_ = browser;
  if (content::WebContents* active = tab_strip_model->GetActiveWebContents();
      IsCapturableWebContents(active, /*avoid_contents=*/nullptr)) {
    last_capturable_tab_id_ = sessions::SessionTabHelper::IdForTab(active).id();
  }
}

content::WebContents* RecorderController::ResolveTrackedCapturableWebContents(
    BrowserView* preferred_browser_view,
    content::WebContents* avoid_contents) const {
  Browser* browser = preferred_browser_view ? preferred_browser_view->browser()
                                            : observed_browser_.get();
  TabStripModel* tab_strip_model = browser ? browser->tab_strip_model() : nullptr;
  content::WebContents* tracked =
      FindWebContentsBySessionTabId(tab_strip_model, last_capturable_tab_id_);
  return IsCapturableWebContents(tracked, avoid_contents) ? tracked : nullptr;
}

// static
RecorderController* RecorderController::GetActive() {
  return g_active_recorder;
}

// static
void RecorderController::SetActive(RecorderController* rc) {
  g_active_recorder = rc;
}

// static
uint64_t RecorderController::GetLatestLocatorCaptureSequence() {
  return g_locator_capture_sequence;
}

// static
std::optional<base::Value::Dict> RecorderController::GetLatestLocatorCaptureResult(
    uint64_t min_sequence_exclusive,
    uint64_t* out_sequence) {
  if (out_sequence) {
    *out_sequence = g_locator_capture_sequence;
  }
  if (!g_has_locator_capture_result ||
      g_locator_capture_sequence <= min_sequence_exclusive) {
    return std::nullopt;
  }
  return LatestLocatorCaptureResult().Clone();
}

base::CallbackListSubscription RecorderController::AddStateChangedCallback(
    StateChangedCallback callback) {
  return state_changed_callbacks_.Add(std::move(callback));
}

RecorderStateSnapshot
RecorderController::GetStateSnapshot() const {
  RecorderStateSnapshot snapshot;
  snapshot.is_recording = is_recording_;
  snapshot.action_count = static_cast<uint32_t>(std::min<size_t>(
      recorded_actions_.size(), std::numeric_limits<uint32_t>::max()));
  snapshot.manual_loop_capture_active = manual_loop_capture_active_;
  snapshot.locator_capture_armed = locator_capture_armed_;
  snapshot.locator_capture_ready = locator_capture_ready_;
  snapshot.guided_active = guided_loop_ui_state_.active;
  snapshot.guided_phase = guided_loop_ui_state_.phase;
  snapshot.guided_capture_pending = guided_loop_ui_state_.capture_pending;
  snapshot.guided_capture_kind = guided_loop_ui_state_.capture_kind;
  snapshot.guided_capture_sequence = guided_loop_ui_state_.capture_sequence;
  snapshot.guided_scrape_target_count = guided_loop_ui_state_.scrape_target_count;
  return snapshot;
}

void RecorderController::NotifyStateChanged() {
  state_changed_callbacks_.Notify(GetStateSnapshot());
}

void RecorderController::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (observed_tab_strip_model_ != tab_strip_model) {
    return;
  }

  if (selection.active_tab_changed()) {
    if (IsCapturableWebContents(selection.new_contents,
                                /*avoid_contents=*/nullptr)) {
      last_capturable_tab_id_ =
          sessions::SessionTabHelper::IdForTab(selection.new_contents).id();
      // Keep one-shot locator capture attached to the tab the user is looking
      // at, otherwise clicks on a different tab never harvest or unarm.
      if (locator_capture_armed_ &&
          selection.new_contents != target_web_contents_) {
        AttachToWebContents(selection.new_contents);
        AttachInputObserver();
        ClearCaptureHoverPreview();
        NotifyStateChanged();
      }
      return;
    }
    if (IsCapturableWebContents(selection.old_contents,
                                /*avoid_contents=*/nullptr)) {
      last_capturable_tab_id_ =
          sessions::SessionTabHelper::IdForTab(selection.old_contents).id();
    }
  }
}

void RecorderController::OnTabStripModelDestroyed(
    TabStripModel* tab_strip_model) {
  if (observed_tab_strip_model_ == tab_strip_model) {
    observed_tab_strip_model_ = nullptr;
    observed_browser_ = nullptr;
    last_capturable_tab_id_ = -1;
  }
}

void RecorderController::Start(BrowserView* browser_view) {
  StartPassiveMode(browser_view, /*explicit_recording=*/true);
}

void RecorderController::Stop() {
  StopPassiveMode();
}
void RecorderController::StartPassiveMode(BrowserView* browser_view,
                                          bool explicit_recording) {
  TRACE_EVENT2("ui", "RecorderController::StartPassiveMode",
               "explicit_recording", explicit_recording, "js_enabled",
               dashboard::IsJavaScriptExecutionEnabled());
  if (is_recording_) {
    explicit_recording_enabled_ =
        explicit_recording_enabled_ || explicit_recording;
    UpdateInstrumentationState();
    NotifyStateChanged();
    LOG(INFO) << "[Recorder] StartPassiveMode ignored: already recording";
    return;
  }
  LOG(INFO) << "[Recorder] Passive recording mode ON";
  LOG(INFO) << "[Recorder] Loop preview enabled="
            << (dashboard::IsLoopPreviewEnabled() ? 1 : 0);
  explicit_recording_enabled_ = explicit_recording;
  recorded_actions_.clear();
  recorded_action_signatures_.clear();
  traffic_store_.Clear();
  network_capture_enabled_ = false;
  last_recorded_navigate_url_.clear();
  pending_scroll_y_ = 0;
  last_hover_probe_point_.reset();
  last_hover_probe_enqueued_at_ = base::TimeTicks();
  last_hover_transition_signature_.clear();
  last_hover_recorded_at_ = base::TimeTicks();
  last_recorded_click_point_.reset();
  last_recorded_click_at_ = base::TimeTicks();
  last_recorded_click_count_ = -1;
  last_recorded_click_frame_id_.clear();
  last_recorded_click_source_.clear();
  active_pointer_down_point_.reset();
  active_pointer_last_point_.reset();
  active_pointer_dragged_ = false;
  last_non_hover_click_point_.reset();
  last_non_hover_click_at_ = base::TimeTicks();
  scroll_capture_timer_.Stop();
  loop_preview_timer_.Stop();
  pending_loop_preview_point_.reset();
  last_loop_preview_point_.reset();
  loop_preview_modifiers_active_ = false;
  loop_preview_key_chord_active_ = false;
  loop_preview_visible_ = false;
  guided_loop_target_confirmation_pending_ = false;
  guided_loop_target_probe_point_.reset();
  SetGuidedLoopFirstClickGuardArmed(false);
  manual_loop_capture_active_ = false;
  pending_manual_loop_capture_start_ = false;
  guided_same_tab_navigation_guard_enabled_ = false;
  guided_loop_origin_url_.clear();

  if (!browser_view) {
    if (BrowserWindowInterface* browser =
            GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
      browser_view = BrowserView::GetBrowserViewForBrowser(browser);
    }
  }
  ObservePreferredTabStrip(browser_view);

  // Attach to a real page tab, not the dashboard/GLIC WebUI that may have
  // initiated the capture flow.
  if (!AttachToBestCapturableTarget(browser_view)) {
    AttachToActiveTab(browser_view);
  }
  if (target_web_contents_)
    Observe(target_web_contents_);
  else
    LOG(WARNING) << "[Recorder] StartPassiveMode: no active tab to attach";

  is_recording_ = true;
  SetActive(this);
  UpdateInstrumentationState();
  if (explicit_recording_enabled_) {
    EnsureNetworkCaptureEnabled();
  }
  if (ShouldObserveInput() && !input_observer_)
    LOG(WARNING) << "[Recorder] StartPassiveMode: input observer not attached";

  NotifyStateChanged();

  // Record an initial navigate action so scripts always have a concrete start
  // URL when replayed. Skip non-http(s) pages like about:blank.
  if (!dashboard::IsRecordingSuppressed()) {
    const GURL u(current_url_);
    if (u.is_valid() && u.SchemeIsHTTPOrHTTPS()) {
      last_recorded_navigate_url_ = u.spec();
      base::Value::Dict action;
      action.Set("action", "navigate");
      base::Value::Dict params;
      params.Set("url", u.spec());
      action.Set("params", std::move(params));
      AppendRecordedAction(std::move(action));
    }
  }
}

void RecorderController::StopPassiveMode() {
  if (locator_capture_armed_) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "locator_capture_canceled");
    CompleteOneShotLocatorCapture(std::move(out));
  }
  if (!is_recording_) return;

  LOG(INFO) << "[Recorder] Passive recording mode OFF";
  scroll_capture_timer_.Stop();
  FlushPendingTypeBuffer();
  is_recording_ = false;
  explicit_recording_enabled_ = false;
  pending_scroll_y_ = 0;
  hover_probe_timer_.Stop();
  pending_hover_probe_point_.reset();
  pending_hover_probe_queued_at_ = base::TimeTicks();
  last_hover_probe_point_.reset();
  last_hover_probe_enqueued_at_ = base::TimeTicks();
  last_hover_transition_signature_.clear();
  last_hover_recorded_at_ = base::TimeTicks();
  last_recorded_click_point_.reset();
  last_recorded_click_at_ = base::TimeTicks();
  last_recorded_click_count_ = -1;
  last_recorded_click_frame_id_.clear();
  last_recorded_click_source_.clear();
  active_pointer_down_point_.reset();
  active_pointer_last_point_.reset();
  active_pointer_dragged_ = false;
  last_non_hover_click_point_.reset();
  last_non_hover_click_at_ = base::TimeTicks();
  loop_preview_modifiers_active_ = false;
  loop_preview_key_chord_active_ = false;
  ClearLoopPreview();
  last_loop_preview_point_.reset();
  guided_loop_target_confirmation_pending_ = false;
  guided_loop_target_probe_point_.reset();
  SetGuidedLoopFirstClickGuardArmed(false);
  EndManualForEachLoopCapture();
  pending_manual_loop_capture_start_ = false;
  SetGuidedLoopSameTabNavigationGuard(false);
  guided_same_tab_navigation_guard_enabled_ = false;
  guided_loop_ui_state_ = GuidedLoopUiState();
  ClearGuidedScrapeSelections();
  guided_loop_origin_url_.clear();
  NotifyStateChanged();
  DashboardUIMessageHandler::NotifyRecordingStopped();
  if (observed_tab_strip_model_) {
    observed_tab_strip_model_->RemoveObserver(this);
    observed_tab_strip_model_ = nullptr;
  }
  observed_browser_ = nullptr;
  last_capturable_tab_id_ = -1;
  DetachInputObserver();
  DetachFromTarget();
  if (RecorderController::GetActive() == this) {
    RecorderController::SetActive(nullptr);
  }
  NotifyStateChanged();
}

void RecorderController::CapturePageMap(PageMapCallback cb) {
  TRACE_EVENT0("loading", "RecorderController::CapturePageMap");
  if (!ShouldAutoCapturePageMapSnapshot("ui_capture")) {
    if (cb) {
      std::move(cb).Run(BuildEmptyPageMapSnapshot(
          current_url_, "autocapture_disabled:ui_capture"));
    }
    return;
  }
  // Public shim used by WebUI to capture a snapshot on demand.
  // Lazily attach to the currently active tab if we are not already bound to
  // a WebContents / DevTools target, so PageMap capture works even when the
  // recorder was never put into full recording mode.
  if (!target_web_contents_ || !agent_host_ || !devtools_client_) {
    if (BrowserWindowInterface* browser =
            GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
      BrowserView* bv = BrowserView::GetBrowserViewForBrowser(browser);
      if (bv) {
        AttachToActiveTab(bv);
        if (target_web_contents_)
          Observe(target_web_contents_);
      }
    }
  }
  RequestPageMapSnapshot("ui_capture", std::move(cb));
}

void RecorderController::RequestPageMapSnapshot(const std::string& reason,
                                                PageMapCallback cb) {
  TRACE_EVENT2("loading", "RecorderController::RequestPageMapSnapshot",
               "reason", reason, "capture_in_progress",
               page_map_capture_in_progress_);
  if (!ShouldAutoCapturePageMapSnapshot(reason)) {
    if (cb) {
      std::move(cb).Run(BuildEmptyPageMapSnapshot(
          current_url_, std::string("autocapture_disabled:") + reason));
    }
    return;
  }

  if (cb) {
    if (auto recent = MaybeCloneRecentPageMapSnapshot(reason)) {
      std::move(cb).Run(std::move(*recent));
      return;
    }
  }

  if (cb)
    pending_page_map_callbacks_.push_back(std::move(cb));

  if (!agent_host_ || !devtools_client_ || !target_web_contents_) {
    if (!pending_page_map_callbacks_.empty())
      HandlePageMapFailure("not_attached");
    return;
  }

  if (page_map_capture_in_progress_)
    return;

  page_map_capture_reason_ = reason;
  StartPageMapCapture();
}

// ... (rest of the code remains the same)

void RecorderController::DispatchPageMapResult(
    base::Value::Dict raw_snapshot) {
  // Fast-path: if the snapshot already has a frames array, reuse it and just
  // normalize metadata/timestamp.
  if (raw_snapshot.FindList("frames") && !raw_snapshot.FindList("frames")->empty() &&
      !raw_snapshot.FindList("elements")) {
    // Ensure timestamp.
    if (!raw_snapshot.Find("timestamp")) {
      raw_snapshot.Set(
          "timestamp",
          static_cast<double>(
              base::Time::Now().InMillisecondsSinceUnixEpoch()));
    }

    // Ensure metadata with interactive counts.
    base::Value::Dict* meta = raw_snapshot.FindDict("metadata");
    if (!meta) {
      raw_snapshot.Set("metadata", base::Value::Dict());
      meta = raw_snapshot.FindDict("metadata");
    }
    int element_count = 0;
    int shadow_count = 0;
    if (const base::Value::List* frames = raw_snapshot.FindList("frames")) {
      for (const auto& f : *frames) {
        if (!f.is_dict())
          continue;
        const base::Value::Dict& fd = f.GetDict();
        if (const base::Value::List* els = fd.FindList("elements"))
          element_count += static_cast<int>(els->size());
        if (const base::Value::List* shadows = fd.FindList("shadowRoots")) {
          for (const auto& sh : *shadows) {
            if (!sh.is_dict())
              continue;
            const base::Value::Dict& sd = sh.GetDict();
            if (const base::Value::List* sh_els = sd.FindList("elements")) {
              int sz = static_cast<int>(sh_els->size());
              element_count += sz;
              shadow_count += sz;
            }
          }
        }
      }
    }
    meta->Set("interactiveCount", element_count);
    meta->Set("shadowDomElementCount", shadow_count);

    StoreAutoCapturedPageMap(raw_snapshot);
    ResolvePageMapCallbacks(raw_snapshot);
    ResetPageMapState();
    return;
  }

  dashboard::PageMapExtractor extractor;

  if (const std::string* url = raw_snapshot.FindString("url"))
    extractor.set_url(GURL(*url));

  if (auto ts_double = raw_snapshot.FindDouble("timestamp")) {
    extractor.set_timestamp_ms(static_cast<int64_t>(*ts_double));
  } else if (auto ts_int = raw_snapshot.FindInt("timestamp")) {
    extractor.set_timestamp_ms(static_cast<int64_t>(*ts_int));
  }

  if (const std::string* focused =
          raw_snapshot.FindString("focusedElementId")) {
    extractor.set_focused_element_id(*focused);
  }

  if (const base::Value::List* elements = raw_snapshot.FindList("elements")) {
    for (const auto& element_value : *elements) {
      if (!element_value.is_dict())
        continue;
      const base::Value::Dict& elem_dict = element_value.GetDict();

      dashboard::PageElementSnapshot snapshot;
      if (const std::string* id = elem_dict.FindString("elementId"))
        snapshot.element_id = *id;
      if (const std::string* frame_id = elem_dict.FindString("frameId"))
        snapshot.frame_id = *frame_id;
      if (const std::string* tag = elem_dict.FindString("tag"))
        snapshot.tag = *tag;
      if (const std::string* role = elem_dict.FindString("role"))
        snapshot.role = *role;
      if (const std::string* type = elem_dict.FindString("type"))
        snapshot.type = *type;
      if (const std::string* text = elem_dict.FindString("text"))
        snapshot.text = *text;
      if (const std::string* acc = elem_dict.FindString("accessibilityName"))
        snapshot.accessibility_name = *acc;
      if (const std::string* css = elem_dict.FindString("cssSelector"))
        snapshot.css_selector = *css;
      if (const std::string* xpath = elem_dict.FindString("xpath"))
        snapshot.xpath = *xpath;
      if (const std::string* shadow_chain =
              elem_dict.FindString("shadowChainSelector"))
        snapshot.shadow_chain_selector = *shadow_chain;
      if (const std::string* resource = elem_dict.FindString("resourcePath"))
        snapshot.resource_path = *resource;
      snapshot.in_shadow_dom = MaybeBool(elem_dict, "inShadowDom");
      if (const std::string* shadow_host =
              elem_dict.FindString("shadowHostSelector"))
        snapshot.shadow_host_selector = *shadow_host;
      snapshot.clickable = MaybeBool(elem_dict, "clickable");
      snapshot.focusable = MaybeBool(elem_dict, "focusable");

      if (const base::Value::Dict* bounds = elem_dict.FindDict("bounds")) {
        float x = MaybeFloat(*bounds, "x");
        float y = MaybeFloat(*bounds, "y");
        float w = MaybeFloat(*bounds, "w");
        float h = MaybeFloat(*bounds, "h");
        snapshot.bounds = gfx::RectF(x, y, w, h);
      }

      extractor.AddElement(std::move(snapshot));
    }
  }

  base::Value::Dict pagemap = extractor.BuildPageMap();
  StoreAutoCapturedPageMap(pagemap);
//     if (global_site_map_snapshots_.size() > 10) {
//     PersistOldSnapshots();
//     global_site_map_snapshots_.erase(global_site_map_snapshots_.begin(),
//                                      global_site_map_snapshots_.end() - 1);
// }


  ResolvePageMapCallbacks(pagemap);
  ResetPageMapState();
}

void RecorderController::StartPageMapCapture() {
  TRACE_EVENT1("loading", "RecorderController::StartPageMapCapture", "reason",
               page_map_capture_reason_);
  page_map_capture_in_progress_ = true;
  LOG(INFO) << "[Recorder] PageMap capture reason=" << page_map_capture_reason_;
  content::WebContents* wc = target_web_contents_;
  if (!wc) {
    HandlePageMapFailure("no_web_contents");
    return;
  }

  dashboard::FetchUnifiedPageMapSnapshot(
      wc,
      base::BindOnce([](base::WeakPtr<RecorderController> self,
                        dashboard::UnifiedPageMapResult result) {
        if (!self) {
          return;
        }
        if (!result.ok) {
          self->HandlePageMapFailure(result.error);
          return;
        }
        self->DispatchPageMapResult(std::move(result.page_map));
      },
                     weak_factory_.GetWeakPtr()));
}

void RecorderController::OnPageMapExtractionFinished(base::Value::Dict pagemap) {
  StoreAutoCapturedPageMap(pagemap);
  ResolvePageMapCallbacks(pagemap);
  ResetPageMapState();
}

void RecorderController::ResolvePageMapCallbacks(
    const base::Value::Dict& pagemap) {
  auto callbacks = std::move(pending_page_map_callbacks_);
  pending_page_map_callbacks_.clear();
  for (auto& cb : callbacks) {
    if (cb)
      std::move(cb).Run(pagemap.Clone());
  }
}

void RecorderController::StoreAutoCapturedPageMap(
    const base::Value::Dict& pagemap) {
  TRACE_EVENT1("loading", "RecorderController::StoreAutoCapturedPageMap",
               "frame_count",
               static_cast<int>(pagemap.FindList("frames")
                                    ? pagemap.FindList("frames")->size()
                                    : 0u));
  last_page_map_snapshot_ = pagemap.Clone();
  last_page_map_snapshot_at_ = base::TimeTicks::Now();
  last_page_map_snapshot_url_ =
      pagemap.FindString("url") ? *pagemap.FindString("url") : current_url_;
  // Keep the last snapshot available for downstream consumers.
}

void RecorderController::ResetPageMapState() {
  page_map_capture_in_progress_ = false;
  page_map_capture_reason_.clear();
}

void RecorderController::ResetPageMapContextForNavigation() {
  page_map_context_id_.reset();
  page_map_frame_id_.clear();
  page_map_frame_request_pending_ = false;
  page_map_world_pending_ = false;
  last_page_map_snapshot_.reset();
  last_page_map_snapshot_at_ = base::TimeTicks();
  last_page_map_snapshot_url_.clear();
}

std::optional<base::Value::Dict> RecorderController::MaybeCloneRecentPageMapSnapshot(
    const std::string& reason) const {
  if (!last_page_map_snapshot_.has_value()) {
    return std::nullopt;
  }
  if (last_page_map_snapshot_at_.is_null() ||
      base::TimeTicks::Now() - last_page_map_snapshot_at_ >
          kRecentPageMapSnapshotReuseWindow) {
    return std::nullopt;
  }
  const std::string current_url = current_url_;
  if (!current_url.empty() && !last_page_map_snapshot_url_.empty() &&
      !IsGuidedReturnUrlMatch(current_url, last_page_map_snapshot_url_)) {
    return std::nullopt;
  }
  LOG(INFO) << "[Recorder] Reusing recent PageMap snapshot for " << reason;
  return last_page_map_snapshot_->Clone();
}

void RecorderController::HandlePageMapFailure(const std::string& reason) {
  TRACE_EVENT1("loading", "RecorderController::HandlePageMapFailure",
               "reason", reason);
  LOG(WARNING) << "[Recorder] PageMap capture failed: " << reason;
  if (locator_capture_armed_) {
    LOG(WARNING) << "[LocatorCapture] pagemap failed reason=" << reason
                 << " url="
                 << (current_url_.empty() ? std::string("<empty>")
                                          : current_url_);
  }
  base::Value::Dict empty;
  empty.Set("url", current_url_);
  empty.Set("timestamp", static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  empty.Set("frames", base::Value::List());
  base::Value::Dict meta;
  meta.Set("error", reason);
  empty.Set("metadata", std::move(meta));
  ResolvePageMapCallbacks(empty);
  ResetPageMapState();
}

void RecorderController::OnUserClickFromRenderer(base::Value::Dict click) {
  if (dashboard::IsRecordingSuppressed())
    return;

  const std::string source =
      click.FindString("__source") ? *click.FindString("__source") : "renderer";
  const std::optional<int> x_opt = ReadIntLike(click, "x");
  const std::optional<int> y_opt = ReadIntLike(click, "y");
  int click_count = ReadIntLike(click, "click_count").value_or(1);
  if (click_count < 0)
    click_count = 0;
  if (click_count > 3)
    click_count = 3;
  const std::string frame_id =
      click.FindString("frame_id") ? *click.FindString("frame_id") : "";
  base::Value::List recorded_locators = BuildRecordedLocatorBundleForClick(click);
  if (x_opt && y_opt && !HasMeaningfulRecordedLocator(recorded_locators)) {
    base::Value::List tracked_state_locators =
        BuildLocatorBundleForPoint(gfx::Point(*x_opt, *y_opt));
    if (HasMeaningfulRecordedLocator(tracked_state_locators)) {
      if (locator_capture_armed_) {
        LOG(INFO) << "[LocatorCapture] rehydrated non-visual bundle from "
                     "tracked state point="
                  << base::StringPrintf("%d,%d", *x_opt, *y_opt)
                  << " locators=" << tracked_state_locators.size()
                  << " [" << SummarizeLocatorTypesForLog(&tracked_state_locators)
                  << "]";
      }
      recorded_locators = std::move(tracked_state_locators);
    }
  }
  const base::TimeTicks now = base::TimeTicks::Now();

  if (is_recording_ && x_opt && y_opt) {
    const gfx::Point point(*x_opt, *y_opt);
    if (post_locator_capture_suppress_point_ &&
        !post_locator_capture_suppress_until_.is_null() &&
        now < post_locator_capture_suppress_until_ &&
        NearPointManhattan(*post_locator_capture_suppress_point_, point,
                           kPostLocatorCaptureSuppressDistancePx)) {
      return;
    }
    if (!post_locator_capture_suppress_until_.is_null() &&
        now >= post_locator_capture_suppress_until_) {
      post_locator_capture_suppress_point_.reset();
      post_locator_capture_suppress_until_ = base::TimeTicks();
    }
    if (click_count == 0 && !last_non_hover_click_at_.is_null() &&
        last_non_hover_click_point_ &&
        now - last_non_hover_click_at_ < kHoverAfterClickSuppressWindow &&
        NearPointManhattan(*last_non_hover_click_point_, point,
                           kHoverAfterClickSuppressDistancePx)) {
      return;
    }

    if (!last_recorded_click_at_.is_null() && last_recorded_click_point_ &&
        now - last_recorded_click_at_ < kDuplicateClickSuppressWindow &&
        NearPointManhattan(*last_recorded_click_point_, point,
                           kDuplicateClickMaxManhattanDistancePx) &&
        click_count == last_recorded_click_count_) {
      const bool same_frame = frame_id.empty() ||
                              last_recorded_click_frame_id_.empty() ||
                              frame_id == last_recorded_click_frame_id_;
      // Deduplicate cross-source captures (e.g. resolver + fallback + mojo)
      // while still allowing fast repeated clicks from a single source.
      if (same_frame && source != last_recorded_click_source_) {
        if (TryUpgradeRecentRecordedClick(
                click, recorded_locators, click_count,
                gfx::Point(*x_opt, *y_opt), frame_id, source)) {
          return;
        }
        return;
      }
      if (same_frame && source == last_recorded_click_source_ &&
          IsSyntheticClickSource(source) &&
          now - last_recorded_click_at_ <
              kSameSourceSyntheticDuplicateSuppressWindow) {
        return;
      }
    }
  }

  const bool guided_phase2_local_selection =
      is_recording_ && guided_loop_ui_state_.active &&
      guided_loop_ui_state_.phase == 2 && !locator_capture_armed_;
  if (guided_phase2_local_selection) {
    return;
  }

  // The loop target is already chosen. Clicks before field-selection mode are
  // not steps; recording them keeps a coordinate-only click that cannot resolve.
  if (is_recording_ && guided_loop_ui_state_.active &&
      guided_loop_ui_state_.phase == 1 && manual_loop_capture_active_ &&
      !locator_capture_armed_) {
    return;
  }

  const bool guided_phase1_loop_target =
      is_recording_ && guided_loop_ui_state_.active &&
      guided_loop_ui_state_.phase == 1 && !manual_loop_capture_active_ &&
      !locator_capture_armed_ &&
      x_opt && y_opt;
  if (guided_phase1_loop_target && !pending_manual_loop_capture_start_) {
    const gfx::Point point(*x_opt, *y_opt);
    // Guided phase-1 is one-click target selection: consume the click,
    // run loop candidate verification, and attempt to start loop capture
    // from that same click probe result once available.
    CancelClickFallback();
    guided_loop_target_confirmation_pending_ = false;
    guided_loop_target_probe_point_ = point;
    pending_manual_loop_capture_start_ = true;
    guided_loop_target_click_started_at_ = now;
    UpdateLoopPreviewAtPoint(point.x(), point.y(), /*active=*/true);
    return;
  }

  CancelClickFallback();

  const bool locator_capture_active = locator_capture_armed_;

  if (locator_capture_active) {
    LOG(INFO) << "[LocatorCapture] click source=" << source
              << " point="
              << (x_opt && y_opt ? base::StringPrintf("%d,%d", *x_opt, *y_opt)
                                 : std::string("<none>"))
              << " frame="
              << (frame_id.empty() ? std::string("<none>") : frame_id)
              << " locators=" << recorded_locators.size()
              << " [" << SummarizeLocatorTypesForLog(&recorded_locators) << "]";
  }

  if (!is_recording_ && !locator_capture_active) {
    LOG(INFO) << "[Recorder] Ignoring user_click (not recording)";
    return;
  }

  if (locator_capture_active) {
    const bool suppress_side_effects =
        locator_capture_suppress_recording_side_effects_;
    base::Value::Dict capture;
    capture.Set("ok", true);
    capture.Set("locators", recorded_locators.Clone());
    if (x_opt)
      capture.Set("x", *x_opt);
    if (y_opt)
      capture.Set("y", *y_opt);
    if (const std::string* captured_frame_id = click.FindString("frame_id"))
      capture.Set("frame_id", *captured_frame_id);
    if (const std::string* captured_shadow = click.FindString("shadow_chain"))
      capture.Set("shadow_chain", *captured_shadow);
    if (const std::string* resource_path = click.FindString("resource_path"))
      capture.Set("resource_path", *resource_path);
    if (const std::string* ax_role = click.FindString("ax_role"))
      capture.Set("ax_role", *ax_role);
    if (const std::string* ax_name = click.FindString("ax_name"))
      capture.Set("ax_name", *ax_name);
    if (const std::string* ax_name = click.FindString("accessibility_name")) {
      if (!capture.Find("ax_name")) {
        capture.Set("ax_name", *ax_name);
      }
    }
    if (const std::string* interactive_role =
            click.FindString("interactive_ancestor_role")) {
      capture.Set("interactive_ancestor_role", *interactive_role);
    }
    AugmentClickParamsFromPageMapSnapshot(capture, click);
    if (!current_url_.empty())
      capture.Set("page_url", current_url_);
    if (!current_url_.empty() && !capture.Find("context_url")) {
      capture.Set("context_url", current_url_);
    }
	  if (const std::string* warning = click.FindString("__warning")) {
	    capture.Set("warning", *warning);
	    if (*warning == "rich_locator_timeout_captured_xy") {
	      capture.Set("warning_message",
	                  "Rich locators timed out; captured XY only.");
	    }
	  }
	    base::Value::Dict capture_debug = BuildCaptureDebugState(
	        capture, recorded_locators, locator_capture_retry_count_, false,
	        last_page_map_snapshot_.has_value(), source);
	    capture.Set("debug", capture_debug.Clone());
	    LOG(WARNING) << "[LocatorCapture][Final] source=" << source
	                 << " locators=" << recorded_locators.size() << " ["
	                 << SummarizeLocatorTypesForLog(&recorded_locators) << "] "
	                 << SummarizeMetadataFields(capture)
	                 << " retries=" << locator_capture_retry_count_;
	    CompleteOneShotLocatorCapture(std::move(capture));
	    if (suppress_side_effects) {
	      return;
	    }
  }

  dashboard::PageMapExtractor extractor;
  dashboard::PageElementSnapshot snap;
  if (const std::string* css = click.FindString("css")) snap.css_selector = *css;
  if (const std::string* xp = click.FindString("xpath")) snap.xpath = *xp;
  snap.locator_bundle = recorded_locators.Clone();
  if (const std::string* acc = click.FindString("accessibility_name"))
    snap.accessibility_name = *acc;
  if (const std::string* role = click.FindString("role"))
    snap.role = *role;
  if (auto disabled = click.FindBool("is_disabled"))
    snap.disabled = *disabled;
  if (const std::string* sh = click.FindString("shadow_chain")) {
    snap.shadow_chain_selector = *sh;
    snap.in_shadow_dom = !sh->empty();
  }
  if (const std::string* fid = click.FindString("frame_id")) snap.frame_id = *fid;
  if (x_opt && y_opt) {
    snap.bounds = gfx::RectF(static_cast<float>(*x_opt), static_cast<float>(*y_opt), 1.0f, 1.0f);
  }
  snap.type = "clickable";
  snap.clickable = true;
  extractor.AddElement(std::move(snap));
  base::Value::Dict pagemap = extractor.BuildPageMap();
  // Tag with the current page URL for downstream consumers.
  pagemap.Set("url", current_url_);
  DispatchPageMapResult(std::move(pagemap));

  // Also append a simple 'click' action to the timeline when recording.
  if (is_recording_) {
    base::Value::Dict action;
    action.Set("action", "click");
    base::Value::Dict params;
    params.Set("click_count", click_count);
    params.Set("clickCount", click_count);
    if (click_count == 0)
      params.Set("semantic_intent", "hover");

    if (x_opt && y_opt) {
      base::Value::Dict coords;
      coords.Set("x", *x_opt);
      coords.Set("y", *y_opt);
      params.Set("coordinates", std::move(coords));
    }

    base::Value::List out_locs = recorded_locators.Clone();

    if (!out_locs.empty())
      params.Set("locators", std::move(out_locs));
    if (x_opt) params.Set("x", *x_opt);
    if (y_opt) params.Set("y", *y_opt);
    if (x_opt && y_opt)
      last_user_click_point_ = gfx::Point(*x_opt, *y_opt);
    if (const std::string* fid = click.FindString("frame_id"))
      params.Set("frame_id", *fid);
    if (auto dom_node_id = click.FindInt("dom_node_id"))
      params.Set("dom_node_id", *dom_node_id);
    if (const std::string* acc = click.FindString("accessibility_name"))
      params.Set("accessibility_name", *acc);
    if (const std::string* role = click.FindString("role"))
      params.Set("role", *role);
    if (auto disabled = click.FindBool("is_disabled"))
      params.Set("disabled", *disabled);
    AugmentClickParamsFromPageMapSnapshot(params, click);
    action.Set("params", std::move(params));
    StampActionContextUrl(&action, current_url_);
    base::Value::Dict meta;
    base::Value::Dict recording_diagnostics;
    recording_diagnostics.Set(
        "raw_click_locators_present",
        click.FindList("locators") && !click.FindList("locators")->empty());
    recording_diagnostics.Set("fell_back_to_visual_coords_only",
                              !recorded_locators.empty() &&
                                  !HasNonVisualRecordedLocator(recorded_locators));
    base::Value::List bundle_types;
    for (const auto& locator_value : recorded_locators) {
      const base::Value::Dict* locator = locator_value.GetIfDict();
      const std::string* type = locator ? locator->FindString("type") : nullptr;
      if (!type) {
        continue;
      }
      bundle_types.Append(*type);
    }
    recording_diagnostics.Set("bundle_types_recorded", std::move(bundle_types));
    meta.Set("recordingDiagnostics", std::move(recording_diagnostics));
    action.Set("meta", std::move(meta));
    AppendRecordedAction(std::move(action));

    if (x_opt && y_opt) {
      last_recorded_click_at_ = now;
      last_recorded_click_point_ = gfx::Point(*x_opt, *y_opt);
      last_recorded_click_count_ = click_count;
      last_recorded_click_frame_id_ = frame_id;
      last_recorded_click_source_ = source;
      if (click_count > 0) {
        last_non_hover_click_at_ = now;
        last_non_hover_click_point_ = gfx::Point(*x_opt, *y_opt);
      }
    }
  }
}

void RecorderController::AugmentClickParamsFromPageMapSnapshot(
    base::Value::Dict& params,
    const base::Value::Dict& click) {
  if (params.Find("frame_id") && params.Find("shadow_chain") &&
      params.Find("resource_path") && params.Find("ax_role") &&
      params.Find("ax_name")) {
    return;
  }
	  if (!last_page_map_snapshot_.has_value()) {
	    if (locator_capture_armed_) {
	      LOG(INFO) << "[LocatorCapture][PageMapMeta] no page map snapshot";
	    }
	    return;
	  }

  auto find_num = [&](const char* key) -> std::optional<double> {
    if (auto d = click.FindDouble(key))
      return *d;
    if (auto i = click.FindInt(key))
      return static_cast<double>(*i);
    return std::nullopt;
  };
  const std::optional<double> x = find_num("x");
  const std::optional<double> y = find_num("y");
  if (!x || !y) {
    return;
  }

  const gfx::Point point(static_cast<int>(*x), static_cast<int>(*y));
  const base::Value::List* frames = last_page_map_snapshot_->FindList("frames");
	  if (!frames) {
	    if (locator_capture_armed_) {
	      LOG(INFO) << "[LocatorCapture][PageMapMeta] page map has no frames";
	    }
	    return;
	  }

  const base::Value::Dict* best_element = nullptr;
  const base::Value::Dict* best_selectors = nullptr;
  std::string best_frame_id;
  double best_score = 0.0;

  auto consider_element =
      [&](const base::Value::Dict& element, const std::string& frame_id) {
        const base::Value::Dict* selectors = element.FindDict("selectors");
        if (!selectors) {
          return;
        }
        const std::string element_frame_id =
            element.FindString("frameId") ? *element.FindString("frameId")
                                          : frame_id;
        const double score = ScorePointAgainstPageMapElement(
            point, element, /*require_containment=*/true);
        if (score <= best_score) {
          return;
        }
        best_score = score;
        best_element = &element;
        best_selectors = selectors;
        best_frame_id = element_frame_id;
      };

  for (const auto& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    if (!frame) {
      continue;
    }
    const std::string frame_id =
        frame->FindString("frameId") ? *frame->FindString("frameId")
                                     : std::string();
    if (const base::Value::List* elements = frame->FindList("elements")) {
      for (const auto& element_value : *elements) {
        const base::Value::Dict* element = element_value.GetIfDict();
        if (element) {
          consider_element(*element, frame_id);
        }
      }
    }
    if (const base::Value::List* shadow_roots = frame->FindList("shadowRoots")) {
      for (const auto& shadow_value : *shadow_roots) {
        const base::Value::Dict* shadow_root = shadow_value.GetIfDict();
        if (!shadow_root) {
          continue;
        }
        if (const base::Value::List* elements = shadow_root->FindList("elements")) {
          for (const auto& element_value : *elements) {
            const base::Value::Dict* element = element_value.GetIfDict();
            if (element) {
              consider_element(*element, frame_id);
            }
          }
        }
      }
    }
  }

	  if (best_score <= 0.0 || !best_element) {
	    if (locator_capture_armed_) {
	      LOG(INFO) << "[LocatorCapture][PageMapMeta] no point match x="
	                << point.x() << " y=" << point.y();
	    }
	    return;
	  }

  if (!best_frame_id.empty() && !params.Find("frame_id")) {
    params.Set("frame_id", best_frame_id);
  }
  if (best_selectors) {
    if (const std::string* shadow_chain = best_selectors->FindString("shadowChain");
        shadow_chain && !params.Find("shadow_chain")) {
      params.Set("shadow_chain", *shadow_chain);
    }
    if (const std::string* resource_path = best_selectors->FindString("resourcePath");
        resource_path && !IsRootLikeResourcePath(*resource_path) &&
        !params.Find("resource_path")) {
      params.Set("resource_path", *resource_path);
    }
  }
  if (const std::string* role = best_element->FindString("role");
      role && !IsUnknownAxRoleValue(*role)) {
    if (!params.Find("role")) {
      params.Set("role", *role);
    }
    if (!params.Find("ax_role")) {
      params.Set("ax_role", *role);
    }
    if (!params.Find("interactive_ancestor_role") &&
        IsInteractiveAxRole(NormalizeAxRoleString(*role))) {
      params.Set("interactive_ancestor_role", NormalizeAxRoleString(*role));
    }
  }
  if (const std::string* accessibility_name =
          best_element->FindString("accessibilityName")) {
    if (!params.Find("accessibility_name")) {
      params.Set("accessibility_name", *accessibility_name);
    }
    if (!params.Find("ax_name")) {
      params.Set("ax_name", *accessibility_name);
    }
  }
  if (const std::string* interactive_role =
          best_element->FindString("interactiveAncestorRole")) {
    if (!params.Find("interactive_ancestor_role")) {
      params.Set("interactive_ancestor_role", *interactive_role);
    }
  }
	  if (!current_url_.empty() && !params.Find("context_url")) {
	    params.Set("context_url", current_url_);
	  }
	  if (locator_capture_armed_) {
	    LOG(INFO) << "[LocatorCapture][PageMapMeta] score=" << best_score
	              << " frame="
	              << (best_frame_id.empty() ? std::string("<none>")
	                                        : best_frame_id)
	              << " " << SummarizeMetadataFields(params);
	  }
	}

std::optional<base::Value::Dict>
RecorderController::BuildElementReportForIndex(int index) const {
  if (index < 0)
    return std::nullopt;
  if (!last_page_map_snapshot_)
    return std::nullopt;
  base::Value::Dict report;
  if (const std::string* url = last_page_map_snapshot_->FindString("url"))
    report.Set("url", *url);
  const base::Value::List* frames = last_page_map_snapshot_->FindList("frames");
  if (!frames)
    return std::nullopt;

  int current_index = 0;
  auto try_elements =
      [&](const base::Value::List* elements,
          const std::string& frame_id,
          const std::string& shadow_host_selector)
      -> std::optional<base::Value::Dict> {
    if (!elements) {
      return std::nullopt;
    }
    for (const auto& element_value : *elements) {
      const base::Value::Dict* element = element_value.GetIfDict();
      if (!element) {
        continue;
      }
      if (current_index == index) {
        base::Value::Dict found;
        found.Set("index", index);
        found.Set("element", element->Clone());
        if (!frame_id.empty()) {
          found.Set("frame_id", frame_id);
        }
        if (!shadow_host_selector.empty()) {
          found.Set("shadow_host_selector", shadow_host_selector);
        }
        return found;
      }
      ++current_index;
    }
    return std::nullopt;
  };

  for (const auto& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    if (!frame) {
      continue;
    }
    const std::string* frame_id_ptr = frame->FindString("frameId");
    const std::string frame_id = frame_id_ptr ? *frame_id_ptr : std::string();
    if (auto found = try_elements(frame->FindList("elements"), frame_id, "")) {
      report.Merge(std::move(*found));
      return report;
    }
    if (const base::Value::List* shadow_roots = frame->FindList("shadowRoots")) {
      for (const auto& shadow_value : *shadow_roots) {
        const base::Value::Dict* shadow = shadow_value.GetIfDict();
        if (!shadow) {
          continue;
        }
        const std::string* host_selector_ptr =
            shadow->FindString("hostSelector");
        if (auto found = try_elements(
                shadow->FindList("elements"), frame_id,
                host_selector_ptr ? *host_selector_ptr : std::string())) {
          report.Merge(std::move(*found));
          return report;
        }
      }
    }
  }

  return std::nullopt;
}

void RecorderController::ArmOneShotLocatorCapture(
    base::TimeDelta timeout,
    bool suppress_recording_side_effects,
    base::OnceCallback<void(base::Value::Dict)> callback) {
  if (callback.is_null())
    return;

  if (locator_capture_armed_) {
    CancelOneShotLocatorCapture("locator_capture_replaced");
  }

  locator_capture_armed_ = true;
  locator_capture_ready_ = false;
  locator_capture_suppress_recording_side_effects_ =
      suppress_recording_side_effects;
  locator_capture_retry_count_ = 0;
  locator_capture_armed_at_ = base::TimeTicks::Now();
  locator_capture_last_input_at_.reset();
  locator_capture_callback_ = std::move(callback);
  RefreshOneShotCaptureClickSuppression();
  UpdateInstrumentationState();

  LOG(INFO) << "[LocatorCapture] armed url="
            << (current_url_.empty() ? std::string("<empty>") : current_url_)
            << " timeout_ms="
            << (timeout.is_positive() ? timeout.InMilliseconds() : -1)
            << " suppress_side_effects="
            << (suppress_recording_side_effects ? 1 : 0)
            << " has_recent_pagemap="
            << (last_page_map_snapshot_.has_value() ? 1 : 0);

  locator_capture_timeout_timer_.Stop();
  if (timeout.is_positive()) {
    locator_capture_timeout_timer_.Start(
        FROM_HERE, timeout,
        base::BindOnce(
            [](base::WeakPtr<RecorderController> weak) {
              if (!weak)
                return;
              base::Value::Dict out;
              out.Set("ok", false);
              out.Set("error", "locator_capture_timeout");
              weak->CompleteOneShotLocatorCapture(std::move(out));
            },
            weak_factory_.GetWeakPtr()));
  }

  // Soft assist only: prewarm APC+sidecar PageMap for geometry matching if
  // Mojo hit-test fails. Primary click path is ResolveAtPoint (single node).
  LOG(INFO) << "[LocatorCapture] prewarm_pagemap requested (apc_geometry_assist)";
  CapturePageMap(base::BindOnce([](base::Value::Dict) {}));
  NotifyStateChanged();
}

void RecorderController::CancelOneShotLocatorCapture(const std::string& error) {
  if (!locator_capture_armed_) {
    return;
  }
  base::Value::Dict canceled;
  canceled.Set("ok", false);
  canceled.Set("error",
               error.empty() ? std::string("locator_capture_canceled") : error);
  CompleteOneShotLocatorCapture(std::move(canceled));
}

void RecorderController::CompleteOneShotLocatorCapture(base::Value::Dict result) {
  const bool suppress_side_effects =
      locator_capture_suppress_recording_side_effects_;
  const bool ok = result.FindBool("ok").value_or(false);
  const base::Value::List* locators = result.FindList("locators");
  LOG(INFO) << "[LocatorCapture] complete ok=" << (ok ? 1 : 0)
            << " error="
            << (result.FindString("error") ? *result.FindString("error")
                                           : std::string("<none>"))
            << " warning="
            << (result.FindString("warning") ? *result.FindString("warning")
                                             : std::string("<none>"))
            << " page_url="
            << (result.FindString("page_url") ? *result.FindString("page_url")
                                              : std::string("<none>"))
            << " locators=" << (locators ? locators->size() : 0)
            << " [" << SummarizeLocatorTypesForLog(locators) << "]";
  if (ok && locators && !locators->empty()) {
    ++g_locator_capture_sequence;
    result.Set(
        "sequence",
        static_cast<double>(g_locator_capture_sequence));
    if (!result.Find("captured_at_ms")) {
      result.Set(
          "captured_at_ms",
          static_cast<double>(
              base::Time::Now().InMillisecondsSinceUnixEpoch()));
    }
    if (!result.Find("page_url") && !current_url_.empty()) {
      result.Set("page_url", current_url_);
    }
    LatestLocatorCaptureResult() = result.Clone();
    g_has_locator_capture_result = true;
  }

  if (suppress_side_effects) {
    if (const std::optional<int> x = ReadIntLike(result, "x"),
        y = ReadIntLike(result, "y");
        x && y) {
      post_locator_capture_suppress_point_ = gfx::Point(*x, *y);
      post_locator_capture_suppress_until_ =
          base::TimeTicks::Now() +
          kPostLocatorCaptureDuplicateSuppressWindow;
    }
  }

  if (locator_capture_armed_at_) {
    const int64_t total_ms =
        (base::TimeTicks::Now() - *locator_capture_armed_at_).InMilliseconds();
    int64_t input_ms = -1;
    if (locator_capture_last_input_at_) {
      input_ms =
          (base::TimeTicks::Now() - *locator_capture_last_input_at_).InMilliseconds();
    }
    LOG(INFO) << "[Recorder][Perf] locator_capture done ok="
              << (ok ? "true" : "false")
              << " total_ms=" << total_ms
              << " from_input_ms=" << input_ms;
  }

	if (guided_loop_ui_state_.active && guided_loop_ui_state_.capture_pending) {
	  guided_loop_ui_state_.capture_pending = false;
	  if (ok && locators && !locators->empty()) {
	    guided_loop_ui_state_.capture_sequence = g_locator_capture_sequence;
	  }
	}

	locator_capture_timeout_timer_.Stop();
	CancelClickFallback();
  locator_capture_armed_ = false;
  locator_capture_ready_ = false;
  locator_capture_retry_count_ = 0;
  locator_capture_armed_at_.reset();
  locator_capture_last_input_at_.reset();
  locator_capture_suppress_recording_side_effects_ = false;
  ClearCaptureHoverPreview();
  RefreshOneShotCaptureClickSuppression();
  UpdateInstrumentationState();
  if (!locator_capture_callback_.is_null()) {
    std::move(locator_capture_callback_).Run(std::move(result));
  }
  NotifyStateChanged();
}

void RecorderController::SetGuidedLoopFirstClickGuardArmed(bool armed) {
  if (guided_loop_first_click_guard_armed_ == armed) {
    // The renderer-side guard self-disarms after one consumed click; force a
    // refresh when we still logically need it armed.
    if (armed) {
      RefreshOneShotCaptureClickSuppression();
    }
    return;
  }
  guided_loop_first_click_guard_armed_ = armed;
  RefreshOneShotCaptureClickSuppression();
}

void RecorderController::RefreshOneShotCaptureClickSuppression() {
  const bool guided_phase2_intercept =
      is_recording_ && guided_loop_ui_state_.active &&
      guided_loop_ui_state_.phase == 2;
  const bool should_arm =
      (locator_capture_armed_ &&
       locator_capture_suppress_recording_side_effects_) ||
      guided_loop_first_click_guard_armed_ || guided_phase2_intercept;
  SetOneShotCaptureClickSuppression(should_arm,
                                    /*persistent=*/guided_phase2_intercept);
}

bool RecorderController::IsGuidedInterceptionPhaseActive() const {
  return is_recording_ && guided_loop_ui_state_.active &&
         (guided_loop_ui_state_.phase == 1 ||
          guided_loop_ui_state_.phase == 2);
}

void RecorderController::SetOneShotCaptureClickSuppression(bool armed,
                                                           bool persistent) {
  // Interception lives in the browser input path (InputObserver), so it
  // survives navigations and needs no per-document setup.
  page_click_interception_armed_ = armed;
}

bool RecorderController::ShouldInterceptPageClicks() const {
  return page_click_interception_armed_ && target_web_contents_ &&
         !dashboard::IsRecordingSuppressed();
}

void RecorderController::SetGuidedLoopSameTabNavigationGuard(
    bool enabled,
    bool block_all_anchor_navigation) {
  // New-tab opens are folded back into the recorded tab in
  // DidOpenRequestedURL. Blocking all navigation during loop-target picking
  // is covered by click interception (the first-click guard).
  guided_same_tab_navigation_guard_enabled_ = enabled;
}

void RecorderController::WaitForOneShotCaptureReady(
    base::TimeDelta timeout,
    base::OnceCallback<void(bool, std::string)> callback) {
  if (!locator_capture_armed_) {
    std::move(callback).Run(false, "locator_capture_not_armed");
    return;
  }
  AttachInputObserver();
  if (!input_observer_) {
    std::move(callback).Run(false, "recorder_not_attached");
    return;
  }
  if (!locator_capture_ready_) {
    locator_capture_ready_ = true;
    NotifyStateChanged();
  }
  std::move(callback).Run(true, std::string());
}

dashboard::mojom::StealthyElementResolver*
RecorderController::MainFrameResolver() {
  content::RenderFrameHost* rfh =
      target_web_contents_ ? target_web_contents_->GetPrimaryMainFrame()
                           : nullptr;
  if (!rfh || !rfh->IsRenderFrameLive()) {
    return nullptr;
  }
  if (!main_frame_resolver_.is_bound() ||
      !main_frame_resolver_.is_connected() ||
      main_frame_resolver_frame_id_ != rfh->GetGlobalId()) {
    main_frame_resolver_.reset();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(&main_frame_resolver_);
    main_frame_resolver_frame_id_ = rfh->GetGlobalId();
  }
  return main_frame_resolver_.is_bound() ? main_frame_resolver_.get()
                                         : nullptr;
}

bool RecorderController::ResetGuidedLoopStepOneForTools(
    std::string* error_out) {
  if (!guided_loop_ui_state_.active || guided_loop_ui_state_.phase != 1) {
    if (error_out) {
      *error_out = "guided_step1_not_active";
    }
    return false;
  }
  if (!IsGuidedReturnUrlMatch(current_url_, guided_loop_origin_url_)) {
    if (error_out) {
      *error_out = "navigate_to_loop_origin_first";
    }
    return false;
  }

  CancelOneShotLocatorCapture("guided_capture_canceled");
  pending_manual_loop_capture_start_ = false;
  guided_loop_target_confirmation_pending_ = false;
  guided_loop_target_probe_point_.reset();
  guided_loop_target_click_started_at_.reset();
  guided_loop_ui_state_.capture_pending = false;
  guided_loop_ui_state_.capture_kind = 0;
  ClearGuidedScrapeSelections();
  ClearLoopPreview();

  bool removed_loop = false;
  if (manual_loop_capture_active_) {
    manual_loop_capture_active_ = false;
    const std::optional<size_t> loop_index =
        FindLatestManualLoopCaptureActionIndex();
    if (loop_index.has_value()) {
      RemoveRecordedActionsFromIndex(*loop_index);
      removed_loop = true;
    }
  }

  SetGuidedLoopFirstClickGuardArmed(true);
  const bool block_all_anchor_navigation =
      guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
      !manual_loop_capture_active_;
  SetGuidedLoopSameTabNavigationGuard(true, block_all_anchor_navigation);
  guided_same_tab_navigation_guard_enabled_ = true;
  RefreshOneShotCaptureClickSuppression();

  if (!removed_loop) {
    NotifyStateChanged();
  }
  return true;
}

void RecorderController::PopGuidedScrapeSelectionForTools(
    base::OnceCallback<void(bool ok,
                            uint32_t remaining_count,
                            std::string error)> callback) {
  if (!guided_scrape_selection_locators_.empty()) {
    guided_scrape_selection_locators_.pop_back();
    guided_scrape_selection_signatures_.pop_back();
    guided_scrape_selection_model_json_.clear();
    PopGuidedScrapeOverlaySelection();
  }
  guided_loop_ui_state_.scrape_target_count =
      static_cast<uint32_t>(guided_scrape_selection_locators_.size());
  NotifyStateChanged();
  std::move(callback).Run(
      true, guided_loop_ui_state_.scrape_target_count, std::string());
}

bool RecorderController::PopLastManualLoopStepForTools(
    std::string* removed_action_out,
    std::string* error_out) {
  if (!manual_loop_capture_active_ || recorded_actions_.empty()) {
    if (error_out) {
      *error_out = "manual_loop_capture_inactive";
    }
    return false;
  }

  const std::optional<size_t> loop_index =
      FindLatestManualLoopCaptureActionIndex();
  if (!loop_index.has_value()) {
    if (error_out) {
      *error_out = "loop_action_unavailable";
    }
    return false;
  }
  base::Value::Dict* tail = recorded_actions_[*loop_index].GetIfDict();

  base::Value::List* steps = tail->FindList("steps");
  if (!steps || steps->empty()) {
    if (error_out) {
      *error_out = "loop_steps_empty";
    }
    return false;
  }

  const base::Value& removed = (*steps)[steps->size() - 1];
  if (removed_action_out) {
    removed_action_out->clear();
  }
  if (const base::Value::Dict* removed_dict = removed.GetIfDict()) {
    if (const std::string* action = removed_dict->FindString("action")) {
      if (removed_action_out) {
        *removed_action_out = *action;
      }
    }
  }
  steps->erase(steps->begin() + static_cast<ptrdiff_t>(steps->size() - 1));
  dashboard::ScriptSchema::CanonicalizeAction(*tail);
  UpdateRecordedActionSignatureAt(*loop_index);
  NotifyStateChanged();
  return true;
}

bool RecorderController::HandleGuidedPageArrowShortcut(int windows_key_code) {
  if (!guided_loop_ui_state_.active) {
    return false;
  }

  constexpr int kLeftArrow = 0x25;
  constexpr int kRightArrow = 0x27;

  if (windows_key_code == kRightArrow) {
    if (guided_loop_ui_state_.phase == 1 && manual_loop_capture_active_) {
      guided_loop_ui_state_.phase = 2;
      guided_loop_ui_state_.capture_pending = false;
      guided_loop_ui_state_.capture_kind = 0;
      ClearGuidedScrapeSelections();
      RefreshOneShotCaptureClickSuppression();
      SetGuidedScrapeOverlayEnabled(true);
      UpdateInstrumentationState();
      NotifyStateChanged();
      return true;
    }
    return false;
  }

  if (windows_key_code != kLeftArrow) {
    return false;
  }

  if (guided_loop_ui_state_.phase == 2) {
    if (!guided_scrape_selection_locators_.empty()) {
      guided_scrape_selection_locators_.pop_back();
      guided_scrape_selection_signatures_.pop_back();
      guided_scrape_selection_model_json_.clear();
      guided_loop_ui_state_.scrape_target_count =
          static_cast<uint32_t>(guided_scrape_selection_locators_.size());
      PopGuidedScrapeOverlaySelection();
      NotifyStateChanged();
      return true;
    }
    if (!IsGuidedReturnUrlMatch(current_url_, guided_loop_origin_url_)) {
      return false;
    }
    guided_loop_ui_state_.phase = 1;
    guided_loop_ui_state_.capture_pending = false;
    guided_loop_ui_state_.capture_kind = 0;
    RefreshOneShotCaptureClickSuppression();
    SetGuidedScrapeOverlayEnabled(false);
    UpdateInstrumentationState();
    NotifyStateChanged();
    return true;
  }

  if (guided_loop_ui_state_.phase == 1 && manual_loop_capture_active_) {
    std::string ignored_error;
    return ResetGuidedLoopStepOneForTools(&ignored_error);
  }

  return false;
}

void RecorderController::CaptureScreenshot(
    base::OnceCallback<void(std::string /*base64_png*/)> cb,
    std::optional<gfx::RectF> clip_override) {
  if (!agent_host_ || !devtools_client_) {
    std::move(cb).Run(std::string());
    return;
  }

  // Compute a square clip region based on the current viewport so that
  // screenshots are normalized to 448x448 while still reflecting the
  // desktop layout (no forced window resize here).
  int vw = 0;
  int vh = 0;
  if (target_web_contents_) {
    if (auto* view = target_web_contents_->GetRenderWidgetHostView()) {
      gfx::Rect bounds = view->GetViewBounds();
      vw = bounds.width();
      vh = bounds.height();
    }
  }

  base::Value::Dict params;
  params.Set("format", "png");
  params.Set("fromSurface", true);

  const int side = (vw > 0 && vh > 0) ? std::min(vw, vh) : 0;
  if (side > 0) {
    constexpr double kTargetPaddingScale = 1.35;
    constexpr float kMinimumTargetSide = 240.0f;
    constexpr int kScreenshotOutputSide = 448;

    base::Value::Dict clip;
    if (clip_override.has_value() && !clip_override->IsEmpty()) {
      gfx::RectF requested = *clip_override;
      const float extra_width =
          requested.width() * static_cast<float>(kTargetPaddingScale - 1.0);
      const float extra_height =
          requested.height() * static_cast<float>(kTargetPaddingScale - 1.0);
      requested.set_x(requested.x() - extra_width * 0.5f);
      requested.set_y(requested.y() - extra_height * 0.5f);
      requested.set_width(requested.width() + extra_width);
      requested.set_height(requested.height() + extra_height);
      const float target_side =
          std::clamp(std::max({requested.width(), requested.height(),
                               kMinimumTargetSide}),
                     1.0f, static_cast<float>(side));
      float clip_x = requested.CenterPoint().x() - target_side / 2.0f;
      float clip_y = requested.CenterPoint().y() - target_side / 2.0f;
      clip_x = std::clamp(clip_x, 0.0f,
                          std::max(0.0f, static_cast<float>(vw) - target_side));
      clip_y = std::clamp(clip_y, 0.0f,
                          std::max(0.0f, static_cast<float>(vh) - target_side));
      clip.Set("x", clip_x);
      clip.Set("y", clip_y);
      clip.Set("width", target_side);
      clip.Set("height", target_side);
      clip.Set("scale",
               static_cast<double>(kScreenshotOutputSide) / target_side);
    } else {
      const int clip_x = (vw > side) ? (vw - side) / 2 : 0;
      const int clip_y = 0;  // focus on above-the-fold content
      clip.Set("x", clip_x);
      clip.Set("y", clip_y);
      clip.Set("width", side);
      clip.Set("height", side);
      clip.Set("scale",
               static_cast<double>(kScreenshotOutputSide) /
                   static_cast<double>(side));
    }
    params.Set("clip", std::move(clip));
  }

  SendCommandExpectResponse(
      "Page.captureScreenshot", std::move(params),
      base::BindOnce(
          [](base::OnceCallback<void(std::string)> cb,
             std::string res_json, std::string /*err_json*/) {
            std::string out;
            if (!res_json.empty()) {
              std::optional<base::Value> v = base::JSONReader::Read(
                  res_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
              if (v && v->is_dict()) {
                const base::Value::Dict& d = v->GetDict();
                if (const std::string* data = d.FindString("data"))
                  out = *data;
              }
            }
            std::move(cb).Run(std::move(out));
          },
          std::move(cb)));
}

void RecorderController::SendCommand(const std::string& method,
                                     base::Value::Dict params) {
  const bool is_domain_enable =
      method == "DOM.enable" || method == "Runtime.enable" ||
      method == "Network.enable";
  if (!is_domain_enable &&
      (base::StartsWith(method, "DOM.") ||
       base::StartsWith(method, "Runtime.") ||
       base::StartsWith(method, "Page.") ||
       base::StartsWith(method, "Input.") ||
       base::StartsWith(method, "Network."))) {
    if (base::StartsWith(method, "Network.")) {
      EnsureNetworkCaptureEnabled();
    } else {
      EnsureCdpDomainsEnabled();
    }
  }
  if (!agent_host_ || !devtools_client_)
    return;
  const int id = next_command_id_++;
  base::Value::Dict msg;
  msg.Set("id", id);
  msg.Set("method", method);
  msg.Set("params", std::move(params));
  std::string json;
  base::JSONWriter::Write(msg, &json);
  agent_host_->DispatchProtocolMessage(devtools_client_.get(), base::as_byte_span(json));
}

int RecorderController::SendCommandExpectResponse(
    const std::string& method,
    base::Value::Dict params,
    base::OnceCallback<void(std::string, std::string)> cb) {
  const bool is_domain_enable =
      method == "DOM.enable" || method == "Runtime.enable" ||
      method == "Network.enable";
  if (!is_domain_enable &&
      (base::StartsWith(method, "DOM.") ||
       base::StartsWith(method, "Runtime.") ||
       base::StartsWith(method, "Page.") ||
       base::StartsWith(method, "Input.") ||
       base::StartsWith(method, "Network."))) {
    if (base::StartsWith(method, "Network.")) {
      EnsureNetworkCaptureEnabled();
    } else {
      EnsureCdpDomainsEnabled();
    }
  }
  if (!agent_host_ || !devtools_client_) {
    std::move(cb).Run(std::string(), std::string("{\"message\":\"not attached\"}"));
    return -1;
  }
  const int id = next_command_id_++;
  pending_eval_[id] = std::move(cb);
  base::Value::Dict msg;
  msg.Set("id", id);
  msg.Set("method", method);
  msg.Set("params", std::move(params));
  std::string json;
  base::JSONWriter::Write(msg, &json);
  agent_host_->DispatchProtocolMessage(devtools_client_.get(), base::as_byte_span(json));
  return id;
}

void RecorderController::OnDevToolsResponse(int id,
                                            base::Value::Dict result,
                                            base::Value::Dict error) {
  auto it = pending_eval_.find(id);
  if (it == pending_eval_.end())
    return;
  auto cb = std::move(it->second);
  pending_eval_.erase(it);
  std::string res_json;
  std::string err_json;
  base::JSONWriter::Write(result, &res_json);
  base::JSONWriter::Write(error, &err_json);
  if (cb)
    std::move(cb).Run(std::move(res_json), std::move(err_json));
}

void RecorderController::FlushTypeBuffer() {
  FlushPendingTypeBuffer();
}
void RecorderController::RemoveRecordedAction(size_t index) {
  if (index >= recorded_actions_.size()) return;
  base::Value::List new_list;
  std::vector<std::string> new_signatures;
  new_signatures.reserve(recorded_actions_.size() - 1);
  size_t i = 0;
  for (const auto& v : recorded_actions_) {
    if (i == index) {
      ++i;
      continue;
    }
    new_list.Append(v.Clone());
    if (i < recorded_action_signatures_.size()) {
      new_signatures.push_back(recorded_action_signatures_[i]);
    } else if (const base::Value::Dict* action_dict = v.GetIfDict()) {
      new_signatures.push_back(BuildAutoLoopSignature(*action_dict));
    } else {
      new_signatures.emplace_back();
    }
    ++i;
  }
  recorded_actions_ = std::move(new_list);
  recorded_action_signatures_ = std::move(new_signatures);
  NotifyStateChanged();
}

std::optional<size_t> RecorderController::FindLatestManualLoopCaptureActionIndex()
    const {
  for (size_t i = recorded_actions_.size(); i > 0; --i) {
    const base::Value::Dict* action = recorded_actions_[i - 1].GetIfDict();
    const std::string action_name =
        (action && action->FindString("action")) ? *action->FindString("action")
                                                 : std::string();
    if (!action ||
        !base::EqualsCaseInsensitiveASCII(action_name, "loop")) {
      continue;
    }
    const base::Value::Dict* params = action->FindDict("params");
    if (params && params->FindBool("manualLoopCaptureActive").value_or(false)) {
      return i - 1;
    }
  }
  return std::nullopt;
}

void RecorderController::RemoveRecordedActionsFromIndex(size_t index) {
  if (index >= recorded_actions_.size()) {
    return;
  }
  base::Value::List new_list;
  std::vector<std::string> new_signatures;
  new_list.reserve(index);
  new_signatures.reserve(std::min(index, recorded_action_signatures_.size()));
  for (size_t i = 0; i < index; ++i) {
    new_list.Append(recorded_actions_[i].Clone());
    if (i < recorded_action_signatures_.size()) {
      new_signatures.push_back(recorded_action_signatures_[i]);
    } else if (const base::Value::Dict* action_dict =
                   recorded_actions_[i].GetIfDict()) {
      new_signatures.push_back(BuildAutoLoopSignature(*action_dict));
    } else {
      new_signatures.emplace_back();
    }
  }
  recorded_actions_ = std::move(new_list);
  recorded_action_signatures_ = std::move(new_signatures);
  NotifyStateChanged();
}

bool RecorderController::MaybeCollapseRecordedTailIntoLoop() {
  const size_t total_actions = recorded_actions_.size();
  if (total_actions < 3) {
    return false;
  }
  if (recorded_action_signatures_.size() != total_actions) {
    RebuildRecordedActionSignatureCache();
  }

  constexpr size_t kMaxPatternLength = 6;
  size_t best_pattern_len = 0;
  size_t best_repeat_count = 0;
  size_t best_total = 0;

  for (size_t pattern_len = 1;
       pattern_len <= kMaxPatternLength && (pattern_len * 2) <= total_actions;
       ++pattern_len) {
    std::vector<std::string> pattern_signatures;
    pattern_signatures.reserve(pattern_len);
    bool valid_pattern = true;
    for (size_t i = 0; i < pattern_len; ++i) {
      const auto* action_dict =
          recorded_actions_[total_actions - pattern_len + i].GetIfDict();
      if (!action_dict) {
        valid_pattern = false;
        break;
      }
      std::string sig =
          (total_actions - pattern_len + i) < recorded_action_signatures_.size()
              ? recorded_action_signatures_[total_actions - pattern_len + i]
              : BuildAutoLoopSignature(*action_dict);
      if (sig.empty()) {
        valid_pattern = false;
        break;
      }
      pattern_signatures.push_back(std::move(sig));
    }
    if (!valid_pattern) {
      continue;
    }

    size_t repeat_count = 1;
    while ((repeat_count + 1) * pattern_len <= total_actions) {
      const size_t candidate_start =
          total_actions - (repeat_count + 1) * pattern_len;
      bool matches = true;
      for (size_t i = 0; i < pattern_len; ++i) {
        const auto* action_dict =
            recorded_actions_[candidate_start + i].GetIfDict();
        const std::string candidate_signature =
            (!action_dict || (candidate_start + i) >= recorded_action_signatures_.size())
                ? (action_dict ? BuildAutoLoopSignature(*action_dict) : std::string())
                : recorded_action_signatures_[candidate_start + i];
        if (!action_dict || candidate_signature != pattern_signatures[i]) {
          matches = false;
          break;
        }
      }
      if (!matches) {
        break;
      }
      ++repeat_count;
    }

    const size_t min_repeats = (pattern_len == 1) ? 3 : 2;
    if (repeat_count < min_repeats) {
      continue;
    }

    const size_t collapsed_actions = pattern_len * repeat_count;
    if (collapsed_actions > best_total ||
        (collapsed_actions == best_total && pattern_len > best_pattern_len)) {
      best_pattern_len = pattern_len;
      best_repeat_count = repeat_count;
      best_total = collapsed_actions;
    }
  }

  if (best_total == 0 || best_pattern_len == 0 || best_repeat_count == 0) {
    return false;
  }

  const size_t repeated_chunk_start = total_actions - best_total;
  const size_t final_pattern_start = total_actions - best_pattern_len;

  base::Value::List loop_steps;
  loop_steps.reserve(best_pattern_len);
  for (size_t i = 0; i < best_pattern_len; ++i) {
    loop_steps.Append(recorded_actions_[final_pattern_start + i].Clone());
  }

  base::Value::Dict loop_action;
  loop_action.Set("action", "loop");
  base::Value::Dict params;
  params.Set("loopMode", "count");
  params.Set("iterations", static_cast<int>(best_repeat_count));
  params.Set("maxIterations", static_cast<int>(best_repeat_count));
  params.Set("variable", "recorded_repeat");
  bool promoted_to_for_each = false;
  if (best_repeat_count >= 2) {
    std::optional<ForEachLocatorCandidate> template_candidate;
    bool values_vary = false;
    bool normalized_any = false;
    bool valid_for_each = true;
    for (size_t rep = 0; rep < best_repeat_count; ++rep) {
      const size_t first_step_index =
          repeated_chunk_start + (rep * best_pattern_len);
      const base::Value::Dict* first_step =
          recorded_actions_[first_step_index].GetIfDict();
      if (!first_step) {
        valid_for_each = false;
        break;
      }
      const std::string* first_action = first_step->FindString("action");
      if (!first_action || base::ToLowerASCII(*first_action) != "click") {
        valid_for_each = false;
        break;
      }
      const std::optional<ForEachLocatorCandidate> candidate =
          ExtractForEachLocatorCandidate(*first_step);
      if (!candidate.has_value()) {
        valid_for_each = false;
        break;
      }
      normalized_any = normalized_any || candidate->normalized_changed;
      if (!template_candidate.has_value()) {
        template_candidate = candidate;
        continue;
      }
      if (candidate->type != template_candidate->type ||
          candidate->normalized_value != template_candidate->normalized_value) {
        valid_for_each = false;
        break;
      }
      if (candidate->raw_value != template_candidate->raw_value) {
        values_vary = true;
      }
    }

    if (valid_for_each && template_candidate.has_value() &&
        (values_vary || normalized_any)) {
      params.Set("loopMode", "for_each");
      params.Remove("iterations");
      params.Remove("variable");
      params.Set("maxIterations", static_cast<int>(best_repeat_count));
      params.Set("forEachStrategy", "snapshot_once");
      base::Value::List for_each_locators;
      base::Value::Dict loop_locator;
      loop_locator.Set("type", template_candidate->type);
      loop_locator.Set("value", template_candidate->normalized_value);
      for_each_locators.Append(std::move(loop_locator));
      params.Set("locators", std::move(for_each_locators));
      promoted_to_for_each = true;
    }
  }
  loop_action.Set("params", std::move(params));
  loop_action.Set("steps", std::move(loop_steps));
  loop_action.Set("comment",
                  promoted_to_for_each
                      ? base::StringPrintf(
                            "Auto-looped repeated sibling-like sequence into "
                            "for_each x%zu.",
                            best_repeat_count)
                      : base::StringPrintf(
                            "Auto-looped repeated action sequence x%zu.",
                            best_repeat_count));
  dashboard::ScriptSchema::CanonicalizeAction(loop_action);

  base::Value::List rebuilt;
  rebuilt.reserve(repeated_chunk_start + 1);
  for (size_t i = 0; i < repeated_chunk_start; ++i) {
    rebuilt.Append(recorded_actions_[i].Clone());
  }
  rebuilt.Append(std::move(loop_action));
  recorded_actions_ = std::move(rebuilt);
  std::vector<std::string> rebuilt_signatures;
  rebuilt_signatures.reserve(repeated_chunk_start + 1);
  for (size_t i = 0; i < repeated_chunk_start; ++i) {
    if (i < recorded_action_signatures_.size()) {
      rebuilt_signatures.push_back(recorded_action_signatures_[i]);
    } else {
      const auto* action_dict = recorded_actions_[i].GetIfDict();
      rebuilt_signatures.push_back(action_dict ? BuildAutoLoopSignature(*action_dict)
                                               : std::string());
    }
  }
  const auto* tail_dict = recorded_actions_.back().GetIfDict();
  rebuilt_signatures.push_back(tail_dict ? BuildAutoLoopSignature(*tail_dict)
                                         : std::string());
  recorded_action_signatures_ = std::move(rebuilt_signatures);
  return true;
}


void RecorderController::AppendRecordedAction(base::Value::Dict action) {
  if (dashboard::IsRecordingSuppressed())
    return;

  StampActionContextUrl(&action, current_url_);
  // Canonicalize and append
  dashboard::ScriptSchema::CanonicalizeAction(action);
  if (TryAppendActionToManualLoop(action)) {
    NotifyStateChanged();
    return;
  }
  AppendRecordedActionSignature(action);
  recorded_actions_.Append(std::move(action));
  while (MaybeCollapseRecordedTailIntoLoop()) {
  }
  NotifyStateChanged();
}

bool RecorderController::TryUpgradeRecentRecordedClick(
    const base::Value::Dict& click,
    const base::Value::List& incoming_locators,
    int click_count,
    const gfx::Point& point,
    const std::string& frame_id,
    const std::string& source) {
  if (!HasNonVisualRecordedLocator(incoming_locators)) {
    return false;
  }
  if (recorded_actions_.empty() || !last_recorded_click_point_ ||
      point != *last_recorded_click_point_ ||
      click_count != last_recorded_click_count_) {
    return false;
  }
  if (!last_recorded_click_frame_id_.empty() && !frame_id.empty() &&
      frame_id != last_recorded_click_frame_id_) {
    return false;
  }

  base::Value::Dict* action = nullptr;
  if (manual_loop_capture_active_) {
    const std::optional<size_t> loop_index =
        FindLatestManualLoopCaptureActionIndex();
    if (loop_index.has_value()) {
      base::Value::Dict* loop_action = recorded_actions_[*loop_index].GetIfDict();
      base::Value::List* steps = loop_action ? loop_action->FindList("steps") : nullptr;
      if (steps && !steps->empty()) {
        action = (*steps)[steps->size() - 1].GetIfDict();
      }
    }
  } else {
    action = recorded_actions_.back().GetIfDict();
  }
  if (!action) {
    return false;
  }

  const std::string action_name =
      action->FindString("action") ? base::ToLowerASCII(*action->FindString("action"))
                                   : std::string();
  if (action_name != "click" && action_name != "hover") {
    return false;
  }

  base::Value::Dict* params = action->FindDict("params");
  if (!params) {
    return false;
  }

  base::Value::List* existing_locators = params->FindList("locators");
  if (existing_locators && HasNonVisualRecordedLocator(*existing_locators)) {
    return false;
  }

  params->Set("locators", ReorderRecordedLocatorBundle(incoming_locators.Clone()));
  AugmentClickParamsFromPageMapSnapshot(*params, click);

  base::Value::Dict meta = action->FindDict("meta")
      ? action->FindDict("meta")->Clone()
      : base::Value::Dict();
  base::Value::Dict recording_diagnostics;
  recording_diagnostics.Set("upgrade_source", source);
  recording_diagnostics.Set("upgraded_from_visual_only", true);
  recording_diagnostics.Set("bundle_types_recorded",
                            static_cast<int>(incoming_locators.size()));
  meta.Set("recordingDiagnostics", std::move(recording_diagnostics));
  action->Set("meta", std::move(meta));

  NotifyStateChanged();
  last_recorded_click_source_ = source;
  return true;
}

void RecorderController::AppendRecordedTimeline(const base::Value::List& timeline) {
  if (dashboard::IsRecordingSuppressed())
    return;

  for (const auto& it : timeline) {
    if (!it.is_dict()) continue;
    base::Value::Dict d = it.GetDict().Clone();
    dashboard::ScriptSchema::CanonicalizeAction(d);
    AppendRecordedActionSignature(d);
    recorded_actions_.Append(std::move(d));
  }
  while (MaybeCollapseRecordedTailIntoLoop()) {
  }
  NotifyStateChanged();
}



base::Value::List RecorderController::GetRecordedActionsClone() const {
  return recorded_actions_.Clone();
}

base::Value::List RecorderController::GetTrafficEntries(size_t limit) const {
  return traffic_store_.ListEntries(limit);
}

void RecorderController::ClearTrafficEntries() {
  traffic_store_.Clear();
}

base::Value::Dict RecorderController::PromoteTrafficEntryToRequestAction(
    const base::Value::Dict& entry,
    const base::flat_map<std::string, std::string>& secret_value_to_token)
    const {
  return dashboard::TrafficCaptureStore::EntryToRequestAction(
      entry, secret_value_to_token);
}

void RecorderController::EnsureNetworkCaptureEnabled() {
  EnsureDevToolsAttached();
  if (!agent_host_ || !devtools_client_ || network_capture_enabled_) {
    return;
  }
  network_capture_enabled_ = true;
  base::Value::Dict params;
  // Include already-in-flight requests when possible.
  params.Set("maxTotalBufferSize", 10000000);
  params.Set("maxResourceBufferSize", 5000000);
  SendCommand("Network.enable", std::move(params));
  LOG(INFO) << "[Recorder] CDP Network domain enabled for traffic capture";
}

void RecorderController::GetTrafficEntry(
    const std::string& request_id,
    base::OnceCallback<void(std::optional<base::Value::Dict>)> callback) {
  auto entry = traffic_store_.GetEntry(request_id);
  if (!entry) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  const bool has_body =
      entry->FindString("responseBody") &&
      !entry->FindString("responseBody")->empty();
  const bool finished = entry->FindBool("finished").value_or(false);
  if (has_body || !finished || !network_capture_enabled_) {
    std::move(callback).Run(std::move(entry));
    return;
  }

  base::Value::Dict params;
  params.Set("requestId", request_id);
  SendCommandExpectResponse(
      "Network.getResponseBody", std::move(params),
      base::BindOnce(&RecorderController::OnNetworkGetResponseBody,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)));
}

void RecorderController::OnNetworkGetResponseBody(
    std::string request_id,
    base::OnceCallback<void(std::optional<base::Value::Dict>)> callback,
    std::string res_json,
    std::string err_json) {
  if (!err_json.empty() && err_json != "{}") {
    auto entry = traffic_store_.GetEntry(request_id);
    std::move(callback).Run(std::move(entry));
    return;
  }
  auto parsed =
      base::JSONReader::Read(res_json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (parsed && parsed->is_dict()) {
    const base::Value::Dict& dict = parsed->GetDict();
    const std::string* body = dict.FindString("body");
    const bool base64 = dict.FindBool("base64Encoded").value_or(false);
    if (body) {
      traffic_store_.SetResponseBody(request_id, *body, base64);
    }
  }
  std::move(callback).Run(traffic_store_.GetEntry(request_id));
}

void RecorderController::OnDevToolsEvent(const std::string& method,
                                         base::Value::Dict params) {
  if (base::StartsWith(method, "Network.", base::CompareCase::SENSITIVE)) {
    traffic_store_.OnNetworkEvent(method, params);
  }
}

void RecorderController::ClearRecordedActions() {
  recorded_actions_.clear();
  recorded_action_signatures_.clear();
  manual_loop_capture_active_ = false;
  pending_manual_loop_capture_start_ = false;
  loop_preview_key_chord_active_ = false;
  last_recorded_click_point_.reset();
  last_recorded_click_at_ = base::TimeTicks();
  last_recorded_click_count_ = -1;
  last_recorded_click_frame_id_.clear();
  last_recorded_click_source_.clear();
  active_pointer_down_point_.reset();
  active_pointer_last_point_.reset();
  active_pointer_dragged_ = false;
  last_non_hover_click_point_.reset();
  last_non_hover_click_at_ = base::TimeTicks();
  last_hover_transition_signature_.clear();
  last_hover_recorded_at_ = base::TimeTicks();
  UpdateInstrumentationState();
  NotifyStateChanged();
}

void RecorderController::DidStartNavigation(content::NavigationHandle* navigation_handle) {
  if (!navigation_handle || !navigation_handle->IsInPrimaryMainFrame())
    return;
  // Keep this handler lightweight to avoid interfering with the core
  // navigation / DevTools instrumentation pipeline. We only track the URL and
  // clear PageMap context; DevTools attachment is controlled elsewhere.
  LOG(INFO) << "[Recorder] DidStartNavigation (passive): "
            << navigation_handle->GetURL();
  scroll_capture_timer_.Stop();
  pending_scroll_y_ = 0;
  loop_preview_modifiers_active_ = false;
  loop_preview_key_chord_active_ = false;
  ClearLoopPreview();
  last_loop_preview_point_.reset();
  // Keep manual loop capture active across same-tab navigations so guided flows
  // can include navigate/click-return steps without losing loop context.
  pending_manual_loop_capture_start_ = false;
  if (guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
      !manual_loop_capture_active_) {
    guided_loop_target_confirmation_pending_ = false;
    guided_loop_target_probe_point_.reset();
    SetGuidedLoopFirstClickGuardArmed(true);
  }
  FlushPendingTypeBuffer();
  current_url_ = navigation_handle->GetURL().spec();
  ResetPageMapContextForNavigation();
}

void RecorderController::DidFinishNavigation(content::NavigationHandle* navigation_handle) {
  if (!navigation_handle || !navigation_handle->HasCommitted() || !navigation_handle->IsInPrimaryMainFrame())
    return;
  current_url_ = navigation_handle->GetURL().spec();
  const int committed_entry_index =
      navigation_handle->GetWebContents()
          ? navigation_handle->GetWebContents()
                ->GetController()
                .GetLastCommittedEntryIndex()
          : -1;
  if (auto* rfh = navigation_handle->GetRenderFrameHost()) {
    target_main_frame_token_ = rfh->GetFrameToken().ToString();
  }
  if (is_recording_ && ShouldObserveInput()) {
    DetachInputObserver();
    AttachInputObserver();
    if (!input_observer_) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<RecorderController> weak) {
                if (!weak || !weak->is_recording_ ||
                    !weak->ShouldObserveInput())
                  return;
                weak->DetachInputObserver();
                weak->AttachInputObserver();
              },
              weak_factory_.GetWeakPtr()),
          base::Milliseconds(200));
    }
  } else {
    DetachInputObserver();
  }
  if (is_recording_ && !dashboard::IsRecordingSuppressed()) {
    // Record browser history back/forward actions when the user uses the
    // browser UI (back/forward buttons, trackpad gesture, etc). This produces
    // deterministic replay behavior and avoids spamming "navigate" entries.
    const ui::PageTransition t = navigation_handle->GetPageTransition();
    const bool is_forward_back = (t & ui::PAGE_TRANSITION_FORWARD_BACK) != 0;
    if (is_forward_back && committed_entry_index >= 0 &&
        last_committed_entry_index_.has_value() &&
        committed_entry_index != *last_committed_entry_index_) {
      const char* kind =
          (committed_entry_index < *last_committed_entry_index_) ? "back"
                                                                 : "forward";
      base::Value::Dict action;
      action.Set("action", kind);
      action.Set("params", base::Value::Dict());
      AppendRecordedAction(std::move(action));
    }

    // Only record a "navigate" action for explicit user address-bar typed
    // URLs / reload. Never synthesize navigate from form submits, search
    // suggestions (GENERATED), link clicks, or contextUrl stamps — those are
    // already covered by click/type/key_press (and contextUrl is metadata for
    // resume/debug only).
    if (!navigation_handle->IsSameDocument()) {
      const bool from_address_bar =
          (t & ui::PAGE_TRANSITION_FROM_ADDRESS_BAR) != 0;
      const bool is_form_submit =
          ui::PageTransitionCoreTypeIs(t, ui::PAGE_TRANSITION_FORM_SUBMIT);
      const bool user_typed_like =
          from_address_bar && !is_form_submit &&
          (ui::PageTransitionCoreTypeIs(t, ui::PAGE_TRANSITION_TYPED) ||
           ui::PageTransitionCoreTypeIs(t, ui::PAGE_TRANSITION_RELOAD));
      if (user_typed_like) {
        const GURL u = navigation_handle->GetURL();
        if (u.is_valid() && u.SchemeIsHTTPOrHTTPS() &&
            u.spec() != last_recorded_navigate_url_) {
          last_recorded_navigate_url_ = u.spec();
          base::Value::Dict action;
          action.Set("action", "navigate");
          base::Value::Dict params;
          params.Set("url", u.spec());
          action.Set("params", std::move(params));
          AppendRecordedAction(std::move(action));
        }
      }
    }
  }
  if (is_recording_ && manual_loop_capture_active_ &&
      guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 3 &&
      IsGuidedReturnUrlMatch(current_url_, guided_loop_origin_url_)) {
    EndManualForEachLoopCapture();
    CancelOneShotLocatorCapture("guided_capture_canceled");
    guided_loop_target_confirmation_pending_ = false;
    guided_loop_target_probe_point_.reset();
    SetGuidedLoopFirstClickGuardArmed(false);
    ClearLoopPreview();
    guided_loop_ui_state_ = GuidedLoopUiState();
    ClearGuidedScrapeSelections();
    guided_loop_origin_url_.clear();
    if (guided_same_tab_navigation_guard_enabled_) {
      SetGuidedLoopSameTabNavigationGuard(false);
      guided_same_tab_navigation_guard_enabled_ = false;
    }
    UpdateInstrumentationState();
    NotifyStateChanged();
  }
  if (committed_entry_index >= 0)
    last_committed_entry_index_ = committed_entry_index;
  if (guided_loop_ui_state_.active) {
    if (guided_loop_ui_state_.phase == 1 && !manual_loop_capture_active_) {
      // Same-tab navigations replace the document and discard runtime helpers.
      // Reinstall the step-1 preview helper.
      SetGuidedLoopFirstClickGuardArmed(true);
    }
    const bool block_all_anchor_navigation =
        guided_loop_ui_state_.phase == 1 && !manual_loop_capture_active_;
    if (guided_same_tab_navigation_guard_enabled_) {
      SetGuidedLoopSameTabNavigationGuard(true, block_all_anchor_navigation);
    }
    // Guided helpers live in the page and are lost on same-tab navigations.
    // Restore the interception/prediction scripts for the new document.
    RefreshOneShotCaptureClickSuppression();
    SetGuidedScrapeOverlayEnabled(guided_loop_ui_state_.phase == 2);
  }
  // For now, avoid automatic DevTools reattachment or snapshot capture here.
  // The Agent console and recorder explicitly request fresh PageMap snapshots
  // when needed (e.g., via dashboardAgentRefreshPageMap), which keeps this
  // observer out of the critical navigation path.
}

void RecorderController::DidOpenRequestedURL(
    content::WebContents* new_contents,
    content::RenderFrameHost* source_render_frame_host,
    const GURL& url,
    const content::Referrer& referrer,
    WindowOpenDisposition disposition,
    ui::PageTransition transition,
    bool started_from_context_menu,
    bool renderer_initiated) {
  if (!is_recording_ || !new_contents || !target_web_contents_ ||
      web_contents() != target_web_contents_) {
    return;
  }

  // Loop capture must stay in one tab: fold new-tab/popup opens (target=_blank,
  // modifier clicks, window.open) back into the recorded tab.
  if (guided_same_tab_navigation_guard_enabled_ && url.SchemeIsHTTPOrHTTPS() &&
      disposition != WindowOpenDisposition::CURRENT_TAB) {
    LOG(INFO) << "[Recorder] Keeping guided loop in the recorded tab: " << url;
    content::NavigationController::LoadURLParams params(url);
    params.referrer = referrer;
    params.transition_type = ui::PAGE_TRANSITION_LINK;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<content::WebContents> source,
               base::WeakPtr<content::WebContents> spawned,
               content::NavigationController::LoadURLParams params) {
              if (spawned) {
                spawned->ClosePage();
              }
              if (!source) {
                return;
              }
              source->GetController().LoadURLWithParams(params);
              if (content::WebContentsDelegate* delegate =
                      source->GetDelegate()) {
                delegate->ActivateContents(source.get());
              }
            },
            target_web_contents_->GetWeakPtr(), new_contents->GetWeakPtr(),
            std::move(params)));
    return;
  }

  // Keep recording bound to the originating tab unless navigation actually
  // moved the user into a new foreground context.
  const bool should_follow_new_target =
      disposition == WindowOpenDisposition::NEW_FOREGROUND_TAB ||
      disposition == WindowOpenDisposition::NEW_BACKGROUND_TAB ||
      disposition == WindowOpenDisposition::NEW_POPUP ||
      disposition == WindowOpenDisposition::NEW_WINDOW;
  if (!should_follow_new_target) {
    return;
  }

  LOG(INFO) << "[Recorder] Following spawned tab for active recording: "
            << new_contents->GetVisibleURL();
  AttachToWebContents(new_contents);
}


void RecorderController::WebContentsDestroyed() {
  LOG(INFO) << "[Recorder] WebContentsDestroyed: detaching recorder state";
  scroll_capture_timer_.Stop();
  pending_scroll_y_ = 0;
  loop_preview_modifiers_active_ = false;
  loop_preview_key_chord_active_ = false;
  ClearLoopPreview();
  last_loop_preview_point_.reset();
  EndManualForEachLoopCapture();
  pending_manual_loop_capture_start_ = false;
  FlushPendingTypeBuffer();
  ResetPageMapContextForNavigation();
  DetachInputObserver();
  DetachFromTarget();
  Observe(nullptr);
}

void RecorderController::FlushPendingTypeBuffer() {
  type_buffer_idle_flush_timer_.Stop();

  if (pending_type_buffer_.empty())
    return;

  if (is_recording_) {
    base::Value::Dict action;
    action.Set("action", "type");
    base::Value::Dict params;
    params.Set("text", base::UTF16ToUTF8(pending_type_buffer_));
    if (last_user_click_point_) {
      params.Set("x", last_user_click_point_->x());
      params.Set("y", last_user_click_point_->y());
    }
    action.Set("params", std::move(params));
    AppendRecordedAction(std::move(action));
  }

  pending_type_buffer_.clear();
}

void RecorderController::ScheduleTypeBufferIdleFlush() {
  type_buffer_idle_flush_timer_.Stop();
  type_buffer_idle_flush_timer_.Start(
      FROM_HERE, kTypeBufferIdleFlushDelay,
      base::BindOnce(&RecorderController::FlushPendingTypeBuffer,
                     weak_factory_.GetWeakPtr()));
}

void RecorderController::ScheduleScrollCapture() {
  if (!is_recording_ || dashboard::IsRecordingSuppressed())
    return;
  scroll_capture_timer_.Stop();
  scroll_capture_timer_.Start(
      FROM_HERE, base::Milliseconds(220),
      base::BindOnce(&RecorderController::CaptureScrollSnapshotForRecording,
                     weak_factory_.GetWeakPtr()));
}

void RecorderController::HandleBackspaceForTypeBuffer() {
  if (!pending_type_buffer_.empty()) {
    pending_type_buffer_.pop_back();
    ScheduleTypeBufferIdleFlush();
    return;
  }

  if (is_recording_) {
    base::Value::Dict action;
    action.Set("action", "key_press");
    base::Value::Dict params;
    params.Set("key", "Backspace");
    action.Set("params", std::move(params));
    AppendRecordedAction(std::move(action));
  }
}

void RecorderController::MaybeAppendCharacterForTypeBuffer(
    const blink::WebKeyboardEvent& event) {
  int modifiers = event.GetModifiers();
  if (modifiers & (blink::WebInputEvent::Modifiers::kControlKey |
                   blink::WebInputEvent::Modifiers::kAltKey |
                   blink::WebInputEvent::Modifiers::kMetaKey)) {
    return;
  }

  bool appended_any = false;
  for (char16_t ch : event.text) {
    if (ch == 0)
      break;
    if (ch == u'\t' || ch == u'\r' || ch == u'\n')
      continue;
    pending_type_buffer_.push_back(ch);
    appended_any = true;
  }

  if (appended_any)
    ScheduleTypeBufferIdleFlush();
}

void RecorderController::MaybeQueueHoverProbe(int x_css, int y_css) {
  if (!ShouldUseLegacyRendererAgentPipeline()) {
    return;
  }
  if (IsGuidedInterceptionPhaseActive()) {
    return;
  }
  if (!is_recording_ || !target_web_contents_) {
    return;
  }
  if (pending_click_fallback_timer_.IsRunning() || pending_click_resolve_point_) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  const gfx::Point point(x_css, y_css);
  if (!last_hover_probe_enqueued_at_.is_null() &&
      now - last_hover_probe_enqueued_at_ < kHoverProbeMinInterval &&
      last_hover_probe_point_ &&
      NearPointManhattan(*last_hover_probe_point_, point, kHoverProbeMinMovePx)) {
    return;
  }
  TRACE_EVENT2("ui", "RecorderController::MaybeQueueHoverProbe", "x",
               x_css, "y", y_css);
  pending_hover_probe_point_ = gfx::Point(x_css, y_css);
  pending_hover_probe_queued_at_ = now;
  last_hover_probe_enqueued_at_ = now;
  last_hover_probe_point_ = point;
  hover_probe_timer_.Start(
      FROM_HERE, kHoverProbeDebounceDelay,
      base::BindOnce(&RecorderController::RunHoverProbe,
                     weak_factory_.GetWeakPtr()));
}

void RecorderController::RunHoverProbe() {
  pending_hover_probe_point_.reset();
  pending_hover_probe_queued_at_ = base::TimeTicks();
}

void RecorderController::ClearCaptureHoverPreview() {
  ++capture_hover_generation_;
  capture_hover_timer_.Stop();
  pending_capture_hover_point_.reset();
  last_capture_hover_point_.reset();
  last_capture_hover_enqueued_at_ = base::TimeTicks();
  capture_hover_rect_.reset();
  capture_hover_tip_.clear();
  capture_hover_tip_anchor_ = gfx::Point();
  RefreshHighlightOverlay();
}

void RecorderController::MaybeQueueCaptureHoverPreview(int x_css, int y_css) {
  if (!locator_capture_armed_ || !target_web_contents_) {
    return;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  const gfx::Point point(x_css, y_css);
  if (!last_capture_hover_enqueued_at_.is_null() &&
      now - last_capture_hover_enqueued_at_ < kHoverProbeMinInterval &&
      last_capture_hover_point_ &&
      NearPointManhattan(*last_capture_hover_point_, point,
                         kHoverProbeMinMovePx)) {
    return;
  }
  pending_capture_hover_point_ = point;
  last_capture_hover_enqueued_at_ = now;
  last_capture_hover_point_ = point;
  capture_hover_timer_.Start(
      FROM_HERE, kHoverProbeDebounceDelay,
      base::BindOnce(&RecorderController::RunCaptureHoverPreview,
                     weak_factory_.GetWeakPtr()));
}

void RecorderController::RunCaptureHoverPreview() {
  if (!pending_capture_hover_point_ || !locator_capture_armed_) {
    pending_capture_hover_point_.reset();
    return;
  }
  const gfx::Point point = *pending_capture_hover_point_;
  pending_capture_hover_point_.reset();
  dashboard::mojom::StealthyElementResolver* resolver = MainFrameResolver();
  if (!resolver) {
    return;
  }
  const uint64_t generation = ++capture_hover_generation_;
  resolver->ResolveAtPoint(
      point.x(), point.y(),
      base::BindOnce(&RecorderController::OnCaptureHoverResolved,
                     weak_factory_.GetWeakPtr(), point));
  // generation is captured via capture_hover_generation_ check in callback.
  (void)generation;
}

void RecorderController::OnCaptureHoverResolved(
    gfx::Point point,
    dashboard::mojom::ResolvedElementInfoPtr info) {
  if (!locator_capture_armed_) {
    return;
  }
  if (!info || !info->ok || info->width <= 0 || info->height <= 0) {
    capture_hover_rect_.reset();
    capture_hover_tip_.clear();
    RefreshHighlightOverlay();
    return;
  }
  capture_hover_rect_ = gfx::Rect(info->x, info->y, info->width, info->height);
  capture_hover_tip_anchor_ = point;

  std::string tip = base::StringPrintf(
      "%s  %dx%d",
      info->tag.empty() ? "element" : info->tag.c_str(), info->width,
      info->height);
  int shown = 0;
  for (const auto& locator : info->locators) {
    if (!locator || locator->type.empty() || locator->value.empty()) {
      continue;
    }
    const std::string type = locator->type;
    if (type == "VISUAL_COORDS") {
      continue;
    }
    std::string value = locator->value;
    if (value.size() > 48) {
      value = value.substr(0, 45) + "...";
    }
    tip.append(base::StringPrintf("\n%s=%s", type.c_str(), value.c_str()));
    if (++shown >= 3) {
      break;
    }
  }
  if (!info->id_attr.empty() && shown < 3) {
    tip.append(base::StringPrintf("\nid=%s", info->id_attr.c_str()));
  }
  capture_hover_tip_ = base::UTF8ToUTF16(tip);
  RefreshHighlightOverlay();
}

bool RecorderController::BeginManualForEachLoopFromPreview() {
  if (!is_recording_ || manual_loop_capture_active_ ||
      !last_loop_preview_result_.has_value()) {
    return false;
  }
  const base::Value::Dict& preview = *last_loop_preview_result_;
  const std::string* locator = preview.FindString("locator");
  const int candidate_count = preview.FindInt("count").value_or(0);
  if (!locator || locator->empty() || candidate_count < 2) {
    return false;
  }
  base::Value::List loop_target_locators;
  if (guided_loop_target_probe_point_) {
    loop_target_locators =
        BuildLocatorBundleForPoint(*guided_loop_target_probe_point_);
  }
  loop_target_locators =
      SanitizeLoopTargetLocators(loop_target_locators, *locator);
  if (loop_target_locators.empty() &&
      IsUsableForEachLocatorValue("CSS_SELECTOR", *locator)) {
    base::Value::Dict loop_locator;
    loop_locator.Set("type", "CSS_SELECTOR");
    loop_locator.Set("value", *locator);
    loop_target_locators.Append(std::move(loop_locator));
  }
  pending_manual_loop_capture_start_ = false;
  guided_loop_target_confirmation_pending_ = false;
  guided_loop_target_probe_point_.reset();
  guided_loop_target_click_started_at_.reset();
  SetGuidedLoopFirstClickGuardArmed(false);
  ClearLoopPreview();
  base::Value::Dict loop_action;
  loop_action.Set("action", "loop");
  base::Value::Dict params;
  params.Set("loopMode", "for_each");
  params.Set("maxIterations", std::max(2, candidate_count));
  params.Set("forEachStrategy", "snapshot_once");
  params.Set("manualLoopCaptureActive", true);
  params.Set("replayTargetClick", true);
  params.Set("locators", std::move(loop_target_locators));
  loop_action.Set("params", std::move(params));
  loop_action.Set("steps", base::Value::List());
  loop_action.Set("comment",
                  "Manual loop capture started: perform looped actions, then "
                  "finish the loop from Guided Next Step or Loop.");
  StampActionContextUrl(&loop_action, current_url_);
  dashboard::ScriptSchema::CanonicalizeAction(loop_action);
  AppendRecordedActionSignature(loop_action);
  recorded_actions_.Append(std::move(loop_action));
  manual_loop_capture_active_ = true;
  const bool block_all_anchor_navigation =
      guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
      !manual_loop_capture_active_;
  SetGuidedLoopSameTabNavigationGuard(true, block_all_anchor_navigation);
  guided_same_tab_navigation_guard_enabled_ = true;
  RefreshOneShotCaptureClickSuppression();
  UpdateInstrumentationState();
  NotifyStateChanged();
  return true;
}

bool RecorderController::BeginManualForEachLoopCaptureWithLocator(
    const std::string& css_locator,
    int candidate_count,
    std::string* error_out) {
  if (!is_recording_) {
    if (error_out) {
      *error_out = "recorder_not_recording";
    }
    return false;
  }
  if (manual_loop_capture_active_) {
    if (error_out) {
      *error_out = "manual_loop_capture_already_active";
    }
    return false;
  }
  if (css_locator.empty()) {
    if (error_out) {
      *error_out = "missing_css_locator";
    }
    return false;
  }

  guided_loop_target_confirmation_pending_ = false;
  guided_loop_target_probe_point_.reset();
  guided_loop_target_click_started_at_.reset();
  SetGuidedLoopFirstClickGuardArmed(false);
  ClearLoopPreview();

  base::Value::Dict loop_action;
  loop_action.Set("action", "loop");
  base::Value::Dict params;
  params.Set("loopMode", "for_each");
  params.Set("maxIterations", std::max(2, candidate_count));
  params.Set("forEachStrategy", "snapshot_once");
  params.Set("manualLoopCaptureActive", true);
  params.Set("replayTargetClick", true);
  base::Value::List locators =
      MaybeReuseFreshLoopTargetLocators(current_url_, css_locator);
  locators = SanitizeLoopTargetLocators(locators, css_locator);
  if (locators.empty()) {
    base::Value::Dict loop_locator;
    loop_locator.Set("type", "CSS_SELECTOR");
    loop_locator.Set("value", css_locator);
    locators.Append(std::move(loop_locator));
  }
  locators = ReorderRecordedLocatorBundle(std::move(locators));
  params.Set("locators", std::move(locators));
  loop_action.Set("params", std::move(params));
  loop_action.Set("steps", base::Value::List());
  loop_action.Set(
      "comment",
      "Manual loop capture started from toolbox: perform looped actions, then "
      "finish loop capture from Guided controls.");
  StampActionContextUrl(&loop_action, current_url_);
  dashboard::ScriptSchema::CanonicalizeAction(loop_action);
  AppendRecordedActionSignature(loop_action);
  recorded_actions_.Append(std::move(loop_action));
  manual_loop_capture_active_ = true;
  pending_manual_loop_capture_start_ = false;
  const bool block_all_anchor_navigation =
      guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
      !manual_loop_capture_active_;
  SetGuidedLoopSameTabNavigationGuard(true, block_all_anchor_navigation);
  guided_same_tab_navigation_guard_enabled_ = true;
  RefreshOneShotCaptureClickSuppression();
  NotifyStateChanged();
  return true;
}

void RecorderController::EndManualForEachLoopCapture() {
  if (!manual_loop_capture_active_) {
    return;
  }
  const std::optional<size_t> loop_index =
      FindLatestManualLoopCaptureActionIndex();
  if (loop_index.has_value()) {
    base::Value::Dict* loop_action =
        recorded_actions_[*loop_index].GetIfDict();
    if (loop_action) {
      if (base::Value::Dict* params = loop_action->FindDict("params")) {
        params->Set("manualLoopCaptureActive", false);
      }
      std::string comment =
          loop_action->FindString("comment") ? *loop_action->FindString("comment")
                                             : "";
      if (base::Value::List* steps = loop_action->FindList("steps")) {
        NormalizeManualLoopCaptureSteps(steps);
      }
      if (comment.find("Manual loop capture ended") == std::string::npos) {
        if (!comment.empty()) {
          comment.append(" | ");
        }
        comment.append("Manual loop capture ended.");
        loop_action->Set("comment", comment);
      }
      dashboard::ScriptSchema::CanonicalizeAction(*loop_action);
      UpdateRecordedActionSignatureAt(*loop_index);
    }
  }
  manual_loop_capture_active_ = false;
  pending_manual_loop_capture_start_ = false;
  UpdateInstrumentationState();
  NotifyStateChanged();
}

bool RecorderController::EndManualForEachLoopCaptureForTools() {
  const bool was_active = manual_loop_capture_active_;
  EndManualForEachLoopCapture();
  return was_active;
}

bool RecorderController::SetGuidedLoopUiState(
    const GuidedLoopUiState& state) {
  const bool was_guided_active = guided_loop_ui_state_.active;
  const int previous_phase = guided_loop_ui_state_.phase;
  GuidedLoopUiState next;
  if (!state.active) {
    CancelOneShotLocatorCapture("guided_capture_canceled");
    guided_loop_target_confirmation_pending_ = false;
    guided_loop_target_probe_point_.reset();
    SetGuidedLoopFirstClickGuardArmed(false);
    ClearLoopPreview();
    if (guided_same_tab_navigation_guard_enabled_) {
      SetGuidedLoopSameTabNavigationGuard(false);
      guided_same_tab_navigation_guard_enabled_ = false;
    }
    guided_loop_ui_state_ = GuidedLoopUiState();
    ClearGuidedScrapeSelections();
    guided_loop_origin_url_.clear();
    RefreshOneShotCaptureClickSuppression();
    UpdateInstrumentationState();
    NotifyStateChanged();
    return true;
  }

  next.active = true;
  next.phase = state.phase;
  if (next.phase < 0 || next.phase > 3) {
    next.phase = 0;
  }
  const bool rewinding_from_selection_to_loop_target =
      was_guided_active && previous_phase == 2 && next.phase == 1;
  if (rewinding_from_selection_to_loop_target &&
      !IsGuidedReturnUrlMatch(current_url_, guided_loop_origin_url_)) {
    return false;
  }
  next.capture_pending = state.capture_pending;
  next.capture_kind = state.capture_kind;
  if (next.capture_kind < 0 || next.capture_kind > 2) {
    next.capture_kind = 0;
  }
  next.capture_sequence = state.capture_sequence;
  const bool entering_phase2 = !was_guided_active || previous_phase != 2;
  if (next.phase != 2 || entering_phase2) {
    ClearGuidedScrapeSelections();
  }
  next.scrape_target_count =
      static_cast<uint32_t>(guided_scrape_selection_locators_.size());
  guided_loop_ui_state_ = std::move(next);
  if (!was_guided_active) {
    guided_loop_origin_url_ = current_url_;
  }
  if (!guided_loop_ui_state_.capture_pending) {
    CancelOneShotLocatorCapture("guided_capture_canceled");
  }

  if (guided_loop_ui_state_.phase != 1) {
    guided_loop_target_confirmation_pending_ = false;
    guided_loop_target_probe_point_.reset();
    SetGuidedLoopFirstClickGuardArmed(false);
    ClearLoopPreview();
  } else if (!manual_loop_capture_active_) {
    SetGuidedLoopFirstClickGuardArmed(true);
    // Preinstall the preview helper before the first guided click so step 1
    // does not pay the injector cost on the critical path.
  }

  const bool block_all_anchor_navigation =
      guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
      !manual_loop_capture_active_;
  SetGuidedLoopSameTabNavigationGuard(true, block_all_anchor_navigation);
  guided_same_tab_navigation_guard_enabled_ = true;
  RefreshOneShotCaptureClickSuppression();
  SetGuidedScrapeOverlayEnabled(guided_loop_ui_state_.active &&
                                guided_loop_ui_state_.phase == 2);
  UpdateInstrumentationState();
  NotifyStateChanged();
  return true;
}

bool RecorderController::TryAppendActionToManualLoop(
    const base::Value::Dict& action) {
  if (!manual_loop_capture_active_ || recorded_actions_.empty()) {
    return false;
  }
  const std::optional<size_t> loop_index =
      FindLatestManualLoopCaptureActionIndex();
  if (!loop_index.has_value()) {
    manual_loop_capture_active_ = false;
    UpdateInstrumentationState();
    NotifyStateChanged();
    return false;
  }
  base::Value::Dict* loop_action =
      recorded_actions_[*loop_index].GetIfDict();
  base::Value::Dict* params = loop_action ? loop_action->FindDict("params") : nullptr;
  const std::string loop_mode =
      (params && params->FindString("loopMode"))
          ? *params->FindString("loopMode")
          : "";
  if (!params || base::ToLowerASCII(loop_mode) != "for_each") {
    manual_loop_capture_active_ = false;
    UpdateInstrumentationState();
    NotifyStateChanged();
    return false;
  }
  base::Value::List* steps = loop_action->FindList("steps");
  if (!steps) {
    loop_action->Set("steps", base::Value::List());
    steps = loop_action->FindList("steps");
  }
  if (!steps) {
    manual_loop_capture_active_ = false;
    UpdateInstrumentationState();
    NotifyStateChanged();
    return false;
  }
  steps->Append(action.Clone());
  dashboard::ScriptSchema::CanonicalizeAction(*loop_action);
  UpdateRecordedActionSignatureAt(*loop_index);
  return true;
}

void RecorderController::RebuildRecordedActionSignatureCache() {
  recorded_action_signatures_.clear();
  recorded_action_signatures_.reserve(recorded_actions_.size());
  for (const auto& action_value : recorded_actions_) {
    const base::Value::Dict* action_dict = action_value.GetIfDict();
    recorded_action_signatures_.push_back(
        action_dict ? BuildAutoLoopSignature(*action_dict) : std::string());
  }
}

void RecorderController::AppendRecordedActionSignature(
    const base::Value::Dict& action) {
  recorded_action_signatures_.push_back(BuildAutoLoopSignature(action));
}

void RecorderController::UpdateRecordedActionSignatureAt(size_t index) {
  if (index >= recorded_actions_.size()) {
    return;
  }
  if (recorded_action_signatures_.size() != recorded_actions_.size()) {
    RebuildRecordedActionSignatureCache();
    return;
  }
  const base::Value::Dict* action_dict = recorded_actions_[index].GetIfDict();
  recorded_action_signatures_[index] =
      action_dict ? BuildAutoLoopSignature(*action_dict) : std::string();
}

void RecorderController::ClearLoopPreview() {
  loop_preview_timer_.Stop();
  pending_loop_preview_point_.reset();
  if (loop_preview_visible_ || loop_preview_modifiers_active_) {
    UpdateLoopPreviewAtPoint(0, 0, /*active=*/false);
  }
  loop_preview_visible_ = false;
  last_loop_preview_result_.reset();
}

void RecorderController::MaybeQueueLoopPreview(int x_css, int y_css) {
  if (!dashboard::IsLoopPreviewEnabled() || !is_recording_ ||
      !target_web_contents_) {
    return;
  }
  const gfx::Point point(x_css, y_css);
  if (last_loop_preview_point_ &&
      NearPointManhattan(*last_loop_preview_point_, point,
                         kLoopPreviewMinMovePx)) {
    return;
  }
  pending_loop_preview_point_ = point;
  const bool guided_phase1_preview =
      guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
      !manual_loop_capture_active_;
  const base::TimeDelta debounce_delay =
      guided_phase1_preview ? kGuidedLoopPreviewIdleDelay
                            : kLoopPreviewDebounceDelay;
  loop_preview_timer_.Start(
      FROM_HERE, debounce_delay,
      base::BindOnce(&RecorderController::RunLoopPreviewProbe,
                     weak_factory_.GetWeakPtr()));
}

void RecorderController::RunLoopPreviewProbe() {
  const bool guided_phase1_preview =
      guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
      !manual_loop_capture_active_;
  if ((!loop_preview_modifiers_active_ && !guided_phase1_preview) ||
      !pending_loop_preview_point_) {
    return;
  }
  const gfx::Point point = *pending_loop_preview_point_;
  pending_loop_preview_point_.reset();
  UpdateLoopPreviewAtPoint(point.x(), point.y(), /*active=*/true);
  last_loop_preview_point_ = point;
  loop_preview_visible_ = true;
}

void RecorderController::UpdateLoopPreviewAtPoint(int x_css,
                                                  int y_css,
                                                  bool active) {
  if (!active) {
    loop_preview_rects_.clear();
    loop_preview_tip_.clear();
    RefreshHighlightOverlay();
    return;
  }
  const gfx::Point point(x_css, y_css);
  auto done = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      base::BindOnce(&RecorderController::OnLoopPreviewResolved,
                     weak_factory_.GetWeakPtr(), active, point),
      nullptr);
  dashboard::mojom::StealthyElementResolver* resolver = MainFrameResolver();
  if (!resolver) {
    std::move(done).Run(nullptr);
    return;
  }
  resolver->FindRepeatedPeersAtPoint(x_css, y_css, /*max_peers=*/48,
                                     std::move(done));
}

void RecorderController::OnLoopPreviewResolved(
    bool active_request,
    gfx::Point point,
    dashboard::mojom::RepeatedPeersPtr peers) {
  const bool guided_phase1_loop_target =
      guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
      !manual_loop_capture_active_;
  const bool ok = peers && peers->peers.size() >= 2 && !peers->locator.empty();
  const int count = ok ? static_cast<int>(peers->peers.size()) : 0;
  const std::string locator = ok ? peers->locator : std::string();

  loop_preview_rects_.clear();
  loop_preview_tip_.clear();
  if (ok) {
    for (const auto& peer : peers->peers) {
      loop_preview_rects_.emplace_back(peer->x, peer->y, peer->width,
                                       peer->height);
    }
    loop_preview_tip_ = base::UTF8ToUTF16(base::StringPrintf(
        "Loop candidates: %d. Click one repeated item to set the loop "
        "target. Locator: %s",
        count, locator.c_str()));
    loop_preview_tip_anchor_ = point;
  }
  RefreshHighlightOverlay();

  if (active_request && ok) {
    base::Value::Dict result;
    result.Set("ok", true);
    result.Set("active", true);
    result.Set("count", count);
    result.Set("locator", locator);
    last_loop_preview_result_ = std::move(result);
    if (guided_phase1_loop_target) {
      guided_loop_target_confirmation_pending_ = true;
    }
    if (pending_manual_loop_capture_start_) {
      pending_manual_loop_capture_start_ = false;
      BeginManualForEachLoopFromPreview();
    }
  } else if (active_request) {
    if (guided_phase1_loop_target) {
      guided_loop_target_confirmation_pending_ = false;
      guided_loop_target_probe_point_.reset();
      SetGuidedLoopFirstClickGuardArmed(true);
    }
    if (pending_manual_loop_capture_start_) {
      LOG(INFO) << "[Recorder] Loop capture start probe found no repeated "
                   "items at "
                << point.ToString();
    }
    pending_manual_loop_capture_start_ = false;
    last_loop_preview_result_.reset();
  } else {
    last_loop_preview_result_.reset();
  }

  if (active_request && guided_loop_target_click_started_at_) {
    LOG(INFO) << "[Recorder][Perf] guided_phase1_preview ok="
              << (ok ? "true" : "false") << " candidates=" << count
              << " latency_ms="
              << (base::TimeTicks::Now() - *guided_loop_target_click_started_at_)
                     .InMilliseconds();
    guided_loop_target_click_started_at_.reset();
  }
}

void RecorderController::RefreshHighlightOverlay() {
  std::vector<dashboard::RecorderHighlightOverlay::Box> boxes;
  for (const gfx::Rect& rect : loop_preview_rects_) {
    boxes.push_back(
        {rect, dashboard::RecorderHighlightOverlay::Style::kCandidate});
  }
  if (guided_scrape_overlay_enabled_) {
    for (const gfx::Rect& rect : guided_scrape_pattern_rects_) {
      if (!rect.IsEmpty()) {
        boxes.push_back(
            {rect, dashboard::RecorderHighlightOverlay::Style::kCandidate});
      }
    }
    for (const auto& detail : guided_scrape_selection_details_) {
      if (!detail.bounds.IsEmpty()) {
        boxes.push_back({detail.bounds,
                         dashboard::RecorderHighlightOverlay::Style::kSelected});
      }
    }
  }
  if (capture_hover_rect_ && !capture_hover_rect_->IsEmpty()) {
    boxes.push_back(
        {*capture_hover_rect_,
         dashboard::RecorderHighlightOverlay::Style::kSelected});
  }
  const std::u16string& tip =
      !capture_hover_tip_.empty() ? capture_hover_tip_ : loop_preview_tip_;
  const gfx::Point tip_anchor = !capture_hover_tip_.empty()
                                    ? capture_hover_tip_anchor_
                                    : loop_preview_tip_anchor_;
  if (boxes.empty() && tip.empty()) {
    if (highlight_overlay_) {
      highlight_overlay_->Hide();
    }
    return;
  }
  if (!highlight_overlay_) {
    highlight_overlay_ = std::make_unique<dashboard::RecorderHighlightOverlay>();
  }
  highlight_overlay_->Show(target_web_contents_, std::move(boxes), tip,
                           tip_anchor);
}

void RecorderController::OnPageScrolledBy(double dy) {
  pending_scroll_y_ += dy;
  if (loop_preview_rects_.empty() && guided_scrape_pattern_rects_.empty() &&
      (!guided_scrape_overlay_enabled_ ||
       guided_scrape_selection_details_.empty())) {
    return;
  }
  const gfx::Vector2d shift(0, -static_cast<int>(std::lround(dy)));
  for (gfx::Rect& rect : loop_preview_rects_) {
    rect.Offset(shift);
  }
  for (auto& detail : guided_scrape_selection_details_) {
    detail.bounds.Offset(shift);
  }
  for (gfx::Rect& rect : guided_scrape_pattern_rects_) {
    rect.Offset(shift);
  }
  for (GuidedScrapePatternOption& option : guided_scrape_pattern_options_) {
    for (gfx::Rect& rect : option.bounds) {
      rect.Offset(shift);
    }
  }
  RefreshHighlightOverlay();
}

void RecorderController::CaptureScrollSnapshotForRecording() {
  const double dy = pending_scroll_y_;
  pending_scroll_y_ = 0;
  if (!is_recording_ || dashboard::IsRecordingSuppressed()) {
    return;
  }
  constexpr double kMinPixelDelta = 10.0;
  if (std::abs(dy) < kMinPixelDelta) {
    return;
  }
  base::Value::Dict action;
  action.Set("action", "scroll");
  base::Value::Dict params;
  params.Set("element", "window");
  params.Set("distance", static_cast<int>(std::lround(dy)));
  action.Set("params", std::move(params));
  AppendRecordedAction(std::move(action));
}

void RecorderController::ProbeSelectorsAtPoint(int x_css, int y_css) {
  const gfx::Point point(x_css, y_css);
  if (TryResolveClickFromTrackedState(point, /*require_non_visual=*/false,
                                      "probe_tracked_state")) {
    return;
  }
  const bool capture_only = locator_capture_armed_ &&
                            locator_capture_suppress_recording_side_effects_;
  base::Value::Dict click;
  click.Set("__source", capture_only ? "capture_xy_fallback" : "fallback");
  click.Set("x", x_css);
  click.Set("y", y_css);
  click.Set("click_count", 1);
  base::Value::List locators = BuildLocatorBundleForPoint(point);
  if (locators.empty()) {
    base::Value::Dict loc;
    loc.Set("type", "VISUAL_COORDS");
    loc.Set("value", base::StringPrintf("%d,%d", x_css, y_css));
    locators.Append(std::move(loc));
    click.Set("__warning", "rich_locator_unavailable_captured_xy");
  }
  click.Set("locators", std::move(locators));
  OnUserClickFromRenderer(std::move(click));
}

// -------------------- InputObserver --------------------
RecorderController::InputObserver::InputObserver(RecorderController* owner,
                                                 content::RenderWidgetHost* host)
    : owner_(owner), host_(host) {
  if (!host_)
    return;
  host_->AddObserver(this);
  host_->AddInputEventObserver(this);
  mouse_event_callback_ = base::BindRepeating(
      &RecorderController::InputObserver::InterceptMouseEvent,
      base::Unretained(this));
  host_->AddMouseEventCallback(mouse_event_callback_);
}

RecorderController::InputObserver::~InputObserver() {
  Detach();
}

void RecorderController::InputObserver::Detach() {
  owner_ = nullptr;
  if (!host_)
    return;
  host_->RemoveMouseEventCallback(mouse_event_callback_);
  host_->RemoveInputEventObserver(this);
  host_->RemoveObserver(this);
  host_ = nullptr;
}

void RecorderController::InputObserver::RenderWidgetHostDestroyed(
    content::RenderWidgetHost* widget_host) {
  // The host is going away; avoid trying to unregister against a freed pointer.
  host_ = nullptr;
  owner_ = nullptr;
}

bool RecorderController::InputObserver::InterceptMouseEvent(
    const blink::WebMouseEvent& event) {
  if (!owner_ || !host_ || !owner_->ShouldInterceptPageClicks()) {
    return false;
  }
  const blink::WebInputEvent::Type type = event.GetType();
  if (type != blink::WebInputEvent::Type::kMouseDown &&
      type != blink::WebInputEvent::Type::kMouseUp) {
    return false;
  }
  OnInputEvent(*host_, event, input::InputEventSource::kBrowser);
  return true;
}

void RecorderController::InputObserver::OnInputEvent(
    const content::RenderWidgetHost& host,
    const blink::WebInputEvent& event,
    InputEventSource source) {
  (void)source;
  if (!owner_ || !owner_->target_web_contents_)
    return;
  if (dashboard::IsRecordingSuppressed())
    return;

  MaybeHandleKeyboardEvent(event);

  const blink::WebInputEvent::Type type = event.GetType();
  if (type == blink::WebInputEvent::Type::kMouseWheel ||
      type == blink::WebInputEvent::Type::kGestureScrollUpdate ||
      type == blink::WebInputEvent::Type::kGestureScrollEnd) {
    // Touchpads report both wheel and generated gesture events; count wheel
    // deltas, and gesture deltas only for touchscreens.
    if (type == blink::WebInputEvent::Type::kMouseWheel) {
      const auto& wheel = static_cast<const blink::WebMouseWheelEvent&>(event);
      if (wheel.delta_units != ui::ScrollGranularity::kScrollByPage) {
        owner_->OnPageScrolledBy(-wheel.delta_y);
      }
    } else if (type == blink::WebInputEvent::Type::kGestureScrollUpdate) {
      const auto& gesture = static_cast<const blink::WebGestureEvent&>(event);
      if (gesture.SourceDevice() == blink::WebGestureDevice::kTouchscreen) {
        owner_->OnPageScrolledBy(-gesture.data.scroll_update.delta_y);
      }
    }
    owner_->FlushPendingTypeBuffer();
    owner_->ScheduleScrollCapture();
  }

  if (type != blink::WebInputEvent::Type::kMouseDown &&
      type != blink::WebInputEvent::Type::kMouseUp &&
      type != blink::WebInputEvent::Type::kMouseMove)
    return;

  const auto& me = static_cast<const blink::WebMouseEvent&>(event);
  int x_widget = static_cast<int>(me.PositionInWidget().x());
  int y_widget = static_cast<int>(me.PositionInWidget().y());

  // `WebMouseEvent::PositionInWidget()` is already in widget/DIP coordinates
  // (i.e. CSS pixels). However, when the event comes from an out-of-process
  // iframe (child local root), the point is in that widget's coordinate space.
  // Convert to the root frame coordinate space before hit-testing / probing.
  int x_css = x_widget;
  int y_css = y_widget;
  if (content::RenderWidgetHostView* view =
          const_cast<content::RenderWidgetHost&>(host).GetView()) {
    const gfx::Point root =
        view->TransformPointToRootCoordSpace(gfx::Point(x_widget, y_widget));
    x_css = root.x();
    y_css = root.y();
  }

  if (type == blink::WebInputEvent::Type::kMouseMove) {
    const bool guided_phase2_local_selection =
        owner_->is_recording_ && owner_->guided_loop_ui_state_.active &&
        owner_->guided_loop_ui_state_.phase == 2;
    if (guided_phase2_local_selection) {
      return;
    }

    const bool guided_phase1_loop_target =
        owner_->is_recording_ && owner_->guided_loop_ui_state_.active &&
        owner_->guided_loop_ui_state_.phase == 1 &&
        !owner_->manual_loop_capture_active_ &&
        !owner_->locator_capture_armed_;
    if (guided_phase1_loop_target) {
      owner_->MaybeQueueLoopPreview(x_widget, y_widget);
      return;
    }

    owner_->loop_preview_modifiers_active_ = false;
    if (owner_->loop_preview_visible_) {
      owner_->ClearLoopPreview();
      owner_->last_loop_preview_point_.reset();
    }

    if (owner_->active_pointer_down_point_) {
      owner_->active_pointer_last_point_ = gfx::Point(x_css, y_css);
      if (!owner_->active_pointer_dragged_ &&
          !NearPointManhattan(*owner_->active_pointer_down_point_,
                              *owner_->active_pointer_last_point_,
                              kDragGestureThresholdPx)) {
        owner_->active_pointer_dragged_ = true;
      }
    }
    if (owner_->locator_capture_armed_ &&
        !owner_->active_pointer_down_point_) {
      owner_->MaybeQueueCaptureHoverPreview(x_css, y_css);
    } else if (owner_->is_recording_ && !owner_->locator_capture_armed_ &&
               !owner_->active_pointer_down_point_) {
      owner_->MaybeQueueHoverProbe(x_css, y_css);
    }
    return;
  }

  if (type == blink::WebInputEvent::Type::kMouseDown) {
    if (me.button == blink::WebMouseEvent::Button::kLeft) {
      owner_->active_pointer_down_point_ = gfx::Point(x_css, y_css);
      owner_->active_pointer_last_point_ = gfx::Point(x_css, y_css);
      owner_->active_pointer_dragged_ = false;
    }
    return;
  }

  const std::optional<gfx::Point> drag_start = owner_->active_pointer_down_point_;
  const bool was_drag = owner_->active_pointer_dragged_;
  owner_->active_pointer_down_point_.reset();
  owner_->active_pointer_last_point_.reset();
  owner_->active_pointer_dragged_ = false;

  owner_->FlushPendingTypeBuffer();

  if (me.button != blink::WebMouseEvent::Button::kLeft)
    return;

  if (drag_start && was_drag) {
    owner_->RecordDragAction(*drag_start, gfx::Point(x_css, y_css));
    return;
  }

  const bool guided_phase1_loop_target =
      owner_->is_recording_ && owner_->guided_loop_ui_state_.active &&
      owner_->guided_loop_ui_state_.phase == 1 &&
      !owner_->manual_loop_capture_active_ &&
      !owner_->locator_capture_armed_;
  if (guided_phase1_loop_target) {
    owner_->CancelClickFallback();
    owner_->guided_loop_target_confirmation_pending_ = false;
    owner_->guided_loop_target_probe_point_ = gfx::Point(x_widget, y_widget);
    owner_->pending_manual_loop_capture_start_ = true;
    owner_->guided_loop_target_click_started_at_ = base::TimeTicks::Now();
    owner_->UpdateLoopPreviewAtPoint(x_widget, y_widget, /*active=*/true);
    return;
  }

  const bool guided_phase2_local_selection =
      owner_->is_recording_ && owner_->guided_loop_ui_state_.active &&
      owner_->guided_loop_ui_state_.phase == 2;
  if (guided_phase2_local_selection) {
    owner_->CancelClickFallback();
    owner_->ToggleGuidedScrapeSelectionAtPoint(gfx::Point(x_css, y_css));
    return;
  }

  const bool capture_only =
      owner_->locator_capture_armed_ &&
      owner_->locator_capture_suppress_recording_side_effects_;
  if (capture_only) {
    owner_->ResolveOneShotCaptureAtPoint(gfx::Point(x_css, y_css));
    return;
  } else {
    owner_->QueueClickFallback(x_css, y_css);
  }

  if (!owner_->TryResolveSelectorsViaRenderer(x_css, y_css)) {
    if (!owner_->TryResolveClickFromTrackedState(
            gfx::Point(x_css, y_css),
            /*require_non_visual=*/capture_only,
            "state_immediate")) {
      owner_->QueueClickFallback(x_css, y_css, base::Milliseconds(150));
    }
  }
}

void RecorderController::QueueClickFallback(int x_css,
                                            int y_css,
                                            base::TimeDelta delay) {
  if (!is_recording_)
    return;
  pending_click_fallback_point_ = gfx::Point(x_css, y_css);
  pending_click_fallback_timer_.Stop();
  pending_click_fallback_timer_.Start(
      FROM_HERE, delay.is_positive() ? delay : base::Milliseconds(900),
      base::BindOnce(
          [](base::WeakPtr<RecorderController> weak) {
            if (!weak || !weak->is_recording_)
              return;
            if (!weak->pending_click_fallback_point_)
              return;
            const gfx::Point pt = *weak->pending_click_fallback_point_;
            weak->pending_click_fallback_point_.reset();

            base::Value::Dict click;
            const bool capture_only =
                weak->locator_capture_armed_ &&
                weak->locator_capture_suppress_recording_side_effects_;
            if (weak->TryResolveClickFromTrackedState(
                    pt, /*require_non_visual=*/capture_only,
                    capture_only ? "capture_state_fallback"
                                 : "state_fallback")) {
              return;
            }
            click.Set("__source",
                      capture_only ? "capture_xy_fallback" : "fallback");
            click.Set("x", pt.x());
            click.Set("y", pt.y());
            click.Set("click_count", 1);
            if (capture_only) {
              base::Value::List locators;
              base::Value::Dict loc;
              loc.Set("type", "VISUAL_COORDS");
              loc.Set("value", base::StringPrintf("%d,%d", pt.x(), pt.y()));
              locators.Append(std::move(loc));
              click.Set("locators", std::move(locators));
              click.Set("__warning", "rich_locator_timeout_captured_xy");
            } else {
              click.Set("locators", weak->BuildLocatorBundleForPoint(pt));
            }
            weak->OnUserClickFromRenderer(std::move(click));
          },
          weak_factory_.GetWeakPtr()));
}

void RecorderController::CancelClickFallback() {
  pending_click_fallback_timer_.Stop();
  pending_click_fallback_point_.reset();
}

bool RecorderController::TryResolveClickFromTrackedState(
    const gfx::Point& point,
    bool require_non_visual,
    const char* source) {
  base::Value::List locators = BuildLocatorBundleForPoint(point);
  if (locators.empty()) {
    return false;
  }
  if (require_non_visual && !HasMeaningfulRecordedLocator(locators)) {
    return false;
  }

  base::Value::Dict click;
  if (source && *source) {
    click.Set("__source", source);
  }
  click.Set("x", point.x());
  click.Set("y", point.y());
  click.Set("click_count", 1);
  click.Set("locators", std::move(locators));
  AugmentClickParamsFromPageMapSnapshot(click, click);
  OnUserClickFromRenderer(std::move(click));
  return true;
}

void RecorderController::ResolveOneShotCaptureAtPoint(const gfx::Point& point) {
  locator_capture_last_input_at_ = base::TimeTicks::Now();
  CancelClickFallback();

  // 1) Primary: Mojo hit-test + single-node locator harvest (no full census).
  if (TryResolveSelectorsViaRenderer(point.x(), point.y())) {
    return;
  }

  // 2) Soft assist: warm APC/sidecar PageMap geometry match.
  if (TryResolveClickFromTrackedState(point, /*require_non_visual=*/true,
                                      "capture_tracked_state")) {
    return;
  }

  // 3) Refresh APC PageMap once, retry geometry, then CDP fallback.
  auto weak = weak_factory_.GetWeakPtr();
  CapturePageMap(base::BindOnce(
      [](base::WeakPtr<RecorderController> weak, gfx::Point point,
         base::Value::Dict /*pagemap*/) {
        if (!weak || !weak->locator_capture_armed_) {
          return;
        }
        if (weak->TryResolveClickFromTrackedState(
                point, /*require_non_visual=*/true,
                "capture_after_pagemap")) {
          return;
        }
        // 4) CDP getNodeForLocation (+ describe/attrs) as last resort.
        weak->ProbeSelectorsAtPoint(point.x(), point.y());
      },
      weak, point));
}

base::Value::List RecorderController::BuildRecordedLocatorBundleForClick(
    const base::Value::Dict& click) const {
  base::Value::List locators;
  std::set<std::pair<std::string, std::string>> seen;

  const std::optional<int> x = ReadIntLike(click, "x");
  const std::optional<int> y = ReadIntLike(click, "y");
  std::string visual_coords;
  if (x && y) {
    visual_coords = base::StringPrintf("%d,%d", *x, *y);
  }

  AppendRecordedLocator(locators, seen, "SHADOW_DOM",
                        click.FindString("shadow_chain")
                            ? *click.FindString("shadow_chain")
                            : std::string());

  const base::Value::List* raw_locators = click.FindList("locators");
  constexpr const char* kPreferredRecordedTypes[] = {
      "SHADOW_DOM",         "ACCESSIBILITY_NAME",
      "LINK_TEXT",         "PARTIAL_LINK_TEXT",  "TEXT_CONTENT",
      "ID",                "CLASS_NAME",         "NAME",
      "CSS_SELECTOR",      "JS_EVAL",
      "XPATH",
      "TAG_NAME",
  };
  for (const char* type : kPreferredRecordedTypes) {
    AppendRecordedLocatorsOfType(locators, seen, raw_locators, type);
  }

  if (const std::string* acc = click.FindString("accessibility_name")) {
    AppendRecordedLocator(locators, seen, "ACCESSIBILITY_NAME", *acc);
  }
  if (const std::string* text = click.FindString("text")) {
    AppendRecordedLocator(locators, seen, "TEXT_CONTENT", *text);
    const std::string tag = click.FindString("tag") ? *click.FindString("tag")
                                                    : std::string();
    if (IsLinkLikeTag(tag)) {
      AppendRecordedLocator(locators, seen, "LINK_TEXT", *text);
      AppendRecordedLocator(locators, seen, "PARTIAL_LINK_TEXT", *text);
    }
  }
  if (const std::string* css = click.FindString("css")) {
    AppendRecordedLocator(locators, seen, "CSS_SELECTOR", *css);
  }
  if (const std::string* xpath = click.FindString("xpath")) {
    AppendRecordedLocator(locators, seen, "XPATH", *xpath);
  }
  if (const std::string* tag = click.FindString("tag")) {
    AppendRecordedLocator(locators, seen, "TAG_NAME", *tag);
  }

  if (!visual_coords.empty() && !manual_loop_capture_active_) {
    AppendRecordedLocator(locators, seen, "VISUAL_COORDS",
                          std::move(visual_coords));
  }

  return ReorderRecordedLocatorBundle(std::move(locators));
}

base::Value::List RecorderController::BuildLocatorBundleForPoint(
    const gfx::Point& point) const {
  base::Value::Dict click_point;
  click_point.Set("x", point.x());
  click_point.Set("y", point.y());
  if (locator_capture_armed_) {
    LOG(INFO) << "[LocatorCapture][Point] build point=" << point.x() << ","
              << point.y() << " pagemap="
              << (last_page_map_snapshot_.has_value() ? 1 : 0);
  }
	  if (last_page_map_snapshot_.has_value()) {
    const base::Value::List* frames = last_page_map_snapshot_->FindList("frames");
    const base::Value::Dict* best_element = nullptr;
    const base::Value::Dict* best_selectors = nullptr;
    std::string best_frame_id;
    double best_score = 0.0;
    auto consider_element =
        [&](const base::Value::Dict& element, const std::string& frame_id) {
          const base::Value::Dict* selectors = element.FindDict("selectors");
          if (!selectors) {
            return;
          }
          const std::string element_frame_id =
              element.FindString("frameId") ? *element.FindString("frameId")
                                            : frame_id;
          const double score = ScorePointAgainstPageMapElement(
              point, element, /*require_containment=*/true);
          if (score <= best_score) {
            return;
          }
          best_score = score;
          best_element = &element;
          best_selectors = selectors;
          best_frame_id = element_frame_id;
        };
    if (frames) {
      for (const auto& frame_value : *frames) {
        const base::Value::Dict* frame = frame_value.GetIfDict();
        if (!frame) {
          continue;
        }
        const std::string frame_id =
            frame->FindString("frameId") ? *frame->FindString("frameId")
                                         : std::string();
        if (const base::Value::List* elements = frame->FindList("elements")) {
          for (const auto& element_value : *elements) {
            const base::Value::Dict* element = element_value.GetIfDict();
            if (element) {
              consider_element(*element, frame_id);
            }
          }
        }
        if (const base::Value::List* shadow_roots = frame->FindList("shadowRoots")) {
          for (const auto& shadow_value : *shadow_roots) {
            const base::Value::Dict* shadow_root = shadow_value.GetIfDict();
            if (!shadow_root) {
              continue;
            }
            if (const base::Value::List* elements =
                    shadow_root->FindList("elements")) {
              for (const auto& element_value : *elements) {
                const base::Value::Dict* element = element_value.GetIfDict();
                if (element) {
                  consider_element(*element, frame_id);
                }
              }
            }
          }
        }
      }
    }
	    if (best_element && best_selectors) {
	      if (locator_capture_armed_) {
	        LOG(INFO) << "[LocatorCapture][Point] pagemap match score="
	                  << best_score << " frame="
	                  << (best_frame_id.empty() ? std::string("<none>")
	                                            : best_frame_id);
	      }
	      std::set<std::pair<std::string, std::string>> seen;
      base::Value::List page_map_locators;
      if (!best_frame_id.empty()) {
        click_point.Set("frame_id", best_frame_id);
      }
      if (const std::string* shadow_chain = best_selectors->FindString("shadowChain")) {
        AppendRecordedLocator(page_map_locators, seen, "SHADOW_DOM", *shadow_chain);
        click_point.Set("shadow_chain", *shadow_chain);
      }
      if (const std::string* resource_path =
              best_selectors->FindString("resourcePath");
          resource_path && !IsRootLikeResourcePath(*resource_path)) {
        click_point.Set("resource_path", *resource_path);
      }
      AppendRecordedLocatorsFromPageMapSelectors(page_map_locators, seen,
                                                 *best_selectors);
      if (const std::string* accessibility_name =
              best_element->FindString("accessibilityName")) {
        AppendRecordedLocator(page_map_locators, seen, "ACCESSIBILITY_NAME",
                              *accessibility_name);
        click_point.Set("accessibility_name", *accessibility_name);
        click_point.Set("ax_name", *accessibility_name);
      }
      if (const std::string* text = best_element->FindString("text")) {
        AppendRecordedLocator(page_map_locators, seen, "TEXT_CONTENT", *text);
      }
      if (const std::string* tag = best_element->FindString("tag")) {
        AppendRecordedLocator(page_map_locators, seen, "TAG_NAME", *tag);
      }
      if (const std::string* role = best_element->FindString("role");
          role && !IsUnknownAxRoleValue(*role)) {
        click_point.Set("role", *role);
        click_point.Set("ax_role", *role);
        if (IsInteractiveAxRole(NormalizeAxRoleString(*role))) {
          click_point.Set("interactive_ancestor_role",
                          NormalizeAxRoleString(*role));
        }
      }
      if (const std::string* interactive_role =
              best_element->FindString("interactiveAncestorRole")) {
        click_point.Set("interactive_ancestor_role", *interactive_role);
      }
      if (!page_map_locators.empty()) {
        click_point.Set("locators",
                        ReorderRecordedLocatorBundle(std::move(page_map_locators)));
	      }
	    }
	    if (!best_element && locator_capture_armed_) {
	      LOG(INFO) << "[LocatorCapture][Point] pagemap no match";
	    }
	  }
	  base::Value::List out = BuildRecordedLocatorBundleForClick(click_point);
	  if (locator_capture_armed_) {
	    LOG(INFO) << "[LocatorCapture][Point] built locators=" << out.size()
	              << " [" << SummarizeLocatorTypesForLog(&out) << "] "
	              << SummarizeMetadataFields(click_point);
	  }
	  return out;
	}

base::Value::List RecorderController::SanitizeGuidedScrapeSelectionLocators(
    const base::Value::List& locators,
    bool allow_visual_fallback) const {
  base::Value::List sanitized;
  base::Value::List visual_fallback;
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& value : locators) {
    const base::Value::Dict* locator = value.GetIfDict();
    const std::string* type = locator ? locator->FindString("type") : nullptr;
    const std::string* raw_value =
        locator ? locator->FindString("value") : nullptr;
    if (!type || !raw_value || raw_value->empty())
      continue;
    const std::string upper_type = base::ToUpperASCII(*type);
    const std::string lower_value = base::ToLowerASCII(*raw_value);
    if (upper_type == "VISUAL_AI" || upper_type == "JS_EVAL") {
      continue;
    }
    if (lower_value.find("__dash_") != std::string::npos) {
      continue;
    }
    if (upper_type == "TAG_NAME" &&
        (lower_value == "a" || lower_value == "div" ||
         lower_value == "span" || lower_value == "body" ||
         lower_value == "html")) {
      continue;
    }
    if (upper_type == "CSS_SELECTOR" &&
        (lower_value == "a" || lower_value == "div" ||
         lower_value == "span" || lower_value == "body" ||
         lower_value == "html")) {
      continue;
    }
    if (upper_type == "XPATH" &&
        (lower_value == "//a" || lower_value == "//div" ||
         lower_value == "//span" || lower_value == "//body" ||
         lower_value == "//html")) {
      continue;
    }
    const auto dedupe_key = std::make_pair(upper_type, *raw_value);
    if (!seen.insert(dedupe_key).second)
      continue;
    if (upper_type == "VISUAL_COORDS") {
      if (allow_visual_fallback) {
        base::Value::Dict copy = locator->Clone();
        copy.Set("type", upper_type);
        visual_fallback.Append(std::move(copy));
      }
      continue;
    }
    base::Value::Dict copy = locator->Clone();
    copy.Set("type", upper_type);
    sanitized.Append(std::move(copy));
  }
  if (sanitized.empty() && allow_visual_fallback && !visual_fallback.empty()) {
    return ReorderRecordedLocatorBundle(std::move(visual_fallback));
  }
  return ReorderRecordedLocatorBundle(std::move(sanitized));
}

std::string RecorderController::GuidedScrapeSelectionSignature(
    const base::Value::List& locators) const {
  std::string json = "[]";
  base::JSONWriter::Write(base::Value(locators.Clone()), &json);
  return json;
}

bool RecorderController::ToggleGuidedScrapeSelectionLocators(
    base::Value::List locators,
    GuidedScrapeSelectionDetail detail) {
  if (locators.empty())
    return false;
  guided_scrape_selection_details_.resize(
      guided_scrape_selection_locators_.size());
  const std::string signature = GuidedScrapeSelectionSignature(locators);
  for (size_t i = 0; i < guided_scrape_selection_signatures_.size(); ++i) {
    if (guided_scrape_selection_signatures_[i] != signature)
      continue;
    guided_scrape_selection_signatures_.erase(
        guided_scrape_selection_signatures_.begin() + i);
    guided_scrape_selection_locators_.erase(
        guided_scrape_selection_locators_.begin() + i);
    guided_scrape_selection_details_.erase(
        guided_scrape_selection_details_.begin() + i);
    guided_loop_ui_state_.scrape_target_count =
        static_cast<uint32_t>(guided_scrape_selection_locators_.size());
    return true;
  }
  guided_scrape_selection_signatures_.push_back(signature);
  guided_scrape_selection_locators_.push_back(std::move(locators));
  guided_scrape_selection_details_.push_back(std::move(detail));
  guided_loop_ui_state_.scrape_target_count =
      static_cast<uint32_t>(guided_scrape_selection_locators_.size());
  return true;
}

void RecorderController::ClearGuidedScrapeSelections() {
  guided_scrape_selection_signatures_.clear();
  guided_scrape_selection_locators_.clear();
  guided_scrape_selection_details_.clear();
  guided_scrape_selection_model_json_.clear();
  guided_scrape_row_locator_.clear();
  guided_scrape_row_count_ = 0;
  guided_scrape_pattern_options_.clear();
  guided_scrape_pattern_index_ = 0;
  guided_scrape_pattern_rects_.clear();
  guided_loop_ui_state_.scrape_target_count = 0;
  SetGuidedScrapeOverlayEnabled(false);
}

void RecorderController::SetGuidedScrapeOverlayEnabled(bool enabled) {
  guided_scrape_overlay_enabled_ = enabled;
  RefreshHighlightOverlay();
}

void RecorderController::PopGuidedScrapeOverlaySelection() {
  guided_scrape_selection_details_.resize(
      guided_scrape_selection_locators_.size());
  if (guided_scrape_selection_locators_.empty()) {
    guided_scrape_row_locator_.clear();
    guided_scrape_row_count_ = 0;
  }
  RebuildGuidedScrapeSelectionModel();
  RefreshHighlightOverlay();
}

void RecorderController::OnGuidedScrapeSelectionCaptureResult(
    const gfx::Point& point,
    base::Value::Dict result) {
  base::Value::List locators = SanitizeGuidedScrapeSelectionLocators(
      result.FindList("locators") ? result.FindList("locators")->Clone()
                                  : base::Value::List(),
      /*allow_visual_fallback=*/false);
  if (!HasMeaningfulRecordedLocator(locators)) {
    locators = SanitizeGuidedScrapeSelectionLocators(
        BuildLocatorBundleForPoint(point), /*allow_visual_fallback=*/true);
  }
  if (!HasMeaningfulRecordedLocator(locators)) {
    LOG(INFO) << "[GuidedScrape] Using VISUAL_COORDS fallback for selection at "
              << point.ToString();
    locators.clear();
    base::Value::Dict visual;
    visual.Set("type", "VISUAL_COORDS");
    visual.Set("value", base::StringPrintf("{\"x\":%d,\"y\":%d}",
                                           point.x(), point.y()));
    locators.Append(std::move(visual));
  }

  auto done = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      base::BindOnce(&RecorderController::OnGuidedScrapeSelectionPeersResolved,
                     weak_factory_.GetWeakPtr(), std::move(locators), point),
      nullptr);
  dashboard::mojom::StealthyElementResolver* resolver = MainFrameResolver();
  if (!resolver) {
    std::move(done).Run(nullptr);
    return;
  }
  resolver->FindRepeatedPeersAtPoint(point.x(), point.y(), /*max_peers=*/200,
                                     std::move(done));
}

void RecorderController::OnGuidedScrapeSelectionPeersResolved(
    base::Value::List locators,
    gfx::Point point,
    dashboard::mojom::RepeatedPeersPtr peers) {
  GuidedScrapeSelectionDetail detail;
  if (peers) {
    detail.relative_locator = peers->relative_locator;
    if (peers->target && peers->target->ok) {
      detail.bounds = gfx::Rect(peers->target->x, peers->target->y,
                                peers->target->width, peers->target->height);
      std::string hint = std::string(base::CollapseWhitespaceASCII(
          peers->target->text, /*trim_sequences_with_line_breaks=*/true));
      if (hint.size() > 40) {
        hint.resize(40);
      }
      detail.field_name_hint = std::move(hint);
    }
    guided_scrape_pattern_options_.clear();
    for (const auto& option : peers->options) {
      if (!option || option->locator.empty()) {
        continue;
      }
      GuidedScrapePatternOption stored;
      stored.locator = option->locator;
      stored.relative_locator = option->relative_locator;
      stored.label = option->label;
      stored.count = option->count;
      stored.samples.assign(option->samples.begin(), option->samples.end());
      for (const auto& peer : option->peers) {
        if (!peer) {
          continue;
        }
        stored.bounds.emplace_back(peer->x, peer->y, peer->width, peer->height);
      }
      guided_scrape_pattern_options_.push_back(std::move(stored));
    }
    if (guided_scrape_pattern_options_.empty() && !peers->locator.empty()) {
      GuidedScrapePatternOption stored;
      stored.locator = peers->locator;
      stored.relative_locator = peers->relative_locator;
      stored.count = static_cast<int>(peers->peers.size());
      stored.label = base::StringPrintf("%d matches", stored.count);
      for (const auto& peer : peers->peers) {
        if (!peer) {
          continue;
        }
        stored.bounds.emplace_back(peer->x, peer->y, peer->width, peer->height);
        if (stored.samples.size() < 6 && !peer->text.empty()) {
          stored.samples.push_back(peer->text);
        }
      }
      guided_scrape_pattern_options_.push_back(std::move(stored));
    }
    guided_scrape_pattern_index_ = 0;
  }
  if (detail.bounds.IsEmpty()) {
    detail.bounds = gfx::Rect(point.x() - 12, point.y() - 12, 24, 24);
  }
  // A later click on a column that was filled in from the first cell removes
  // that column instead of recording a second, absolute locator.
  if (!detail.relative_locator.empty() &&
      guided_scrape_selection_details_.size() > 1) {
    for (size_t i = 0; i < guided_scrape_selection_details_.size(); ++i) {
      if (guided_scrape_selection_details_[i].relative_locator !=
          detail.relative_locator) {
        continue;
      }
      guided_scrape_selection_signatures_.erase(
          guided_scrape_selection_signatures_.begin() +
          static_cast<ptrdiff_t>(i));
      guided_scrape_selection_locators_.erase(
          guided_scrape_selection_locators_.begin() +
          static_cast<ptrdiff_t>(i));
      guided_scrape_selection_details_.erase(
          guided_scrape_selection_details_.begin() +
          static_cast<ptrdiff_t>(i));
      guided_loop_ui_state_.scrape_target_count =
          static_cast<uint32_t>(guided_scrape_selection_locators_.size());
      RebuildGuidedScrapeSelectionModel();
      RefreshHighlightOverlay();
      NotifyStateChanged();
      return;
    }
  }
  const size_t selection_count_before = guided_scrape_selection_locators_.size();
  if (!ToggleGuidedScrapeSelectionLocators(std::move(locators),
                                           std::move(detail))) {
    return;
  }
  // One click in a table fills the other columns of that row. Playback applies
  // each column to every row of the chosen pattern.
  if (selection_count_before == 0 &&
      guided_scrape_selection_locators_.size() == 1) {
    const GuidedScrapePatternOption* cell_option = nullptr;
    bool has_rows = false;
    for (const auto& option : guided_scrape_pattern_options_) {
      if (option.locator.find("> tr") != std::string::npos ||
          option.label.find("rows") != std::string::npos) {
        has_rows = true;
      }
      const bool is_td = option.locator.find("> td") != std::string::npos;
      const bool is_th = option.locator.find("> th") != std::string::npos;
      if ((!is_td && !is_th) || option.count < 2 || option.count > 16) {
        continue;
      }
      const bool chosen_is_td =
          cell_option &&
          cell_option->locator.find("> td") != std::string::npos;
      if (!cell_option || (is_td && !chosen_is_td) ||
          (is_td == chosen_is_td && option.count < cell_option->count)) {
        cell_option = &option;
      }
    }
    if (has_rows && cell_option) {
      const std::string cell_tag =
          cell_option->locator.find("> th") != std::string::npos &&
                  cell_option->locator.find("> td") == std::string::npos
              ? "th"
              : "td";
      const int columns = cell_option->count;
      guided_scrape_selection_signatures_.clear();
      guided_scrape_selection_locators_.clear();
      guided_scrape_selection_details_.clear();
      for (int column = 1; column <= columns; ++column) {
        const std::string relative =
            base::StringPrintf("%s:nth-child(%d)", cell_tag.c_str(), column);
        base::Value::List column_locators;
        base::Value::Dict column_locator;
        column_locator.Set("type", "CSS_SELECTOR");
        column_locator.Set("value", relative);
        column_locators.Append(std::move(column_locator));
        GuidedScrapeSelectionDetail column_detail;
        column_detail.relative_locator = relative;
        if (static_cast<size_t>(column - 1) < cell_option->samples.size()) {
          column_detail.field_name_hint =
              cell_option->samples[static_cast<size_t>(column - 1)];
          if (column_detail.field_name_hint.size() > 40) {
            column_detail.field_name_hint.resize(40);
          }
        }
        if (static_cast<size_t>(column - 1) < cell_option->bounds.size()) {
          column_detail.bounds =
              cell_option->bounds[static_cast<size_t>(column - 1)];
        }
        ToggleGuidedScrapeSelectionLocators(std::move(column_locators),
                                            std::move(column_detail));
      }
    }
  }
  if (guided_scrape_selection_locators_.empty()) {
    guided_scrape_row_locator_.clear();
    guided_scrape_row_count_ = 0;
    guided_scrape_pattern_options_.clear();
    guided_scrape_pattern_rects_.clear();
  } else if (!guided_scrape_pattern_options_.empty()) {
    SelectGuidedScrapePatternForTools(guided_scrape_pattern_index_, nullptr);
    return;
  }
  RebuildGuidedScrapeSelectionModel();
  RefreshHighlightOverlay();
  NotifyStateChanged();
}

RecorderController::GuidedScrapePatternOption::GuidedScrapePatternOption() =
    default;
RecorderController::GuidedScrapePatternOption::~GuidedScrapePatternOption() =
    default;
RecorderController::GuidedScrapePatternOption::GuidedScrapePatternOption(
    const GuidedScrapePatternOption&) = default;
RecorderController::GuidedScrapePatternOption&
RecorderController::GuidedScrapePatternOption::operator=(
    const GuidedScrapePatternOption&) = default;
RecorderController::GuidedScrapePatternOption::GuidedScrapePatternOption(
    GuidedScrapePatternOption&&) noexcept = default;
RecorderController::GuidedScrapePatternOption&
RecorderController::GuidedScrapePatternOption::operator=(
    GuidedScrapePatternOption&&) noexcept = default;

bool RecorderController::SelectGuidedScrapePatternForTools(
    int index,
    std::string* error_out) {
  if (guided_scrape_pattern_options_.empty()) {
    if (error_out) {
      *error_out = "no_pattern_options";
    }
    return false;
  }
  if (index < 0 ||
      index >= static_cast<int>(guided_scrape_pattern_options_.size())) {
    if (error_out) {
      *error_out = "pattern_index_out_of_range";
    }
    return false;
  }
  guided_scrape_pattern_index_ = index;
  const GuidedScrapePatternOption& option =
      guided_scrape_pattern_options_[static_cast<size_t>(index)];
  guided_scrape_row_locator_ = option.locator;
  guided_scrape_row_count_ = option.count;
  guided_scrape_pattern_rects_ = option.bounds;
  // A single clicked field takes its path from the chosen pattern. Column
  // fields already have their own row-relative selectors.
  if (guided_scrape_selection_details_.size() == 1 &&
      !option.relative_locator.empty()) {
    guided_scrape_selection_details_.back().relative_locator =
        option.relative_locator;
  }
  if (guided_loop_ui_state_.phase == 2) {
    loop_preview_tip_ = base::UTF8ToUTF16(base::StringPrintf(
        "%s highlighted. [ ] cycles patterns. Right Arrow scrapes this page.",
        option.label.c_str()));
    loop_preview_tip_anchor_ = option.bounds.empty()
                                   ? gfx::Point()
                                   : option.bounds.front().origin();
  }
  RebuildGuidedScrapeSelectionModel();
  RefreshHighlightOverlay();
  NotifyStateChanged();
  return true;
}

void RecorderController::RebuildGuidedScrapeSelectionModel() {
  auto css_locator = [](const std::string& value) {
    base::Value::List list;
    if (!value.empty()) {
      base::Value::Dict loc;
      loc.Set("type", "CSS_SELECTOR");
      loc.Set("value", value);
      list.Append(std::move(loc));
    }
    return list;
  };
  base::Value::List selections;
  for (size_t i = 0; i < guided_scrape_selection_locators_.size(); ++i) {
    base::Value::Dict selection;
    selection.Set("locators", guided_scrape_selection_locators_[i].Clone());
    if (i < guided_scrape_selection_details_.size()) {
      const GuidedScrapeSelectionDetail& detail =
          guided_scrape_selection_details_[i];
      selection.Set("relativeLocators", css_locator(detail.relative_locator));
      if (!detail.field_name_hint.empty()) {
        selection.Set("fieldNameHint", detail.field_name_hint);
      }
    }
    selections.Append(std::move(selection));
  }
  base::Value::List options;
  for (size_t i = 0; i < guided_scrape_pattern_options_.size(); ++i) {
    const GuidedScrapePatternOption& option = guided_scrape_pattern_options_[i];
    base::Value::Dict entry;
    entry.Set("locator", option.locator);
    entry.Set("relativeLocator", option.relative_locator);
    entry.Set("label", option.label);
    entry.Set("count", option.count);
    entry.Set("selected", static_cast<int>(i) == guided_scrape_pattern_index_);
    base::Value::List samples;
    for (const std::string& sample : option.samples) {
      samples.Append(sample);
    }
    entry.Set("samples", std::move(samples));
    base::Value::List row_locators = css_locator(option.locator);
    entry.Set("rowLocators", std::move(row_locators));
    options.Append(std::move(entry));
  }
  base::Value::Dict model;
  model.Set("selections", std::move(selections));
  model.Set("rowLocators", css_locator(guided_scrape_row_locator_));
  model.Set("rowCount", guided_scrape_row_count_);
  model.Set("selectedOption", guided_scrape_pattern_index_);
  model.Set("options", std::move(options));
  guided_scrape_selection_model_json_.clear();
  base::JSONWriter::Write(base::Value(std::move(model)),
                          &guided_scrape_selection_model_json_);
}

void RecorderController::GetGuidedScrapeSelectionLocatorsForTools(
    base::OnceCallback<void(bool ok,
                            std::string selections_json,
                            std::string error)> callback) {
  if (guided_scrape_selection_model_json_.empty()) {
    RebuildGuidedScrapeSelectionModel();
  }
  std::move(callback).Run(true, guided_scrape_selection_model_json_,
                          std::string());
}

void RecorderController::ToggleGuidedScrapeSelectionAtPoint(
    const gfx::Point& point) {
  ArmOneShotLocatorCapture(
      base::Seconds(3),
      /*suppress_recording_side_effects=*/true,
      base::BindOnce(&RecorderController::OnGuidedScrapeSelectionCaptureResult,
                     weak_factory_.GetWeakPtr(), point));
  ResolveOneShotCaptureAtPoint(point);
}

void RecorderController::RecordDragAction(const gfx::Point& from_point,
                                          const gfx::Point& to_point) {
  CancelClickFallback();
  if (!is_recording_ || dashboard::IsRecordingSuppressed()) {
    return;
  }

  base::Value::Dict action;
  action.Set("action", "drag");
  base::Value::Dict params;

  base::Value::Dict from_coords;
  from_coords.Set("x", from_point.x());
  from_coords.Set("y", from_point.y());
  base::Value::Dict to_coords;
  to_coords.Set("x", to_point.x());
  to_coords.Set("y", to_point.y());
  params.Set("fromCoordinates", std::move(from_coords));
  params.Set("toCoordinates", std::move(to_coords));
  params.Set("fromLocators", BuildLocatorBundleForPoint(from_point));
  params.Set("toLocators", BuildLocatorBundleForPoint(to_point));
  params.Set("steps", 12);
  params.Set("waitTimeout", 15000);
  params.Set("humanOffset", true);

  action.Set("params", std::move(params));
  AppendRecordedAction(std::move(action));

  const base::TimeTicks now = base::TimeTicks::Now();
  last_recorded_click_at_ = now;
  last_recorded_click_point_ = to_point;
  last_recorded_click_count_ = 1;
  last_recorded_click_frame_id_.clear();
  last_recorded_click_source_ = "drag_input";
  last_non_hover_click_at_ = now;
  last_non_hover_click_point_ = to_point;
}

void RecorderController::InputObserver::MaybeHandleKeyboardEvent(
    const blink::WebInputEvent& event) {
  if (!owner_)
    return;

  if (event.GetType() == blink::WebInputEvent::Type::kKeyUp) {
    const auto& ku = static_cast<const blink::WebKeyboardEvent&>(event);
    const bool is_shift_or_ctrl =
        ku.windows_key_code == 0x10 || ku.windows_key_code == 0x11 ||
        ku.windows_key_code == 0x5B || ku.windows_key_code == 0x5C;
    if (is_shift_or_ctrl) {
      owner_->loop_preview_modifiers_active_ = false;
      owner_->loop_preview_key_chord_active_ = false;
      owner_->ClearLoopPreview();
      owner_->last_loop_preview_point_.reset();
      owner_->pending_manual_loop_capture_start_ = false;
    }
    return;
  }

  if (event.GetType() != blink::WebInputEvent::Type::kKeyDown &&
      event.GetType() != blink::WebInputEvent::Type::kRawKeyDown) {
    return;
  }

  const auto& ke = static_cast<const blink::WebKeyboardEvent&>(event);
  owner_->loop_preview_key_chord_active_ = false;

  if (owner_->HandleGuidedPageArrowShortcut(ke.windows_key_code)) {
    return;
  }

  const bool is_tab = ke.windows_key_code == 0x09;
  const bool is_enter = ke.windows_key_code == 0x0D;
  const bool is_backspace = ke.windows_key_code == 0x08;

  if (is_tab || is_enter) {
    owner_->FlushPendingTypeBuffer();
    if (owner_->is_recording()) {
      base::Value::Dict action;
      action.Set("action", "key_press");
      base::Value::Dict params;
      params.Set("key", is_tab ? "Tab" : "Enter");
      action.Set("params", std::move(params));
      owner_->AppendRecordedAction(std::move(action));
    }
    return;
  }

  if (is_backspace) {
    owner_->HandleBackspaceForTypeBuffer();
    return;
  }

  owner_->MaybeAppendCharacterForTypeBuffer(ke);

  if (!owner_->pending_type_buffer_.empty()) {
    bool is_modifier_key = ke.windows_key_code == 0x10 || ke.windows_key_code == 0x11 ||
                           ke.windows_key_code == 0x12 || ke.windows_key_code == 0x5B ||
                           ke.windows_key_code == 0x5C;
    if (!is_modifier_key && ke.text[0] == 0) {
      owner_->FlushPendingTypeBuffer();
    }
  }
}

void RecorderController::AttachInputObserver() {
  if (input_observer_) {
    return;
  }
  if (!target_web_contents_)
    return;
  content::RenderWidgetHostView* view =
      target_web_contents_->GetRenderWidgetHostView();
  if (!view)
    return;
  content::RenderWidgetHost* host = view->GetRenderWidgetHost();
  if (!host)
    return;
  input_observer_ = std::make_unique<InputObserver>(this, host);
}

void RecorderController::DetachInputObserver() {
  if (!input_observer_)
    return;
  // Unregister from the exact RenderWidgetHost we registered against. The
  // active WebContents can swap RenderWidgetHost instances during navigation or
  // focus changes (e.g., menus / subframes), and removing from the "current"
  // host can miss the original registration and leave a dangling pointer.
  input_observer_->Detach();
  input_observer_.reset();
}

bool RecorderController::ShouldMaintainInstrumentationSession() const {
  return explicit_recording_enabled_ || locator_capture_armed_ ||
         manual_loop_capture_active_ || guided_loop_ui_state_.active;
}

bool RecorderController::ShouldObserveInput() const {
  return ShouldMaintainInstrumentationSession();
}

void RecorderController::EnsureDevToolsAttached() {
  if (!target_web_contents_) {
    return;
  }
  if (!agent_host_ || !devtools_client_) {
    agent_host_ =
        content::DevToolsAgentHost::GetOrCreateFor(target_web_contents_);
    if (!agent_host_) {
      return;
    }

    devtools_client_ = std::make_unique<RecorderDevToolsClient>(
        base::DoNothing(),
        base::BindRepeating(&RecorderController::OnDevToolsResponse,
                            weak_factory_.GetWeakPtr()),
        base::BindRepeating(&RecorderController::OnDevToolsEvent,
                            weak_factory_.GetWeakPtr()));

    agent_host_->AttachClient(devtools_client_.get());
    cdp_domains_enabled_ = false;
    network_capture_enabled_ = false;
  }

  // Stealth default: do not Runtime.enable/DOM.enable until a CDP call needs
  // them (lazy via EnsureCdpDomainsEnabled). Eager only when opted in.
  if (dashboard::RecordingEagerCdpAttach() && !cdp_domains_enabled_) {
    SendCommand("DOM.enable", base::Value::Dict());
    SendCommand("Runtime.enable", base::Value::Dict());
    cdp_domains_enabled_ = true;
  }

  if (dashboard::RecordingAllowPageInjects()) {
    if (guided_same_tab_navigation_guard_enabled_) {
      const bool block_all_anchor_navigation =
          guided_loop_ui_state_.active && guided_loop_ui_state_.phase == 1 &&
          !manual_loop_capture_active_;
      SetGuidedLoopSameTabNavigationGuard(true, block_all_anchor_navigation);
    }
    RefreshOneShotCaptureClickSuppression();
  }
}

void RecorderController::EnsureCdpDomainsEnabled() {
  EnsureDevToolsAttached();
  if (!agent_host_ || !devtools_client_ || cdp_domains_enabled_) {
    return;
  }
  // Mark enabled first so SendCommand re-entry does not recurse.
  cdp_domains_enabled_ = true;
  SendCommand("DOM.enable", base::Value::Dict());
  SendCommand("Runtime.enable", base::Value::Dict());
  LOG(INFO) << "[Recorder] CDP DOM/Runtime domains enabled (lazy)";
}

void RecorderController::DetachDevToolsSession() {
  if (agent_host_ && devtools_client_) {
    agent_host_->DetachClient(devtools_client_.get());
  }
  devtools_client_.reset();
  agent_host_.reset();
  next_command_id_ = 1;
  pending_eval_.clear();
  click_stealth_resolver_.reset();
  click_resolver_.reset();
  cdp_domains_enabled_ = false;
  network_capture_enabled_ = false;
}

void RecorderController::UpdateInstrumentationState() {
  if (!ShouldMaintainInstrumentationSession()) {
    DetachInputObserver();
    DetachDevToolsSession();
    return;
  }

  // Always observe input browser-side (not page-visible).
  // CDP attach is optional in stealth mode until a CDP fallback is needed.
  if (dashboard::RecordingEagerCdpAttach() ||
      dashboard::RecordingAllowPageInjects()) {
    EnsureDevToolsAttached();
  }
  if (ShouldObserveInput()) {
    AttachInputObserver();
  } else {
    DetachInputObserver();
  }
}


// -------------------- Active tab + DevTools attach --------------------
void RecorderController::AttachToWebContents(content::WebContents* wc) {
  if (!wc) {
    return;
  }

  if (IsCapturableWebContents(wc, /*avoid_contents=*/nullptr)) {
    last_capturable_tab_id_ = sessions::SessionTabHelper::IdForTab(wc).id();
  }

  if (target_web_contents_ == wc && agent_host_ && devtools_client_) {
    current_url_ = wc->GetVisibleURL().spec();
    last_committed_entry_index_ = wc->GetController().GetLastCommittedEntryIndex();
    if (auto* rfh = wc->GetPrimaryMainFrame()) {
      target_main_frame_token_ = rfh->GetFrameToken().ToString();
    } else {
      target_main_frame_token_.clear();
    }
    Observe(wc);
    UpdateInstrumentationState();
    return;
  }

  DetachInputObserver();
  DetachFromTarget();

  target_web_contents_ = wc;
  current_url_ = wc->GetVisibleURL().spec();
  last_committed_entry_index_ = wc->GetController().GetLastCommittedEntryIndex();
  if (auto* rfh = wc->GetPrimaryMainFrame()) {
    target_main_frame_token_ = rfh->GetFrameToken().ToString();
  } else {
    target_main_frame_token_.clear();
  }
  Observe(target_web_contents_);
  UpdateInstrumentationState();
}

void RecorderController::AttachToActiveTab(BrowserView* browser_view) {
  if (!browser_view || !browser_view->browser())
    return;
  Browser* browser = browser_view->browser();
  content::WebContents* wc = browser->tab_strip_model()->GetActiveWebContents();
  AttachToWebContents(wc);
}

bool RecorderController::AttachToBestCapturableTarget(
    BrowserView* preferred_browser_view,
    content::WebContents* avoid_contents,
    const std::string& preferred_context_url) {
  ObservePreferredTabStrip(preferred_browser_view);
  GURL preferred_context_gurl(preferred_context_url);
  if (content::WebContents* tracked =
          ResolveTrackedCapturableWebContents(preferred_browser_view,
                                              avoid_contents);
      tracked &&
      (!preferred_context_gurl.is_valid() ||
       ContextUrlsMatchExactlyOrByHost(
           tracked->GetLastCommittedURL().is_valid()
               ? tracked->GetLastCommittedURL()
               : tracked->GetVisibleURL(),
           preferred_context_gurl))) {
    AttachToWebContents(tracked);
    return target_web_contents_ == tracked;
  }
  if (content::WebContents* best =
          FindBestCapturableWebContents(preferred_browser_view, avoid_contents,
                                        preferred_context_url)) {
    AttachToWebContents(best);
    return target_web_contents_ == best;
  }

  if (IsCapturableWebContents(target_web_contents_, avoid_contents)) {
    return true;
  }

  return false;
}

// -------------------- DevTools-based selector probe --------------------
bool RecorderController::TryResolveSelectorsViaRenderer(int x_css, int y_css) {
  if (!ShouldUseRendererHitTestLocatorCapture()) {
    if (locator_capture_armed_) {
      LOG(INFO) << "[LocatorCapture] renderer_resolver skipped:"
                << " renderer_hit_test_locator_capture_disabled";
    }
    return false;
  }
  if (!target_web_contents_)
    return false;
  content::RenderFrameHost* rfh = target_web_contents_->GetPrimaryMainFrame();
  if (!rfh)
    return false;

  pending_click_resolve_point_ = gfx::Point(x_css, y_css);
  if (locator_capture_armed_) {
    LOG(INFO) << "[LocatorCapture] renderer_resolver start point="
              << base::StringPrintf("%d,%d", x_css, y_css);
  }

  // Prepare callbacks to normalize the resolver result.
  auto make_cb = [this, x_css, y_css]() {
    return base::BindOnce(
      [](base::WeakPtr<RecorderController> weak,
         int x_css_cb,
         int y_css_cb,
         dashboard::mojom::ResolvedElementInfoPtr info) {
        if (!weak)
          return;

        // If the resolver failed, fall back: APC geometry → CDP probe.
        if (!info || !info->ok) {
          if (weak->locator_capture_armed_) {
            LOG(INFO) << "[LocatorCapture] renderer_resolver failed -> "
                      << "apc_geometry/cdp_fallback";
          }
          const bool capture_only =
              weak->locator_capture_armed_ &&
              weak->locator_capture_suppress_recording_side_effects_;
          if (weak->TryResolveClickFromTrackedState(
                  gfx::Point(x_css_cb, y_css_cb),
                  /*require_non_visual=*/capture_only,
                  "state_after_renderer_failure")) {
            return;
          }
          if (weak->locator_capture_armed_) {
            weak->ProbeSelectorsAtPoint(x_css_cb, y_css_cb);
            return;
          }
          weak->QueueClickFallback(x_css_cb, y_css_cb,
                                   base::Milliseconds(150));
          return;
        }

        const std::string tag_lower = base::ToLowerASCII(info->tag);
        if (tag_lower == "body" || tag_lower == "html" ||
            info->css == "body" || info->css == "html" ||
            info->xpath == "//body" || info->xpath == "//html") {
          if (weak->locator_capture_armed_) {
            LOG(INFO) << "[LocatorCapture] renderer_resolver root-only result"
                      << " tag=" << info->tag << " css=" << info->css
                      << " xpath=" << info->xpath
                      << " -> apc_geometry/cdp_fallback";
          }
          const bool capture_only =
              weak->locator_capture_armed_ &&
              weak->locator_capture_suppress_recording_side_effects_;
          if (weak->TryResolveClickFromTrackedState(
                  gfx::Point(x_css_cb, y_css_cb),
                  /*require_non_visual=*/capture_only,
                  "state_after_renderer_root")) {
            return;
          }
          if (weak->locator_capture_armed_) {
            weak->ProbeSelectorsAtPoint(x_css_cb, y_css_cb);
            return;
          }
          weak->QueueClickFallback(x_css_cb, y_css_cb,
                                   base::Milliseconds(150));
          return;
        }

        if (weak->locator_capture_armed_) {
          LOG(INFO) << "[LocatorCapture] renderer_resolver success tag="
                    << (info->tag.empty() ? std::string("<empty>") : info->tag)
                    << " frame="
                    << (info->frame_id.empty() ? std::string("<none>")
                                               : info->frame_id)
                    << " locators=" << info->locators.size();
        }

        weak->pending_click_resolve_point_.reset();

        base::Value::Dict dict;
        dict.Set("__source", "renderer_resolver");
        dict.Set("css", info->css);
        dict.Set("xpath", info->xpath);
        dict.Set("x", info->center_x);
        dict.Set("y", info->center_y);
        dict.Set("frame_id", info->frame_id);
        dict.Set("dom_node_id", info->dom_node_id);
        if (!info->tag.empty()) dict.Set("tag", info->tag);
        if (!info->text.empty()) dict.Set("text", info->text);
        if (!info->shadow_chain_selector.empty()) {
          dict.Set("shadow_chain", info->shadow_chain_selector);
        }
        if (!info->resource_path.empty()) {
          dict.Set("resource_path", info->resource_path);
        }
        if (!info->accessibility_name.empty()) {
          dict.Set("accessibility_name", info->accessibility_name);
          dict.Set("ax_name", info->accessibility_name);
        }
        if (!info->role.empty()) {
          dict.Set("role", info->role);
          dict.Set("ax_role", info->role);
          if (info->clickable || info->focusable) {
            dict.Set("interactive_ancestor_role", info->role);
          }
        }
        dict.Set("is_disabled", info->is_disabled);
        if (!info->locators.empty()) {
          base::Value::List locs;
          for (const auto& l : info->locators) {
            base::Value::Dict d; d.Set("type", l->type); d.Set("value", l->value); locs.Append(std::move(d));
          }
          dict.Set("locators", std::move(locs));
        }
        weak->OnUserClickFromRenderer(std::move(dict));
      },
      weak_factory_.GetWeakPtr(), x_css, y_css);
  };

  // 1) Try StealthyElementResolver.
  {
    click_stealth_resolver_.reset();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(&click_stealth_resolver_);
    if (click_stealth_resolver_.is_bound()) {
      click_stealth_resolver_.set_disconnect_handler(base::BindOnce(
          [](base::WeakPtr<RecorderController> weak) {
            if (!weak)
              return;
            if (weak->pending_click_resolve_point_) {
              const gfx::Point p = *weak->pending_click_resolve_point_;
              weak->pending_click_resolve_point_.reset();
              const bool capture_only =
                  weak->locator_capture_armed_ &&
                  weak->locator_capture_suppress_recording_side_effects_;
              if (!weak->TryResolveClickFromTrackedState(
                      p, /*require_non_visual=*/capture_only,
                      "state_after_renderer_disconnect")) {
                weak->QueueClickFallback(p.x(), p.y(),
                                         base::Milliseconds(150));
              }
            }
          },
          weak_factory_.GetWeakPtr()));
      auto cb1 = make_cb();
      click_stealth_resolver_->ResolveAtPoint(x_css, y_css, std::move(cb1));
      if (locator_capture_armed_) {
        LOG(INFO) << "[LocatorCapture] using StealthyElementResolver";
      }
      return true;
    }
  }
  // 2) Fallback to ElementResolver.
  {
    click_resolver_.reset();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(&click_resolver_);
    if (click_resolver_.is_bound()) {
      click_resolver_.set_disconnect_handler(base::BindOnce(
          [](base::WeakPtr<RecorderController> weak) {
            if (!weak)
              return;
            if (weak->pending_click_resolve_point_) {
              const gfx::Point p = *weak->pending_click_resolve_point_;
              weak->pending_click_resolve_point_.reset();
              const bool capture_only =
                  weak->locator_capture_armed_ &&
                  weak->locator_capture_suppress_recording_side_effects_;
              if (!weak->TryResolveClickFromTrackedState(
                      p, /*require_non_visual=*/capture_only,
                      "state_after_resolver_disconnect")) {
                weak->QueueClickFallback(p.x(), p.y(),
                                         base::Milliseconds(150));
              }
            }
          },
          weak_factory_.GetWeakPtr()));
      auto cb2 = make_cb();
      click_resolver_->ResolveAtPoint(x_css, y_css, std::move(cb2));
      if (locator_capture_armed_) {
        LOG(INFO) << "[LocatorCapture] using ElementResolver";
      }
      return true;
    }
  }
  if (locator_capture_armed_) {
    LOG(INFO) << "[LocatorCapture] renderer_resolver unavailable";
  }
  return false;
}

// Note: console message hook was removed; alternate event paths can be used.

void RecorderController::DetachFromTarget() {
  loop_preview_modifiers_active_ = false;
  loop_preview_key_chord_active_ = false;
  ClearLoopPreview();
  ClearCaptureHoverPreview();
  last_loop_preview_point_.reset();
  guided_loop_target_confirmation_pending_ = false;
  guided_loop_target_probe_point_.reset();
  SetGuidedLoopFirstClickGuardArmed(false);
  DetachDevToolsSession();
  main_frame_resolver_.reset();
  if (highlight_overlay_) {
    highlight_overlay_->Hide();
  }
  target_web_contents_ = nullptr;
  target_main_frame_token_.clear();
  Observe(nullptr);
}

void RecorderController::ReattachToTarget() {
  // If we have a WebContents in scope (from previous attach), try to reuse it;
  // otherwise fall back to the last active browser tab.
  if (!target_web_contents_) {
    if (BrowserWindowInterface* browser =
            GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
      target_web_contents_ = browser->GetTabStripModel()->GetActiveWebContents();
    }
  }
  if (!target_web_contents_)
    return;
  current_url_ = target_web_contents_->GetVisibleURL().spec();
  if (auto* rfh = target_web_contents_->GetPrimaryMainFrame()) {
    target_main_frame_token_ = rfh->GetFrameToken().ToString();
  } else {
    target_main_frame_token_.clear();
  }
  UpdateInstrumentationState();
}

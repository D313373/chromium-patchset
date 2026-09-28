#include "chrome/browser/ui/webui/dashboard/actions/click_action.h"

#include <algorithm>
#include <optional>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_metadata_promotion.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"
#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"
#include "chrome/browser/ui/webui/dashboard/click_handler.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "base/strings/stringprintf.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

ClickAction::ClickAction(Profile* profile)
    : dashboard::actions::Action(profile) {}
ClickAction::~ClickAction() = default;

namespace {

std::string SummarizeResolvedElement(const dashboard::ResolvedElement& element) {
  return base::StringPrintf(
      "target_rect=(%d,%d %dx%d) target_center=(%d,%d) role=%s href=%s text=%s",
      element.left, element.top, element.width, element.height,
      element.center_x, element.center_y, element.role.c_str(),
      element.href.c_str(), element.text.c_str());
}

}  // namespace

bool ClickAction::SetParams(const base::Value::Dict& params) {
  // Accept either a list of locators or a single locator dict.
  const base::Value::List* locators_list = params.FindList("locators");
  // Bridge legacy `{locator: {...}}` into a one-element `locators` list while
  // keeping storage owned for the duration of SetParams. The previous
  // implementation stored a pointer to a stack-local List inside the `if`
  // block, which resulted in a dangling pointer once the block ended.
  base::Value::List tmp_locators;
  if (!locators_list) {
    if (const base::Value::Dict* single = params.FindDict("locator")) {
      tmp_locators.Append(single->Clone());
      locators_list = &tmp_locators;
    }
  }

  has_direct_coords_ = false;
  screenshot_point_ = false;
  direct_x_ = direct_y_ = 0;
  frame_id_hint_.clear();
  shadow_chain_hint_.clear();

  // Direct coordinate path: coordinates: {x, y} or x/y at top-level.
  // JSON round-trips often store whole numbers as doubles — accept both.
  auto read_xy = [](const base::Value::Dict* dict, const char* key,
                    int fallback) -> std::pair<int, bool> {
    if (!dict) {
      return {fallback, false};
    }
    if (auto i = dict->FindInt(key)) {
      return {*i, true};
    }
    if (auto d = dict->FindDouble(key)) {
      return {base::ClampRound(*d), true};
    }
    return {fallback, false};
  };
  const base::Value::Dict* coords = params.FindDict("coordinates");
  if (!coords)
    coords = params.FindDict("coordinate");
  auto [x_top, has_x_top] = read_xy(&params, "x", 0);
  auto [y_top, has_y_top] = read_xy(&params, "y", 0);
  auto [x_c, has_x_c] = read_xy(coords, "x", x_top);
  auto [y_c, has_y_c] = read_xy(coords, "y", y_top);
  auto read_list_xy = [&](const char* key) -> std::pair<int, int> {
    const base::Value::List* list = params.FindList(key);
    if (!list || list->size() < 2) {
      return {0, 0};
    }
    auto as_num = [](const base::Value& v) -> std::optional<int> {
      if (v.is_int()) {
        return v.GetInt();
      }
      if (v.is_double()) {
        return base::ClampRound(v.GetDouble());
      }
      return std::nullopt;
    };
    const std::optional<int> x = as_num((*list)[0]);
    const std::optional<int> y = as_num((*list)[1]);
    if (!x || !y) {
      return {0, 0};
    }
    return {*x, *y};
  };
  if (!has_x_c && !has_y_c) {
    const auto [lx, ly] = read_list_xy("coordinates");
    if (lx || ly) {
      x_c = lx;
      y_c = ly;
      has_x_c = has_y_c = true;
    } else {
      const auto [lx2, ly2] = read_list_xy("coordinate");
      if (lx2 || ly2) {
        x_c = lx2;
        y_c = ly2;
        has_x_c = has_y_c = true;
      }
    }
  }
  const bool has_coord_values =
      has_x_c || has_y_c || has_x_top || has_y_top;
  if (has_coord_values) {
    has_direct_coords_ = true;
    direct_x_ = has_x_c ? x_c : x_top;
    direct_y_ = has_y_c ? y_c : y_top;
    if (dashboard::IsVerboseLoggingEnabled()) {
      LOG(INFO) << "[ClickAction] Parsed direct coordinates: x=" << direct_x_
                << " y=" << direct_y_;
    }
  }

  if (locators_list && !locators_list->empty()) {
    locators_ = dashboard::ConvertLocators(*locators_list);
  }

  // Optional recorded hints for frame / shadow routing. These are used only at
  // execution time (to reduce traversal), and are kept out of the user-facing
  // locator list used for display.
  if (const std::string* v = params.FindString("frame_id")) {
    frame_id_hint_ = *v;
  } else if (const std::string* v2 = params.FindString("frameId")) {
    frame_id_hint_ = *v2;
  }
  if (const std::string* v = params.FindString("shadow_chain")) {
    shadow_chain_hint_ = *v;
  } else if (const std::string* v2 = params.FindString("shadowChain")) {
    shadow_chain_hint_ = *v2;
  } else if (const std::string* v3 = params.FindString("shadow_chain_selector")) {
    shadow_chain_hint_ = *v3;
  }

  if (!has_direct_coords_ && locators_.empty()) {
    return false;
  }
  // Increase default timeout to 15s to better tolerate late-rendered targets.
  wait_timeout_ms_ = params.FindInt("waitTimeout").value_or(15000);
  // Avoid multi-minute tarpits; the executor already retries once.
  if (wait_timeout_ms_ < 1000)
    wait_timeout_ms_ = 1000;
  if (wait_timeout_ms_ > 30000)
    wait_timeout_ms_ = 30000;
  // Coordinate-only clicks (captcha checkboxes, canvas hits) are already a
  // precise point. Do not jitter them off a 24px target.
  const std::optional<bool> human_offset = params.FindBool("humanOffset");
  use_human_offset_ = human_offset.value_or(!has_direct_coords_);
  screenshot_point_ =
      has_direct_coords_ && human_offset.has_value() && !*human_offset;
  prefer_center_point_ = params.FindBool("preferCenterPoint").value_or(false);
  allow_href_navigation_fallback_ =
      params.FindBool("allowHrefNavigationFallback").value_or(false);
  force_activation_ = params.FindBool("forceActivation")
                          .value_or(params.FindBool("force_activation")
                                        .value_or(false));
  int cc = 1;
  if (auto v = params.FindInt("clickCount")) cc = *v;
  else if (auto v2 = params.FindInt("click_count")) cc = *v2;
  // Allow a hover‑only mode when click_count == 0, and clamp to a small
  // range for safety. This keeps the default at 1 click while supporting up
  // to triple‑clicks explicitly.
  if (cc < 0)
    cc = 0;
  if (cc > 3)
    cc = 3;
  click_count_ = cc;

  base::Value::Dict params_dict = params.Clone();
  base::Value::List locator_values;
  for (const auto& locator : locators_) {
    locator_values.Append(dashboard::LocatorToValue(locator));
  }
  params_dict.Set("locators", std::move(locator_values));
  params_dict.Set("click_count", click_count_);
  if (params.Find("clickCount"))
    params_dict.Set("clickCount", click_count_);
  params_dict.Set("waitTimeout", wait_timeout_ms_);
  params_dict.Set("humanOffset", use_human_offset_);
  params_dict.Set("forceActivation", force_activation_);
  if (prefer_center_point_)
    params_dict.Set("preferCenterPoint", true);
  if (!allow_href_navigation_fallback_)
    params_dict.Set("allowHrefNavigationFallback", false);
  if (has_direct_coords_) {
    base::Value::Dict c; c.Set("x", direct_x_); c.Set("y", direct_y_);
    params_dict.Set("coordinates", std::move(c));
  }
  SetSerializedAction("click", params_dict.Clone());
  return true;
}

void ClickAction::Execute(content::WebContents* web_contents,
                          Callback callback) {
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[ClickAction] Starting click with " << locators_.size()
              << " locators, timeout=" << wait_timeout_ms_ << "ms"
              << ", has_direct_coords=" << has_direct_coords_;
  }

  // Screenshot/grid clicks (humanOffset=false) must not be retargeted by
  // locators — a wide wrapper like #captcha_container or `.class button`
  // would snap the click to the Claim/Collect control.
  if (!locators_.empty() && !screenshot_point_) {
    // Primary path: resolve a concrete element using the supplied locators.
    // Create heap-allocated checker that will survive beyond this function.
    checker_ = std::make_unique<ElementReadinessChecker>(web_contents);

    // Use async ElementReadinessChecker to avoid blocking. 
    std::vector<dashboard::Locator> locators_for_resolution = locators_;
    if (!frame_id_hint_.empty()) {
      dashboard::Locator l;
      l.type = dashboard::LocatorType::FRAME_ID;
      l.value = frame_id_hint_;
      locators_for_resolution.insert(locators_for_resolution.begin(),
                                     std::move(l));
    }
    if (!shadow_chain_hint_.empty()) {
      const bool already_present = std::any_of(
          locators_for_resolution.begin(), locators_for_resolution.end(),
          [this](const dashboard::Locator& locator) {
            return locator.type == dashboard::LocatorType::SHADOW_DOM &&
                   locator.value == shadow_chain_hint_;
          });
      if (!already_present) {
        dashboard::Locator l;
        l.type = dashboard::LocatorType::SHADOW_DOM;
        l.value = shadow_chain_hint_;
        const auto insert_at =
            !frame_id_hint_.empty() ? locators_for_resolution.begin() + 1
                                    : locators_for_resolution.begin();
        locators_for_resolution.insert(insert_at, std::move(l));
      }
    }

    checker_->WaitForElementReadyAsync(
        // Do not pre-filter by "interactive" for action targets. Many
        // real-world locators (e.g. XPath copied from DevTools) point at a
        // descendant span/div inside a clickable ancestor <a>/<button>.
        // The resolver will still prefer interactive ancestors when possible.
        locators_for_resolution, dashboard::SearchScope::kAll,
        wait_timeout_ms_,
        base::BindOnce(&ClickAction::OnElementReady,
                       weak_ptr_factory_.GetWeakPtr(), web_contents,
                       std::move(callback)));
    return;
  }

  // Coordinates-only path (no locators).
  if (has_direct_coords_) {
    dashboard::ClickHandler handler(web_contents);
    std::string diagnostics;
    handler.ExecuteClickAt(
        direct_x_, direct_y_, use_human_offset_, click_count_,
        /*force_activation=*/force_activation_,
        /*preserve_release_point=*/true, &diagnostics,
        base::BindOnce(&ClickAction::OnNativeClickDone,
                       weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                       "path=native_coords_only " + diagnostics,
                       "Coordinate click failed: " + diagnostics));
    return;
  }

  // No locators and no coordinates: fail.
  checker_.reset();
  std::move(callback).Run(false,
                          "ClickAction requires at least one locator or "
                          "coordinates");
}

void ClickAction::OnElementReady(content::WebContents* web_contents,
                                 Callback callback,
                                 bool ready,
                                 const dashboard::ResolvedElement& element,
                                 const std::string& error) {
  if (!ready) {
    if (dashboard::IsVerboseLoggingEnabled()) {
      LOG(ERROR) << "[ClickAction] Element not ready: " << error;
    }
    // If locators failed but explicit coordinates were provided, fall back to
    // a direct coordinate click / hover.
    if (has_direct_coords_) {
      if (dashboard::IsVerboseLoggingEnabled()) {
        LOG(WARNING) << "[ClickAction] Falling back to direct coordinates x="
                     << direct_x_ << " y=" << direct_y_;
      }
      dashboard::ClickHandler handler(web_contents);
      std::string diagnostics;
      handler.ExecuteClickAt(
          direct_x_, direct_y_, use_human_offset_, click_count_,
          /*force_activation=*/force_activation_,
          /*preserve_release_point=*/true, &diagnostics,
          base::BindOnce(&ClickAction::OnNativeClickDone,
                         weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                         "path=native_coords_fallback " + diagnostics,
                         "Element not ready; coordinate fallback click failed"));
      return;
    }

    checker_.reset();
    std::move(callback).Run(false, error);
    return;
  }

  const bool has_usable_geometry =
      element.width > 1 && element.height > 1 &&
      !(element.center_x == 0 && element.center_y == 0 && element.left == 0 &&
        element.top == 0 && element.width == 0 && element.height == 0);
  if (!has_usable_geometry) {
    LOG(WARNING) << "[ClickAction] Ready callback received unusable geometry: "
                 << SummarizeResolvedElement(element);
    checker_.reset();
    std::move(callback).Run(
        false, "Resolved click target had unusable geometry after readiness");
    return;
  }

  // Log the resolved center to diagnose mapping issues
  int click_x = element.center_x;
  int click_y = element.center_y;
  if (!prefer_center_point_ && element.width > 4 && element.height > 4) {
    // Avoid deterministic center clicks; some sites have overlays or
    // non-interactive children centered inside a larger clickable ancestor.
    const int inset_x = std::max(2, std::min(element.width - 3, element.width / 3));
    const int inset_y = std::max(2, std::min(element.height - 3, element.height / 2));
    click_x = element.left + inset_x;
    click_y = element.top + inset_y;
  }

  LOG(WARNING) << "[ClickAction] Resolved click target "
               << SummarizeResolvedElement(element)
               << " click_point=(" << click_x << "," << click_y << ")"
               << " human_offset=" << use_human_offset_
               << " prefer_center=" << prefer_center_point_
               << " allow_href_fallback=" << allow_href_navigation_fallback_
               << " click_count=" << click_count_;

  // Determine the frame that produced these client coordinates so trusted
  // input injection targets the correct widget (OOPIF-safe).
  content::RenderFrameHost* target_rfh = nullptr;
  if (element.frame_id.frame_routing_id != -1)
    target_rfh = content::RenderFrameHost::FromID(element.frame_id);
  if (!target_rfh)
    target_rfh = web_contents ? web_contents->GetPrimaryMainFrame() : nullptr;
  if (!target_rfh) {
    checker_.reset();
    std::move(callback).Run(false, "No render frame for click target");
    return;
  }

  dashboard::PromoteActionParamsFromResolvedElement(&serialized_params_, element);
  const bool should_force_activation = force_activation_;

  // Hover-only mode: move the synthetic cursor without firing a click.
  if (click_count_ <= 0) {
    dashboard::ClickHandler handler(web_contents);
    std::string ignored_diagnostics;
    handler.ExecuteClickAtInFrame(
        target_rfh, click_x, click_y, use_human_offset_, click_count_,
        should_force_activation,
        /*preserve_release_point=*/false, &ignored_diagnostics,
        base::BindOnce(&ClickAction::OnNativeClickDone,
                       weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                       std::string("path=native_hover"),
                       std::string("Hover-only cursor move failed")));
    return;
  }

  // Prefer a trusted click via RenderWidgetHost input injection. This tends to
  // succeed on pages that ignore untrusted JS-synthesized clicks.
  dashboard::ClickHandler handler(web_contents);
  std::string diagnostics;
  const std::string target_summary = SummarizeResolvedElement(element);
  handler.ExecuteClickAtInFrame(
      target_rfh, click_x, click_y, use_human_offset_, click_count_,
      should_force_activation,
      /*preserve_release_point=*/false, &diagnostics,
      base::BindOnce(&ClickAction::OnTrustedClickDone,
                     weak_ptr_factory_.GetWeakPtr(),
                     base::Unretained(web_contents), element,
                     std::move(callback),
                     "path=native_trusted " + target_summary + " " +
                         diagnostics));
}

void ClickAction::OnNativeClickDone(Callback callback,
                                    std::string ok_message,
                                    std::string fail_message,
                                    bool success) {
  checker_.reset();
  std::move(callback).Run(success, success ? ok_message : fail_message);
}

void ClickAction::OnTrustedClickDone(content::WebContents* web_contents,
                                     dashboard::ResolvedElement element,
                                     Callback callback,
                                     std::string ok_message,
                                     bool success) {
  if (success) {
    checker_.reset();
    std::move(callback).Run(true, std::move(ok_message));
    return;
  }

  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(WARNING) << "[ClickAction] Trusted click injection failed; "
                    "attempting native browser-side fallback.";
  }

  if (allow_href_navigation_fallback_ && !element.href.empty() &&
      web_contents && !web_contents->IsBeingDestroyed()) {
    const GURL target(element.href);
    if (target.is_valid() &&
        (target.SchemeIsHTTPOrHTTPS() || target.SchemeIs("chrome"))) {
      if (dashboard::IsVerboseLoggingEnabled()) {
        LOG(INFO) << "[ClickAction] Falling back to browser navigation for href="
                  << target;
      }
      content::NavigationController::LoadURLParams params(target);
      params.transition_type = ui::PAGE_TRANSITION_LINK;
      web_contents->GetController().LoadURLWithParams(params);
      checker_.reset();
      std::move(callback).Run(true, "path=native_href_navigation");
      return;
    }
  }

  checker_.reset();
  std::move(callback).Run(false, "Trusted click injection failed");
}

#include "chrome/browser/ui/webui/dashboard/actions/scroll_action.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "ui/events/types/scroll_types.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size.h"

#include <algorithm>
#include <cmath>

ScrollAction::ScrollAction(Profile* profile)
    : dashboard::actions::Action(profile) {}

ScrollAction::~ScrollAction() = default;

bool ScrollAction::SetParams(const base::Value::Dict& params) {
  const bool has_distance_param = params.Find("distance") != nullptr;
  if (auto distance = params.FindDouble("distance")) {
    distance_ = *distance;
  } else if (auto distance_i = params.FindInt("distance")) {
    distance_ = static_cast<double>(*distance_i);
  } else {
    distance_ = 0.0;
  }
  if (const std::string* direction = params.FindString("direction")) {
    if (*direction == "up") {
      distance_ = -std::abs(distance_);
    } else if (*direction == "down") {
      distance_ = std::abs(distance_);
    }
  }
  // Keep percent-based behavior for legacy scripts unless an explicit non-zero
  // pixel delta is provided.
  use_distance_ = has_distance_param && std::abs(distance_) > 0.0001;
  if (auto percent_y = params.FindDouble("percentY")) {
    percent_y_ = *percent_y;
  } else if (auto percent_y_i = params.FindInt("percentY")) {
    percent_y_ = static_cast<double>(*percent_y_i);
  } else {
    percent_y_ = 0.0;
  }
  if (auto percent_x = params.FindDouble("percentX")) {
    percent_x_ = *percent_x;
  } else if (auto percent_x_i = params.FindInt("percentX")) {
    percent_x_ = static_cast<double>(*percent_x_i);
  } else {
    percent_x_ = 0.0;
  }
  
  const std::string* scope = params.FindString("scope");
  const std::string* element = params.FindString("element");
  if (scope && (*scope == "page" || *scope == "window" || *scope == "document")) {
    element_type_ = "window";
  } else if (scope && (*scope == "iframe" || *scope == "frame" ||
                      *scope == "element")) {
    element_type_ = "element";
  } else {
    element_type_ = element ? *element : "window";
  }
  
  // If scrolling a specific element, get locators
  if (element_type_ == "element") {
    const base::Value::List* locators_list = params.FindList("locators");
    if (locators_list && !locators_list->empty()) {
      locators_ = dashboard::ConvertLocators(*locators_list);
    } else {
      // No locators for element scroll - invalid
      return false;
    }
  }
  SetSerializedAction("scroll", params.Clone());
  return true;
}

void ScrollAction::Execute(content::WebContents* web_contents,
                           Callback callback) {
  if (!web_contents) {
    std::move(callback).Run(false, "WebContents null");
    return;
  }

  if (dashboard::IsVerboseLoggingEnabled()) {
    if (use_distance_) {
      LOG(INFO) << "[ScrollAction] Scrolling " << element_type_
                << " by distance=" << distance_ << "px";
    } else {
      LOG(INFO) << "[ScrollAction] Scrolling " << element_type_
                << " to percentY=" << percent_y_
                << " percentX=" << percent_x_;
    }
  }

  // Page/window scroll must move the primary document, not whatever iframe
  // happens to sit under the last cursor / viewport center.
  if (element_type_ == "window") {
    ScrollMainDocument(web_contents, std::move(callback));
    return;
  }

  if (locators_.empty()) {
    std::move(callback).Run(false, "No locators for element scroll");
    return;
  }

  checker_ = std::make_unique<ElementReadinessChecker>(web_contents);
  checker_->WaitForElementReadyAsync(
      locators_,
      dashboard::SearchScope::kAll,  // Any element can be scrolled
      5000,  // 5 second timeout
      base::BindOnce(&ScrollAction::OnElementReady,
                     weak_ptr_factory_.GetWeakPtr(),
                     web_contents,
                     std::move(callback)));
}

void ScrollAction::ScrollMainDocument(content::WebContents* web_contents,
                                      Callback callback) {
  content::RenderFrameHost* rfh =
      web_contents ? web_contents->GetPrimaryMainFrame() : nullptr;
  if (!rfh) {
    std::move(callback).Run(false, "No render frame host");
    return;
  }

  // Trusted wheel on the main-frame widget, so sites whose real scroller is
  // an inner container (body locked, app shell div) move like a user scroll.
  // Aimed at the right gutter, mid-height: centered iframes (captchas) sit
  // away from that edge, and non-scrollable hits chain up to the page
  // scroller.
  content::RenderWidgetHostView* view = rfh->GetView();
  const gfx::Size size =
      view ? view->GetVisibleViewportSize() : gfx::Size(1280, 800);
  const int x = std::max(1, size.width() - 24);
  const int y = std::max(1, size.height() / 2);
  DispatchTrustedWheelScroll(web_contents, x, y, size.height(), size.width(),
                             std::move(callback));
}

void ScrollAction::OnElementReady(content::WebContents* web_contents,
                                  Callback callback,
                                  bool ready,
                                  const dashboard::ResolvedElement& element,
                                  const std::string& error) {
  checker_.reset();

  if (!ready) {
    if (dashboard::IsVerboseLoggingEnabled()) {
      LOG(ERROR) << "[ScrollAction] Element not ready: " << error;
    }
    std::move(callback).Run(false, error);
    return;
  }

  int target_x = element.center_x;
  int target_y = element.center_y;
  content::RenderFrameHost* rfh =
      content::RenderFrameHost::FromID(element.frame_id);
  if (!rfh) {
    rfh = web_contents ? web_contents->GetPrimaryMainFrame() : nullptr;
  }
  if (rfh && web_contents) {
    content::RenderWidgetHost* frame_widget = rfh->GetRenderWidgetHost();
    content::RenderWidgetHostView* frame_view =
        frame_widget ? frame_widget->GetView() : nullptr;
    if (frame_view) {
      const gfx::PointF root_point =
          frame_view->TransformPointToRootCoordSpaceF(
              gfx::PointF(target_x, target_y));
      target_x = static_cast<int>(std::lround(root_point.x()));
      target_y = static_cast<int>(std::lround(root_point.y()));
    }
  }
  DispatchTrustedWheelScroll(web_contents, target_x, target_y,
                             std::max(1, element.height),
                             std::max(1, element.width), std::move(callback));
}

void ScrollAction::ComputeScrollDeltas(double target_height,
                                       double target_width,
                                       double* out_delta_x,
                                       double* out_delta_y) const {
  if (!out_delta_x || !out_delta_y) {
    return;
  }
  if (use_distance_) {
    *out_delta_x = 0.0;
    *out_delta_y = std::clamp(distance_, -4000.0, 4000.0);
    return;
  }
  const double y_scale = std::max(120.0, target_height * 0.9);
  const double x_scale = std::max(120.0, target_width * 0.9);
  *out_delta_y = std::clamp((percent_y_ / 100.0) * y_scale, -4000.0, 4000.0);
  *out_delta_x = std::clamp((percent_x_ / 100.0) * x_scale, -4000.0, 4000.0);
}

void ScrollAction::DispatchTrustedWheelScroll(content::WebContents* web_contents,
                                              int target_x,
                                              int target_y,
                                              double target_height,
                                              double target_width,
                                              Callback callback) {
  content::RenderFrameHost* primary =
      web_contents ? web_contents->GetPrimaryMainFrame() : nullptr;
  content::RenderWidgetHost* rwh =
      primary ? primary->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    std::move(callback).Run(false, "RenderWidgetHost unavailable");
    return;
  }

  double delta_x = 0.0;
  double delta_y = 0.0;
  ComputeScrollDeltas(target_height, target_width, &delta_x, &delta_y);
  if (std::abs(delta_x) < 0.01 && std::abs(delta_y) < 0.01) {
    std::move(callback).Run(true, "path=native_wheel_scroll (no-op)");
    return;
  }

  // Blink wheel deltas are inverted relative to scroll offset: positive
  // delta_y scrolls up. Only the Changed phase carries movement.
  blink::WebMouseWheelEvent wheel_event(
      blink::WebMouseWheelEvent::Type::kMouseWheel, /*modifiers=*/0,
      base::TimeTicks::Now());
  wheel_event.SetPositionInWidget(gfx::PointF(target_x, target_y));
  wheel_event.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;
  wheel_event.phase = blink::WebMouseWheelEvent::kPhaseBegan;
  rwh->ForwardWheelEvent(wheel_event);

  wheel_event.delta_x = static_cast<float>(-delta_x);
  wheel_event.delta_y = static_cast<float>(-delta_y);
  wheel_event.phase = blink::WebMouseWheelEvent::kPhaseChanged;
  rwh->ForwardWheelEvent(wheel_event);

  wheel_event.delta_x = 0;
  wheel_event.delta_y = 0;
  wheel_event.phase = blink::WebMouseWheelEvent::kPhaseEnded;
  rwh->ForwardWheelEvent(wheel_event);

  std::move(callback).Run(true, "path=native_wheel_scroll");
}

#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_scroll_action.h"

#include <algorithm>
#include <cmath>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_devtools_client.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/geometry/point_f.h"

CdpScrollAction::CdpScrollAction(Profile* profile) : CdpActionBase(profile) {}

CdpScrollAction::~CdpScrollAction() = default;

bool CdpScrollAction::SetParams(const base::Value::Dict& params) {
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

  const std::string* element = params.FindString("element");
  element_type_ = element ? *element : "window";

  locators_.clear();
  if (element_type_ == "element") {
    const base::Value::List* locators_list = params.FindList("locators");
    if (!locators_list || locators_list->empty()) {
      return false;
    }
    locators_ = dashboard::ConvertLocators(*locators_list);
  }

  base::Value::Dict out = params.Clone();
  out.Set("element", element_type_);
  out.Set("distance", distance_);
  out.Set("percentY", percent_y_);
  out.Set("percentX", percent_x_);
  SetSerializedAction("scroll", std::move(out));
  return true;
}

void CdpScrollAction::Execute(content::WebContents* web_contents,
                              Callback callback) {
  if (!web_contents) {
    std::move(callback).Run(false, "WebContents null");
    return;
  }

  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[CdpScrollAction] element=" << element_type_
              << " use_distance=" << use_distance_
              << " distance=" << distance_ << " percentY=" << percent_y_
              << " percentX=" << percent_x_;
  }

  if (element_type_ == "element") {
    if (locators_.empty()) {
      std::move(callback).Run(false, "No locators for element scroll");
      return;
    }
    checker_ = std::make_unique<ElementReadinessChecker>(web_contents);
    checker_->WaitForElementReadyAsync(
        locators_, dashboard::SearchScope::kAll,
        /*timeout_ms=*/5000,
        base::BindOnce(&CdpScrollAction::OnElementReady,
                       weak_ptr_factory_.GetWeakPtr(), web_contents,
                       std::move(callback)));
    return;
  }

  content::RenderFrameHost* primary = web_contents->GetPrimaryMainFrame();
  content::RenderWidgetHost* rwh = primary ? primary->GetRenderWidgetHost()
                                           : nullptr;
  content::RenderWidgetHostView* view = rwh ? rwh->GetView() : nullptr;
  int target_x = 400;
  int target_y = 300;
  double target_height = 600.0;
  double target_width = 800.0;
  if (view) {
    const gfx::Rect bounds = view->GetViewBounds();
    target_x = std::max(1, bounds.width() / 2);
    target_y = std::max(1, bounds.height() / 2);
    target_height = std::max(1, bounds.height());
    target_width = std::max(1, bounds.width());
  }
  DispatchCdpScroll(web_contents, target_x, target_y, target_height,
                    target_width, std::move(callback));
}

void CdpScrollAction::OnElementReady(content::WebContents* web_contents,
                                     Callback callback,
                                     bool ready,
                                     const dashboard::ResolvedElement& element,
                                     const std::string& error) {
  checker_.reset();
  if (!ready) {
    std::move(callback).Run(false, error);
    return;
  }

  int target_x = element.center_x;
  int target_y = element.center_y;
  content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(
      element.frame_id);
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

  DispatchCdpScroll(web_contents, target_x, target_y,
                    std::max(1, element.height), std::max(1, element.width),
                    std::move(callback));
}

void CdpScrollAction::DispatchCdpScroll(content::WebContents* web_contents,
                                        int target_x,
                                        int target_y,
                                        double target_height,
                                        double target_width,
                                        Callback callback) {
  dashboard::DashboardDevToolsClient* client = GetDevToolsClient(web_contents);
  if (!client) {
    std::move(callback).Run(false, "CDP client unavailable");
    return;
  }

  double delta_x = 0.0;
  double delta_y = 0.0;
  ComputeScrollDeltas(target_height, target_width, &delta_x, &delta_y);
  if (std::abs(delta_x) < 0.01 && std::abs(delta_y) < 0.01) {
    std::move(callback).Run(true, "path=cdp_scroll (no-op)");
    return;
  }

  base::Value::Dict params;
  params.Set("type", "mouseWheel");
  params.Set("x", target_x);
  params.Set("y", target_y);
  params.Set("deltaX", delta_x);
  params.Set("deltaY", delta_y);
  if (web_contents) {
    SessionState& state = GetSessionState(web_contents);
    state.cursor_x = target_x;
    state.cursor_y = target_y;
  }
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(params),
      base::BindOnce(&CdpScrollAction::OnCdpScrollDispatched,
                     weak_ptr_factory_.GetWeakPtr(), web_contents, target_x,
                     target_y,
                     std::move(callback)));
}

void CdpScrollAction::OnCdpScrollDispatched(content::WebContents* web_contents,
                                            int target_x,
                                            int target_y,
                                            Callback callback,
                                            bool success,
                                            const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, "CDP scroll failed: " + error);
    return;
  }
  std::move(callback).Run(true, "path=cdp_scroll");
}

void CdpScrollAction::ComputeScrollDeltas(double target_height,
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

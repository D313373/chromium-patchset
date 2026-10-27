#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_drag_action.h"

#include <cmath>

#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"
#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"
#include "chrome/browser/ui/webui/dashboard/click_handler.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace {

std::optional<std::pair<int, int>> ReadXYPair(const base::Value::Dict& params,
                                              const char* dict_key_a,
                                              const char* dict_key_b,
                                              const char* dict_key_c) {
  const base::Value::Dict* dict = params.FindDict(dict_key_a);
  if (!dict && dict_key_b) {
    dict = params.FindDict(dict_key_b);
  }
  if (!dict && dict_key_c) {
    dict = params.FindDict(dict_key_c);
  }
  if (!dict) {
    return std::nullopt;
  }

  std::optional<int> x = dict->FindInt("x");
  std::optional<int> y = dict->FindInt("y");
  if (!x) {
    if (const std::optional<double> dv = dict->FindDouble("x")) {
      x = static_cast<int>(std::lround(*dv));
    }
  }
  if (!y) {
    if (const std::optional<double> dv = dict->FindDouble("y")) {
      y = static_cast<int>(std::lround(*dv));
    }
  }
  if (!x || !y) {
    return std::nullopt;
  }
  return std::make_pair(*x, *y);
}

dashboard::ResolvedElement NormalizeDragElement(
    dashboard::ResolvedElement element) {
  if (element.center_x == 0 && element.center_y == 0) {
    if (element.width > 0 || element.height > 0) {
      element.center_x = element.left + element.width / 2;
      element.center_y = element.top + element.height / 2;
    } else if (element.left != 0 || element.top != 0) {
      element.center_x = element.left;
      element.center_y = element.top;
    }
  }
  return element;
}

bool ResolveElementPointToRoot(content::WebContents* web_contents,
                               const dashboard::ResolvedElement& element,
                               int* x,
                               int* y) {
  if (!x || !y) {
    return false;
  }
  *x = element.center_x;
  *y = element.center_y;

  if (!web_contents) {
    return false;
  }

  content::RenderFrameHost* frame =
      content::RenderFrameHost::FromID(element.frame_id);
  if (!frame) {
    return false;
  }

  int root_x = *x;
  int root_y = *y;
  if (!dashboard::ConvertFramePointToRootCoordinates(web_contents, frame, *x,
                                                     *y, &root_x, &root_y)) {
    return false;
  }

  *x = root_x;
  *y = root_y;
  return true;
}

constexpr int kCdpDragPressHoldDelayMs = 95;
constexpr int kCdpDragLatchDelayMs = 75;
constexpr int kCdpDragReleaseSettleDelayMs = 90;

std::pair<int, int> ComputeDragLatchPoint(int start_x,
                                          int start_y,
                                          int target_x,
                                          int target_y) {
  const double dx = static_cast<double>(target_x - start_x);
  const double dy = static_cast<double>(target_y - start_y);
  const double distance = std::hypot(dx, dy);
  if (distance < 4.0) {
    return {start_x, start_y};
  }
  const double safe_distance = std::max(1.0, distance);
  const double unit_x = dx / safe_distance;
  const double unit_y = dy / safe_distance;
  const double latch_distance =
      std::clamp(distance * 0.12, 7.0, std::min(18.0, distance - 1.0));
  return {static_cast<int>(std::lround(start_x + unit_x * latch_distance)),
          static_cast<int>(std::lround(start_y + unit_y * latch_distance))};
}

}  // namespace

CdpDragAction::CdpDragAction(Profile* profile) : CdpActionBase(profile) {}

CdpDragAction::~CdpDragAction() = default;

bool CdpDragAction::SetParams(const base::Value::Dict& params) {
  from_locators_.clear();
  to_locators_.clear();
  has_from_coords_ = false;
  has_to_coords_ = false;
  from_x_ = 0;
  from_y_ = 0;
  to_x_ = 0;
  to_y_ = 0;

  const base::Value::List* from_locators = params.FindList("fromLocators");
  if (from_locators) {
    from_locators_ = dashboard::ConvertLocators(*from_locators);
  }

  const base::Value::List* to_locators = params.FindList("toLocators");
  if (to_locators) {
    to_locators_ = dashboard::ConvertLocators(*to_locators);
  }

  // Handle coordinates
  if (auto from_xy =
          ReadXYPair(params, "fromCoords", "fromCoordinates",
                     "from_coordinates")) {
    has_from_coords_ = true;
    from_x_ = from_xy->first;
    from_y_ = from_xy->second;
  }

  if (auto to_xy =
          ReadXYPair(params, "toCoords", "toCoordinates", "to_coordinates")) {
    has_to_coords_ = true;
    to_x_ = to_xy->first;
    to_y_ = to_xy->second;
  }

  wait_timeout_ms_ = params.FindInt("waitTimeout").value_or(15000);
  if (wait_timeout_ms_ < 1000) {
    wait_timeout_ms_ = 1000;
  }
  if (wait_timeout_ms_ > 30000) {
    wait_timeout_ms_ = 30000;
  }

  use_human_offset_ = params.FindBool("humanOffset").value_or(true);
  steps_ = params.FindInt("steps").value_or(12);
  if (steps_ < 1) {
    steps_ = 1;
  }
  if (steps_ > 50) {
    steps_ = 50;
  }

  // CDP-specific options
  use_cdp_input_ = params.FindBool("useCdpInput").value_or(IsCdpInputEnabled());

  base::Value::Dict params_dict = params.Clone();
  base::Value::List from_locator_values;
  for (const auto& locator : from_locators_) {
    from_locator_values.Append(dashboard::LocatorToValue(locator));
  }
  base::Value::List to_locator_values;
  for (const auto& locator : to_locators_) {
    to_locator_values.Append(dashboard::LocatorToValue(locator));
  }
  params_dict.Set("fromLocators", std::move(from_locator_values));
  params_dict.Set("toLocators", std::move(to_locator_values));
  params_dict.Set("useCdpInput", use_cdp_input_);

  SetSerializedAction("drag", std::move(params_dict));
  return true;
}

void CdpDragAction::Execute(content::WebContents* web_contents,
                            Callback callback) {
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[CdpDragAction] Starting drag with " << from_locators_.size()
              << " from locators, " << to_locators_.size() << " to locators"
              << " timeout=" << wait_timeout_ms_ << "ms"
              << " cdp_input=" << use_cdp_input_;
  }

  if (!web_contents) {
    std::move(callback).Run(false, "No web contents");
    return;
  }
  resolved_from_element_.reset();

  // Handle direct coordinates for both from and to
  if (has_from_coords_ && has_to_coords_) {
    dashboard::ResolvedElement dummy_from, dummy_to;
    dummy_from.left = from_x_;
    dummy_from.top = from_y_;
    dummy_from.center_x = from_x_;
    dummy_from.center_y = from_y_;
    dummy_from.width = 1;
    dummy_from.height = 1;
    dummy_to.left = to_x_;
    dummy_to.top = to_y_;
    dummy_to.center_x = to_x_;
    dummy_to.center_y = to_y_;
    dummy_to.width = 1;
    dummy_to.height = 1;

    ExecuteCdpDrag(web_contents, dummy_from, dummy_to, std::move(callback));
    return;
  }

  // Need to resolve at least one element for each side
  if (!has_from_coords_ && from_locators_.empty()) {
    std::move(callback).Run(false,
                            "DragAction requires fromLocators or fromCoords");
    return;
  }

  if (!has_to_coords_ && to_locators_.empty()) {
    std::move(callback).Run(false,
                            "DragAction requires toLocators or toCoords");
    return;
  }

  // Resolve from element
  if (!has_from_coords_) {
    from_checker_ = std::make_unique<ElementReadinessChecker>(web_contents);
    from_checker_->WaitForElementReadyAsync(
        from_locators_, dashboard::SearchScope::kAll, wait_timeout_ms_,
        base::BindOnce(&CdpDragAction::OnFromReady,
                       weak_ptr_factory_.GetWeakPtr(), web_contents,
                       std::move(callback)));
    return;
  }

  // Resolve to element
  if (!has_to_coords_) {
    to_checker_ = std::make_unique<ElementReadinessChecker>(web_contents);
    to_checker_->WaitForElementReadyAsync(
        to_locators_, dashboard::SearchScope::kAll, wait_timeout_ms_,
        base::BindOnce(&CdpDragAction::OnToReady,
                       weak_ptr_factory_.GetWeakPtr(), web_contents,
                       std::move(callback)));
    return;
  }

  // Both coordinates - execute directly
  dashboard::ResolvedElement dummy_from, dummy_to;
  dummy_from.left = from_x_;
  dummy_from.top = from_y_;
  dummy_from.center_x = from_x_;
  dummy_from.center_y = from_y_;
  dummy_from.width = 1;
  dummy_from.height = 1;
  dummy_to.left = to_x_;
  dummy_to.top = to_y_;
  dummy_to.center_x = to_x_;
  dummy_to.center_y = to_y_;
  dummy_to.width = 1;
  dummy_to.height = 1;

  ExecuteCdpDrag(web_contents, dummy_from, dummy_to, std::move(callback));
}

void CdpDragAction::OnFromReady(content::WebContents* web_contents,
                                Callback callback,
                                bool ready,
                                const dashboard::ResolvedElement& element,
                                const std::string& error) {
  const dashboard::ResolvedElement element_copy = element;
  if (!ready) {
    resolved_from_element_.reset();
    from_checker_.reset();
    std::move(callback).Run(false, error);
    return;
  }
  resolved_from_element_ = NormalizeDragElement(element_copy);

  // Now resolve to element
  if (!has_to_coords_) {
    to_checker_ = std::make_unique<ElementReadinessChecker>(web_contents);
    to_checker_->WaitForElementReadyAsync(
        to_locators_, dashboard::SearchScope::kAll, wait_timeout_ms_,
        base::BindOnce(&CdpDragAction::OnToReady,
                       weak_ptr_factory_.GetWeakPtr(), web_contents,
                       std::move(callback)));
    from_checker_.reset();
    return;
  }

  // Both coordinates - execute directly
  dashboard::ResolvedElement dummy_to;
  dummy_to.left = to_x_;
  dummy_to.top = to_y_;
  dummy_to.center_x = to_x_;
  dummy_to.center_y = to_y_;
  dummy_to.width = 1;
  dummy_to.height = 1;

  ExecuteCdpDrag(web_contents, element, dummy_to, std::move(callback));
}

void CdpDragAction::OnToReady(content::WebContents* web_contents,
                              Callback callback,
                              bool ready,
                              const dashboard::ResolvedElement& element,
                              const std::string& error) {
  const dashboard::ResolvedElement element_copy = element;
  to_checker_.reset();

  if (!ready) {
    std::move(callback).Run(false, error);
    return;
  }

  // Get from element (either coordinates or resolved)
  dashboard::ResolvedElement from_element;
  if (has_from_coords_) {
    from_element.left = from_x_;
    from_element.top = from_y_;
    from_element.center_x = from_x_;
    from_element.center_y = from_y_;
    from_element.width = 1;
    from_element.height = 1;
  } else if (resolved_from_element_.has_value()) {
    from_element = *resolved_from_element_;
  } else {
    std::move(callback).Run(false, "Missing source element for drag");
    return;
  }

  ExecuteCdpDrag(web_contents, NormalizeDragElement(from_element),
                 NormalizeDragElement(element_copy), std::move(callback));
}

void CdpDragAction::ExecuteCdpDrag(
    content::WebContents* web_contents,
    const dashboard::ResolvedElement& from_element,
    const dashboard::ResolvedElement& to_element,
    Callback callback) {
  const dashboard::ResolvedElement normalized_from =
      NormalizeDragElement(from_element);
  const dashboard::ResolvedElement normalized_to =
      NormalizeDragElement(to_element);
  LOG(WARNING) << "[CdpDragAction] Executing CDP drag from ("
               << normalized_from.center_x << "," << normalized_from.center_y
               << ") to (" << normalized_to.center_x << ","
               << normalized_to.center_y
               << ") from_rect=(" << normalized_from.left << ","
               << normalized_from.top << " " << normalized_from.width << "x"
               << normalized_from.height << ") to_rect=(" << normalized_to.left
               << "," << normalized_to.top << " " << normalized_to.width << "x"
               << normalized_to.height << ")"
               << " steps=" << steps_ << " cdp_input=" << use_cdp_input_;

  dashboard::DashboardDevToolsClient* client = GetDevToolsClient(web_contents);
  if (!client) {
    std::move(callback).Run(false, "DevTools client not available");
    return;
  }

  if (!use_cdp_input_) {
    std::move(callback).Run(false,
                            "CDP drag not implemented without CDP input");
    return;
  }

  // Use CDP Input.dispatchMouseEvent for drag simulation
  int start_x = normalized_from.center_x;
  int start_y = normalized_from.center_y;
  int target_x = normalized_to.center_x;
  int target_y = normalized_to.center_y;
  ResolveElementPointToRoot(web_contents, normalized_from, &start_x, &start_y);
  ResolveElementPointToRoot(web_contents, normalized_to, &target_x, &target_y);

  SessionState& state = GetSessionState(web_contents);
  state.cursor_x = start_x;
  state.cursor_y = start_y;

  base::Value::Dict mouse_move_to_start;
  mouse_move_to_start.Set("type", "mouseMoved");
  mouse_move_to_start.Set("x", start_x);
  mouse_move_to_start.Set("y", start_y);
  mouse_move_to_start.Set("button", "none");
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(mouse_move_to_start),
      base::BindOnce(&CdpDragAction::DispatchPressAtStart,
                     weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                     start_x, start_y, target_x, target_y,
                     std::move(callback)));
}

void CdpDragAction::DispatchPressAtStart(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    int start_x,
    int start_y,
    int target_x,
    int target_y,
    Callback callback,
    bool success,
    const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, "CDP drag start move failed: " + error);
    return;
  }

  base::Value::Dict mouse_down;
  mouse_down.Set("type", "mousePressed");
  mouse_down.Set("x", start_x);
  mouse_down.Set("y", start_y);
  mouse_down.Set("button", "left");
  mouse_down.Set("buttons", 1);
  mouse_down.Set("clickCount", 1);
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(mouse_down),
      base::BindOnce(
          [](base::WeakPtr<CdpDragAction> weak,
             dashboard::DashboardDevToolsClient* client,
             content::WebContents* web_contents, int start_x, int start_y,
             int target_x, int target_y, Callback callback, bool success,
             const std::string& error) {
            if (!weak) {
              return;
            }
            if (!success) {
              std::move(callback).Run(false,
                                      "CDP drag mouse press failed: " + error);
              return;
            }
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
                FROM_HERE,
                base::BindOnce(&CdpDragAction::DispatchLatchMove, weak, client,
                               web_contents, start_x, start_y, target_x,
                               target_y, std::move(callback)),
                base::Milliseconds(kCdpDragPressHoldDelayMs));
          },
          weak_ptr_factory_.GetWeakPtr(), client, web_contents, start_x, start_y,
          target_x, target_y, std::move(callback)));
}

void CdpDragAction::DispatchLatchMove(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    int start_x,
    int start_y,
    int target_x,
    int target_y,
    Callback callback) {
  const auto latch_point =
      ComputeDragLatchPoint(start_x, start_y, target_x, target_y);
  if (latch_point.first == start_x && latch_point.second == start_y) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CdpDragAction::DispatchDragStep,
                       weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                       start_x, start_y, target_x, target_y, /*step_index=*/1,
                       std::move(callback)),
        base::Milliseconds(kCdpDragLatchDelayMs));
    return;
  }

  base::Value::Dict mouse_move;
  mouse_move.Set("type", "mouseMoved");
  mouse_move.Set("x", latch_point.first);
  mouse_move.Set("y", latch_point.second);
  mouse_move.Set("button", "left");
  mouse_move.Set("buttons", 1);
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(mouse_move),
      base::BindOnce(
          [](base::WeakPtr<CdpDragAction> weak,
             dashboard::DashboardDevToolsClient* client,
             content::WebContents* web_contents, int latch_x, int latch_y,
             int target_x, int target_y, Callback callback, bool success,
             const std::string& error) {
            if (!weak) {
              return;
            }
            if (!success) {
              std::move(callback).Run(false,
                                      "CDP drag latch move failed: " + error);
              return;
            }
            SessionState& state = weak->GetSessionState(web_contents);
            state.cursor_x = latch_x;
            state.cursor_y = latch_y;
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
                FROM_HERE,
                base::BindOnce(&CdpDragAction::DispatchDragStep, weak, client,
                               web_contents, latch_x, latch_y, target_x,
                               target_y, /*step_index=*/1,
                               std::move(callback)),
                base::Milliseconds(kCdpDragLatchDelayMs));
          },
          weak_ptr_factory_.GetWeakPtr(), client, web_contents,
          latch_point.first, latch_point.second, target_x, target_y,
          std::move(callback)));
}

void CdpDragAction::DispatchDragStep(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    int origin_x,
    int origin_y,
    int target_x,
    int target_y,
    int step_index,
    Callback callback) {
  if (step_index > steps_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CdpDragAction::DispatchReleaseAtTarget,
                       weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                       target_x, target_y, std::move(callback), true,
                       std::string()),
        base::Milliseconds(kCdpDragReleaseSettleDelayMs));
    return;
  }

  const double progress =
      static_cast<double>(step_index) / static_cast<double>(steps_);
  int current_x =
      static_cast<int>(std::lround(origin_x + (target_x - origin_x) * progress));
  int current_y =
      static_cast<int>(std::lround(origin_y + (target_y - origin_y) * progress));
  if (step_index < steps_) {
    const double jitter = use_human_offset_ ? GetRandomDouble(-1.5, 1.5) : 0.0;
    current_x += static_cast<int>(std::lround(jitter));
    current_y += static_cast<int>(std::lround(
        use_human_offset_ ? GetRandomDouble(-1.5, 1.5) : 0.0));
  } else {
    current_x = target_x;
    current_y = target_y;
  }

  SessionState& state = GetSessionState(web_contents);
  state.cursor_x = current_x;
  state.cursor_y = current_y;

  base::Value::Dict mouse_move;
  mouse_move.Set("type", "mouseMoved");
  mouse_move.Set("x", current_x);
  mouse_move.Set("y", current_y);
  mouse_move.Set("button", "left");
  mouse_move.Set("buttons", 1);
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(mouse_move),
      base::BindOnce(
          [](base::WeakPtr<CdpDragAction> weak,
             dashboard::DashboardDevToolsClient* client,
             content::WebContents* web_contents, int origin_x, int origin_y,
             int target_x, int target_y, int current_x, int current_y,
             int next_step_index, Callback callback, bool success,
             const std::string& error) {
            if (!weak) {
              return;
            }
            if (!success) {
              std::move(callback).Run(false,
                                      "CDP drag move failed: " + error);
              return;
            }
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
                FROM_HERE,
                base::BindOnce(&CdpDragAction::DispatchDragStep, weak, client,
                               web_contents, origin_x, origin_y, target_x,
                               target_y, next_step_index, std::move(callback)),
                base::Milliseconds(24));
          },
          weak_ptr_factory_.GetWeakPtr(), client, web_contents, origin_x,
          origin_y, target_x, target_y, current_x, current_y, step_index + 1,
          std::move(callback)));
}

void CdpDragAction::DispatchReleaseAtTarget(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    int target_x,
    int target_y,
    Callback callback,
    bool success,
    const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, error);
    return;
  }

  base::Value::Dict mouse_up;
  mouse_up.Set("type", "mouseReleased");
  mouse_up.Set("x", target_x);
  mouse_up.Set("y", target_y);
  mouse_up.Set("button", "left");
  mouse_up.Set("clickCount", 1);

  SessionState& state = GetSessionState(web_contents);
  state.cursor_x = target_x;
  state.cursor_y = target_y;
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(mouse_up),
      base::BindOnce(&CdpDragAction::OnDragComplete,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void CdpDragAction::OnDragComplete(Callback callback,
                                   bool success,
                                   const std::string& error) {
  std::move(callback).Run(
      success, success ? "CDP drag completed" : ("CDP drag failed: " + error));
}

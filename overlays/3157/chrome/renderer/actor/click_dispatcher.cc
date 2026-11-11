// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/actor/click_dispatcher.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/check.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/rand_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_runner.h"
#include "base/time/time.h"
#include "chrome/common/actor.mojom.h"
#include "chrome/common/actor/action_result.h"
#include "chrome/common/chrome_features.h"
#include "chrome/renderer/actor/tool_base.h"
#include "chrome/renderer/actor/tool_utils.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/platform/web_input_event_result.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_widget.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/events/base_event_utils.h"
#include "ui/latency/latency_info.h"

namespace actor {

using ::blink::WebInputEvent;
using ::blink::WebLocalFrame;
using ::blink::WebMouseEvent;
using ::blink::WebWidget;

namespace {

double EaseInOut(double t) {
  if (t <= 0.0)
    return 0.0;
  if (t >= 1.0)
    return 1.0;
  return (t < 0.5) ? (2.0 * t * t)
                   : (1.0 - std::pow(-2.0 * t + 2.0, 2.0) / 2.0);
}

gfx::PointF JitteredPoint(const gfx::PointF& p, int jitter_px) {
  if (jitter_px <= 0)
    return p;
  const float jx = static_cast<float>(base::RandDouble() * 2.0 * jitter_px -
                                      jitter_px);
  const float jy = static_cast<float>(base::RandDouble() * 2.0 * jitter_px -
                                      jitter_px);
  return gfx::PointF(p.x() + jx, p.y() + jy);
}

gfx::PointF ClampToNonNegative(const gfx::PointF& p) {
  return gfx::PointF(std::max(0.0f, p.x()), std::max(0.0f, p.y()));
}

// Widget position is blink space. Screen position has to be DIPs or the DOM
// event's screenX/screenY stay 0 and the page treats the click as synthetic.
void PositionMouseEvent(const ToolBase& tool,
                        WebWidget* widget,
                        WebMouseEvent& event,
                        const gfx::PointF& widget_point) {
  event.SetPositionInWidget(widget_point);
  gfx::PointF dip = widget_point;
  if (blink::WebLocalFrame* web_frame = tool.frame()->GetWebFrame()) {
    if (blink::WebFrameWidget* frame_widget = web_frame->FrameWidget()) {
      dip = frame_widget->BlinkSpaceToDIPs(gfx::RectF(widget_point, gfx::SizeF()))
                .origin();
    }
  }
  const gfx::Rect view_in_dips = widget->ViewRect();
  event.SetPositionInScreen(view_in_dips.x() + dip.x(),
                            view_in_dips.y() + dip.y());
}

}  // namespace

ClickDispatcher::ClickDispatcher(
    WebMouseEvent::Button button,
    int count,
    const ResolvedTarget& target,
    const ToolBase& tool,
    base::OnceCallback<void(mojom::ActionResultPtr)> on_complete,
    int key_modifiers)
    : key_modifiers_(key_modifiers),
      target_(target),
      tool_(tool),
      on_complete_(std::move(on_complete)) {
  WebWidget* widget = target.GetWidget(*tool_);
  CHECK(widget);

  if (base::FeatureList::IsEnabled(features::kGlicActorHumanizedClick)) {
    const int steps_min = features::kGlicActorHumanizedClickMoveStepsMin.Get();
    const int steps_max = features::kGlicActorHumanizedClickMoveStepsMax.Get();
    const int jitter_px = features::kGlicActorHumanizedClickPathJitterPx.Get();
    const int start_offset =
        features::kGlicActorHumanizedClickStartOffsetPx.Get();
    const int micro_moves = features::kGlicActorHumanizedClickMicroMoves.Get();
    const int micro_radius =
        features::kGlicActorHumanizedClickMicroRadiusPx.Get();

    const int steps = std::max(
        1, base::RandInt(std::min(steps_min, steps_max),
                         std::max(steps_min, steps_max)));
    const gfx::PointF end = ClampToNonNegative(target.widget_point);
    gfx::PointF start = end;
    if (start_offset > 0) {
      start = gfx::PointF(
          end.x() + static_cast<float>(base::RandInt(-start_offset, start_offset)),
          end.y() + static_cast<float>(base::RandInt(-start_offset, start_offset)));
      start = ClampToNonNegative(start);
    }

    for (int i = 1; i <= steps; ++i) {
      const double t = static_cast<double>(i) / steps;
      const double et = EaseInOut(t);
      gfx::PointF p(static_cast<float>(start.x() + (end.x() - start.x()) * et),
                    static_cast<float>(start.y() + (end.y() - start.y()) * et));
      p = ClampToNonNegative(JitteredPoint(p, jitter_px));
      WebMouseEvent mouse_move(WebInputEvent::Type::kMouseMove,
                               WebInputEvent::kNoModifiers,
                               ui::EventTimeForNow());
      mouse_move.button = WebMouseEvent::Button::kNoButton;
      PositionMouseEvent(*tool_, widget, mouse_move, p);
      widget->HandleInputEvent(
          blink::WebCoalescedInputEvent(mouse_move, ui::LatencyInfo()));
    }

    for (int i = 0; i < std::max(0, micro_moves); ++i) {
      gfx::PointF p = end;
      if (micro_radius > 0) {
        p = gfx::PointF(
            end.x() + static_cast<float>(base::RandInt(-micro_radius, micro_radius)),
            end.y() + static_cast<float>(base::RandInt(-micro_radius, micro_radius)));
      }
      p = ClampToNonNegative(p);
      WebMouseEvent mouse_move(WebInputEvent::Type::kMouseMove,
                               WebInputEvent::kNoModifiers,
                               ui::EventTimeForNow());
      mouse_move.button = WebMouseEvent::Button::kNoButton;
      PositionMouseEvent(*tool_, widget, mouse_move, p);
      widget->HandleInputEvent(
          blink::WebCoalescedInputEvent(mouse_move, ui::LatencyInfo()));
    }

    // Press and release on this stack. The old PostDelayedTask for the
    // mouse-down never ran while the renderer was inside the tool call, so a
    // humanized click moved the pointer and never pressed. The human-like
    // pacing comes from the move path above, not from a delay before press.
    DoMouseDown(button, count, target);
  } else if (base::FeatureList::IsEnabled(features::kGlicActorMoveBeforeClick)) {
    // Move from a nearby point so the gesture has movement, then press and
    // release on this stack. A posted delay never runs while the renderer is
    // inside this tool call, so the page was never receiving mouse down.
    const gfx::PointF end = ClampToNonNegative(target.widget_point);
    const gfx::PointF start =
        ClampToNonNegative(gfx::PointF(end.x() - 12.f, end.y() - 8.f));
    WebMouseEvent mouse_move(WebInputEvent::Type::kMouseMove,
                             WebInputEvent::kNoModifiers,
                             ui::EventTimeForNow());
    mouse_move.button = WebMouseEvent::Button::kNoButton;
    PositionMouseEvent(*tool_, widget, mouse_move, start);
    widget->HandleInputEvent(
        blink::WebCoalescedInputEvent(mouse_move, ui::LatencyInfo()));

    WebMouseEvent mouse_move_end(WebInputEvent::Type::kMouseMove,
                                 WebInputEvent::kNoModifiers,
                                 ui::EventTimeForNow());
    mouse_move_end.button = WebMouseEvent::Button::kNoButton;
    PositionMouseEvent(*tool_, widget, mouse_move_end, end);
    widget->HandleInputEvent(
        blink::WebCoalescedInputEvent(mouse_move_end, ui::LatencyInfo()));

    DoMouseDown(button, count, target);
  } else {
    DoMouseDown(button, count, target);
  }
}

ClickDispatcher::~ClickDispatcher() = default;

void ClickDispatcher::Cancel() {
  weak_ptr_factory_.InvalidateWeakPtrs();
  on_complete_.Reset();

  if (mouse_up_event_) {
    // If the mouse is down during cancelation, lift the mouse back up.
    DoMouseUpImpl();
  }
}

void ClickDispatcher::DoMouseDown(WebMouseEvent::Button button,
                                  int count,
                                  const ResolvedTarget& target) {
  for (int press = 1; press <= std::max(1, count); ++press) {
    WebWidget* widget = target.GetWidget(*tool_);
    if (!widget) {
      Finish(MakeResult(mojom::ActionResultCode::kFrameWentAway,
                        /*requires_page_stabilization=*/true,
                        "No widget when dispatching mouse down"));
      return;
    }

    int modifiers = key_modifiers_;
    if (button == WebMouseEvent::Button::kLeft) {
      modifiers |= WebInputEvent::kLeftButtonDown;
    } else if (button == WebMouseEvent::Button::kRight) {
      modifiers |= WebInputEvent::kRightButtonDown;
    }
    WebMouseEvent mouse_down(WebInputEvent::Type::kMouseDown, modifiers,
                             ui::EventTimeForNow());
    mouse_down.button = button;
    mouse_down.click_count = press;
    PositionMouseEvent(*tool_, widget, mouse_down, target.widget_point);

    blink::WebInputEventResult result = widget->HandleInputEvent(
        blink::WebCoalescedInputEvent(mouse_down, ui::LatencyInfo()));

    if (result == blink::WebInputEventResult::kHandledSuppressed) {
      Finish(MakeResult(mojom::ActionResultCode::kClickSuppressed,
                        /*requires_page_stabilization=*/false,
                        DescribeClickedElement(target.node)));
      return;
    }

    mouse_up_event_ = mouse_down;
    mouse_up_event_->SetType(WebInputEvent::Type::kMouseUp);
    mouse_up_event_->SetModifiers(key_modifiers_);

    if (press < count) {
      // Release between presses. If that failed (widget gone, suppressed) the
      // completion has already been delivered and there is nothing left to do.
      DoMouseUpImpl();
      if (!on_complete_) {
        return;
      }
    }
  }
  // Release on this stack. Posting the mouse-up task left the button down
  // whenever the renderer could not pump tasks inside the tool call.
  DoMouseUp();
}

void ClickDispatcher::DoMouseUp() {
  DoMouseUpImpl();
  // The node was resolved before the events, so this is the element that was
  // under the click point, not whatever the page became afterwards.
  mojom::ActionResultPtr result = MakeOkResult();
  result->message = DescribeClickedElement(target_.node);
  Finish(std::move(result));
}

void ClickDispatcher::DoMouseUpImpl() {
  WebWidget* widget = target_.GetWidget(*tool_);

  if (!widget) {
    Finish(MakeResult(mojom::ActionResultCode::kFrameWentAway,
                      /*requires_page_stabilization=*/true,
                      "No widget when dispatching mouse up"));
    return;
  }

  mouse_up_event_->SetTimeStamp(ui::EventTimeForNow());
  blink::WebInputEventResult result =
      widget->HandleInputEvent(blink::WebCoalescedInputEvent(
          std::move(*mouse_up_event_), ui::LatencyInfo()));
  mouse_up_event_.reset();
  if (result == blink::WebInputEventResult::kHandledSuppressed) {
    Finish(MakeResult(mojom::ActionResultCode::kClickSuppressed,
                      /*requires_page_stabilization=*/false,
                      DescribeClickedElement(target_.node)));
    return;
  }
}

void ClickDispatcher::Finish(mojom::ActionResultPtr result) {
  if (!on_complete_) {
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(on_complete_), std::move(result)));
  // This instance may now be deleted once the callback runs.
}

}  // namespace actor

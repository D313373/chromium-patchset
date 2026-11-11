// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/agent_input_injector.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/common/actor.mojom.h"
#include "chrome/common/actor/action_result.h"
#include "chrome/common/actor/task_id.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_render_frame.mojom.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "ui/gfx/geometry/point.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/common/input/web_pointer_properties.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"

namespace simulachrome {
namespace {

content::RenderWidgetHost* GetRwh(content::WebContents* web_contents) {
  if (!web_contents) {
    return nullptr;
  }
  content::RenderFrameHost* rfh = web_contents->GetPrimaryMainFrame();
  return rfh ? rfh->GetRenderWidgetHost() : nullptr;
}

// Browser-side fallback used when the actor tool is unavailable. Down and up
// share one timestamp and skip the renderer tool, so this is not the path a
// page click should take.
void ForwardClickAt(content::RenderWidgetHost* rwh,
                    content::RenderWidgetHostView* view,
                    int dip_x,
                    int dip_y) {
  const base::TimeTicks now = base::TimeTicks::Now();
  gfx::Point screen(dip_x, dip_y);
  if (view) {
    const gfx::Rect bounds = view->GetViewBounds();
    screen = gfx::Point(bounds.x() + dip_x, bounds.y() + dip_y);
  }

  blink::WebMouseEvent down(blink::WebInputEvent::Type::kMouseDown,
                            blink::WebInputEvent::kNoModifiers, now);
  // WebMouseEvent::Button aliases WebPointerProperties::Button.
  down.button = blink::WebPointerProperties::Button::kLeft;
  down.click_count = 1;
  down.SetPositionInWidget(dip_x, dip_y);
  down.SetPositionInScreen(screen.x(), screen.y());
  rwh->ForwardMouseEvent(down);

  blink::WebMouseEvent up(blink::WebInputEvent::Type::kMouseUp,
                          blink::WebInputEvent::kNoModifiers, now);
  up.button = blink::WebPointerProperties::Button::kLeft;
  up.click_count = 1;
  up.SetPositionInWidget(dip_x, dip_y);
  up.SetPositionInScreen(screen.x(), screen.y());
  rwh->ForwardMouseEvent(up);
}

// scroll_dx/dy are viewport fractions. The previous fixed 100px notch meant
// even a full-magnitude scroll moved ~8% of a 1200dip page, so the agent could
// never actually reach below-the-fold content.
void ForwardScrollAt(content::RenderWidgetHost* rwh,
                     const gfx::Size& viewport_dip,
                     int dip_x,
                     int dip_y,
                     float dx,
                     float dy) {
  const float px_x = dx * static_cast<float>(std::max(1, viewport_dip.width()));
  const float px_y =
      dy * static_cast<float>(std::max(1, viewport_dip.height()));

  const base::TimeTicks now = base::TimeTicks::Now();
  blink::WebMouseWheelEvent wheel(blink::WebInputEvent::Type::kMouseWheel,
                                  blink::WebInputEvent::kNoModifiers, now);
  wheel.SetPositionInWidget(dip_x, dip_y);
  wheel.delta_x = px_x;
  wheel.delta_y = -px_y;  // positive scroll_dy => scroll down
  wheel.phase = blink::WebMouseWheelEvent::kPhaseBegan;
  rwh->ForwardWheelEvent(wheel);

  // Wheel latching expects a terminating phase; without one the scroll gesture
  // stays open and the next wheel event can be coalesced into it.
  blink::WebMouseWheelEvent end(blink::WebInputEvent::Type::kMouseWheel,
                                blink::WebInputEvent::kNoModifiers, now);
  end.SetPositionInWidget(dip_x, dip_y);
  end.delta_x = 0.f;
  end.delta_y = 0.f;
  end.phase = blink::WebMouseWheelEvent::kPhaseEnded;
  rwh->ForwardWheelEvent(end);
}

// Maps the model's key names onto the fields Blink needs. |text| is the
// character to deliver in the kChar event; 0 means the key produces no
// character (Enter still needs '\r' or forms will not submit).
bool ResolveNamedKey(const std::string& name,
                     ui::KeyboardCode* code,
                     ui::DomKey* dom_key,
                     char16_t* text) {
  struct Entry {
    const char* name;
    ui::KeyboardCode code;
    ui::DomKey::Base dom_key;
    char16_t text;
  };
  static const Entry kEntries[] = {
      {"enter", ui::VKEY_RETURN, ui::DomKey::ENTER, u'\r'},
      {"return", ui::VKEY_RETURN, ui::DomKey::ENTER, u'\r'},
      {"tab", ui::VKEY_TAB, ui::DomKey::TAB, u'\t'},
      {"escape", ui::VKEY_ESCAPE, ui::DomKey::ESCAPE, 0},
      {"esc", ui::VKEY_ESCAPE, ui::DomKey::ESCAPE, 0},
      {"backspace", ui::VKEY_BACK, ui::DomKey::BACKSPACE, 0},
      {"delete", ui::VKEY_DELETE, ui::DomKey::DEL, 0},
      {"up", ui::VKEY_UP, ui::DomKey::ARROW_UP, 0},
      {"down", ui::VKEY_DOWN, ui::DomKey::ARROW_DOWN, 0},
      {"left", ui::VKEY_LEFT, ui::DomKey::ARROW_LEFT, 0},
      {"right", ui::VKEY_RIGHT, ui::DomKey::ARROW_RIGHT, 0},
  };
  for (const Entry& e : kEntries) {
    if (base::EqualsCaseInsensitiveASCII(name, e.name)) {
      *code = e.code;
      *dom_key = ui::DomKey(e.dom_key);
      *text = e.text;
      return true;
    }
  }
  return false;
}

// kRawKeyDown -> kChar -> kKeyUp against the focused element. Unlike the
// kType path this sets windows_key_code, which is what Blink surfaces as
// KeyboardEvent.keyCode and what form submission keys off.
void ForwardKeyPress(content::RenderWidgetHost* rwh,
                     gfx::NativeView native_view,
                     ui::KeyboardCode code,
                     ui::DomKey dom_key,
                     char16_t text) {
  auto send = [&](blink::WebInputEvent::Type type, bool with_text) {
    blink::WebKeyboardEvent key(type, blink::WebInputEvent::kNoModifiers,
                                base::TimeTicks::Now());
    key.windows_key_code = code;
    key.dom_key = dom_key;
    if (with_text && text) {
      key.text[0] = text;
      key.unmodified_text[0] = text;
    }
    input::NativeWebKeyboardEvent native_key(key, native_view);
    rwh->ForwardKeyboardEvent(native_key);
  };

  send(blink::WebInputEvent::Type::kRawKeyDown, false);
  if (text) {
    send(blink::WebInputEvent::Type::kChar, true);
  }
  send(blink::WebInputEvent::Type::kKeyUp, false);
}


// Trusted click: ChromeRenderFrame.InvokeTool -> renderer ClickTool ->
// WebWidget::HandleInputEvent. The call is posted so it leaves the browser
// after the utility-process reply that invoked us. A nested run loop inside
// that reply never flushes the associated interface, so the renderer never
// sees the click.
void DispatchTrustedTool(content::RenderFrameHost* frame,
                         int dip_x,
                         int dip_y,
                         actor::mojom::ToolActionPtr tool_action,
                         AgentInputInjector::ApplyCallback done) {
  auto fail = [&done](const char* error) {
    std::move(done).Run(false, error);
  };
  if (!frame || !frame->IsRenderFrameLive()) {
    fail("trusted_click_no_frame");
    return;
  }
  if (!base::FeatureList::IsEnabled(features::kGlicActor)) {
    fail("trusted_click_actor_disabled");
    return;
  }

  struct WaitState {
    bool finished = false;
    std::unique_ptr<mojo::AssociatedRemote<chrome::mojom::ChromeRenderFrame>>
        remote;
    AgentInputInjector::ApplyCallback done;

    void Finish(bool success, std::string message) {
      if (finished) {
        return;
      }
      finished = true;
      if (done) {
        std::move(done).Run(success, std::move(message));
      }
    }
  };
  auto state = std::make_shared<WaitState>();
  state->done = std::move(done);
  state->remote = std::make_unique<
      mojo::AssociatedRemote<chrome::mojom::ChromeRenderFrame>>();
  frame->GetRemoteAssociatedInterfaces()->GetInterface(state->remote.get());
  if (!state->remote->is_bound()) {
    state->Finish(false, "trusted_click_unbound");
    return;
  }
  state->remote->set_disconnect_handler(base::BindOnce(
      [](std::shared_ptr<WaitState> state) {
        state->Finish(false, "trusted_click_disconnected");
      },
      state));

  auto invocation = actor::mojom::ToolInvocation::New();
  invocation->task_id = actor::TaskId(1);
  invocation->action = std::move(tool_action);
  invocation->target =
      actor::mojom::ToolTarget::NewCoordinateDip(gfx::Point(dip_x, dip_y));

  if (content::RenderWidgetHost* widget_host = frame->GetRenderWidgetHost()) {
    blink::WebMouseEvent activation;
    activation.SetType(blink::WebInputEvent::Type::kMouseDown);
    widget_host->WillSendInputEventToRenderer(activation);
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<WaitState> state,
             actor::mojom::ToolInvocationPtr invocation, int dip_x, int dip_y) {
            if (state->finished || !state->remote ||
                !state->remote->is_bound()) {
              state->Finish(false, "trusted_click_unbound");
              return;
            }
            LOG(INFO) << "[AgentInputInjector] InvokeTool click dip=(" << dip_x
                      << "," << dip_y << ")";
            chrome::mojom::ChromeRenderFrame* raw = state->remote->get();
            raw->InvokeTool(
                std::move(invocation),
                base::BindOnce(
                    [](std::shared_ptr<WaitState> state,
                       actor::mojom::ActionResultPtr result) {
                      const int code =
                          result ? static_cast<int>(result->code) : -1;
                      const std::string message =
                          result ? result->message : std::string();
                      LOG(INFO)
                          << "[AgentInputInjector] trusted click result code="
                          << code << " msg=" << message;
                      const bool success = result && actor::IsOk(*result);
                      // On success `message` is the element under the click.
                      state->Finish(
                          success,
                          success ? message
                                  : base::StringPrintf("trusted_click:%d %s",
                                                       code, message.c_str()));
                    },
                    state));
          },
          state, std::move(invocation), dip_x, dip_y));

  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<WaitState> state) {
            state->Finish(false, "trusted_click_timeout");
          },
          state),
      base::Seconds(3));
}

void DispatchTrustedClick(content::RenderFrameHost* frame,
                          int dip_x,
                          int dip_y,
                          actor::mojom::ClickAction::Type button,
                          actor::mojom::ClickAction::Count count,
                          AgentInputInjector::ApplyCallback done) {
  auto click = actor::mojom::ClickAction::New();
  click->type = button;
  click->count = count;
  DispatchTrustedTool(frame, dip_x, dip_y,
                      actor::mojom::ToolAction::NewClick(std::move(click)),
                      std::move(done));
}


// --simulachrome-blink-input: click, double click and right click go through
// ChromeRenderFrame.InvokeTool (Blink-trusted, renderer-side hit testing)
// instead of the ScriptExecutor bridge. Other operations are unaffected.
bool BlinkInputRequested() {
  static const bool requested =
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          "simulachrome-blink-input");
  return requested;
}

// "Enter", "Tab", or a chord such as "ctrl+a" / "shift+Tab". The last piece
// is the key; everything before it is a modifier. The renderer resolves the
// key name, so this only splits the chord.
actor::mojom::KeyPressActionPtr ParseKeyPress(const std::string& payload) {
  const std::string trimmed(
      base::TrimWhitespaceASCII(payload, base::TRIM_ALL));
  if (trimmed.empty()) {
    return nullptr;
  }
  const std::vector<std::string> parts = base::SplitString(
      trimmed, "+", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  if (parts.empty()) {
    return nullptr;
  }
  uint32_t modifiers = 0;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    const std::string& mod = parts[i];
    if (base::EqualsCaseInsensitiveASCII(mod, "ctrl") ||
        base::EqualsCaseInsensitiveASCII(mod, "control")) {
      modifiers |= actor::mojom::kInputModifierControl;
    } else if (base::EqualsCaseInsensitiveASCII(mod, "alt") ||
               base::EqualsCaseInsensitiveASCII(mod, "option")) {
      modifiers |= actor::mojom::kInputModifierAlt;
    } else if (base::EqualsCaseInsensitiveASCII(mod, "shift")) {
      modifiers |= actor::mojom::kInputModifierShift;
    } else if (base::EqualsCaseInsensitiveASCII(mod, "meta") ||
               base::EqualsCaseInsensitiveASCII(mod, "cmd") ||
               base::EqualsCaseInsensitiveASCII(mod, "command")) {
      modifiers |= actor::mojom::kInputModifierMeta;
    } else {
      return nullptr;
    }
  }
  const std::string& key = parts.back();
  if (key.empty() || key.size() > 32) {
    return nullptr;
  }
  auto press = actor::mojom::KeyPressAction::New();
  press->key = key;
  press->modifiers = modifiers;
  press->count = 1;
  return press;
}

bool IsBlinkRoutableClick(mojom::AgentActionType type) {
  return type == mojom::AgentActionType::kClick ||
         type == mojom::AgentActionType::kDoubleClick ||
         type == mojom::AgentActionType::kRightClick ||
         type == mojom::AgentActionType::kKeyPress ||
         type == mojom::AgentActionType::kScroll ||
         type == mojom::AgentActionType::kType;
}

AgentInputInjector::ApplyOverride& ApplyOverrideSlot() {
  static base::NoDestructor<AgentInputInjector::ApplyOverride> slot;
  return *slot;
}

AgentInputInjector::TokenExpander& TokenExpanderSlot() {
  static base::NoDestructor<AgentInputInjector::TokenExpander> slot;
  return *slot;
}

}  // namespace

void AgentInputInjector::SetApplyOverride(ApplyOverride override) {
  ApplyOverrideSlot() = std::move(override);
}

void AgentInputInjector::SetTokenExpander(TokenExpander expander) {
  TokenExpanderSlot() = std::move(expander);
}

bool AgentInputInjector::NormalizedToWidgetDip(
    content::WebContents* web_contents,
    float norm_x,
    float norm_y,
    int* out_x,
    int* out_y) {
  if (!web_contents || !out_x || !out_y) {
    return false;
  }
  content::RenderWidgetHostView* view = web_contents->GetRenderWidgetHostView();
  if (!view) {
    return false;
  }

  // AgentAction x/y are normalized [0,1] in CSS viewport space (top-left origin).
  // GetViewBounds().size() is the view size in DIPs — multiply, do NOT also
  // multiply by DeviceScaleFactor (PositionInWidget is DIP / CSS pixels).
  const gfx::Size dip_size = view->GetViewBounds().size();
  const int width = std::max(1, dip_size.width());
  const int height = std::max(1, dip_size.height());

  const float nx = std::clamp(norm_x, 0.f, 1.f);
  const float ny = std::clamp(norm_y, 0.f, 1.f);
  *out_x = static_cast<int>(std::lround(nx * static_cast<float>(width - 1)));
  *out_y = static_cast<int>(std::lround(ny * static_cast<float>(height - 1)));
  return true;
}

void AgentInputInjector::ApplyAction(content::WebContents* web_contents,
                                     const mojom::AgentAction& action,
                                     ApplyCallback done) {
  // Blink type never enters ScriptExecutor, so the playback catalog pass
  // would not run. Expand here, then hand the same action to either path.
  // The caller's action is left as the token form for the step log.
  mojom::AgentActionPtr expanded = action.Clone();
  if (expanded->type == mojom::AgentActionType::kType &&
      TokenExpanderSlot() &&
      expanded->payload.find('{') != std::string::npos) {
    const size_t before = expanded->payload.size();
    expanded->payload =
        TokenExpanderSlot().Run(web_contents, expanded->payload);
    LOG(INFO) << "[AgentInputInjector] type token expansion before_len="
              << before << " after_len=" << expanded->payload.size();
  }
  const mojom::AgentAction& step = *expanded;

  if (ApplyOverrideSlot() &&
      !(BlinkInputRequested() && IsBlinkRoutableClick(step.type))) {
    ApplyOverrideSlot().Run(web_contents, step, std::move(done));
    return;
  }
  auto fail = [&done](const char* error) {
    std::move(done).Run(false, error);
  };
  content::RenderWidgetHost* rwh = GetRwh(web_contents);
  content::RenderWidgetHostView* view =
      web_contents ? web_contents->GetRenderWidgetHostView() : nullptr;
  if (!rwh || !view) {
    fail("input_inject_failed");
    return;
  }

  int dip_x = 0;
  int dip_y = 0;
  if (step.type == mojom::AgentActionType::kClick ||
      step.type == mojom::AgentActionType::kDoubleClick ||
      step.type == mojom::AgentActionType::kRightClick ||
      step.type == mojom::AgentActionType::kHover ||
      step.type == mojom::AgentActionType::kDrag ||
      step.type == mojom::AgentActionType::kScroll ||
      step.type == mojom::AgentActionType::kType) {
    if (!NormalizedToWidgetDip(web_contents, step.x, step.y, &dip_x,
                               &dip_y)) {
      fail("input_inject_failed");
      return;
    }
    LOG(INFO) << "[AgentInputInjector] norm=(" << step.x << "," << step.y
              << ") → DIP=(" << dip_x << "," << dip_y << ") type="
              << static_cast<int>(step.type);
  }

  switch (step.type) {
    case mojom::AgentActionType::kClick:
    case mojom::AgentActionType::kDoubleClick:
    case mojom::AgentActionType::kRightClick: {
      DispatchTrustedClick(
          web_contents->GetPrimaryMainFrame(), dip_x, dip_y,
          step.type == mojom::AgentActionType::kRightClick
              ? actor::mojom::ClickAction::Type::kRight
              : actor::mojom::ClickAction::Type::kLeft,
          step.type == mojom::AgentActionType::kDoubleClick
              ? actor::mojom::ClickAction::Count::kDouble
              : actor::mojom::ClickAction::Count::kSingle,
          std::move(done));
      return;
    }
    case mojom::AgentActionType::kHover:
    case mojom::AgentActionType::kDrag:
      // Hover and drag only exist on the ScriptExecutor bridge (drag is CDP by
      // design). Without it there is nothing safe to fall back to.
      fail("input_unsupported_without_executor");
      return;
    case mojom::AgentActionType::kScroll:
      if (BlinkInputRequested()) {
        auto scroll = actor::mojom::ScrollAction::New();
        scroll->mode = actor::mojom::ScrollAction::Mode::kWheel;
        const bool vertical =
            std::abs(step.scroll_dy) >= std::abs(step.scroll_dx);
        scroll->direction =
            vertical ? (step.scroll_dy >= 0
                            ? actor::mojom::ScrollAction::ScrollDirection::kDown
                            : actor::mojom::ScrollAction::ScrollDirection::kUp)
                     : (step.scroll_dx >= 0
                            ? actor::mojom::ScrollAction::ScrollDirection::kRight
                            : actor::mojom::ScrollAction::ScrollDirection::kLeft);
        scroll->distance = std::max(
            1.0f, vertical ? std::abs(step.scroll_dy)
                           : std::abs(step.scroll_dx));
        DispatchTrustedTool(web_contents->GetPrimaryMainFrame(), dip_x, dip_y,
                            actor::mojom::ToolAction::NewScroll(
                                std::move(scroll)),
                            std::move(done));
        return;
      }
      ForwardScrollAt(rwh, view->GetViewBounds().size(), dip_x, dip_y,
                      step.scroll_dx, step.scroll_dy);
      std::move(done).Run(true, std::string());
      return;
    case mojom::AgentActionType::kType: {
      if (BlinkInputRequested()) {
        // Blink TypeTool: trusted focusing click at the point, then keys.
        // require_editable makes "typed into nothing" a failure, not a no-op.
        std::string payload = step.payload;
        bool submit = false;
        while (!payload.empty() && payload.back() == '\n') {
          payload.pop_back();
          submit = true;
        }
        auto type = actor::mojom::TypeAction::New();
        type->mode = actor::mojom::TypeAction::Mode::kAppend;
        type->text = std::move(payload);
        type->follow_by_enter = submit;
        type->require_editable = true;
        DispatchTrustedTool(web_contents->GetPrimaryMainFrame(), dip_x, dip_y,
                            actor::mojom::ToolAction::NewType(std::move(type)),
                            std::move(done));
        return;
      }
      ForwardClickAt(rwh, view, dip_x, dip_y);
      // UI-TARS submits a field by ending content with a newline. Delivering
      // that '\n' as a kChar does not submit a form, so it is consumed here
      // and replayed as a real Enter press after the text.
      std::string payload = step.payload;
      bool submit = false;
      while (!payload.empty() && payload.back() == '\n') {
        payload.pop_back();
        submit = true;
      }
      const std::u16string text = base::UTF8ToUTF16(payload);
      gfx::NativeView native_view = view->GetNativeView();
      for (char16_t ch : text) {
        blink::WebKeyboardEvent key(blink::WebInputEvent::Type::kChar,
                                    blink::WebInputEvent::kNoModifiers,
                                    base::TimeTicks::Now());
        key.dom_key = ui::DomKey::FromCharacter(ch);
        key.text[0] = ch;
        key.unmodified_text[0] = ch;
        input::NativeWebKeyboardEvent native_key(key, native_view);
        rwh->ForwardKeyboardEvent(native_key);
      }
      if (submit) {
        LOG(INFO) << "[AgentInputInjector] type + Enter (trailing newline)";
        ForwardKeyPress(rwh, native_view, ui::VKEY_RETURN,
                        ui::DomKey(ui::DomKey::ENTER), u'\r');
      }
      std::move(done).Run(true, std::string());
      return;
    }
    case mojom::AgentActionType::kKeyPress: {
      if (BlinkInputRequested()) {
        actor::mojom::KeyPressActionPtr press = ParseKeyPress(step.payload);
        if (!press) {
          fail("input_inject_failed");
          return;
        }
        // Key presses go to the focused element; the target is a dummy.
        DispatchTrustedTool(web_contents->GetPrimaryMainFrame(), 0, 0,
                            actor::mojom::ToolAction::NewKeyPress(
                                std::move(press)),
                            std::move(done));
        return;
      }
      ui::KeyboardCode code = ui::VKEY_UNKNOWN;
      ui::DomKey dom_key = ui::DomKey::NONE;
      char16_t text = 0;
      if (!ResolveNamedKey(step.payload, &code, &dom_key, &text)) {
        LOG(ERROR) << "[AgentInputInjector] unknown key \"" << step.payload
                   << "\"";
        fail("input_inject_failed");
        return;
      }
      LOG(INFO) << "[AgentInputInjector] press \"" << step.payload << "\"";
      ForwardKeyPress(rwh, view->GetNativeView(), code, dom_key, text);
      std::move(done).Run(true, std::string());
      return;
    }
    case mojom::AgentActionType::kWait:
    case mojom::AgentActionType::kDone:
    case mojom::AgentActionType::kNone:
    case mojom::AgentActionType::kFail:
    case mojom::AgentActionType::kElementNotFound:
      std::move(done).Run(true, std::string());
      return;
  }
  fail("input_inject_failed");
}

}  // namespace simulachrome

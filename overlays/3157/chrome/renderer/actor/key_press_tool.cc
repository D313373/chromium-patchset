// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/actor/key_press_tool.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "base/strings/string_util.h"
#include "chrome/common/actor/action_result.h"
#include "chrome/common/actor/journal_details_builder.h"
#include "chrome/renderer/actor/tool_utils.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/abseil-cpp/absl/strings/str_format.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/platform/web_input_event_result.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/latency/latency_info.h"

namespace actor {

using ::blink::WebInputEvent;
using ::blink::WebInputEventResult;
using ::blink::WebKeyboardEvent;

namespace {

constexpr uint32_t kMaxRepeat = 20;

struct KeySpec {
  int windows_key_code = 0;
  ui::DomCode dom_code = ui::DomCode::NONE;
  std::string dom_key;
  char16_t text = u'\0';  // Nonzero => a Char event follows the RawKeyDown.
};

struct NamedKey {
  std::string_view name;  // Lower-case.
  int vkey;
  ui::DomCode code;
  std::string_view dom_key;
  char16_t text;
};

constexpr NamedKey kNamedKeys[] = {
    {"enter", ui::VKEY_RETURN, ui::DomCode::ENTER, "Enter", u'\r'},
    {"return", ui::VKEY_RETURN, ui::DomCode::ENTER, "Enter", u'\r'},
    {"tab", ui::VKEY_TAB, ui::DomCode::TAB, "Tab", u'\t'},
    {"escape", ui::VKEY_ESCAPE, ui::DomCode::ESCAPE, "Escape", 0},
    {"esc", ui::VKEY_ESCAPE, ui::DomCode::ESCAPE, "Escape", 0},
    {"backspace", ui::VKEY_BACK, ui::DomCode::BACKSPACE, "Backspace", 0},
    {"delete", ui::VKEY_DELETE, ui::DomCode::DEL, "Delete", 0},
    {"space", ui::VKEY_SPACE, ui::DomCode::SPACE, " ", u' '},
    {"arrowup", ui::VKEY_UP, ui::DomCode::ARROW_UP, "ArrowUp", 0},
    {"arrowdown", ui::VKEY_DOWN, ui::DomCode::ARROW_DOWN, "ArrowDown", 0},
    {"arrowleft", ui::VKEY_LEFT, ui::DomCode::ARROW_LEFT, "ArrowLeft", 0},
    {"arrowright", ui::VKEY_RIGHT, ui::DomCode::ARROW_RIGHT, "ArrowRight", 0},
    {"up", ui::VKEY_UP, ui::DomCode::ARROW_UP, "ArrowUp", 0},
    {"down", ui::VKEY_DOWN, ui::DomCode::ARROW_DOWN, "ArrowDown", 0},
    {"left", ui::VKEY_LEFT, ui::DomCode::ARROW_LEFT, "ArrowLeft", 0},
    {"right", ui::VKEY_RIGHT, ui::DomCode::ARROW_RIGHT, "ArrowRight", 0},
    {"home", ui::VKEY_HOME, ui::DomCode::HOME, "Home", 0},
    {"end", ui::VKEY_END, ui::DomCode::END, "End", 0},
    {"pageup", ui::VKEY_PRIOR, ui::DomCode::PAGE_UP, "PageUp", 0},
    {"pagedown", ui::VKEY_NEXT, ui::DomCode::PAGE_DOWN, "PageDown", 0},
};

std::optional<KeySpec> ResolveKey(const std::string& key) {
  std::string lower = base::ToLowerASCII(key);
  for (const NamedKey& k : kNamedKeys) {
    if (lower == k.name) {
      return KeySpec{k.vkey, k.code, std::string(k.dom_key), k.text};
    }
  }
  // Single printable ASCII character.
  if (key.size() == 1) {
    char c = key[0];
    KeySpec spec;
    spec.dom_key = key;
    spec.text = static_cast<char16_t>(c);
    if (c >= 'a' && c <= 'z') {
      spec.windows_key_code = ui::VKEY_A + (c - 'a');
      spec.dom_code = static_cast<ui::DomCode>(
          static_cast<int>(ui::DomCode::US_A) + (c - 'a'));
      return spec;
    }
    if (c >= 'A' && c <= 'Z') {
      spec.windows_key_code = ui::VKEY_A + (c - 'A');
      spec.dom_code = static_cast<ui::DomCode>(
          static_cast<int>(ui::DomCode::US_A) + (c - 'A'));
      return spec;
    }
    if (c >= '0' && c <= '9') {
      spec.windows_key_code = ui::VKEY_0 + (c - '0');
      spec.dom_code = c == '0' ? ui::DomCode::DIGIT0
                               : static_cast<ui::DomCode>(
                                     static_cast<int>(ui::DomCode::DIGIT1) +
                                     (c - '1'));
      return spec;
    }
  }
  return std::nullopt;
}

}  // namespace

KeyPressTool::KeyPressTool(content::RenderFrame& frame,
                           TaskId task_id,
                           Journal& journal,
                           mojom::KeyPressActionPtr action,
                           mojom::ToolTargetPtr target,
                           mojom::ObservedToolTargetPtr observed_target)
    : ToolBase(frame,
               task_id,
               journal,
               std::move(target),
               std::move(observed_target)),
      action_(std::move(action)) {}

KeyPressTool::~KeyPressTool() = default;

bool KeyPressTool::EnsureTargetInView() {
  // Keys go to the focused element; never scroll for a dummy target.
  return false;
}

void KeyPressTool::Execute(ToolFinishedCallback callback) {
  std::optional<KeySpec> spec = ResolveKey(action_->key);
  if (!spec) {
    std::move(callback).Run(MakeResult(
        mojom::ActionResultCode::kArgumentsInvalid,
        /*requires_page_stabilization=*/false,
        absl::StrFormat("unknown_key[%s]", action_->key)));
    return;
  }
  blink::WebLocalFrame* web_frame = frame_->GetWebFrame();
  blink::WebFrameWidget* widget = web_frame ? web_frame->FrameWidget() : nullptr;
  if (!widget) {
    std::move(callback).Run(MakeResult(mojom::ActionResultCode::kFrameWentAway,
                                       /*requires_page_stabilization=*/false,
                                       "No widget for key press"));
    return;
  }

  const int modifiers = ToBlinkInputModifiers(action_->modifiers);
  // Ctrl/Meta/Alt chords produce commands, not text.
  const bool produces_text =
      spec->text != 0 && !(modifiers & (WebInputEvent::kControlKey |
                                        WebInputEvent::kMetaKey |
                                        WebInputEvent::kAltKey));
  const uint32_t count = std::clamp<uint32_t>(action_->count, 1, kMaxRepeat);
  const int native_key = ui::KeycodeConverter::DomCodeToNativeKeycode(spec->dom_code);

  auto dispatch = [&](WebInputEvent::Type type) {
    WebKeyboardEvent e(type, modifiers, ui::EventTimeForNow());
    e.windows_key_code = spec->windows_key_code;
    e.native_key_code = native_key;
    e.dom_code = static_cast<int>(spec->dom_code);
    e.dom_key = ui::KeycodeConverter::KeyStringToDomKey(spec->dom_key);
    if (type == WebInputEvent::Type::kChar || type == WebInputEvent::Type::kRawKeyDown) {
      e.text[0] = produces_text ? spec->text : 0;
      e.unmodified_text[0] = spec->text;
    }
    return widget->HandleInputEvent(
        blink::WebCoalescedInputEvent(e, ui::LatencyInfo()));
  };

  uint32_t suppressed = 0;
  for (uint32_t i = 0; i < count; ++i) {
    WebInputEventResult down = dispatch(WebInputEvent::Type::kRawKeyDown);
    if (down == WebInputEventResult::kHandledSuppressed) {
      ++suppressed;  // Page preventDefault()'d keydown; Char is then skipped.
    } else if (produces_text) {
      dispatch(WebInputEvent::Type::kChar);
    }
    dispatch(WebInputEvent::Type::kKeyUp);
    // Page script may have torn the frame down; stop if so.
    web_frame = frame_->GetWebFrame();
    widget = web_frame ? web_frame->FrameWidget() : nullptr;
    if (!widget) {
      break;
    }
  }

  journal_->Log(task_id_, "KeyPressTool::Execute",
                JournalDetailsBuilder()
                    .Add("key", action_->key)
                    .Add("count", static_cast<int>(count))
                    .Add("suppressed", static_cast<int>(suppressed))
                    .Build());

  // A suppressed keydown still means the page observed the key (common for
  // shortcuts handled by script), so it is a successful action.
  std::move(callback).Run(MakeOkResult());
}

std::string KeyPressTool::DebugString() const {
  return absl::StrFormat("KeyPressTool[key(%s);modifiers(%u);count(%u)]",
                         action_->key, action_->modifiers, action_->count);
}

}  // namespace actor

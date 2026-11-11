#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_type_action.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

#include "base/json/string_escape.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_click_action.h"
#include "chrome/browser/ui/webui/dashboard/click_handler.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_devtools_client.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace {

struct KeyPosition {
  double x;
  double y;
};

struct CdpKeyEventSpec {
  std::string key;
  std::string code;
  int key_code = 0;
  bool send_text = false;
  char text_char = 0;
};

char NormalizeKeyboardKey(char c) {
  switch (c) {
    case '!':
      return '1';
    case '@':
      return '2';
    case '#':
      return '3';
    case '$':
      return '4';
    case '%':
      return '5';
    case '^':
      return '6';
    case '&':
      return '7';
    case '*':
      return '8';
    case '(':
      return '9';
    case ')':
      return '0';
    case '_':
      return '-';
    case '+':
      return '=';
    case '{':
      return '[';
    case '}':
      return ']';
    case '|':
      return '\\';
    case ':':
      return ';';
    case '"':
      return '\'';
    case '<':
      return ',';
    case '>':
      return '.';
    case '?':
      return '/';
    case '~':
      return '`';
    default:
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
}

std::optional<KeyPosition> LookupKeyboardPosition(char key) {
  const char normalized = NormalizeKeyboardKey(key);

  const std::string number_row = "1234567890";
  const std::string top_row = "qwertyuiop";
  const std::string home_row = "asdfghjkl";
  const std::string bottom_row = "zxcvbnm";

  size_t index = number_row.find(normalized);
  if (index != std::string::npos) {
    return KeyPosition{static_cast<double>(index), -1.0};
  }

  index = top_row.find(normalized);
  if (index != std::string::npos) {
    return KeyPosition{static_cast<double>(index), 0.0};
  }

  index = home_row.find(normalized);
  if (index != std::string::npos) {
    return KeyPosition{static_cast<double>(index) + 0.5, 1.0};
  }

  index = bottom_row.find(normalized);
  if (index != std::string::npos) {
    return KeyPosition{static_cast<double>(index) + 1.0, 2.0};
  }

  switch (normalized) {
    case '`':
      return KeyPosition{-1.0, -1.0};
    case '-':
      return KeyPosition{10.0, -1.0};
    case '=':
      return KeyPosition{11.0, -1.0};
    case '[':
      return KeyPosition{10.0, 0.0};
    case ']':
      return KeyPosition{11.0, 0.0};
    case '\\':
      return KeyPosition{12.0, 0.0};
    case ';':
      return KeyPosition{9.5, 1.0};
    case '\'':
      return KeyPosition{10.5, 1.0};
    case ',':
      return KeyPosition{8.0, 2.0};
    case '.':
      return KeyPosition{9.0, 2.0};
    case '/':
      return KeyPosition{10.0, 2.0};
    case ' ':
      return KeyPosition{5.0, 3.0};
    default:
      return std::nullopt;
  }
}

CdpKeyEventSpec BuildCdpKeyEventSpec(char c) {
  CdpKeyEventSpec spec;
  if (c == '\n' || c == '\r') {
    spec.key = "Enter";
    spec.code = "Enter";
    spec.key_code = ui::VKEY_RETURN;
    spec.send_text = true;
    spec.text_char = '\r';
    return spec;
  }
  if (c == '\t') {
    spec.key = "Tab";
    spec.code = "Tab";
    spec.key_code = ui::VKEY_TAB;
    return spec;
  }
  if (c == '\b') {
    spec.key = "Backspace";
    spec.code = "Backspace";
    spec.key_code = ui::VKEY_BACK;
    return spec;
  }

  spec.key = std::string(1, c);
  const unsigned char uc = static_cast<unsigned char>(c);
  const char normalized = NormalizeKeyboardKey(c);
  spec.send_text = (uc >= 32 && uc != 127);
  spec.text_char = spec.send_text ? c : 0;

  if (normalized >= 'a' && normalized <= 'z') {
    spec.key_code =
        static_cast<int>(ui::VKEY_A + (normalized - 'a'));
    spec.code = "Key" +
                std::string(1,
                            static_cast<char>(std::toupper(normalized)));
    return spec;
  }
  if (normalized >= '0' && normalized <= '9') {
    spec.key_code =
        static_cast<int>(ui::VKEY_0 + (normalized - '0'));
    spec.code = "Digit" + std::string(1, normalized);
    return spec;
  }

  switch (normalized) {
    case ' ':
      spec.key_code = ui::VKEY_SPACE;
      spec.code = "Space";
      return spec;
    case '-':
      spec.key_code = ui::VKEY_OEM_MINUS;
      spec.code = "Minus";
      return spec;
    case '=':
      spec.key_code = ui::VKEY_OEM_PLUS;
      spec.code = "Equal";
      return spec;
    case '[':
      spec.key_code = ui::VKEY_OEM_4;
      spec.code = "BracketLeft";
      return spec;
    case ']':
      spec.key_code = ui::VKEY_OEM_6;
      spec.code = "BracketRight";
      return spec;
    case '\\':
      spec.key_code = ui::VKEY_OEM_5;
      spec.code = "Backslash";
      return spec;
    case ';':
      spec.key_code = ui::VKEY_OEM_1;
      spec.code = "Semicolon";
      return spec;
    case '\'':
      spec.key_code = ui::VKEY_OEM_7;
      spec.code = "Quote";
      return spec;
    case ',':
      spec.key_code = ui::VKEY_OEM_COMMA;
      spec.code = "Comma";
      return spec;
    case '.':
      spec.key_code = ui::VKEY_OEM_PERIOD;
      spec.code = "Period";
      return spec;
    case '/':
      spec.key_code = ui::VKEY_OEM_2;
      spec.code = "Slash";
      return spec;
    case '`':
      spec.key_code = ui::VKEY_OEM_3;
      spec.code = "Backquote";
      return spec;
    default:
      return spec;
  }
}

bool IsFrameHintLocator(const dashboard::Locator& locator) {
  return locator.type == dashboard::LocatorType::FRAME_ID;
}

bool IsTypingFocusLocator(const dashboard::Locator& locator) {
  switch (locator.type) {
    case dashboard::LocatorType::FRAME_ID:
    case dashboard::LocatorType::RESOURCE_TREE:
      return false;
    default:
      return !locator.value.empty();
  }
}

std::vector<dashboard::Locator> BuildTypingFocusLocators(
    const std::vector<dashboard::Locator>& locators) {
  std::vector<dashboard::Locator> filtered;
  filtered.reserve(locators.size());
  for (const auto& locator : locators) {
    if (IsTypingFocusLocator(locator)) {
      filtered.push_back(locator);
    }
  }
  return filtered;
}

std::vector<dashboard::Locator> BuildFrameHintLocators(
    const std::vector<dashboard::Locator>& locators) {
  std::vector<dashboard::Locator> filtered;
  filtered.reserve(locators.size());
  for (const auto& locator : locators) {
    if (IsFrameHintLocator(locator) && !locator.value.empty()) {
      filtered.push_back(locator);
    }
  }
  return filtered;
}

void MaybeAppendFrameHintFromParams(const base::Value::Dict& params,
                                    std::vector<dashboard::Locator>* hints) {
  if (!hints) {
    return;
  }
  const std::string* frame_id = params.FindString("frame_id");
  if (!frame_id || frame_id->empty()) {
    frame_id = params.FindString("frameId");
  }
  if (!frame_id || frame_id->empty()) {
    return;
  }
  for (const auto& locator : *hints) {
    if (locator.type == dashboard::LocatorType::FRAME_ID &&
        locator.value == *frame_id) {
      return;
    }
  }
  dashboard::Locator locator;
  locator.type = dashboard::LocatorType::FRAME_ID;
  locator.value = *frame_id;
  hints->push_back(std::move(locator));
}

std::vector<dashboard::Locator> BuildScopedTypingLocators(
    const std::vector<dashboard::Locator>& actionable,
    const std::vector<dashboard::Locator>& frame_hints) {
  std::vector<dashboard::Locator> combined;
  combined.reserve(frame_hints.size() + actionable.size());
  combined.insert(combined.end(), frame_hints.begin(), frame_hints.end());
  combined.insert(combined.end(), actionable.begin(), actionable.end());
  return combined;
}

base::Value::Dict BuildTypeFocusClickParams(
    const std::vector<dashboard::Locator>& locators,
    int wait_timeout_ms) {
  base::Value::Dict params;
  base::Value::List locator_values;
  for (const auto& locator : locators) {
    locator_values.Append(dashboard::LocatorToValue(locator));
  }
  params.Set("locators", std::move(locator_values));
  params.Set("waitTimeout", wait_timeout_ms);
  params.Set("clickCount", 1);
  params.Set("click_count", 1);
  return params;
}

}  // namespace

CdpTypeAction::CdpTypeAction(Profile* profile) : CdpActionBase(profile) {}
CdpTypeAction::~CdpTypeAction() = default;

bool CdpTypeAction::SetParams(const base::Value::Dict& params) {
  const std::string* text = params.FindString("text");
  if (!text) {
    return false;
  }
  text_to_type_ = *text;

  const base::Value::List* locators_list = params.FindList("locators");
  base::Value::List tmp_locators;
  if (!locators_list) {
    if (const base::Value::Dict* single = params.FindDict("locator")) {
      tmp_locators.Append(single->Clone());
      locators_list = &tmp_locators;
    }
  }
  if (locators_list) {
    const std::vector<dashboard::Locator> incoming =
        dashboard::ConvertLocators(*locators_list);
    locators_ = BuildTypingFocusLocators(incoming);
    frame_hint_locators_ = BuildFrameHintLocators(incoming);
  } else {
    locators_.clear();
    frame_hint_locators_.clear();
  }
  MaybeAppendFrameHintFromParams(params, &frame_hint_locators_);
  wait_timeout_ms_ = params.FindInt("waitTimeout").value_or(15000);

  // CDP-specific options
  use_cdp_input_ = params.FindBool("useCdpInput").value_or(IsCdpInputEnabled());

  base::Value::Dict serialized = params.Clone();
  serialized.Remove("frame_id");
  serialized.Remove("frameId");
  serialized.Remove("resource_path");
  base::Value::List locator_values;
  for (const auto& locator : locators_) {
    locator_values.Append(dashboard::LocatorToValue(locator));
  }
  serialized.Set("locators", std::move(locator_values));
  serialized.Set("text", text_to_type_);
  serialized.Set("waitTimeout", wait_timeout_ms_);
  serialized.Set("useCdpInput", use_cdp_input_);
  SetSerializedAction("type", std::move(serialized));
  return true;
}

void CdpTypeAction::Execute(content::WebContents* web_contents,
                            Callback callback) {
  if (!web_contents) {
    std::move(callback).Run(false, "No web contents");
    return;
  }
  if (!locators_.empty()) {
    const std::vector<dashboard::Locator> scoped_locators =
        BuildScopedTypingLocators(locators_, frame_hint_locators_);
    focus_click_action_ = std::make_unique<CdpClickAction>(profile_);
    focus_click_action_->SetExecutionPathLabel("cdp");
    if (focus_click_action_->SetParams(
            BuildTypeFocusClickParams(scoped_locators, wait_timeout_ms_))) {
      focus_click_action_->Execute(
          web_contents,
          base::BindOnce(&CdpTypeAction::OnFocusClickComplete,
                         weak_ptr_factory_.GetWeakPtr(), web_contents,
                         std::move(callback)));
      return;
    }
    focus_click_action_.reset();
  }
  if (locators_.empty()) {
    dashboard::DashboardDevToolsClient* client = GetDevToolsClient(web_contents);
    if (!client) {
      std::move(callback).Run(false, "CDP Client Unavailable");
      return;
    }
    if (!use_cdp_input_) {
      std::move(callback).Run(false, "CDP typing disabled");
      return;
    }
    TypeNextCharacter(client, web_contents, 0, std::move(callback));
    return;
  }

  checker_ = std::make_unique<ElementReadinessChecker>(web_contents);
  checker_->WaitForElementReadyAsync(
      BuildScopedTypingLocators(locators_, frame_hint_locators_),
      dashboard::SearchScope::kAll, wait_timeout_ms_,
      base::BindOnce(&CdpTypeAction::OnElementReady,
                     weak_ptr_factory_.GetWeakPtr(), web_contents,
                     std::move(callback)));
}

void CdpTypeAction::OnFocusClickComplete(content::WebContents* web_contents,
                                         Callback callback,
                                         bool success,
                                         const std::string& message) {
  focus_click_action_.reset();
  if (!success) {
    std::move(callback).Run(false, "Type focus click failed: " + message);
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&CdpTypeAction::StartTypingAfterFocusClick,
                     weak_ptr_factory_.GetWeakPtr(), web_contents,
                     std::move(callback)),
      base::Milliseconds(120));
}

void CdpTypeAction::StartTypingAfterFocusClick(content::WebContents* web_contents,
                                               Callback callback) {
  dashboard::DashboardDevToolsClient* client = GetDevToolsClient(web_contents);
  if (!client) {
    std::move(callback).Run(false, "CDP Client Unavailable");
    return;
  }
  if (!use_cdp_input_) {
    std::move(callback).Run(false, "CDP typing disabled");
    return;
  }
  TypeNextCharacter(client, web_contents, 0, std::move(callback));
}

void CdpTypeAction::InsertExactTextViaCdp(
    dashboard::DashboardDevToolsClient* client,
    Callback callback) {
  // Deprecated: typing uses CDP key events via TypeNextCharacter.
  if (!client) {
    std::move(callback).Run(false, "CDP Client Unavailable");
    return;
  }
  std::move(callback).Run(false, "InsertExactTextViaCdp retired; use TypeNextCharacter");
}

void CdpTypeAction::OnInsertExactTextComplete(Callback callback,
                                              bool success,
                                              const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, error);
    return;
  }
  std::move(callback).Run(true, "");
}

void CdpTypeAction::OnElementReady(
    content::WebContents* web_contents,
    Callback callback,
    bool ready,
    const dashboard::ResolvedElement& element,
    const std::string& error) {
  checker_.reset();
  if (!ready) {
    std::move(callback).Run(false, "Target not found: " + error);
    return;
  }

  dashboard::DashboardDevToolsClient* client = GetDevToolsClient(web_contents);
  if (!client) {
    std::move(callback).Run(false, "CDP Client Unavailable");
    return;
  }
  if (!use_cdp_input_) {
    std::move(callback).Run(false, "CDP typing disabled");
    return;
  }

  int target_x = element.center_x;
  int target_y = element.center_y;
  content::RenderFrameHost* frame =
      content::RenderFrameHost::FromID(element.frame_id);
  if (!frame) {
    frame = web_contents->GetPrimaryMainFrame();
  }
  if (frame && web_contents) {
    int root_x = target_x;
    int root_y = target_y;
    if (dashboard::ConvertFramePointToRootCoordinates(
            web_contents, frame, target_x, target_y, &root_x, &root_y)) {
      target_x = root_x;
      target_y = root_y;
    }
  } else if (!frame && !locators_.empty()) {
    std::move(callback).Run(false, "Target frame unavailable");
    return;
  }

  SessionState& session = GetSessionState(web_contents);
  base::Value::Dict move;
  move.Set("type", "mouseMoved");
  move.Set("x", target_x);
  move.Set("y", target_y);
  move.Set("button", "none");
  session.cursor_x = target_x;
  session.cursor_y = target_y;
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(move),
      base::BindOnce(&CdpTypeAction::OnFocusMoveComplete,
                     weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                     target_x, target_y, std::move(callback)));
}

void CdpTypeAction::OnFocusMoveComplete(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    int target_x,
    int target_y,
    Callback callback,
    bool success,
    const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, "Failed to move to CDP type target: " +
                                       error);
    return;
  }
  base::Value::Dict down;
  down.Set("type", "mousePressed");
  down.Set("button", "left");
  down.Set("x", target_x);
  down.Set("y", target_y);
  down.Set("buttons", 1);
  down.Set("clickCount", 1);
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(down),
      base::BindOnce(&CdpTypeAction::OnFocusMouseDownComplete,
                     weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                     target_x, target_y, std::move(callback)));
}

void CdpTypeAction::OnFocusMouseDownComplete(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    int target_x,
    int target_y,
    Callback callback,
    bool success,
    const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, "Failed to press CDP type target: " +
                                       error);
    return;
  }
  base::Value::Dict up;
  up.Set("type", "mouseReleased");
  up.Set("button", "left");
  up.Set("x", target_x);
  up.Set("y", target_y);
  up.Set("clickCount", 1);
  ExecuteCdpCommand(
      client, "Input.dispatchMouseEvent", std::move(up),
      base::BindOnce(&CdpTypeAction::OnFocusMouseUpComplete,
                     weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                     std::move(callback)));
}

void CdpTypeAction::OnFocusMouseUpComplete(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    Callback callback,
    bool success,
    const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, "Failed to focus CDP type target: " +
                                       error);
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&CdpTypeAction::TypeNextCharacter,
                     weak_ptr_factory_.GetWeakPtr(), client, web_contents, 0,
                     std::move(callback)),
      base::Milliseconds(120));
}

void CdpTypeAction::TypeNextCharacter(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    size_t index,
    Callback callback) {
  if (index >= text_to_type_.length()) {
    if (GetSessionState(web_contents).shift_active) {
      ToggleShift(client, web_contents, false);
    }
    std::move(callback).Run(true, "");
    return;
  }

  char c = text_to_type_[index];

  SessionState& session = GetSessionState(web_contents);
  const int consecutive_caps = CountUpcomingConsecutiveCaps(index);

  if (consecutive_caps >= 3 && !session.caps_lock_active) {
    ToggleCapsLock(client, web_contents, true);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CdpTypeAction::TypeNextCharacter,
                       weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                       index, std::move(callback)),
        base::Milliseconds(GetRandomRange(150, 300)));
    return;
  }

  if (session.caps_lock_active && std::islower(static_cast<unsigned char>(c))) {
    ToggleCapsLock(client, web_contents, false);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CdpTypeAction::TypeNextCharacter,
                       weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                       index, std::move(callback)),
        base::Milliseconds(GetRandomRange(150, 300)));
    return;
  }

  bool needs_shift = (std::isupper(static_cast<unsigned char>(c)) &&
                      !session.caps_lock_active) ||
                     IsSymbolRequiringShift(c);

  if (needs_shift && !session.shift_active) {
    ToggleShift(client, web_contents, true);
  }

  const CdpKeyEventSpec spec = BuildCdpKeyEventSpec(c);
  base::Value::Dict key_down;
  key_down.Set("type", spec.send_text ? "keyDown" : "rawKeyDown");
  key_down.Set("key", spec.key);
  if (!spec.code.empty()) {
    key_down.Set("code", spec.code);
  }
  if (spec.key_code > 0) {
    key_down.Set("windowsVirtualKeyCode", spec.key_code);
    key_down.Set("nativeVirtualKeyCode", spec.key_code);
  }
  if (spec.send_text && spec.text_char != 0) {
    const std::string text(1, spec.text_char);
    key_down.Set("text", text);
    key_down.Set("unmodifiedText", text);
  }
  if (GetSessionState(web_contents).shift_active) {
    key_down.Set("modifiers", 8);
  }
  ExecuteCdpCommand(
      client, "Input.dispatchKeyEvent", std::move(key_down),
      base::BindOnce(&CdpTypeAction::OnKeyDownComplete,
                     weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                     index, c, needs_shift, std::move(callback)));
}

void CdpTypeAction::OnKeyDownComplete(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    size_t index,
    char c,
    bool needs_shift,
    Callback callback,
    bool success,
    const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, "CDP keyDown failed: " + error);
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&CdpTypeAction::SendKeyUpAndContinue,
                     weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                     index, c, needs_shift, std::move(callback)),
      base::Milliseconds(GetRandomRange(40, 95)));
}

void CdpTypeAction::SendKeyUpAndContinue(
    dashboard::DashboardDevToolsClient* client,
    content::WebContents* web_contents,
    size_t index,
    char c,
    bool needs_shift,
    Callback callback) {
  base::Value::Dict key_up;
  key_up.Set("type", "keyUp");
  const CdpKeyEventSpec spec = BuildCdpKeyEventSpec(c);
  key_up.Set("key", spec.key);
  if (!spec.code.empty()) {
    key_up.Set("code", spec.code);
  }
  if (spec.key_code > 0) {
    key_up.Set("windowsVirtualKeyCode", spec.key_code);
    key_up.Set("nativeVirtualKeyCode", spec.key_code);
  }
  if (GetSessionState(web_contents).shift_active) {
    key_up.Set("modifiers", 8);
  }
  client->SendCommand("Input.dispatchKeyEvent", std::move(key_up));

  if (needs_shift && GetSessionState(web_contents).shift_active) {
    ToggleShift(client, web_contents, false);
  }

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&CdpTypeAction::TypeNextCharacter,
                     weak_ptr_factory_.GetWeakPtr(), client, web_contents,
                     index + 1, std::move(callback)),
      base::Milliseconds(GetInterKeyDelayMs(index)));
}

void CdpTypeAction::ToggleCapsLock(dashboard::DashboardDevToolsClient* client,
                                   content::WebContents* web_contents,
                                   bool active) {
  SessionState& session = GetSessionState(web_contents);
  if (session.caps_lock_active == active) {
    return;
  }

  base::Value::Dict down;
  down.Set("type", "rawKeyDown");
  down.Set("key", "CapsLock");
  down.Set("code", "CapsLock");
  client->SendCommand("Input.dispatchKeyEvent", std::move(down));

  base::Value::Dict up;
  up.Set("type", "keyUp");
  up.Set("key", "CapsLock");
  up.Set("code", "CapsLock");
  client->SendCommand("Input.dispatchKeyEvent", std::move(up));

  session.caps_lock_active = active;
}

void CdpTypeAction::ToggleShift(dashboard::DashboardDevToolsClient* client,
                                content::WebContents* web_contents,
                                bool active) {
  SessionState& session = GetSessionState(web_contents);
  if (session.shift_active == active) {
    return;
  }

  base::Value::Dict shift;
  shift.Set("type", active ? "rawKeyDown" : "keyUp");
  shift.Set("key", "Shift");
  shift.Set("code", "ShiftLeft");
  if (active) {
    shift.Set("modifiers", 8);
  }
  client->SendCommand("Input.dispatchKeyEvent", std::move(shift));
  session.shift_active = active;
}

int CdpTypeAction::CountUpcomingConsecutiveCaps(size_t index) const {
  int consecutive_caps = 0;
  for (size_t i = index; i < text_to_type_.size() && i < index + 4; ++i) {
    if (std::isupper(static_cast<unsigned char>(text_to_type_[i]))) {
      ++consecutive_caps;
    } else {
      break;
    }
  }
  return consecutive_caps;
}

bool CdpTypeAction::IsSymbolRequiringShift(char c) const {
  switch (c) {
    case '~':
    case '!':
    case '@':
    case '#':
    case '$':
    case '%':
    case '^':
    case '&':
    case '*':
    case '(':
    case ')':
    case '_':
    case '+':
    case '{':
    case '}':
    case '|':
    case ':':
    case '"':
    case '<':
    case '>':
    case '?':
      return true;
    default:
      return false;
  }
}

double CdpTypeAction::GetKeyboardDistance(char key1, char key2) const {
  const std::optional<KeyPosition> pos1 = LookupKeyboardPosition(key1);
  const std::optional<KeyPosition> pos2 = LookupKeyboardPosition(key2);
  if (!pos1.has_value() || !pos2.has_value()) {
    return 2.0;
  }

  const double dx = pos2->x - pos1->x;
  const double dy = (pos2->y - pos1->y) * 1.5;
  return std::sqrt((dx * dx) + (dy * dy));
}

int CdpTypeAction::GetInterKeyDelayMs(size_t index) const {
  if (index == 0 || index >= text_to_type_.size()) {
    return GetRandomRange(35, 90);
  }

  const double distance =
      GetKeyboardDistance(text_to_type_[index - 1], text_to_type_[index]);
  const double travel_time = distance * 25.0;
  const double hesitation = GetRandomGaussian(30.0, 15.0);
  const double total_delay = std::clamp(travel_time + hesitation, 20.0, 450.0);
  return static_cast<int>(std::round(total_delay));
}

std::string CdpTypeAction::GetDomCodeForChar(char c) const {
  const char key = NormalizeKeyboardKey(c);

  if (key >= 'a' && key <= 'z') {
    return "Key" + std::string(1, static_cast<char>(std::toupper(key)));
  }
  if (key >= '0' && key <= '9') {
    return "Digit" + std::string(1, key);
  }

  switch (key) {
    case '`':
      return "Backquote";
    case '-':
      return "Minus";
    case '=':
      return "Equal";
    case '[':
      return "BracketLeft";
    case ']':
      return "BracketRight";
    case '\\':
      return "Backslash";
    case ';':
      return "Semicolon";
    case '\'':
      return "Quote";
    case ',':
      return "Comma";
    case '.':
      return "Period";
    case '/':
      return "Slash";
    case ' ':
      return "Space";
    default:
      return "";
  }
}

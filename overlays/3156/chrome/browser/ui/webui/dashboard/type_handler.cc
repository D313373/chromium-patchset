#include "chrome/browser/ui/webui/dashboard/type_handler.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/webui/dashboard/click_handler.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace {

constexpr char kForceWindowActivationSwitch[] =
    "simulachrome-force-window-activation";

// US-QWERTY map aligned with chrome/renderer/actor/type_tool.cc so each
// expanded secret/variable character is dispatched as that exact key.
struct NativeKeyInfo {
  int windows_key_code = 0;
  std::string dom_code;
  char unmodified_char = 0;  // non-zero => requires Shift
};

bool LookupPrintableKey(char c, NativeKeyInfo* out) {
  if (!out) {
    return false;
  }
  const unsigned char uc = static_cast<unsigned char>(c);
  if (std::isalpha(uc)) {
    const char upper = static_cast<char>(std::toupper(uc));
    out->windows_key_code = ui::VKEY_A + (upper - 'A');
    out->dom_code = std::string("Key") + upper;
    if (std::isupper(uc)) {
      out->unmodified_char = static_cast<char>(std::tolower(uc));
    }
    return true;
  }
  if (std::isdigit(uc)) {
    out->windows_key_code = ui::VKEY_0 + (c - '0');
    out->dom_code = std::string("Digit") + c;
    return true;
  }

  switch (c) {
    case ' ':
      *out = {ui::VKEY_SPACE, "Space", 0};
      return true;
    case ')':
      *out = {ui::VKEY_0, "Digit0", '0'};
      return true;
    case '!':
      *out = {ui::VKEY_1, "Digit1", '1'};
      return true;
    case '@':
      *out = {ui::VKEY_2, "Digit2", '2'};
      return true;
    case '#':
      *out = {ui::VKEY_3, "Digit3", '3'};
      return true;
    case '$':
      *out = {ui::VKEY_4, "Digit4", '4'};
      return true;
    case '%':
      *out = {ui::VKEY_5, "Digit5", '5'};
      return true;
    case '^':
      *out = {ui::VKEY_6, "Digit6", '6'};
      return true;
    case '&':
      *out = {ui::VKEY_7, "Digit7", '7'};
      return true;
    case '*':
      *out = {ui::VKEY_8, "Digit8", '8'};
      return true;
    case '(':
      *out = {ui::VKEY_9, "Digit9", '9'};
      return true;
    case ';':
      *out = {ui::VKEY_OEM_1, "Semicolon", 0};
      return true;
    case ':':
      *out = {ui::VKEY_OEM_1, "Semicolon", ';'};
      return true;
    case '=':
      *out = {ui::VKEY_OEM_PLUS, "Equal", 0};
      return true;
    case '+':
      *out = {ui::VKEY_OEM_PLUS, "Equal", '='};
      return true;
    case ',':
      *out = {ui::VKEY_OEM_COMMA, "Comma", 0};
      return true;
    case '<':
      *out = {ui::VKEY_OEM_COMMA, "Comma", ','};
      return true;
    case '-':
      *out = {ui::VKEY_OEM_MINUS, "Minus", 0};
      return true;
    case '_':
      *out = {ui::VKEY_OEM_MINUS, "Minus", '-'};
      return true;
    case '.':
      *out = {ui::VKEY_OEM_PERIOD, "Period", 0};
      return true;
    case '>':
      *out = {ui::VKEY_OEM_PERIOD, "Period", '.'};
      return true;
    case '/':
      *out = {ui::VKEY_OEM_2, "Slash", 0};
      return true;
    case '?':
      *out = {ui::VKEY_OEM_2, "Slash", '/'};
      return true;
    case '`':
      *out = {ui::VKEY_OEM_3, "Backquote", 0};
      return true;
    case '~':
      *out = {ui::VKEY_OEM_3, "Backquote", '`'};
      return true;
    case '[':
      *out = {ui::VKEY_OEM_4, "BracketLeft", 0};
      return true;
    case '{':
      *out = {ui::VKEY_OEM_4, "BracketLeft", '['};
      return true;
    case '\\':
      *out = {ui::VKEY_OEM_5, "Backslash", 0};
      return true;
    case '|':
      *out = {ui::VKEY_OEM_5, "Backslash", '\\'};
      return true;
    case ']':
      *out = {ui::VKEY_OEM_6, "BracketRight", 0};
      return true;
    case '}':
      *out = {ui::VKEY_OEM_6, "BracketRight", ']'};
      return true;
    case '\'':
      *out = {ui::VKEY_OEM_7, "Quote", 0};
      return true;
    case '"':
      *out = {ui::VKEY_OEM_7, "Quote", '\''};
      return true;
    default:
      return false;
  }
}

void ActivateWebContentsForNativeInput(content::WebContents* web_contents,
                                       bool force_activation) {
  if (!web_contents) {
    return;
  }
  force_activation =
      force_activation ||
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          kForceWindowActivationSwitch);
  if (!force_activation) {
    return;
  }

  web_contents->Focus();
  Browser* browser = chrome::FindBrowserWithTab(web_contents);
  if (!browser) {
    return;
  }
  TabStripModel* tabs = browser->tab_strip_model();
  if (tabs) {
    const int index = tabs->GetIndexOfWebContents(web_contents);
    if (index >= 0 && tabs->GetActiveWebContents() != web_contents) {
      tabs->ActivateTabAt(index);
    }
  }

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (browser_view && browser_view->GetWidget()) {
    browser_view->GetWidget()->Activate();
    return;
  }
  if (browser->window()) {
    browser->window()->Activate();
  }
}

}  // namespace

TypeHandler::TypeHandler(content::WebContents* web_contents)
    : web_contents_(web_contents) {}

TypeHandler::~TypeHandler() = default;

dashboard::ResolvedElement TypeHandler::BuildTypingElementSnapshot(
    const dashboard::ResolvedElement& element) const {
  dashboard::ResolvedElement snapshot;
  snapshot.center_x = element.center_x;
  snapshot.center_y = element.center_y;
  snapshot.left = element.left;
  snapshot.top = element.top;
  snapshot.width = element.width;
  snapshot.height = element.height;
  snapshot.frame_id = element.frame_id;
  snapshot.text = element.text;
  snapshot.accessibility_name = element.accessibility_name;
  snapshot.role = element.role;
  snapshot.id_attr = element.id_attr;
  snapshot.class_attr = element.class_attr;
  snapshot.name_attr = element.name_attr;
  snapshot.css_selector = element.css_selector;
  snapshot.xpath = element.xpath;
  snapshot.shadow_chain_selector = element.shadow_chain_selector;
  snapshot.resource_path = element.resource_path;
  snapshot.inner_html = element.inner_html;
  snapshot.href = element.href;
  snapshot.src = element.src;
  snapshot.value = element.value;
  snapshot.visible = element.visible;
  snapshot.clickable = element.clickable;
  snapshot.focusable = element.focusable;
  return snapshot;
}

void TypeHandler::ExecuteTypeAsync(
    const std::vector<dashboard::Locator>& locators,
    const std::string& text_to_type,
    int wait_timeout_ms,
    TypeCallback callback) {
  if (!web_contents_) {
    std::move(callback).Run(false, dashboard::ResolvedElement(),
                            "No web contents");
    return;
  }

  // TypeAction often focuses first, then calls us with empty locators.
  // Native key-by-key insert — no Cmd+A / backspace clear.
  if (locators.empty()) {
    ActivateWebContentsForNativeInput(web_contents_, /*force_activation=*/false);
    const size_t bang_count = static_cast<size_t>(
        std::count(text_to_type.begin(), text_to_type.end(), '!'));
    LOG(INFO) << "[TypeHandler] native type start len=" << text_to_type.size()
              << " bang_count=" << bang_count
              << " build_tag=native_keys_no_select";
    TypeCharacterNativeAsync(dashboard::ResolvedElement(), text_to_type, 0,
                             std::move(callback));
    return;
  }

  checker_ = std::make_unique<ElementReadinessChecker>(web_contents_);
  checker_->WaitForElementReadyAsync(
      locators, dashboard::SearchScope::kAll, wait_timeout_ms,
      base::BindOnce(&TypeHandler::OnTypeTargetReady,
                     weak_ptr_factory_.GetWeakPtr(), text_to_type,
                     std::move(callback)));
}

void TypeHandler::OnTypeTargetReady(const std::string& text,
                                    TypeCallback callback,
                                    bool ready,
                                    const dashboard::ResolvedElement& element,
                                    const std::string& error) {
  checker_.reset();
  if (!ready) {
    std::move(callback).Run(false, element, "Target not found: " + error);
    return;
  }
  ContinueWithNativeTyping(BuildTypingElementSnapshot(element), text,
                           std::move(callback));
}

void TypeHandler::ContinueWithNativeTyping(
    dashboard::ResolvedElement element,
    const std::string& text,
    TypeCallback callback) {
  if (!web_contents_) {
    std::move(callback).Run(false, element, "No web contents");
    return;
  }

  ActivateWebContentsForNativeInput(web_contents_, /*force_activation=*/false);

  content::RenderFrameHost* target_rfh = ResolveTypingTargetFrame(element);
  if (!target_rfh) {
    std::move(callback).Run(false, element, "No render frame for type target");
    return;
  }

  LOG(INFO) << "[TypeHandler] Starting native type focus click target_frame="
            << target_rfh << " focused_frame_before="
            << web_contents_->GetFocusedFrame();

  dashboard::ClickHandler handler(web_contents_);
  std::string diagnostics;
  handler.ExecuteClickAtInFrame(
      target_rfh, element.center_x, element.center_y, false, 1,
      /*force_activation=*/false,
      /*preserve_release_point=*/true, &diagnostics,
      base::BindOnce(&TypeHandler::OnFocusClickDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(element), text,
                     std::move(callback)));
}

void TypeHandler::OnFocusClickDone(dashboard::ResolvedElement element,
                                   std::string text,
                                   TypeCallback callback,
                                   bool success) {
  if (!success) {
    std::move(callback).Run(false, element, "Failed to focus type target");
    return;
  }

  const size_t bang_count =
      static_cast<size_t>(std::count(text.begin(), text.end(), '!'));
  LOG(INFO) << "[TypeHandler] native type start len=" << text.size()
            << " bang_count=" << bang_count
            << " build_tag=native_keys_no_select";

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&TypeHandler::TypeCharacterNativeAsync,
                     weak_ptr_factory_.GetWeakPtr(), std::move(element),
                     std::move(text), 0, std::move(callback)),
      base::Milliseconds(80));
}

content::RenderFrameHost* TypeHandler::ResolveTypingTargetFrame(
    const dashboard::ResolvedElement& element) const {
  if (!web_contents_) {
    return nullptr;
  }
  if (element.frame_id.frame_routing_id != -1) {
    if (content::RenderFrameHost* rfh =
            content::RenderFrameHost::FromID(element.frame_id)) {
      return rfh;
    }
  }
  if (content::RenderFrameHost* focused = web_contents_->GetFocusedFrame()) {
    return focused;
  }
  return web_contents_->GetPrimaryMainFrame();
}

void TypeHandler::TypeCharacterNativeAsync(
    dashboard::ResolvedElement element,
    const std::string& text,
    size_t char_index,
    TypeCallback callback) {
  if (!web_contents_) {
    std::move(callback).Run(false, element, "No web contents");
    return;
  }
  if (char_index >= text.size()) {
    std::move(callback).Run(true, element, "");
    return;
  }

  content::RenderFrameHost* rfh = ResolveTypingTargetFrame(element);
  if (!rfh) {
    rfh = web_contents_->GetFocusedFrame();
  }
  if (!rfh) {
    rfh = web_contents_->GetPrimaryMainFrame();
  }
  content::RenderFrameHost* focused_rfh = web_contents_->GetFocusedFrame();
  if (focused_rfh && focused_rfh != rfh) {
    LOG(WARNING) << "[TypeHandler] Focus drift before native key dispatch "
                 << "expected=" << rfh << " focused=" << focused_rfh;
    rfh = focused_rfh;
  }

  content::RenderWidgetHost* rwh = rfh ? rfh->GetRenderWidgetHost() : nullptr;
  content::RenderWidgetHostView* rwhv = rwh ? rwh->GetView() : nullptr;
  if (!rwh || !rwhv || !rwhv->GetNativeView()) {
    std::move(callback).Run(false, element, "No render widget for type");
    return;
  }

  if (!DispatchNativeKeyPress(rwh, rwhv, text[char_index])) {
    std::move(callback).Run(
        false, element,
        "Failed to dispatch native key for character index " +
            std::to_string(char_index));
    return;
  }

  const int delay_ms = 25 + static_cast<int>(base::RandGenerator(35));
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&TypeHandler::TypeCharacterNativeAsync,
                     weak_ptr_factory_.GetWeakPtr(), std::move(element), text,
                     char_index + 1, std::move(callback)),
      base::Milliseconds(delay_ms));
}

bool TypeHandler::DispatchNativeKeyPress(content::RenderWidgetHost* rwh,
                                         content::RenderWidgetHostView* rwhv,
                                         char c) const {
  if (!rwh || !rwhv || !rwhv->GetNativeView()) {
    return false;
  }

  int windows_key_code = 0;
  int native_key_code = 0;
  int modifiers = blink::WebInputEvent::kNoModifiers;
  bool send_char = false;
  char text_char = 0;
  char unmodified_char = 0;
  std::string dom_code;
  std::string dom_key;

  if (c == '\n' || c == '\r') {
    windows_key_code = ui::VKEY_RETURN;
    dom_code = "Enter";
    dom_key = "Enter";
    send_char = true;
    text_char = '\r';
    unmodified_char = '\r';
  } else if (c == '\t') {
    windows_key_code = ui::VKEY_TAB;
    dom_code = "Tab";
    dom_key = "Tab";
  } else if (c == '\b') {
    windows_key_code = ui::VKEY_BACK;
    dom_code = "Backspace";
    dom_key = "Backspace";
  } else {
    NativeKeyInfo info;
    if (!LookupPrintableKey(c, &info)) {
      LOG(WARNING) << "[TypeHandler] Unmapped character code="
                   << static_cast<int>(static_cast<unsigned char>(c));
      return false;
    }
    windows_key_code = info.windows_key_code;
    dom_code = info.dom_code;
    send_char = true;
    text_char = c;
    if (info.unmodified_char != 0) {
      modifiers = blink::WebInputEvent::kShiftKey;
      unmodified_char = info.unmodified_char;
    } else {
      unmodified_char = c;
    }
    dom_key.assign(1, c);
  }

  native_key_code = windows_key_code;
  if (!dom_code.empty()) {
    const ui::DomCode code =
        ui::KeycodeConverter::CodeStringToDomCode(dom_code);
    if (code != ui::DomCode::NONE) {
      native_key_code = ui::KeycodeConverter::DomCodeToNativeKeycode(code);
    }
  }

  auto forward = [&](blink::WebInputEvent::Type type) {
    blink::WebKeyboardEvent event(type, modifiers, base::TimeTicks::Now());
    event.windows_key_code = windows_key_code;
    event.native_key_code = native_key_code;
    if (!dom_code.empty()) {
      event.dom_code = static_cast<int>(
          ui::KeycodeConverter::CodeStringToDomCode(dom_code));
    }
    if (!dom_key.empty()) {
      event.dom_key = ui::KeycodeConverter::KeyStringToDomKey(dom_key);
    }
    // Text on RawKeyDown/Char/KeyUp so Mac does not coerce to FlagsChanged,
    // and unmodified_text is the physical key (e.g. '1' for '!').
    if (text_char != 0) {
      event.text[0] = text_char;
      event.unmodified_text[0] = unmodified_char ? unmodified_char : text_char;
    }
    input::NativeWebKeyboardEvent native_event(event, rwhv->GetNativeView());
    rwh->ForwardKeyboardEvent(native_event);
  };

  forward(blink::WebInputEvent::Type::kRawKeyDown);
  if (send_char) {
    forward(blink::WebInputEvent::Type::kChar);
  }
  forward(blink::WebInputEvent::Type::kKeyUp);
  return true;
}

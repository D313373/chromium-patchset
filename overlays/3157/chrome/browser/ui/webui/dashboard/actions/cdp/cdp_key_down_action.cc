#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_key_down_action.h"

#include <utility>

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "content/public/browser/web_contents.h"

namespace {

int ComputeKeyModifiers(const std::vector<std::string>& modifiers) {
  int value = 0;
  for (const auto& modifier : modifiers) {
    if (modifier == "ctrl") {
      value |= 2;
    }
    if (modifier == "alt") {
      value |= 1;
    }
    if (modifier == "shift") {
      value |= 8;
    }
    if (modifier == "meta") {
      value |= 4;
    }
  }
  return value;
}

int ComputeVirtualKeyCode(const std::string& key, int explicit_key_code) {
  if (explicit_key_code != 0) {
    return explicit_key_code;
  }
  if (key.length() == 1) {
    return static_cast<int>(key[0]);
  }
  if (key == "Enter" || key == "Return") {
    return 13;
  }
  if (key == "Tab") {
    return 9;
  }
  if (key == "Escape") {
    return 27;
  }
  if (key == "Backspace") {
    return 8;
  }
  if (key == "Delete") {
    return 46;
  }
  if (key == "ArrowUp") {
    return 38;
  }
  if (key == "ArrowDown") {
    return 40;
  }
  if (key == "ArrowLeft") {
    return 37;
  }
  if (key == "ArrowRight") {
    return 39;
  }
  if (key == "Home") {
    return 36;
  }
  if (key == "End") {
    return 35;
  }
  if (key == "PageUp") {
    return 33;
  }
  if (key == "PageDown") {
    return 34;
  }
  return 0;
}

}  // namespace

CdpKeyDownAction::CdpKeyDownAction(Profile* profile) : CdpActionBase(profile) {}

CdpKeyDownAction::~CdpKeyDownAction() = default;

bool CdpKeyDownAction::SetParams(const base::Value::Dict& params) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    return false;
  }
  key_ = *key;

  const base::Value::List* modifiers = params.FindList("modifiers");
  if (modifiers) {
    modifiers_.clear();
    for (const auto& modifier : *modifiers) {
      if (modifier.is_string()) {
        modifiers_.push_back(modifier.GetString());
      }
    }
  }

  key_code_ = params.FindInt("keyCode").value_or(0);

  // CDP-specific options
  use_cdp_input_ = params.FindBool("useCdpInput").value_or(IsCdpInputEnabled());

  base::Value::Dict params_dict = params.Clone();
  params_dict.Set("key", key_);
  base::Value::List modifier_values;
  for (const auto& modifier : modifiers_) {
    modifier_values.Append(modifier);
  }
  params_dict.Set("modifiers", std::move(modifier_values));
  params_dict.Set("keyCode", key_code_);
  params_dict.Set("useCdpInput", use_cdp_input_);

  SetSerializedAction("key_down", std::move(params_dict));
  return true;
}

void CdpKeyDownAction::Execute(content::WebContents* web_contents,
                               Callback callback) {
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[CdpKeyDownAction] Starting key down for key='" << key_
              << "' modifiers=" << modifiers_.size() << " keyCode=" << key_code_
              << " cdp_input=" << use_cdp_input_;
  }

  if (!web_contents) {
    std::move(callback).Run(false, "No web contents");
    return;
  }

  dashboard::DashboardDevToolsClient* client = GetDevToolsClient(web_contents);
  if (!client) {
    std::move(callback).Run(false, "DevTools client not available");
    return;
  }

  SessionState& session = GetSessionState(web_contents);
  if (key_ == "Shift") {
    session.shift_active = true;
  } else if (key_ == "CapsLock") {
    session.caps_lock_active = !session.caps_lock_active;
  }

  if (!use_cdp_input_) {
    std::move(callback).Run(false,
                            "CDP keydown not implemented without CDP input");
    return;
  }

  const int modifiers = ComputeKeyModifiers(modifiers_);
  const int key_code = ComputeVirtualKeyCode(key_, key_code_);
  const bool is_enter_key = key_ == "Enter" || key_ == "Return";
  const bool is_printable_single_char =
      key_.length() == 1 &&
      static_cast<unsigned char>(key_[0]) >= 32 &&
      static_cast<unsigned char>(key_[0]) <= 126;
  const bool send_text = is_printable_single_char || is_enter_key;

  // Send keyDown event via CDP
  base::Value::Dict key_params;
  key_params.Set("type", send_text ? "keyDown" : "rawKeyDown");
  key_params.Set("key", key_);
  key_params.Set("code", is_enter_key ? "Enter" : key_);
  if (key_code > 0) {
    key_params.Set("windowsVirtualKeyCode", key_code);
    key_params.Set("nativeVirtualKeyCode", key_code);
  }
  if (send_text) {
    const std::string text = is_enter_key ? std::string("\r") : key_;
    key_params.Set("text", text);
    key_params.Set("unmodifiedText", text);
  }
  if (modifiers > 0) {
    key_params.Set("modifiers", modifiers);
  }

  ExecuteCdpCommand(client, "Input.dispatchKeyEvent", std::move(key_params),
                    base::BindOnce(&CdpKeyDownAction::OnCdpKeyDownComplete,
                                   weak_ptr_factory_.GetWeakPtr(),
                                   web_contents, client, std::move(callback)));
}

void CdpKeyDownAction::OnCdpKeyDownComplete(
    content::WebContents* web_contents,
    dashboard::DashboardDevToolsClient* client,
    Callback callback,
    bool success,
    const std::string& error) {
  if (!success) {
    std::move(callback).Run(false, "CDP keydown failed: " + error);
    return;
  }
  if (!web_contents || !client) {
    std::move(callback).Run(false, "DevTools client not available");
    return;
  }

  const bool is_enter_key = key_ == "Enter" || key_ == "Return";
  base::Value::Dict key_up_params;
  key_up_params.Set("type", "keyUp");
  key_up_params.Set("key", key_);
  key_up_params.Set("code", is_enter_key ? "Enter" : key_);
  const int key_code = ComputeVirtualKeyCode(key_, key_code_);
  if (key_code > 0) {
    key_up_params.Set("windowsVirtualKeyCode", key_code);
    key_up_params.Set("nativeVirtualKeyCode", key_code);
  }
  const int modifiers = ComputeKeyModifiers(modifiers_);
  if (modifiers > 0) {
    key_up_params.Set("modifiers", modifiers);
  }

  ExecuteCdpCommand(
      client, "Input.dispatchKeyEvent", std::move(key_up_params),
      base::BindOnce(&CdpKeyDownAction::OnCdpComplete,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback),
                     "CDP keydown"));
}

void CdpKeyDownAction::OnCdpComplete(Callback callback,
                                     const std::string& operation,
                                     bool success,
                                     const std::string& error) {
  std::move(callback).Run(success, success ? (operation + " completed")
                                           : (operation + " failed: " + error));
}

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/actions/authenticate_action.h"

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_factory.h"
#include "chrome/browser/ui/webui/dashboard/actions/history_nav_action.h"
#include "content/public/browser/web_contents.h"

namespace {

std::string Field(const base::Value::Dict& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

bool Enabled(const base::Value::Dict& dict) {
  return dict.FindBool("enabled").value_or(true);
}

std::string ReadAdaptField(const std::string& message, const char* key) {
  const std::string token = std::string(key) + "=";
  const size_t at = message.find(token);
  if (at == std::string::npos) {
    return std::string();
  }
  size_t begin = at + token.size();
  while (begin < message.size() &&
         base::IsAsciiWhitespace(message[begin])) {
    ++begin;
  }
  size_t end = begin;
  while (end < message.size() &&
         (base::IsAsciiAlphaNumeric(message[end]) || message[end] == '_')) {
    ++end;
  }
  return message.substr(begin, end - begin);
}

std::string CaptchaHint(const std::string& brand, const std::string& challenge) {
  if (brand == "recaptcha") {
    if (challenge == "audio") {
      return "Use the audio challenge and enter the words you hear.";
    }
    if (challenge == "image_grid") {
      return "Select every tile that matches the caption, then confirm.";
    }
    return "Click the checkbox. If an image challenge opens, solve that challenge.";
  }
  if (brand == "hcaptcha") {
    if (challenge == "image_grid") {
      return "Select every tile that matches the caption, then confirm.";
    }
    return "Click the checkbox. If an image challenge opens, solve that challenge.";
  }
  if (brand == "turnstile") {
    if (challenge == "managed") {
      return "Wait for the managed check to finish. Click it if it asks.";
    }
    if (challenge == "interactive") {
      return "Complete the interactive challenge shown in the widget.";
    }
    return "Click the Turnstile checkbox and wait for it to finish.";
  }
  if (brand == "funcaptcha") {
    if (challenge == "rotate") {
      return "Rotate the object until it matches the instruction, then submit.";
    }
    return "Complete the tile or rotation challenge, then submit.";
  }
  if (brand == "geetest") {
    if (challenge == "slide") {
      return "Drag the slider until the puzzle piece fits.";
    }
    if (challenge == "icon_click") {
      return "Click the icons in the requested order.";
    }
    return "Complete the GeeTest challenge shown on the page.";
  }
  if (brand == "iconcaptcha") {
    return "Click the icon that matches the instruction.";
  }
  return "Complete the captcha challenge that is visible.";
}

}  // namespace

AuthenticateAction::AuthenticateAction(Profile* profile)
    : dashboard::actions::Action(profile) {}

AuthenticateAction::~AuthenticateAction() = default;

bool AuthenticateAction::SetParams(const base::Value::Dict& params) {
  const base::Value::List* methods = params.FindList("methods");
  if (!methods || methods->empty()) {
    return false;
  }
  params_ = params.Clone();
  SetSerializedAction("authenticate", params_.Clone());
  return true;
}

std::unique_ptr<dashboard::actions::Action> AuthenticateAction::MakeGlic(
    const std::string& prompt,
    bool stay_on_tab) {
  base::Value::Dict data;
  data.Set("action", "glic_prompt");
  base::Value::Dict glic_params;
  glic_params.Set("prompt", prompt);
  glic_params.Set("stayOnCurrentTab", stay_on_tab);
  glic_params.Set("skipAutoSave", true);
  data.Set("params", std::move(glic_params));
  dashboard::actions::ActionFactory factory(profile_);
  factory.SetAgentMode(IsAgentModeRun());
  factory.SetForceNativeExecution(IsForceNativeRun());
  return factory.CreateAction(data);
}

std::unique_ptr<dashboard::actions::Action> AuthenticateAction::MakeType(
    const std::string& text) {
  base::Value::Dict data;
  data.Set("action", "type");
  base::Value::Dict type_params;
  type_params.Set("text", text);
  data.Set("params", std::move(type_params));
  dashboard::actions::ActionFactory factory(profile_);
  factory.SetAgentMode(IsAgentModeRun());
  factory.SetForceNativeExecution(IsForceNativeRun());
  return factory.CreateAction(data);
}

std::string AuthenticateAction::CaptchaPrompt(
    const base::Value::Dict& method) const {
  const base::Value::Dict* captcha = method.FindDict("captcha");
  const std::string brand = captcha ? Field(*captcha, "brand") : std::string();
  const std::string challenge =
      captcha ? Field(*captcha, "challenge") : std::string();
  return "Solve only the captcha on this page. Do not click Log In and do not "
         "type a password.\nExpected brand: " +
         brand + ".\nExpected challenge: " + challenge + ".\n" +
         CaptchaHint(brand, challenge) +
         "\nIf the widget is a different brand or challenge, stop and finish "
         "with this exact summary and no other actions:\n"
         "ADAPT_CAPTCHA brand=<id> challenge=<id>\n"
         "Brand ids: recaptcha, hcaptcha, turnstile, funcaptcha, geetest, "
         "iconcaptcha.";
}

bool AuthenticateAction::BuildPlan(const base::Value::Dict& method,
                                   std::string* error) {
  planned_kinds_.clear();
  planned_actions_.clear();
  const base::Value::List* steps = method.FindList("steps");
  const bool custom = method.FindBool("custom").value_or(false);
  if (custom) {
    const base::Value::List* actions = method.FindList("actions");
    if (!actions || actions->empty()) {
      *error = "Custom login method has no actions.";
      return false;
    }
    dashboard::actions::ActionFactory factory(profile_);
    factory.SetAgentMode(IsAgentModeRun());
    factory.SetForceNativeExecution(IsForceNativeRun());
    for (const base::Value& value : *actions) {
      const base::Value::Dict* dict = value.GetIfDict();
      if (!dict || dict->FindBool("__draft").value_or(false)) {
        continue;
      }
      planned_kinds_.push_back("custom");
      planned_actions_.push_back(factory.CreateAction(*dict));
    }
    if (planned_actions_.empty()) {
      *error = "Custom login method has no actions.";
      return false;
    }
    for (const auto& action : planned_actions_) {
      if (!action) {
        *error = "Could not build a login step.";
        return false;
      }
    }
    return true;
  }
  if (!steps) {
    *error = "Login method has no steps.";
    return false;
  }
  const std::string label = Field(method, "label");
  const base::Value::Dict* captcha = method.FindDict("captcha");
  const bool captcha_on = captcha && Enabled(*captcha);
  std::string host;
  if (web_contents_) {
    host = web_contents_->GetLastCommittedURL().host();
  }
  if (host.empty()) {
    host = "this site";
  }

  auto add_step = [&](const std::string& kind,
                      std::unique_ptr<dashboard::actions::Action> action) {
    planned_kinds_.push_back(kind);
    planned_actions_.push_back(std::move(action));
  };
  auto add_glic = [&](const std::string& kind, const std::string& prompt,
                      bool stay) {
    add_step(kind, MakeGlic(prompt, stay));
  };

  for (const base::Value& value : *steps) {
    const base::Value::Dict* step = value.GetIfDict();
    if (!step || !Enabled(*step)) {
      continue;
    }
    const std::string kind = Field(*step, "kind");
    if (kind == "open_menu") {
      add_glic(kind,
               "Open the login menu or modal if it is not already open. Stop "
               "when the login choices are visible. Do not choose a method "
               "and do not type.",
               true);
    } else if (kind == "username" || kind == "password") {
      const std::string token = Field(*step, "token");
      if (token == "third_party") {
        continue;
      }
      if (token.empty()) {
        *error = kind == "username" ? "Select a username or email key."
                                    : "Select a password key.";
        return false;
      }
      const char* focus = kind == "username" ?
          "Click the username or email field. Do not type and do not submit." :
          "Click the password field. Do not type and do not submit.";
      add_glic(std::string("focus_") + kind, focus, true);
      add_step(std::string("type_") + kind, MakeType(token));
    } else if (kind == "captcha") {
      if (!captcha_on) {
        continue;
      }
      add_glic("captcha", CaptchaPrompt(method), true);
    } else if (kind == "two_factor") {
      const std::string email = Field(*step, "emailToken");
      if (email.empty()) {
        *error = "Select the inbox email for the 2FA code.";
        return false;
      }
      add_glic(
          kind,
          "A one-time code is required. Open a tab to the inbox for " + email +
              ". Refresh the inbox and find a 2FA or verification code from " +
              host +
              ". Return to the login tab. Type only that code into the 2FA "
              "field with a type action, then press Enter. Do not type the "
              "account password.",
          false);
    } else if (kind == "provider_click") {
      add_glic(kind,
               "Click the control for " + label +
                   ". Do not type credentials. Stop when that login flow has "
                   "started.",
               true);
    } else if (kind == "submit") {
      add_glic(kind,
               "If a login, continue, or submit control is still on screen, "
               "click it or submit the form. If the login form is already "
               "gone, stop without clicking anything else.",
               true);
    }
  }
  if (planned_actions_.empty()) {
    *error = "Login method has no enabled steps.";
    return false;
  }
  for (const auto& action : planned_actions_) {
    if (!action) {
      *error = "Could not build a login step.";
      return false;
    }
  }
  return true;
}

bool AuthenticateAction::ApplyCaptchaAdapt(const std::string& message) {
  if (message.find("ADAPT_CAPTCHA") == std::string::npos) {
    return false;
  }
  const std::string brand = ReadAdaptField(message, "brand");
  const std::string challenge = ReadAdaptField(message, "challenge");
  if (brand.empty() || challenge.empty() || adapt_count_ >= 2) {
    return false;
  }
  base::Value::List* methods = params_.FindList("methods");
  if (!methods || method_index_ >= methods->size()) {
    return false;
  }
  base::Value::Dict* method = (*methods)[method_index_].GetIfDict();
  if (!method) {
    return false;
  }
  base::Value::Dict* captcha = method->FindDict("captcha");
  if (!captcha) {
    method->Set("captcha", base::Value::Dict());
    captcha = method->FindDict("captcha");
  }
  captcha->Set("enabled", true);
  captcha->Set("brand", brand);
  captcha->Set("challenge", challenge);
  SetSerializedAction("authenticate", params_.Clone());
  if (step_index_ >= planned_actions_.size()) {
    return false;
  }
  planned_actions_[step_index_] = MakeGlic(CaptchaPrompt(*method), true);
  if (!planned_actions_[step_index_]) {
    return false;
  }
  ++adapt_count_;
  return true;
}

void AuthenticateAction::Execute(content::WebContents* web_contents,
                                 Callback callback) {
  web_contents_ = web_contents;
  callback_ = std::move(callback);
  method_index_ = 0;
  if (!web_contents_) {
    Finish(false, "No web contents");
    return;
  }
  StartMethod();
}

void AuthenticateAction::StartMethod() {
  phase_ = Phase::kSteps;
  bridge_action_.reset();
  base::Value::List* methods = params_.FindList("methods");
  if (!methods) {
    Finish(false, "Authenticate has no login methods.");
    return;
  }
  while (method_index_ < methods->size()) {
    const base::Value::Dict* method = (*methods)[method_index_].GetIfDict();
    if (method && Enabled(*method)) {
      break;
    }
    ++method_index_;
  }
  if (method_index_ >= methods->size()) {
    Finish(false, "Every enabled login method failed.");
    return;
  }
  const base::Value::Dict* method = (*methods)[method_index_].GetIfDict();
  std::string error;
  ran_step_ = false;
  step_index_ = 0;
  adapt_count_ = 0;
  if (!BuildPlan(*method, &error)) {
    BeginFallback();
    return;
  }
  RunStep();
}

void AuthenticateAction::RunStep() {
  if (step_index_ >= planned_actions_.size()) {
    Finish(true, "Login method completed.");
    return;
  }
  dashboard::actions::Action* action = planned_actions_[step_index_].get();
  if (!action) {
    BeginFallback();
    return;
  }
  action->Execute(web_contents_,
                  base::BindOnce(&AuthenticateAction::OnStepDone,
                                 weak_factory_.GetWeakPtr()));
}

void AuthenticateAction::OnStepDone(bool success, const std::string& message) {
  ran_step_ = true;
  step_succeeded_ = success;
  step_message_ = message;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&AuthenticateAction::ContinueAfterStep,
                                weak_factory_.GetWeakPtr()));
}

void AuthenticateAction::ContinueAfterStep() {
  if (step_index_ < planned_kinds_.size() &&
      planned_kinds_[step_index_] == "captcha" &&
      ApplyCaptchaAdapt(step_message_)) {
    RunStep();
    return;
  }
  if (!step_succeeded_) {
    BeginFallback();
    return;
  }
  ++step_index_;
  adapt_count_ = 0;
  RunStep();
}

void AuthenticateAction::BeginFallback() {
  ++method_index_;
  base::Value::List* methods = params_.FindList("methods");
  if (!methods || method_index_ >= methods->size()) {
    Finish(false, "Every enabled login method failed.");
    return;
  }
  if (!ran_step_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&AuthenticateAction::StartMethod,
                                  weak_factory_.GetWeakPtr()));
    return;
  }
  phase_ = Phase::kBack;
  bridge_action_ = std::make_unique<dashboard::actions::HistoryNavAction>(
      profile_, dashboard::actions::HistoryNavAction::Direction::kBack);
  bridge_action_->SetParams(base::Value::Dict());
  bridge_action_->Execute(
      web_contents_, base::BindOnce(&AuthenticateAction::OnReturnDone,
                                    weak_factory_.GetWeakPtr()));
}

void AuthenticateAction::RunReturn() {
  phase_ = Phase::kReturnPrompt;
  bridge_action_ = MakeGlic(
      "The login method failed. Return to the login menu. Prefer the Back "
      "button or a control that returns to the login choices. Do not start "
      "another login method and do not type credentials.",
      true);
  if (!bridge_action_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&AuthenticateAction::StartMethod,
                                  weak_factory_.GetWeakPtr()));
    return;
  }
  bridge_action_->Execute(
      web_contents_, base::BindOnce(&AuthenticateAction::OnReturnDone,
                                    weak_factory_.GetWeakPtr()));
}

void AuthenticateAction::OnReturnDone(bool success,
                                       const std::string& message) {
  (void)message;
  if (phase_ == Phase::kBack && !success) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&AuthenticateAction::RunReturn,
                                  weak_factory_.GetWeakPtr()));
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&AuthenticateAction::StartMethod,
                                weak_factory_.GetWeakPtr()));
}

void AuthenticateAction::Finish(bool success, const std::string& message) {
  if (!callback_) {
    return;
  }
  planned_kinds_.clear();
  planned_actions_.clear();
  bridge_action_.reset();
  std::move(callback_).Run(success, message);
}

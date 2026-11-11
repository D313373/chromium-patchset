// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_AUTHENTICATE_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_AUTHENTICATE_ACTION_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"

namespace content {
class WebContents;
}

// Runs login methods from left to right. Disabled methods are skipped. A
// failed method returns to the login menu, then the next enabled method runs.
class AuthenticateAction : public dashboard::actions::Action {
 public:
  explicit AuthenticateAction(Profile* profile);
  ~AuthenticateAction() override;

  void Execute(content::WebContents* web_contents, Callback callback) override;
  bool SetParams(const base::Value::Dict& params) override;

 private:
  enum class Phase { kSteps, kBack, kReturnPrompt };

  void StartMethod();
  void RunStep();
  void OnStepDone(bool success, const std::string& message);
  void ContinueAfterStep();
  void BeginFallback();
  void RunReturn();
  void OnReturnDone(bool success, const std::string& message);
  void Finish(bool success, const std::string& message);
  bool BuildPlan(const base::Value::Dict& method, std::string* error);
  bool ApplyCaptchaAdapt(const std::string& message);
  std::unique_ptr<dashboard::actions::Action> MakeGlic(
      const std::string& prompt,
      bool stay_on_tab);
  std::unique_ptr<dashboard::actions::Action> MakeType(const std::string& text);
  std::string CaptchaPrompt(const base::Value::Dict& method) const;

  base::Value::Dict params_;
  raw_ptr<content::WebContents> web_contents_ = nullptr;
  Callback callback_;
  std::vector<std::string> planned_kinds_;
  std::vector<std::unique_ptr<dashboard::actions::Action>> planned_actions_;
  std::unique_ptr<dashboard::actions::Action> bridge_action_;
  size_t method_index_ = 0;
  size_t step_index_ = 0;
  int adapt_count_ = 0;
  bool ran_step_ = false;
  bool step_succeeded_ = false;
  std::string step_message_;
  Phase phase_ = Phase::kSteps;

  base::WeakPtrFactory<AuthenticateAction> weak_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_AUTHENTICATE_ACTION_H_

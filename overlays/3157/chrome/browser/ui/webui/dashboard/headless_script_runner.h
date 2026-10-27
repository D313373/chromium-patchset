// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_SCRIPT_RUNNER_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_SCRIPT_RUNNER_H_

#include <string>

#include "base/containers/flat_set.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

class Profile;

namespace content {
class WebContents;
}

namespace dashboard {

class Dashboard;

using ScriptRunDoneCallback =
    base::OnceCallback<void(bool ok, int failed_step, std::string message)>;

// Prefer actions[], then legacy url_contexts[].actions (same as listeners /
// DashboardHandler).
base::Value::List FlattenScriptTimeline(const base::Value::Dict& script);

base::Value::List ExpandCallScriptsInTimeline(
    Dashboard* dashboard,
    const base::Value::List& in,
    base::flat_set<std::string>* visiting);

// Load + flatten + expand. Returns empty list and sets *error on failure.
base::Value::List LoadExpandedScriptTimeline(Dashboard* dashboard,
                                             const std::string& script_id,
                                             std::string* error);

// Force-native ScriptExecutionService runs on |wc|.
std::string StartNativeAdHocRun(Profile* profile,
                                const base::Value::List& timeline,
                                content::WebContents* wc,
                                ScriptRunDoneCallback done);

std::string StartNativeScriptRun(Profile* profile,
                                 const std::string& script_id,
                                 const base::Value::List& timeline,
                                 content::WebContents* wc,
                                 ScriptRunDoneCallback done);

std::string StartNativeScriptRunFromStep(Profile* profile,
                                         const std::string& script_id,
                                         const base::Value::List& timeline,
                                         content::WebContents* wc,
                                         int start_step_index,
                                         const std::string& resume_url,
                                         ScriptRunDoneCallback done);

// Mirrors DashboardHandler::ResumeScriptFromStep KEY fallback: try main from
// step; on early fail run keyScriptId then retry main from the same step.
class ResumeFromStepWithKeyFallback {
 public:
  ResumeFromStepWithKeyFallback(Profile* profile,
                                Dashboard* dashboard,
                                std::string script_id,
                                int step_index,
                                content::WebContents* wc,
                                ScriptRunDoneCallback done);
  ~ResumeFromStepWithKeyFallback();

  ResumeFromStepWithKeyFallback(const ResumeFromStepWithKeyFallback&) = delete;
  ResumeFromStepWithKeyFallback& operator=(
      const ResumeFromStepWithKeyFallback&) = delete;

  // Returns false if start failed synchronously (*error set). Ownership of
  // `this` is transferred to the async flow via unique_ptr held in callbacks;
  // caller should release ownership after a successful Start().
  bool Start(std::string* error);

 private:
  void RunMain();
  void OnMainDone(bool ok, int failed_step, std::string message);
  void RunKeyThenRetry();
  void OnKeyDone(bool ok, int failed_step, std::string message);
  void Finish(bool ok, int failed_step, std::string message);

  raw_ptr<Profile> profile_ = nullptr;
  raw_ptr<Dashboard> dashboard_ = nullptr;
  std::string script_id_;
  int step_index_ = 0;
  raw_ptr<content::WebContents> wc_ = nullptr;
  ScriptRunDoneCallback done_;
  base::Value::List main_seq_;
  base::Value::List key_seq_;
  std::string resume_url_;
  bool tried_key_ = false;
  base::WeakPtrFactory<ResumeFromStepWithKeyFallback> weak_factory_{this};
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_SCRIPT_RUNNER_H_

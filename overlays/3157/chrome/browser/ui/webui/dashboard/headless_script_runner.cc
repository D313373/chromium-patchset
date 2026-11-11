// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/headless_script_runner.h"

#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"
#include "content/public/browser/web_contents.h"

namespace dashboard {

base::Value::List FlattenScriptTimeline(const base::Value::Dict& script) {
  base::Value::List flat;
  bool appended_from_actions = false;
  if (const base::Value::List* acts = script.FindList("actions")) {
    for (const auto& a : *acts) {
      flat.Append(a.Clone());
      appended_from_actions = true;
    }
  }
  if (appended_from_actions) {
    return flat;
  }
  if (const base::Value::List* ctxs = script.FindList("url_contexts")) {
    for (const auto& c : *ctxs) {
      const base::Value::Dict* cd = c.GetIfDict();
      if (!cd) {
        continue;
      }
      if (const base::Value::List* acts = cd->FindList("actions")) {
        for (const auto& a : *acts) {
          flat.Append(a.Clone());
        }
      }
    }
  }
  return flat;
}

base::Value::List ExpandCallScriptsInTimeline(
    Dashboard* dashboard,
    const base::Value::List& in,
    base::flat_set<std::string>* visiting) {
  base::Value::List out;
  for (const auto& v : in) {
    if (!v.is_dict()) {
      continue;
    }
    const base::Value::Dict& d = v.GetDict();
    const std::string* act = d.FindString("action");
    const std::string kind = act ? *act : std::string();
    if (base::EqualsCaseInsensitiveASCII(kind, "call_script")) {
      const base::Value::Dict* params = d.FindDict("params");
      std::string sid;
      if (params) {
        if (const std::string* s = params->FindString("scriptId")) {
          sid = *s;
        }
      }
      if (!sid.empty() && dashboard && visiting && !visiting->contains(sid)) {
        visiting->insert(sid);
        base::Value::Dict callee = dashboard->LoadScript(sid);
        const std::string callee_kind =
            callee.FindString("kind") ? *callee.FindString("kind")
                                      : std::string("main");
        if (base::EqualsCaseInsensitiveASCII(callee_kind, "listener")) {
          LOG(WARNING) << "[HeadlessScriptRunner] call_script skips listener "
                       << "sid=" << sid;
          visiting->erase(sid);
          continue;
        }
        base::Value::List flat = FlattenScriptTimeline(callee);
        base::Value::List expanded =
            ExpandCallScriptsInTimeline(dashboard, flat, visiting);
        for (auto& e : expanded) {
          out.Append(std::move(e));
        }
        visiting->erase(sid);
        continue;
      }
    }
    out.Append(v.Clone());
  }
  return out;
}

base::Value::List LoadExpandedScriptTimeline(Dashboard* dashboard,
                                             const std::string& script_id,
                                             std::string* error) {
  if (!dashboard || script_id.empty()) {
    if (error) {
      *error = "Missing dashboard or script_id.";
    }
    return base::Value::List();
  }
  base::Value::Dict script = dashboard->LoadScript(script_id);
  if (script.empty()) {
    if (error) {
      *error = "Script not found: " + script_id;
    }
    return base::Value::List();
  }
  base::flat_set<std::string> visiting;
  return ExpandCallScriptsInTimeline(dashboard, FlattenScriptTimeline(script),
                                     &visiting);
}

std::string StartNativeAdHocRun(Profile* profile,
                                const base::Value::List& timeline,
                                content::WebContents* wc,
                                ScriptRunDoneCallback done) {
  ScriptExecutionService* exec =
      ScriptExecutionServiceFactory::GetForProfile(profile);
  if (!exec || !wc) {
    if (done) {
      std::move(done).Run(false, -1, "No ScriptExecutionService or tab.");
    }
    return std::string();
  }
  ScriptExecutionService::RunOptions options;
  options.force_native_execution = true;
  return exec->StartAdHocRunWithOptions(timeline, wc, options, std::move(done));
}

std::string StartNativeScriptRun(Profile* profile,
                                 const std::string& script_id,
                                 const base::Value::List& timeline,
                                 content::WebContents* wc,
                                 ScriptRunDoneCallback done) {
  ScriptExecutionService* exec =
      ScriptExecutionServiceFactory::GetForProfile(profile);
  if (!exec || !wc) {
    if (done) {
      std::move(done).Run(false, -1, "No ScriptExecutionService or tab.");
    }
    return std::string();
  }
  ScriptExecutionService::RunOptions options;
  options.force_native_execution = true;
  return exec->StartScriptRunWithOptions(script_id, timeline, wc,
                                         /*key_timeline=*/nullptr, options,
                                         std::move(done));
}

std::string StartNativeScriptRunFromStep(Profile* profile,
                                         const std::string& script_id,
                                         const base::Value::List& timeline,
                                         content::WebContents* wc,
                                         int start_step_index,
                                         const std::string& resume_url,
                                         ScriptRunDoneCallback done) {
  ScriptExecutionService* exec =
      ScriptExecutionServiceFactory::GetForProfile(profile);
  if (!exec || !wc) {
    if (done) {
      std::move(done).Run(false, -1, "No ScriptExecutionService or tab.");
    }
    return std::string();
  }
  // StartScriptRunFromStep does not take RunOptions; native path is default
  // for headless when CDP is unavailable. Prefer ad-hoc slice if needed.
  return exec->StartScriptRunFromStep(script_id, timeline, wc, start_step_index,
                                      resume_url, std::move(done));
}

ResumeFromStepWithKeyFallback::ResumeFromStepWithKeyFallback(
    Profile* profile,
    Dashboard* dashboard,
    std::string script_id,
    int step_index,
    content::WebContents* wc,
    ScriptRunDoneCallback done)
    : profile_(profile),
      dashboard_(dashboard),
      script_id_(std::move(script_id)),
      step_index_(step_index),
      wc_(wc),
      done_(std::move(done)) {}

ResumeFromStepWithKeyFallback::~ResumeFromStepWithKeyFallback() = default;

bool ResumeFromStepWithKeyFallback::Start(std::string* error) {
  if (!profile_ || !dashboard_ || !wc_ || script_id_.empty()) {
    if (error) {
      *error = "Missing profile, dashboard, tab, or script_id.";
    }
    return false;
  }
  base::Value::Dict script = dashboard_->LoadScript(script_id_);
  if (script.empty()) {
    if (error) {
      *error = "Script not found: " + script_id_;
    }
    return false;
  }
  base::flat_set<std::string> visiting;
  main_seq_ = ExpandCallScriptsInTimeline(
      dashboard_, FlattenScriptTimeline(script), &visiting);
  if (main_seq_.empty()) {
    if (error) {
      *error = "Script has no actions to resume.";
    }
    return false;
  }

  std::string resume_url;
  if (const base::Value::Dict* last_run = script.FindDict("last_run")) {
    resume_url = ScriptSchema::ExtractRunHistoryStepUrl(
        last_run->FindList("steps"), step_index_);
  }
  const std::string timeline_context_url =
      ScriptSchema::ResolveResumeNavigationUrl(main_seq_, step_index_);
  if (resume_url.empty()) {
    resume_url = timeline_context_url;
  }
  resume_url_ = resume_url;

  std::string key_script_id;
  if (const std::string* ks = script.FindString("keyScriptId")) {
    key_script_id = *ks;
  }
  if (!key_script_id.empty()) {
    base::Value::Dict key_script = dashboard_->LoadScript(key_script_id);
    if (!key_script.empty()) {
      base::flat_set<std::string> kvisiting;
      key_seq_ = ExpandCallScriptsInTimeline(
          dashboard_, FlattenScriptTimeline(key_script), &kvisiting);
    }
  }

  RunMain();
  return true;
}

void ResumeFromStepWithKeyFallback::RunMain() {
  const std::string id = StartNativeScriptRunFromStep(
      profile_, script_id_, main_seq_, wc_, step_index_, resume_url_,
      base::BindOnce(&ResumeFromStepWithKeyFallback::OnMainDone,
                     weak_factory_.GetWeakPtr()));
  if (id.empty()) {
    Finish(false, -1, "Failed to start resume run.");
  }
}

void ResumeFromStepWithKeyFallback::OnMainDone(bool ok,
                                               int failed_step,
                                               std::string message) {
  if (ok) {
    Finish(true, -1, std::string());
    return;
  }
  const bool early_fail =
      failed_step >= step_index_ && failed_step <= step_index_ + 2;
  if (!tried_key_ && early_fail && !key_seq_.empty()) {
    tried_key_ = true;
    RunKeyThenRetry();
    return;
  }
  Finish(false, failed_step, std::move(message));
}

void ResumeFromStepWithKeyFallback::RunKeyThenRetry() {
  LOG(WARNING) << "[HeadlessScriptRunner] resume early fail; running KEY "
               << "then retry script=" << script_id_
               << " step=" << step_index_;
  const std::string id = StartNativeAdHocRun(
      profile_, key_seq_, wc_,
      base::BindOnce(&ResumeFromStepWithKeyFallback::OnKeyDone,
                     weak_factory_.GetWeakPtr()));
  if (id.empty()) {
    Finish(false, -1, "Failed to start KEY script for resume.");
  }
}

void ResumeFromStepWithKeyFallback::OnKeyDone(bool ok,
                                              int failed_step,
                                              std::string message) {
  if (!ok) {
    Finish(false, failed_step, "Key Script failed: " + message);
    return;
  }
  RunMain();
}

void ResumeFromStepWithKeyFallback::Finish(bool ok,
                                           int failed_step,
                                           std::string message) {
  if (done_) {
    std::move(done_).Run(ok, failed_step, std::move(message));
  }
}

}  // namespace dashboard

#include "chrome/browser/ui/webui/dashboard/dashboard_handler.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_automation_controller.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/run_history_store.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "ui/base/window_open_disposition.h"

namespace {

std::optional<base::Time> Iso8601ToTime(const std::string& iso) {
  if (iso.empty()) {
    return std::nullopt;
  }
  base::Time time;
  if (!base::Time::FromString(iso.c_str(), &time)) {
    return std::nullopt;
  }
  return time;
}

content::WebContents* OpenNewForegroundTab(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  Browser* browser = chrome::FindLastActiveWithProfile(profile);
  if (!browser) {
    browser = chrome::FindBrowserWithProfile(profile);
  }
  if (!browser) {
    return nullptr;
  }

  NavigateParams np(browser, GURL("about:blank"),
                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  np.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&np);
  return np.navigated_or_inserted_contents;
}

base::Value::List FlattenTimelinePreferActions(
    const base::Value::Dict& script) {
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

}  // namespace

DashboardHandler::DashboardHandler(
    mojo::PendingReceiver<dashboard::mojom::DashboardHandler> receiver,
    Profile* profile,
    dashboard::Dashboard* dashboard)
    : profile_(profile),
      dashboard_(dashboard),
      script_executor_(std::make_unique<ScriptExecutor>(profile)),
      scheduler_service_(
          std::make_unique<dashboard::SchedulerService>(profile, dashboard)),
      listener_manager_(
          std::make_unique<dashboard::ListenerManager>(profile, dashboard)),
      receiver_(this, std::move(receiver)) {}

DashboardHandler::~DashboardHandler() = default;

void DashboardHandler::Initialize(InitializeCallback callback) {
  // This can be used for any one-time setup. For now, it just completes.
  std::move(callback).Run();
}

void DashboardHandler::GetScripts(GetScriptsCallback callback) {
  base::Value::Dict scripts =
      dashboard_ ? dashboard_->LoadAllScripts() : base::Value::Dict();
  if (scripts.empty()) {
    scripts = dashboard::ScriptStore::LoadAll(profile_);
    if (dashboard_) {
      for (const auto [id, script] : scripts) {
        if (script.is_dict()) {
          dashboard_->AddScript(id, script.GetDict().Clone());
        }
      }
    }
  }
  std::move(callback).Run(std::move(scripts));
}

void DashboardHandler::CreateNewScript(base::Value::Dict payload,
                                       CreateNewScriptCallback callback) {
  std::string id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  std::string name =
      payload.Find("name") ? payload.Find("name")->GetString() : "New Script";

  base::Value::Dict script;
  script.Set("name", name);
  script.Set("actions", base::Value::List());
  script.Set("settings", base::Value::Dict());

  dashboard_->AddScript(id, std::move(script));

  base::Value::Dict result;
  result.Set("scriptId", id);
  std::move(callback).Run(std::move(result));
}

void DashboardHandler::SaveScript(const std::string& id,
                                  base::Value::Dict script_dict) {
  auto canonical = dashboard::ScriptSchema::CanonicalizeForSave(
      std::move(script_dict), id, base::Time::Now());
  dashboard::ScriptStore::Write(profile_, id, canonical);
  dashboard_->SaveScript(id, canonical.Clone());
  DashboardUIMessageHandler::NotifyScriptsChanged();
}

void DashboardHandler::AppendToScript(const std::string& id,
                                      base::Value::Dict delta) {
  // Load existing (disk is source of truth)
  base::Value::Dict existing;
  dashboard::ScriptStore::Read(profile_, id, &existing);
  // Canonicalize existing to ensure actions/url_contexts are synchronized.
  auto existing_canon = dashboard::ScriptSchema::CanonicalizeForSave(
      std::move(existing), id, base::Time::Now());
  base::Value::List* existing_actions = existing_canon.FindList("actions");
  if (!existing_actions) {
    existing_canon.Set("actions", base::Value::List());
    existing_actions = existing_canon.FindList("actions");
  }
  if (const base::Value::List* delta_actions = delta.FindList("actions")) {
    for (const auto& action : *delta_actions) {
      existing_actions->Append(action.Clone());
    }
  }
  if (const base::Value::List* url_ctx = delta.FindList("url_contexts")) {
    for (const auto& group : *url_ctx) {
      const base::Value::Dict* dict = group.GetIfDict();
      if (!dict) {
        continue;
      }
      if (const base::Value::List* group_actions = dict->FindList("actions")) {
        for (const auto& action : *group_actions) {
          existing_actions->Append(action.Clone());
        }
      }
    }
  }

  // Persist
  existing_canon = dashboard::ScriptSchema::CanonicalizeForSave(
      std::move(existing_canon), id, base::Time::Now());
  dashboard::ScriptStore::Write(profile_, id, existing_canon);
  dashboard_->SaveScript(id, existing_canon.Clone());
  DashboardUIMessageHandler::NotifyScriptsChanged();
}

void DashboardHandler::DeleteScript(const std::string& id) {
  dashboard::ScriptStore::Delete(profile_, id);
  dashboard_->DeleteScript(id);
}

void DashboardHandler::PlayScript(const std::string& id,
                                  PlayScriptCallback callback) {
  LOG(INFO) << "[Dashboard] PlayScript called id=" << id;
  base::Value::Dict script = dashboard_->LoadScript(id);
  if (script.empty()) {
    LOG(ERROR) << "[Dashboard] PlayScript: script not found id=" << id;
    std::move(callback).Run();
    return;
  }

  if (const base::Value::List* ctxs = script.FindList("url_contexts")) {
    LOG(INFO) << "[Dashboard] Loaded script url_contexts=" << ctxs->size();
  } else {
    LOG(WARNING) << "[Dashboard] Loaded script has no actions";
  }

  auto flatten_from_script =
      [&](const base::Value::Dict& s) -> base::Value::List {
    return FlattenTimelinePreferActions(s);
  };

  // Recursively expand call_script actions inline by scriptId
  base::flat_set<std::string> visiting;
  std::vector<std::string> blocked_listener_targets;
  std::function<base::Value::List(const base::Value::List&)> expand_calls;
  expand_calls = [&](const base::Value::List& in) -> base::Value::List {
    base::Value::List out;
    for (const auto& v : in) {
      if (!v.is_dict()) {
        out.Append(v.Clone());
        continue;
      }
      const base::Value::Dict& d = v.GetDict();
      const std::string* action = d.FindString("action");
      const base::Value::Dict* params = d.FindDict("params");
      std::string kind = action ? *action : "";
      if (kind == "call_script") {
        std::string sid;
        if (params) {
          if (const std::string* s = params->FindString("scriptId")) {
            sid = *s;
          }
        }
        if (!sid.empty() && !visiting.contains(sid)) {
          visiting.insert(sid);
          base::Value::Dict callee = dashboard_->LoadScript(sid);
          const std::string callee_kind = callee.FindString("kind")
                                              ? *callee.FindString("kind")
                                              : std::string("main");
          if (base::EqualsCaseInsensitiveASCII(callee_kind, "listener")) {
            blocked_listener_targets.push_back(sid);
            visiting.erase(sid);
            continue;
          }
          base::Value::List flat = flatten_from_script(callee);
          base::Value::List expanded = expand_calls(flat);
          for (auto& e : expanded) {
            out.Append(std::move(e));
          }
          visiting.erase(sid);
          continue;  // skip original call_script node
        }
      }
      out.Append(v.Clone());
    }
    return out;
  };

  base::Value::List initial = flatten_from_script(script);
  LOG(INFO) << "[Dashboard] PlayScript flatten size=" << initial.size();
  base::Value::List final_seq = expand_calls(initial);
  if (const std::string* start_url = script.FindString("startUrl")) {
    GURL start(*start_url);
    if (start.is_valid()) {
      bool first_is_navigate = false;
      if (!final_seq.empty()) {
        const base::Value::Dict* first = final_seq.front().GetIfDict();
        const std::string* action =
            first ? first->FindString("action") : nullptr;
        first_is_navigate =
            action && base::EqualsCaseInsensitiveASCII(*action, "navigate");
      }
      if (!first_is_navigate) {
        base::Value::Dict nav;
        nav.Set("action", "navigate");
        base::Value::Dict params;
        params.Set("url", start.spec());
        nav.Set("params", std::move(params));
        base::Value::List prefixed;
        prefixed.Append(base::Value(std::move(nav)));
        for (const auto& step : final_seq) {
          prefixed.Append(step.Clone());
        }
        final_seq = std::move(prefixed);
      }
    }
  }
  LOG(INFO) << "[Dashboard] PlayScript expanded size=" << final_seq.size();
  if (!blocked_listener_targets.empty()) {
    LOG(WARNING) << "[Dashboard] call_script cannot target listener scripts; "
                 << "skipped count=" << blocked_listener_targets.size();
  }
  if (final_seq.empty()) {
    std::move(callback).Run();
    return;
  }

  // Optional Key Script: recovery sequence run by ScriptExecutor after
  // repeated critical failures.
  std::string key_script_id;
  if (const std::string* ks = script.FindString("keyScriptId")) {
    key_script_id = *ks;
  }
  base::Value::List key_seq;
  if (!key_script_id.empty()) {
    base::Value::Dict key_script = dashboard_->LoadScript(key_script_id);
    if (!key_script.empty()) {
      base::Value::List key_initial = flatten_from_script(key_script);
      key_seq = expand_calls(key_initial);
      LOG(INFO) << "[Dashboard] PlayScript key script id=" << key_script_id
                << " expanded size=" << key_seq.size();
    }
  }

  content::WebContents* target_wc = OpenNewForegroundTab(profile_);
  if (!target_wc) {
    LOG(ERROR) << "[Dashboard] PlayScript: failed to open target tab";
    std::move(callback).Run();
    return;
  }

  if (script_executor_->GetStatus().is_running) {
    script_executor_->StopExecution();
  }
  active_script_context_ =
      std::make_unique<dashboard::ScriptContext>(id, target_wc);
  script_executor_->SetCompletionCallback(base::BindOnce(
      [](base::WeakPtr<DashboardHandler> self, bool, int, std::string) {
        if (!self) {
          return;
        }
        self->active_script_context_.reset();
      },
      weak_ptr_factory_.GetWeakPtr()));

  const base::Value::List* key_ptr = key_seq.empty() ? nullptr : &key_seq;
  script_executor_->ExecuteScript(final_seq, target_wc,
                                  active_script_context_.get(), key_ptr);
  std::move(callback).Run();
}

void DashboardHandler::SetDebugLogging(bool enabled,
                                       SetDebugLoggingCallback callback) {
  dashboard::SetVerboseLoggingEnabled(enabled);
  base::Value::Dict result;
  result.Set("ok", true);
  result.Set("enabled", enabled);
  std::move(callback).Run(std::move(result));
}

void DashboardHandler::GetDebugLogging(GetDebugLoggingCallback callback) {
  base::Value::Dict result;
  result.Set("enabled", dashboard::IsVerboseLoggingEnabled());
  std::move(callback).Run(std::move(result));
}

void DashboardHandler::GetRunHistory(const std::string& script_id_filter,
                                     const std::string& from_iso,
                                     const std::string& to_iso,
                                     uint32_t limit,
                                     GetRunHistoryCallback callback) {
  const base::Time now = base::Time::Now();
  base::Time from = now - base::Days(7);
  base::Time to = now + base::Days(1);
  if (auto t = Iso8601ToTime(from_iso)) {
    from = *t;
  }
  if (auto t = Iso8601ToTime(to_iso)) {
    to = *t;
  }

  base::Value::List summaries =
      dashboard::RunHistoryStore::ReadSummaries(profile_, from, to, limit);

  if (!script_id_filter.empty()) {
    base::Value::List filtered;
    for (const auto& v : summaries) {
      const base::Value::Dict* d = v.GetIfDict();
      if (!d) {
        continue;
      }
      const std::string* sid = d->FindString("script_id");
      if (sid && *sid == script_id_filter) {
        filtered.Append(v.Clone());
      }
    }
    summaries = std::move(filtered);
  }

  std::move(callback).Run(std::move(summaries));
}

void DashboardHandler::GetRunDetails(const std::string& run_id,
                                     GetRunDetailsCallback callback) {
  base::Value::Dict detail;
  if (!dashboard::RunHistoryStore::ReadRunDetail(profile_, run_id, &detail)) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "Run detail not found: " + run_id);
    std::move(callback).Run(std::move(out));
    return;
  }
  detail.Set("ok", true);
  std::move(callback).Run(std::move(detail));
}

void DashboardHandler::PauseScript(PauseScriptCallback callback) {
  script_executor_->PauseExecution();
  std::move(callback).Run();
}

void DashboardHandler::ResumeScript(ResumeScriptCallback callback) {
  script_executor_->ResumeExecution();
  std::move(callback).Run();
}

void DashboardHandler::ResumeScriptFromStep(
    const std::string& script_id,
    uint32_t step_index,
    ResumeScriptFromStepCallback callback) {
  if (!script_executor_) {
    std::move(callback).Run(false, "ScriptExecutor unavailable");
    return;
  }
  if (script_executor_->GetStatus().is_running) {
    std::move(callback).Run(false, "A script is already running");
    return;
  }

  base::Value::Dict script = dashboard_->LoadScript(script_id);
  if (script.empty()) {
    std::move(callback).Run(false, "Script not found: " + script_id);
    return;
  }

  auto flatten_from_script =
      [&](const base::Value::Dict& s) -> base::Value::List {
    return FlattenTimelinePreferActions(s);
  };

  base::flat_set<std::string> visiting;
  std::function<base::Value::List(const base::Value::List&)> expand_calls;
  expand_calls = [&](const base::Value::List& in) -> base::Value::List {
    base::Value::List out;
    for (const auto& v : in) {
      const base::Value::Dict* d = v.GetIfDict();
      if (!d) {
        out.Append(v.Clone());
        continue;
      }

      const std::string* action = d->FindString("action");
      const base::Value::Dict* params = d->FindDict("params");
      const std::string kind = action ? *action : "";
      if (kind == "call_script") {
        std::string sid;
        if (params) {
          if (const std::string* s = params->FindString("scriptId")) {
            sid = *s;
          }
        }
        if (!sid.empty() && !visiting.contains(sid)) {
          visiting.insert(sid);
          base::Value::Dict callee = dashboard_->LoadScript(sid);
          base::Value::List expanded =
              expand_calls(flatten_from_script(callee));
          for (auto& e : expanded) {
            out.Append(std::move(e));
          }
          visiting.erase(sid);
          continue;
        }
      }

      out.Append(v.Clone());
    }
    return out;
  };

  base::Value::List main_seq = expand_calls(flatten_from_script(script));
  if (main_seq.empty()) {
    std::move(callback).Run(false, "Script has no actions to resume");
    return;
  }

  std::string resume_url;
  if (const base::Value::Dict* last_run = script.FindDict("last_run")) {
    resume_url = dashboard::ScriptSchema::ExtractRunHistoryStepUrl(
        last_run->FindList("steps"), static_cast<int>(step_index));
  }
  const std::string timeline_context_url =
      dashboard::ScriptSchema::ResolveResumeNavigationUrl(
          main_seq, static_cast<int>(step_index));
  const std::string run_history_url = resume_url;
  // Prefer run history over timeline contextUrl. Do not rewrite contextUrl on
  // failure paths — successful executions update it in ScriptExecutor.
  if (resume_url.empty()) {
    resume_url = timeline_context_url;
  } else if (!timeline_context_url.empty() &&
             !dashboard::ScriptSchema::ContextUrlsEquivalent(
                 run_history_url, timeline_context_url)) {
    LOG(WARNING) << "[DashboardHandler] Resume URL mismatch script="
                 << script_id << " step=" << step_index
                 << " run_history=" << run_history_url
                 << " contextUrl=" << timeline_context_url;
  }

  std::string key_script_id;
  if (const std::string* ks = script.FindString("keyScriptId")) {
    key_script_id = *ks;
  }

  base::Value::List key_seq;
  if (!key_script_id.empty()) {
    base::Value::Dict key_script = dashboard_->LoadScript(key_script_id);
    if (!key_script.empty()) {
      key_seq = expand_calls(flatten_from_script(key_script));
    }
  }

  content::WebContents* target_wc = OpenNewForegroundTab(profile_);
  if (!target_wc) {
    std::move(callback).Run(false, "Unable to open a new tab for resume");
    return;
  }

  active_script_context_ =
      std::make_unique<dashboard::ScriptContext>(script_id, target_wc);

  struct Flow {
    base::Value::List main_seq;
    base::Value::List key_seq;
    int start_step = 0;
    std::string resume_url;
    bool tried_key = false;
    ResumeScriptFromStepCallback callback;
  };

  auto flow = std::make_unique<Flow>();
  flow->main_seq = main_seq.Clone();
  flow->key_seq = key_seq.Clone();
  flow->start_step = static_cast<int>(step_index);
  flow->resume_url = resume_url;
  flow->callback = std::move(callback);

  auto run_main = [this](std::unique_ptr<Flow> f) {
    if (!script_executor_ || !active_script_context_) {
      std::move(f->callback).Run(false, "Executor/context unavailable");
      return;
    }

    auto* fptr = f.get();
    script_executor_->SetCompletionCallback(base::BindOnce(
        [](base::WeakPtr<DashboardHandler> self, std::unique_ptr<Flow> flow,
           bool ok, int failed_step, std::string message) mutable {
          if (!self) {
            std::move(flow->callback).Run(false, "DashboardHandler destroyed");
            return;
          }
          if (ok) {
            std::move(flow->callback).Run(true, "");
            return;
          }

          const bool early_fail = failed_step >= flow->start_step &&
                                  failed_step <= flow->start_step + 2;
          if (!flow->tried_key && early_fail && !flow->key_seq.empty()) {
            flow->tried_key = true;

            if (!self->script_executor_ || !self->active_script_context_) {
              std::move(flow->callback)
                  .Run(false, "Executor/context unavailable");
              return;
            }

            auto* flow_ptr = flow.get();
            self->script_executor_->SetCompletionCallback(base::BindOnce(
                [](base::WeakPtr<DashboardHandler> self2,
                   std::unique_ptr<Flow> flow2, bool ok2, int,
                   std::string message2) mutable {
                  if (!self2) {
                    std::move(flow2->callback)
                        .Run(false, "DashboardHandler destroyed");
                    return;
                  }
                  if (!ok2) {
                    std::move(flow2->callback)
                        .Run(false, "Key Script failed: " + message2);
                    return;
                  }

                  if (!self2->script_executor_ ||
                      !self2->active_script_context_) {
                    std::move(flow2->callback)
                        .Run(false, "Executor/context unavailable");
                    return;
                  }

                  auto* fptr2 = flow2.get();
                  self2->script_executor_->SetCompletionCallback(base::BindOnce(
                      [](base::WeakPtr<DashboardHandler> self3,
                         std::unique_ptr<Flow> flow3, bool ok3, int,
                         std::string message3) mutable {
                        if (!self3) {
                          std::move(flow3->callback)
                              .Run(false, "DashboardHandler destroyed");
                          return;
                        }
                        if (ok3) {
                          std::move(flow3->callback).Run(true, "");
                          return;
                        }
                        std::move(flow3->callback)
                            .Run(false,
                                 "Resume failed after Key Script: " + message3);
                      },
                      self2, std::move(flow2)));

                  self2->script_executor_->ExecuteScriptFromStep(
                      fptr2->main_seq,
                      self2->active_script_context_->main_web_contents(),
                      self2->active_script_context_.get(), fptr2->start_step,
                      fptr2->resume_url);
                },
                self, std::move(flow)));

            self->script_executor_->ExecuteScriptFromStep(
                flow_ptr->key_seq,
                self->active_script_context_->main_web_contents(),
                self->active_script_context_.get(),
                /*start_step_index=*/0, /*resume_url=*/"");
            return;
          }

          std::move(flow->callback).Run(false, std::move(message));
        },
        weak_ptr_factory_.GetWeakPtr(), std::move(f)));

    script_executor_->ExecuteScriptFromStep(
        fptr->main_seq, active_script_context_->main_web_contents(),
        active_script_context_.get(), fptr->start_step, fptr->resume_url);
  };

  run_main(std::move(flow));
}

void DashboardHandler::StopScript(StopScriptCallback callback) {
  script_executor_->StopExecution();
  std::move(callback).Run();
}

void DashboardHandler::GetExecutionStatus(GetExecutionStatusCallback callback) {
  auto status = script_executor_->GetStatus();
  auto mojo_status = dashboard::mojom::ExecutionStatus::New();
  mojo_status->is_running = status.is_running;
  mojo_status->current_step = status.current_step;
  mojo_status->total_steps = status.total_steps;
  mojo_status->status_message = status.status_message;
  std::move(callback).Run(std::move(mojo_status));
}

void DashboardHandler::GetActionTemplate(const std::string& action_type,
                                         GetActionTemplateCallback callback) {
  auto tmpl = dashboard::ScriptSchema::GetActionTemplate(action_type);
  std::move(callback).Run(std::move(tmpl));
}

void DashboardHandler::GetSupportedActions(
    GetSupportedActionsCallback callback) {
  auto actions = dashboard::ScriptSchema::GetSupportedActions();
  std::move(callback).Run(std::move(actions));
}

void DashboardHandler::ValidateAction(base::Value::Dict action,
                                      ValidateActionCallback callback) {
  std::string error;
  bool valid = dashboard::ScriptSchema::ValidateAction(action, &error);
  std::move(callback).Run(valid, error);
}
// #include "chrome/browser/ui/webui/dashboard/dashboard_handler.h"
//
// #include "base/values.h"
// #include "chrome/browser/profiles/profile.h"
//
// DashboardHandler::DashboardHandler(
//     mojo::PendingReceiver<dashboard::mojom::DashboardHandler> receiver,
//     Profile* profile,
//     dashboard::Dashboard* dashboard)
//     : dashboard_(dashboard),
//       profile_(profile),
//       script_executor_(std::make_unique<ScriptExecutor>(profile)),
//       receiver_(this, std::move(receiver)) {}
//
// DashboardHandler::~DashboardHandler() = default;
//
// void DashboardHandler::LoadAllScripts(LoadAllScriptsCallback callback) {
//   std::move(callback).Run(dashboard_->LoadAllScripts());
// }
//
// void DashboardHandler::SaveScript(const std::string& id,
//                                   base::Value::Dict script_dict) {
//   dashboard_->SaveScript(id, std::move(script_dict));
// }
//
// void DashboardHandler::AddScript(const std::string& new_id,
//                                  base::Value::Dict new_script) {
//   dashboard_->AddScript(new_id, std::move(new_script));
// }
//
// void DashboardHandler::DeleteScript(const std::string& id) {
//   dashboard_->DeleteScript(id);
// }
//
// void DashboardHandler::LoadVariables(LoadVariablesCallback callback) {
//   std::move(callback).Run(dashboard_->LoadVariables());
// }
//
// void DashboardHandler::SaveVariables(base::Value::Dict variables) {
//   dashboard_->SaveVariables(std::move(variables));
// }
//
// void DashboardHandler::ToggleListenerScript(const std::string& id,
//                                               bool enabled) {
//   dashboard_->ToggleListenerScript(id, enabled);
// }
//
// void DashboardHandler::GetListenerScripts(GetListenerScriptsCallback
// callback) {
//   std::move(callback).Run(dashboard_->GetListenerScripts());
// }
//
// void DashboardHandler::ExecuteScript(base::Value::Dict timeline,
//                                      ExecuteScriptCallback callback) {
//   // The executor expects a List, not a Dict. This needs to be resolved
//   // based on how your timeline data is structured.
//   // For now, we pass an empty list to allow compilation.
//   script_executor_->ExecuteScript(base::Value::List());
//   std::move(callback).Run();
// }
//
// void DashboardHandler::PauseScript(PauseScriptCallback callback) {
//   script_executor_->PauseExecution();
//   std::move(callback).Run();
// }
//
// void DashboardHandler::ResumeScript(ResumeScriptCallback callback) {
//   script_executor_->ResumeExecution();
//   std::move(callback).Run();
// }
//
// void DashboardHandler::StopScript(StopScriptCallback callback) {
//   script_executor_->StopExecution();
//   std::move(callback).Run();
// }
//
// void DashboardHandler::GetExecutionStatus(GetExecutionStatusCallback
// callback) {
//   auto status = script_executor_->GetStatus();
//   auto mojo_status = dashboard::mojom::ExecutionStatus::New();
//   mojo_status->is_running = status.is_running;
//   mojo_status->current_step = status.current_step;
//   mojo_status->total_steps = status.total_steps;
//   mojo_status->status_message = status.status_message;
//   std::move(callback).Run(std::move(mojo_status));
// }
//

// Schedule Management

void DashboardHandler::SetScriptSchedule(
    const std::string& script_id,
    dashboard::mojom::ScheduleConfigPtr config,
    SetScriptScheduleCallback callback) {
  std::string error;
  dashboard::ScheduleConfig schedule =
      dashboard::ScheduleConfig::FromMojo(*config);
  bool success =
      scheduler_service_->SetScriptSchedule(script_id, schedule, &error);
  std::move(callback).Run(success, error);
}

void DashboardHandler::GetScheduledScripts(
    GetScheduledScriptsCallback callback) {
  std::vector<dashboard::ScheduledScriptInfo> scripts =
      scheduler_service_->GetScheduledScripts();
  std::vector<dashboard::mojom::ScheduledScriptInfoPtr> mojo_scripts;
  for (const auto& script : scripts) {
    mojo_scripts.push_back(script.ToMojo());
  }
  std::move(callback).Run(std::move(mojo_scripts));
}

void DashboardHandler::PauseScheduledScript(
    const std::string& script_id,
    PauseScheduledScriptCallback callback) {
  std::string error;
  bool success = scheduler_service_->PauseScheduledScript(script_id, &error);
  std::move(callback).Run(success, error);
}

void DashboardHandler::ResumeScheduledScript(
    const std::string& script_id,
    ResumeScheduledScriptCallback callback) {
  std::string error;
  bool success = scheduler_service_->ResumeScheduledScript(script_id, &error);
  std::move(callback).Run(success, error);
}

void DashboardHandler::RunScheduledScriptNow(
    const std::string& script_id,
    RunScheduledScriptNowCallback callback) {
  std::string error;
  bool success = scheduler_service_->RunScheduledScriptNow(script_id, &error);
  std::move(callback).Run(success, error);
}

void DashboardHandler::ClearScriptSchedule(
    const std::string& script_id,
    ClearScriptScheduleCallback callback) {
  std::string error;
  bool success = scheduler_service_->ClearScriptSchedule(script_id, &error);
  std::move(callback).Run(success, error);
}

// Listener Management

void DashboardHandler::StartListener(const std::string& script_id,
                                     StartListenerCallback callback) {
  std::string error;
  bool success = listener_manager_->StartListener(script_id, &error);
  std::move(callback).Run(success, error);
}

void DashboardHandler::StopListener(const std::string& script_id,
                                    StopListenerCallback callback) {
  std::string error;
  bool success = listener_manager_->StopListener(script_id, &error);
  std::move(callback).Run(success, error);
}

void DashboardHandler::GetActiveListeners(GetActiveListenersCallback callback) {
  std::vector<dashboard::ListenerInfo> listeners =
      listener_manager_->GetActiveListeners();
  std::vector<dashboard::mojom::ListenerInfoPtr> mojo_listeners;
  for (const auto& listener : listeners) {
    mojo_listeners.push_back(listener.ToMojo());
  }
  std::move(callback).Run(std::move(mojo_listeners));
}

dashboard::DashboardDevToolsClient* DashboardHandler::GetDevToolsClientForTab(
    content::WebContents* tab) {
  if (!tab) {
    LOG(WARNING) << "[DashboardHandler] No tab for DevTools client request";
    return nullptr;
  }
  return GetOrCreateDashboardDevToolsClientForTab(tab);
}

// Variable Management
void DashboardHandler::LoadVariables(LoadVariablesCallback callback) {
  base::Value::Dict variables =
      dashboard_ ? dashboard_->LoadVariables() : base::Value::Dict();
  std::move(callback).Run(std::move(variables));
}

void DashboardHandler::SaveVariables(base::Value::Dict variables) {
  if (dashboard_) {
    dashboard_->SaveVariables(std::move(variables));
  }
}

// Listener Script Management
void DashboardHandler::ToggleListenerScript(const std::string& script_id,
                                            bool enabled) {
  if (dashboard_) {
    dashboard_->ToggleListenerScript(script_id, enabled);
  }
}

void DashboardHandler::GetListenerScripts(GetListenerScriptsCallback callback) {
  std::vector<base::Value::Dict> scripts =
      dashboard_ ? dashboard_->GetListenerScripts()
                 : std::vector<base::Value::Dict>();
  std::move(callback).Run(std::move(scripts));
}

// Copyright 2026
#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_EXECUTION_SERVICE_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_EXECUTION_SERVICE_H_

#include <map>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;
class ScriptExecutor;
namespace content {
class WebContents;
}

namespace dashboard {

class ScriptContext;

// Owns ScriptExecutor instances so runs can outlive the Dashboard tab (WebUI).
// This also enables multiple concurrent runs.
class ScriptExecutionService : public KeyedService {
 public:
  struct RunOptions {
    bool force_cdp_execution = false;
    bool force_native_execution = false;
    bool is_agent_mode = false;
  };

  using CompletionCallback =
      base::OnceCallback<void(bool success, int failed_step, std::string message)>;

  explicit ScriptExecutionService(Profile* profile);
  ~ScriptExecutionService() override;

  ScriptExecutionService(const ScriptExecutionService&) = delete;
  ScriptExecutionService& operator=(const ScriptExecutionService&) = delete;

  static void InvalidateTokenCatalogCacheForProfile(Profile* profile);

  // The {Token} / {{Token}} pass playback already runs. Scaffolding calls this
  // before injecting a type, including the Blink path that never reaches
  // StartRunInternal. Unknown names stay literal.
  static std::string ExpandExecutionTokens(Profile* profile,
                                           const std::string& input);

  // Starts a new run for a persisted script id.
  // Returns a unique execution id (not the same as ScriptExecutor's internal
  // run_id). Returns empty string on failure.
  std::string StartScriptRun(const std::string& script_id,
                             const base::Value::List& timeline,
                             content::WebContents* target_web_contents,
                             const base::Value::List* key_timeline = nullptr);
  std::string StartScriptRunWithOptions(
      const std::string& script_id,
      const base::Value::List& timeline,
      content::WebContents* target_web_contents,
      const base::Value::List* key_timeline,
      const RunOptions& options);
  std::string StartScriptRun(const std::string& script_id,
                             const base::Value::List& timeline,
                             content::WebContents* target_web_contents,
                             const base::Value::List* key_timeline,
                             CompletionCallback completion);
  std::string StartScriptRunWithOptions(
      const std::string& script_id,
      const base::Value::List& timeline,
      content::WebContents* target_web_contents,
      const base::Value::List* key_timeline,
      const RunOptions& options,
      CompletionCallback completion);
  // Starts a persisted run from a specific step index.
  std::string StartScriptRunFromStep(const std::string& script_id,
                                     const base::Value::List& timeline,
                                     content::WebContents* target_web_contents,
                                     int start_step_index,
                                     const std::string& resume_url);
  std::string StartScriptRunFromStep(const std::string& script_id,
                                     const base::Value::List& timeline,
                                     content::WebContents* target_web_contents,
                                     int start_step_index,
                                     const std::string& resume_url,
                                     CompletionCallback completion);

  // Starts a one-off run (no script persistence / last_run updates). Useful for
  // ad-hoc queues (e.g. "run next action" from the overlay).
  std::string StartAdHocRun(const base::Value::List& timeline,
                            content::WebContents* target_web_contents);
  std::string StartAdHocRunWithOptions(const base::Value::List& timeline,
                                       content::WebContents* target_web_contents,
                                       const RunOptions& options);
  std::string StartAdHocRun(const base::Value::List& timeline,
                            content::WebContents* target_web_contents,
                            CompletionCallback completion);
  std::string StartAdHocRunWithOptions(const base::Value::List& timeline,
                                       content::WebContents* target_web_contents,
                                       const RunOptions& options,
                                       CompletionCallback completion);

  // Best-effort stop. Returns false if run id not found.
  bool StopRun(const std::string& execution_id, const std::string& reason);

  // Stop every in-flight run (Play, one-shot onMatch chains, triggers, etc.).
  // Returns how many runs were signaled to stop.
  size_t StopAllRuns(const std::string& reason);

  // Returns {ok,is_running,current_step,total_steps,error,status_message}.
  base::Value::Dict GetRunStatusDict(const std::string& execution_id) const;

  size_t running_count() const { return runs_.size(); }
  void set_max_concurrent_runs(size_t max) { max_concurrent_runs_ = max; }

 private:
  struct RunState {
    RunState();
    ~RunState();
    RunState(RunState&&);
    RunState& operator=(RunState&&);
    RunState(const RunState&) = delete;
    RunState& operator=(const RunState&) = delete;

    std::unique_ptr<::ScriptExecutor> executor;
    std::string context_id;
    base::WeakPtr<content::WebContents> web_contents;
    CompletionCallback completion;
  };

  std::string StartRunInternal(const std::string& script_id,
                               bool persist,
                               const base::Value::List& timeline,
                               content::WebContents* target_web_contents,
                               const base::Value::List* key_timeline,
                               int start_step_index,
                               const std::string& resume_url,
                               const RunOptions& options,
                               CompletionCallback completion);

  void OnRunCompleted(const std::string& execution_id,
                      bool success,
                      int failed_step,
                      std::string message);
  void OnPersistedInputBehaviorSettingsLoaded(base::Value::Dict input_behavior);

  raw_ptr<Profile> profile_ = nullptr;
  size_t max_concurrent_runs_ = 20;
  std::map<std::string, RunState> runs_;

  base::WeakPtrFactory<ScriptExecutionService> weak_ptr_factory_{this};
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_EXECUTION_SERVICE_H_

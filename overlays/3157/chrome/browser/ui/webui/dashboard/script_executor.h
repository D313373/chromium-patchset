// script_executor.h — now with agent context + history tracking

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_EXECUTOR_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_EXECUTOR_H_

#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <cstdint>
#include "base/memory/raw_ptr.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_factory.h"
#include "chrome/browser/ui/webui/dashboard/failure_replay_recorder.h"
#include "chrome/browser/ui/webui/dashboard/script_context.h"
#include "chrome/browser/ui/webui/dashboard/recording_suppression.h"
#include "chrome/browser/ui/webui/dashboard/token_redaction.h"
#include "url/gurl.h"


class Profile;

namespace dashboard {
class Action;
class ActionFactory;
}

class ScriptExecutor : public content::WebContentsObserver {
 public:
  struct ExecutionStatus {
    bool is_running;
    int current_step;
    int total_steps;
    std::string error;
    std::string status_message;
  };

  using StatusCallback = base::RepeatingCallback<void(const ExecutionStatus&)>;
  using CompletionCallback =
      base::OnceCallback<void(bool success, int failed_step, std::string message)>;

  explicit ScriptExecutor(Profile* profile);
  ~ScriptExecutor() override;

  void ExecuteScript(const base::Value::List& timeline,
                     content::WebContents* web_contents,
                     dashboard::ScriptContext* script_context,
                     const base::Value::List* key_timeline = nullptr);

  // Resume/replay a script starting from a specific step index. If |resume_url|
  // is provided, the executor navigates to it before executing the step.
  void ExecuteScriptFromStep(const base::Value::List& timeline,
                             content::WebContents* web_contents,
                             dashboard::ScriptContext* script_context,
                             int start_step_index,
                             const std::string& resume_url);
    
  void SetPageSnapshot(base::Value::Dict snapshot);
  void PauseExecution();
  void ResumeExecution();
  void AppendAction(base::Value::Dict action_dict);
  void StopExecution();
  ExecutionStatus GetStatus() const;
  void SetStatusCallback(StatusCallback callback);
  void SetCompletionCallback(CompletionCallback callback);
  void SetForceCdpExecutionForRun(bool force);
  void SetForceNativeExecutionForRun(bool force);
  void SetAgentModeForRun(bool agent_mode);
  // The timeline handed to this executor already has {Token} values expanded
  // to cleartext. Everything the executor records (step history, params
  // promoted back into saved scripts, training rows) is passed through this
  // map first so placeholders, not secrets, are what get persisted.
  void SetTokenReverseMapForRun(dashboard::TokenReverseMap reverse_map);

  // Tear down observers / overlays before destroying this executor.
  void CleanupAfterRun();

 private:
  // content::WebContentsObserver:
  void WebContentsDestroyed() override;

  void ExecuteNextAction();
  void AttemptAction(bool is_key_script, size_t index, int attempt);
  void OnActionCompleteInternal(bool is_key_script,
                                size_t index,
                                int attempt,
                                bool success,
                                const std::string& message);
  void FinishRun(bool success, int failed_step, const std::string& message);
  void StartKeyScriptAfterFailures(int resume_step_index);
  void FinishKeyScriptAndResumeMain(bool success, const std::string& message);
  // After a key script's steps all run, wait out the post-login navigation
  // and re-read the page. A wall that is still up is a credential failure,
  // not a reason to submit again.
  void VerifyLoginThenResumeMain(base::TimeTicks deadline);
  void OnKeyScriptVerified(base::Value::Dict page_map);
  void NavigateAndStart(int start_step_index, const std::string& resume_url);
  void OnPersistedInputBehaviorSettingsLoaded(base::Value::Dict input_behavior);
  void PollUntilLoadedThenStart(int start_step_index,
                                const GURL& desired_url,
                                base::TimeTicks deadline);
  void WaitForTargetLoadThenExecute(base::TimeTicks deadline);
  void EvaluateAuthRecoveryForKeyScript(size_t failed_index,
                                        bool skip_humanized_delay_for_next);
  void OnAuthRecoverySnapshotCaptured(size_t failed_index,
                                      bool skip_humanized_delay_for_next,
                                      base::Value::Dict page_map);
  void ScheduleContinuationWithHumanizedDelay(base::OnceClosure continuation,
                                              bool skip_delay = false);
  void UpdateStatus(const ExecutionStatus& status);
  void UpdatePlaybackOverlayProgress(const std::string& detail = std::string());
  void ClearPlaybackOverlayProgress();
  void SaveExecutionResult(bool success, int failed_step, const std::string& message);
  bool ShouldCapturePerStepTrainingArtifacts() const;

  struct StepCaptureData {
    StepCaptureData();
    StepCaptureData(const StepCaptureData&) = delete;
    StepCaptureData(StepCaptureData&&);
    StepCaptureData& operator=(const StepCaptureData&) = delete;
    StepCaptureData& operator=(StepCaptureData&&);
    ~StepCaptureData();

    base::Value::Dict pre_page_map;
    base::Value::Dict post_page_map;
    std::string pre_png_b64;
    std::string post_png_b64;
  };

  using StepContextCaptureCallback =
      base::OnceCallback<void(base::Value::Dict, std::vector<uint8_t>)>;
  using PageMapCaptureCallback = base::OnceCallback<void(base::Value::Dict)>;
  using ScreenshotCaptureCallback =
      base::OnceCallback<void(std::vector<uint8_t>)>;
  void CaptureStepContext(StepContextCaptureCallback cb);
  void CapturePageMapSnapshot(PageMapCaptureCallback cb);
  void CaptureScreenshotPng(ScreenshotCaptureCallback cb);
  void OnActionExecutedWithPreCapture(bool is_key_script,
                                      size_t index,
                                      int attempt,
                                      StepCaptureData capture,
                                      bool success,
                                      const std::string& message);
  void OnActionCompleteWithPostCapture(bool is_key_script,
                                       size_t index,
                                       int attempt,
                                       bool success,
                                       std::string message,
                                       StepCaptureData capture,
                                       base::Value::Dict post_page_map,
                                       std::vector<uint8_t> post_png);

  int MaxRetriesFor(bool is_key_script, size_t index) const;
  int BaseRetryDelayMsFor(bool is_key_script, size_t index) const;
  bool IsNonCritical(bool is_key_script, size_t index) const;
  bool is_paused_ = false;
  bool run_completion_in_progress_ = false;
  bool humanized_inter_action_delay_enabled_ = true;
  bool force_cdp_execution_for_run_ = false;
  bool force_native_execution_for_run_ = false;
  bool is_agent_mode_for_run_ = false;
  dashboard::TokenReverseMap token_reverse_map_;

  base::Value::Dict current_snapshot_;
  raw_ptr<Profile> profile_;
  std::unique_ptr<dashboard::actions::ActionFactory> action_factory_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;

  std::vector<std::unique_ptr<dashboard::actions::Action>> action_queue_;
  std::vector<bool> non_critical_;
  base::Value::List timeline_;
  std::vector<std::unique_ptr<dashboard::actions::Action>> key_action_queue_;
  std::vector<bool> key_non_critical_;
  base::Value::List key_timeline_;

  int current_step_ = 0;
  int key_step_ = 0;
  int step_index_ = 0;
  int consecutive_critical_failures_ = 0;
  bool has_any_critical_failure_ = false;
  int first_critical_failed_step_ = -1;
  std::string last_critical_error_;

  bool in_key_script_ = false;
  int resume_main_step_after_key_ = 0;
  int key_script_runs_ = 0;
  std::string preflight_failed_action_index_;
  ExecutionStatus status_;
  StatusCallback status_callback_;
  CompletionCallback completion_callback_;

  raw_ptr<dashboard::ScriptContext> script_context_ = nullptr;
  raw_ptr<content::WebContents> target_web_contents_ = nullptr;

  std::string run_id_;
  base::TimeTicks run_started_at_;
  base::Time run_started_wall_time_;
  std::optional<std::string> pending_url_before_;
  base::TimeTicks pending_action_started_at_;
  std::optional<StepCaptureData> pending_step_capture_;
  std::unique_ptr<dashboard::ScopedRecordingSuppression> recording_suppression_;
  std::unique_ptr<dashboard::FailureReplayRecorder> failure_replay_recorder_;

struct AgentContext {
  std::string goal;
  std::vector<base::Value::Dict> history;
    
  AgentContext();
  ~AgentContext();
} current_context_;

  base::WeakPtrFactory<ScriptExecutor> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_EXECUTOR_H_

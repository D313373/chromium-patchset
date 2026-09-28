#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_GLIC_PROMPT_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_GLIC_PROMPT_ACTION_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/values.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "ui/gfx/geometry/rect_f.h"

class Profile;
class ElementReadinessChecker;
namespace content {
class WebContents;
}
namespace dashboard {
struct ResolvedElement;
}

namespace network {
class SimpleURLLoader;
}

namespace dashboard {
namespace actions {

class GlicPromptAction : public Action {
 public:
  explicit GlicPromptAction(Profile* profile);
  ~GlicPromptAction() override;

  void Execute(content::WebContents* web_contents, Callback callback) override;
  bool SetParams(const base::Value::Dict& params) override;

 private:
  void ResetRunState();
  void OnConfiguredModelLoaded(base::Value::Dict configured_agent);
  void FetchInitialPageContext();
  void ProbeInitialContextFocusAreaReadiness(int check_index);
  void OnInitialContextFocusAreaReadiness(
      int check_index,
      bool ready,
      const dashboard::ResolvedElement& element,
      const std::string& error);
  void ProbeInteractiveReadinessBeforeInitialContext(int wait_attempt);
  void OnInteractiveReadinessBeforeInitialContext(int wait_attempt,
                                                  base::Value result);
  void OnPageContextFetched(std::string page_title,
                            std::string page_url,
                            dashboard::UnifiedPageMapResult result);
  void StartPromptTurn(const std::string& user_prompt,
                       bool include_page_context,
                       bool include_system_prompt,
                       bool attach_screenshot,
                       std::optional<gfx::RectF> screenshot_clip =
                           std::nullopt,
                       base::Value::Dict screenshot_request_meta =
                           base::Value::Dict());
  void ContinueStartPromptTurn(const std::string& user_prompt,
                               bool include_page_context,
                               bool include_system_prompt,
                               std::string screenshot_base64,
                               base::Value::Dict screenshot_request_meta);
  void OnScreenshotCaptured(const std::string& user_prompt,
                            bool include_page_context,
                            bool include_system_prompt,
                            base::Value::Dict screenshot_request_meta,
                            std::string screenshot_base64);
  void OnModelResponse(std::unique_ptr<std::string> response_body);
  void DispatchCommand(base::Value::Dict command);
  void DispatchCommandWhenPageReady(base::Value::Dict command,
                                    int wait_attempt);
  void ProbeInteractiveReadinessBeforeDispatch(base::Value::Dict command,
                                               int wait_attempt);
  void OnInteractiveReadinessBeforeDispatch(base::Value::Dict command,
                                            int wait_attempt,
                                            base::Value result);
  void ContinueAfterActionWhenPageReady(base::Value::Dict prior_command,
                                        base::Value::Dict prior_result,
                                        std::string previous_page_title,
                                        std::string previous_page_url,
                                        int wait_attempt);
  void RefreshContextCommandWhenPageReady(std::string command_name,
                                          base::Value::Dict payload,
                                          int wait_attempt);
  void RefreshScreenshotContextWhenPageReady(std::string command_name,
                                             base::Value::Dict payload,
                                             int wait_attempt);
  void OnTimelineCommandComplete(std::string command_name,
                                 base::Value::Dict payload,
                                 bool success,
                                 int failed_step,
                                 std::string message);
  void OnPostActionContextRefreshComplete(base::Value::Dict prior_command,
                                          base::Value::Dict prior_result,
                                          std::string previous_page_title,
                                          std::string previous_page_url,
                                          dashboard::UnifiedPageMapResult result);
  void OnContextRefreshComplete(std::string command_name,
                                base::Value::Dict payload,
                                dashboard::UnifiedPageMapResult result);
  void OnScreenshotContextRefreshComplete(
      std::string command_name,
      base::Value::Dict payload,
      dashboard::UnifiedPageMapResult result);
  void RecordTraceCompletion(bool success, std::string_view message);
  void Finalize(bool success, std::string message);
  // Auto-persist succeeded task_actions_ and optionally ask the model to prune.
  bool AutoSaveSucceededTaskScript(std::string* out_script_name,
                                   std::string* out_error);
  void BeginPostTaskScriptReview(const std::string& draft_script_name,
                                 const std::string& completion_summary);

  std::string prompt_;
  base::Value::List focus_areas_;
  int step_index_ = 0;
  int failure_streak_ = 0;
  int plain_text_retry_count_ = 0;
  bool pending_context_refresh_ = false;
  bool focus_only_context_ = false;
  bool stay_on_current_tab_ = false;
  bool skip_auto_save_ = false;
  int context_batch_index_ = 0;
  bool controls_menu_active_ = false;
  bool save_script_menu_active_ = false;
  bool script_review_active_ = false;
  std::string auto_saved_script_id_;
  std::string auto_saved_script_name_;
  std::string pending_completion_summary_;
  std::string current_page_title_;
  std::string current_page_url_;
  std::string previous_page_title_;
  std::string previous_page_url_;
  std::string provider_;
  std::string provider_api_key_;
  std::string selected_model_;
  std::string loop_profile_;
  std::string agent_endpoint_;
  std::string last_model_response_;
  bool use_choice_response_format_ = false;
  base::Value::Dict current_page_map_;
  base::Value::List current_choice_menu_;
  base::Value::List previous_choice_menu_;
  base::Value::List task_actions_;
  base::Value::List ollama_context_;
  base::Value::Dict trace_;
  std::unique_ptr<ElementReadinessChecker> initial_context_readiness_checker_;
  raw_ptr<content::WebContents> active_web_contents_ = nullptr;
  std::optional<Callback> pending_callback_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<GlicPromptAction> weak_ptr_factory_{this};
};

}  // namespace actions
}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_GLIC_PROMPT_ACTION_H_

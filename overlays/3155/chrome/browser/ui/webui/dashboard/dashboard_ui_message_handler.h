#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_UI_MESSAGE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_UI_MESSAGE_HANDLER_H_

#include <string>
#include <vector>
#include <memory>
#include <map>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

namespace network { class SimpleURLLoader; }

class Profile;
namespace dashboard {
  class Dashboard;
  class AntiDetectionController;
  class ListenerManager;
}
class RecorderController;  // forward declaration

class DashboardUIMessageHandler : public content::WebUIMessageHandler {
 public:
  DashboardUIMessageHandler(Profile* profile, dashboard::Dashboard* dashboard);
  ~DashboardUIMessageHandler() override;

  DashboardUIMessageHandler(const DashboardUIMessageHandler&) = delete;
  DashboardUIMessageHandler& operator=(const DashboardUIMessageHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

  // Recorder -> Dashboard broadcast: called by RecorderController::Stop().
  static void NotifyRecordingStopped();
  // Scripts changed broadcast: call after save/delete to refresh UI.
  static void NotifyScriptsChanged();
  // Update cached script payloads after runtime persistence and broadcast the
  // refreshed scripts list to open dashboard surfaces.
  static void UpdateRuntimeScriptCache(Profile* profile,
                                       const std::string& id,
                                       base::Value::Dict script);
  // Listener-state changed broadcast: call after listener status/url updates.
  static void NotifyListenersChanged();

  // WebUI message handlers accessible from JS (must remain public)
  void HandlePerformAgentAction(const base::Value::List& args);
  // Agent console aliases used by dashboard_app.js
  void HandleAgentCallAction(const base::Value::List& args);
  void HandleAgentRunTimeline(const base::Value::List& args);
  void HandleAgentInvestigatePage(const base::Value::List& args);
  void HandleAgentRefreshPageMap(const base::Value::List& args);
  void HandleAgentReloadPage(const base::Value::List& args);
  void HandleAgentInspectElement(const base::Value::List& args);
  void HandleAgentGetActivePageInfo(const base::Value::List& args);
  void HandleInitialize(const base::Value::List& args);
  void HandleGetScripts(const base::Value::List& args);
  void HandleCreateNewScript(const base::Value::List& args);
  void HandleSaveScript(const base::Value::List& args);
  void HandleDeleteScript(const base::Value::List& args);
  void HandlePlayScript(const base::Value::List& args);
  void HandleResumeScriptFromStep(const base::Value::List& args);
  void HandleSetDebugLogging(const base::Value::List& args);
  void HandleGetDebugLogging(const base::Value::List& args);
  // Run history / debugging
  void HandleGetRunHistory(const base::Value::List& args);
  void HandleGetRunDetails(const base::Value::List& args);
  void HandleGetListenerEvents(const base::Value::List& args);
  // Listener management (Debugging tab)
  void HandleStartListener(const base::Value::List& args);
  void HandleStopListener(const base::Value::List& args);
  void HandleStopHeadlessTask(const base::Value::List& args);
  void HandleRefreshHeadlessTab(const base::Value::List& args);
  void HandleGetActiveListeners(const base::Value::List& args);
  void HandleRequestHeadlessScreenshot(const base::Value::List& args);
  // Data tab (variables + secrets)
  void HandleGetDataVariables(const base::Value::List& args);
  void HandleSaveDataVariables(const base::Value::List& args);
  void HandleGetDataSecrets(const base::Value::List& args);
  void HandleSaveDataSecrets(const base::Value::List& args);
  // Settings API (global + effective settings)
  void HandleGetEffectiveSettings(const base::Value::List& args);
  void HandleSetGlobalSettings(const base::Value::List& args);
  // Recorder controls
  void HandleArmLocatorCapture(const base::Value::List& args);
  void HandleCancelLocatorCapture(const base::Value::List& args);
  void HandlePollLocatorCapture(const base::Value::List& args);
  // Temporary debug path: simulate renderer user click feed via WebUI
  void HandleDebugUserClick(const base::Value::List& args);
  void HandleToggleRecording(const base::Value::List& args);
  void HandleAgentScreenshot(const base::Value::List& args);
  void HandleAgentOllamaChat(const base::Value::List& args);

 private:
  // Continues HandleAgentOllamaChat after API-key resolution (hosted providers).
  void StartAgentOllamaChatRequest(std::string cb,
                                   std::string provider,
                                   std::string endpoint,
                                   std::string model,
                                   std::string system_prompt,
                                   int max_tokens,
                                   base::Value::List chat_messages,
                                   std::string api_key_name,
                                   std::string api_key);
  // Utility to call back into JS with the given callback id and payload
  void ResolveCallback(const std::string& cb_fn, const base::Value& payload);
  void ResolveCallback(const std::string& cb_fn, const base::Value::Dict& payload);
  void ResolveCallback(const std::string& cb_fn, const base::Value::List& payload);

  void OnGetScriptsLoaded(std::string cb, base::Value::Dict raw);

  // Background init helpers to avoid blocking the UI thread
  void OnInitializeDone(std::string cb, base::Value::Dict scripts);
  // Background script mutations
  void OnCreateNewDone(std::string cb, std::string id, base::Value::Dict script);
  void OnSaveDone(std::string cb, std::string id, base::Value::Dict script);
  void OnDeleteDone(std::string cb, std::string id);

  // Background settings helpers
  void OnGetEffectiveSettingsDone(std::string cb, base::Value::Dict settings);
  void OnSetGlobalSettingsDone(std::string cb, bool ok);

  // Anti-detection helpers
  void InitializeAntiDetectionController();
  void ApplyAntiDetectionSettings(const base::Value::Dict& settings);
  void HandleSaveSnapshot(const base::Value::List& args);
  void StartBackgroundTabListenerWithTokenMapAsync(const std::string& cb,
                                                   const std::string& script_id,
                                                   const std::string& warning,
                                                   bool report_headless_mode);
    
    // NEW: Helper function to do all blocking work on a background thread
    // It returns a pair: <success, error_message>
    static std::pair<bool, std::string> SaveSnapshotToDisk(
        base::FilePath profile_path,
        base::Value data_to_save);

    // NEW: Helper function to run the callback on the UI thread on completion
    void OnSnapshotSaved(const std::string& callback_name,
                         std::pair<bool, std::string> result);
    
    
  raw_ptr<Profile> profile_ = nullptr;
  raw_ptr<dashboard::Dashboard> dashboard_ = nullptr;

  std::unique_ptr<RecorderController> recorder_;
  std::unique_ptr<dashboard::AntiDetectionController> anti_detection_controller_;
  std::unique_ptr<dashboard::ListenerManager> listener_manager_;
  std::map<std::string, base::Time> headless_listener_started_at_;

  // Registry accessor for live handlers to broadcast to.
  static std::vector<DashboardUIMessageHandler*>& Instances();

  // Tracks whether the dashboard is currently in agent mode (vs user recording)
  bool is_agent_mode_ = false;

  // Must be last: weak pointer factory for safe callbacks
  base::WeakPtrFactory<DashboardUIMessageHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_UI_MESSAGE_HANDLER_H_

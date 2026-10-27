// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GLIC_HOST_GLIC_PAGE_HANDLER_H_
#define CHROME_BROWSER_GLIC_HOST_GLIC_PAGE_HANDLER_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/glic/host/glic.mojom.h"
#include "chrome/browser/glic/host/host.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace content {
class BrowserContext;
class RenderFrameHost;
class WebContents;
}  // namespace content
namespace gfx {
class Size;
}  // namespace gfx

class RecorderController;

namespace glic {
class GlicKeyedService;
class GlicWebClientHandler;
class TabConsoleObserver;

// Handles the Mojo requests coming from the Glic WebUI.
class GlicPageHandler : public glic::mojom::PageHandler,
                        public PanelStateObserver {
 public:
  GlicPageHandler(content::WebContents* webui_contents,
                  mojo::PendingReceiver<glic::mojom::PageHandler> receiver,
                  mojo::PendingRemote<glic::mojom::Page> page);

  GlicPageHandler(const GlicPageHandler&) = delete;
  GlicPageHandler& operator=(const GlicPageHandler&) = delete;

  ~GlicPageHandler() override;

  content::WebContents* webui_contents() { return webui_contents_; }

  void NotifyWindowIntentToShow();

  // Returns the main frame of the guest view that lives within this WebUI. May
  // be null.
  content::RenderFrameHost* GetGuestMainFrame();

  // glic::mojom::PageHandler implementation.

  void CreateWebClient(::mojo::PendingReceiver<glic::mojom::WebClientHandler>
                           web_client_receiver) override;
  void PrepareForClient(base::OnceCallback<void(mojom::PrepareForClientResult)>
                            callback) override;
  // Called whenever the webview main frame commits.
  void WebviewCommitted(const GURL& origin) override;

  void ClosePanel(ClosePanelCallback callback) override;

  void OpenProfilePickerAndClosePanel() override;

  void SignInAndClosePanel() override;

  void OpenDisabledByAdminLinkAndClosePanel() override;

  void ResizeWidget(const gfx::Size& size,
                    base::TimeDelta duration,
                    ResizeWidgetCallback callback) override;

  void EnableDragResize(bool enabled) override;

  void GetUseStockGeminiGuest(GetUseStockGeminiGuestCallback callback) override;
  void SetUseStockGeminiGuest(bool use_stock,
                              SetUseStockGeminiGuestCallback callback) override;

  void GetDashboardScripts(GetDashboardScriptsCallback callback) override;
  void StartDashboardScript(const std::string& script_id,
                            mojom::AutomationRunTarget target,
                            StartDashboardScriptCallback callback) override;
  void StopDashboardRun(const std::string& execution_id,
                        const std::string& reason,
                        StopDashboardRunCallback callback) override;

  void StartRecorder(StartRecorderCallback callback) override;
  void StopRecorder() override;
  void GetRecorderSnapshot(GetRecorderSnapshotCallback callback) override;
  void GetRecorderState(GetRecorderStateCallback callback) override;
  void SetRecorderGuidedLoopUiState(
      bool active,
      const std::string& phase,
      bool capture_pending,
      const std::string& capture_kind,
      uint64_t capture_sequence,
      uint32_t scrape_target_count,
      SetRecorderGuidedLoopUiStateCallback callback) override;
  void GetRecorderGuidedLoopUiState(
      GetRecorderGuidedLoopUiStateCallback callback) override;
  void GetGuidedScrapeSelectionLocators(
      GetGuidedScrapeSelectionLocatorsCallback callback) override;
  void ResetRecorderGuidedLoopStepOne(
      ResetRecorderGuidedLoopStepOneCallback callback) override;
  void PopGuidedScrapeSelection(
      PopGuidedScrapeSelectionCallback callback) override;
  void PopRecorderManualLoopStep(
      PopRecorderManualLoopStepCallback callback) override;
  void RemoveRecordedAction(uint32_t index,
                            RemoveRecordedActionCallback callback) override;
  void SaveRecorderToNewDashboardScript(
      const std::string& name,
      SaveRecorderToNewDashboardScriptCallback callback) override;
  void AppendRecorderToDashboardScript(
      const std::string& script_id,
      AppendRecorderToDashboardScriptCallback callback) override;
  void DiscardRecorderActions(DiscardRecorderActionsCallback callback) override;
  void ArmLocatorCaptureForDashboard(
      ArmLocatorCaptureForDashboardCallback callback) override;
  void CancelLocatorCaptureForDashboard(
      CancelLocatorCaptureForDashboardCallback callback) override;
  void BeginRecorderManualLoopCapture(
      const std::string& css_selector,
      int32_t candidate_count,
      BeginRecorderManualLoopCaptureCallback callback) override;
  void EndRecorderManualLoopCapture(
      EndRecorderManualLoopCaptureCallback callback) override;
  void AppendRecorderActionJson(const std::string& action_json,
                                AppendRecorderActionJsonCallback callback) override;
  void StartAgentRun(mojom::StartAgentRunRequestPtr request,
                     StartAgentRunCallback callback) override;
  void SendAgentTurn(mojom::AgentTurnRequestPtr request,
                     SendAgentTurnCallback callback) override;
  void CancelAgentRun(const std::string& run_id,
                      CancelAgentRunCallback callback) override;
  void GetAgentRunTrace(const std::string& run_id,
                        GetAgentRunTraceCallback callback) override;
  void ListRecentAgentRuns(ListRecentAgentRunsCallback callback) override;
  void ExpandAgentElementRefs(
      const std::string& run_id,
      const std::string& snapshot_id,
      const std::vector<std::string>& element_refs,
      ExpandAgentElementRefsCallback callback) override;
  void GetDashboardDataTokenCatalog(
      GetDashboardDataTokenCatalogCallback callback) override;
  void GetDashboardDevToolsConsoleLogs(
      const std::string& level_filter,
      uint32_t limit,
      GetDashboardDevToolsConsoleLogsCallback callback) override;
  void InvestigateDashboardPage(
      const std::string& request_json,
      InvestigateDashboardPageCallback callback) override;

  // Notifies the web client about zero state suggestions.
  void ZeroStateSuggestionChanged(mojom::ZeroStateSuggestionsV2Ptr suggestions,
                                  mojom::ZeroStateSuggestionsOptions options);

  void WebUiStateChanged(glic::mojom::WebUiState new_state) override;

  // PanelStateObserver implementation.
  void PanelStateChanged(const glic::mojom::PanelState& panel_state,
                         const PanelStateContext& context) override;

  void UpdatePageState(mojom::PanelStateKind panelStateKind);

 Host& host() { return *host_; }

 private:
  friend class GlicWebClientHandler;
  void OnDashboardScriptRunCompleted(const std::string& script_id,
                                     const std::string& script_name,
                                     bool auto_record_started,
                                     bool success,
                                     int failed_step,
                                     std::string message);
  void ExportAutomationTrainingSample(const std::string& script_id,
                                      const std::string& script_name,
                                      bool success,
                                      int failed_step,
                                      std::string message,
                                      base::Value::List actions,
                                      const std::string& current_url);

  void UpdateProfileReadyState();
  void EnsureRecorderStateSubscription();
  void OnRecorderStateChanged();
  void PushRecorderStateToPage();
  static std::string GuidedPhaseToString(int phase);
  static std::string CaptureKindToString(int kind);
  void ObserveConsoleForWebContents(content::WebContents* web_contents);
  void OnObservedTabConsoleMessage(const base::Value::Dict& entry);
  std::string GetObservedTabConsoleLogsJson(const std::string& level_filter,
                                            uint32_t limit) const;

  GlicKeyedService* GetGlicService();

  // HostManager keeps the host alive while GlicPageHandler is alive.
  raw_ptr<Host> host_;
  // There should at most one WebClientHandler at a time. A new one is created
  // each time the webview loads a page.
  std::unique_ptr<GlicWebClientHandler> web_client_handler_;
  std::unique_ptr<TabConsoleObserver> tab_console_observer_;
  std::deque<base::Value::Dict> observed_tab_console_entries_;
  raw_ptr<content::WebContents> webui_contents_;
  raw_ptr<content::BrowserContext> browser_context_;
  mojo::Receiver<glic::mojom::PageHandler> receiver_;
  mojo::Remote<glic::mojom::Page> page_;
  mojo::Remote<glic::mojom::WebClient> web_client_;
  std::unique_ptr<RecorderController> recorder_controller_;
  raw_ptr<RecorderController> recorder_state_source_ = nullptr;
  base::CallbackListSubscription recorder_state_subscription_;
  uint64_t locator_capture_generation_ = 0;
  std::optional<int32_t> last_dashboard_new_tab_id_;
  std::vector<base::CallbackListSubscription> subscriptions_;
  base::WeakPtrFactory<GlicPageHandler> weak_ptr_factory_{this};
};

}  // namespace glic
#endif  // CHROME_BROWSER_GLIC_HOST_GLIC_PAGE_HANDLER_H_

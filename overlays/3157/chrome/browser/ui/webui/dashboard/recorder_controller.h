// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_RECORDER_CONTROLLER_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_RECORDER_CONTROLLER_H_

#include <memory>
#include <string>
#include <map>
#include <cstdint>

#include <deque>
#include <optional>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_observer.h"
#include "content/public/common/referrer.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/mojom/input/input_messages.mojom-forward.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "content/public/browser/global_routing_id.h"
#include "url/gurl.h"

namespace dashboard {
class PageMapExtractor;
class RecorderHighlightOverlay;
}  // namespace dashboard

namespace content {
class DevToolsAgentHost;
class WebContents;
class NavigationHandle;
}

class BrowserView;
class Browser;
class Profile;

class RecorderDevToolsClient;

struct RecorderStateSnapshot {
  bool is_recording;
  uint32_t action_count;
  bool manual_loop_capture_active;
  bool locator_capture_armed;
  bool locator_capture_ready;
  bool guided_active;
  int guided_phase;
  bool guided_capture_pending;
  int guided_capture_kind;
  uint64_t guided_capture_sequence;
  uint32_t guided_scrape_target_count;
};

// Simulachrome recorder controller:
// - Track recording state
// - Attach to the target tab and observe automation-relevant input/state
// - Surface state to dashboard/Glic consumers through callbacks and snapshots
#include "content/public/browser/web_contents_observer.h"
class RecorderController : public content::WebContentsObserver,
                           public TabStripModelObserver {
 public:
  struct GuidedLoopUiState {
    bool active = false;
    int phase = 0;  // 0=idle, 1=await_loop_target, 2=select_scrape_targets, 3=record_return
    bool capture_pending = false;
    int capture_kind = 0;  // 0=none, 1=loop_target, 2=scrape_target
    uint64_t capture_sequence = 0;
    uint32_t scrape_target_count = 0;
  };

  // Native extras for one guided scrape pick, parallel to its locator bundle.
  struct GuidedScrapeSelectionDetail {
    std::string relative_locator;  // Row-relative CSS, empty if unknown.
    std::string field_name_hint;
    gfx::Rect bounds;  // Web contents DIPs at pick time.
  };

  RecorderController();
  RecorderController(const RecorderController&) = delete;
  RecorderController& operator=(const RecorderController&) = delete;
  ~RecorderController() override;

  bool is_recording() const { return is_recording_; }

  // Global access to the active recorder for shared dashboard/Glic recorder
  // flows and one-shot capture helpers.
  static RecorderController* GetActive();
  static void SetActive(RecorderController* rc);
  static uint64_t GetLatestLocatorCaptureSequence();
  static std::optional<base::Value::Dict> GetLatestLocatorCaptureResult(
      uint64_t min_sequence_exclusive,
      uint64_t* out_sequence);

  // Starts a recording session.
  void Start(BrowserView* browser_view);

  // Stops the session.
  void Stop();

  // Lightweight passive mode used by dashboard/Glic recorder flows.
  void StartPassiveMode(BrowserView* browser_view = nullptr,
                        bool explicit_recording = false);
  void StopPassiveMode();

  // Remove an action by index from the in-memory list.
  void RemoveRecordedAction(size_t index);


  // Append one canonicalized action.
  void AppendRecordedAction(base::Value::Dict action);

  // Starts manual for_each loop capture using an explicit CSS locator.
  // While active, appended actions are nested into the loop's `steps`.
  bool BeginManualForEachLoopCaptureWithLocator(
      const std::string& css_locator,
      int candidate_count,
      std::string* error_out = nullptr);

  // Ends manual for_each loop capture if active. Returns true when an active
  // capture was ended.
  bool EndManualForEachLoopCaptureForTools();

  bool IsManualForEachLoopCaptureActive() const {
    return manual_loop_capture_active_;
  }

  using StateChangedCallback =
      base::RepeatingCallback<void(const RecorderStateSnapshot&)>;
  base::CallbackListSubscription AddStateChangedCallback(
      StateChangedCallback callback);
  RecorderStateSnapshot GetStateSnapshot() const;
  std::string GetCurrentUrl() const { return current_url_; }

  bool SetGuidedLoopUiState(const GuidedLoopUiState& state);
  GuidedLoopUiState GetGuidedLoopUiState() const { return guided_loop_ui_state_; }

  // Append a list of actions (timeline).
  void AppendRecordedTimeline(const base::Value::List& timeline);

  // Expose a snapshot of recorded actions for the dashboard to consume.
  base::Value::List GetRecordedActionsClone() const;
  size_t GetRecordedActionCount() const { return recorded_actions_.size(); }

  // Clear all recorded actions (used when discarding a recording).
  void ClearRecordedActions();

  // Ask the injected recorder to flush any pending type buffer.
  void FlushTypeBuffer();

  // Capture a PNG screenshot of the active tab via DevTools and return the
  // base64-encoded image data (no data: prefix). Empty string on failure.
  void CaptureScreenshot(
      base::OnceCallback<void(std::string /*base64_png*/)> cb,
      std::optional<gfx::RectF> clip_override = std::nullopt);

  // Arms a one-shot locator capture. The next left-click resolved from the
  // target page returns a locator bundle through `callback`.
  void ArmOneShotLocatorCapture(
      base::TimeDelta timeout,
      bool suppress_recording_side_effects,
      base::OnceCallback<void(base::Value::Dict)> callback);
  bool AttachToBestCapturableTarget(
      BrowserView* preferred_browser_view = nullptr,
      content::WebContents* avoid_contents = nullptr,
      const std::string& preferred_context_url = std::string());
  bool is_locator_capture_armed() const { return locator_capture_armed_; }
  bool is_locator_capture_ready() const { return locator_capture_ready_; }
  void WaitForOneShotCaptureReady(
      base::TimeDelta timeout,
      base::OnceCallback<void(bool /*ready*/, std::string /*error*/)> callback);
  void CancelOneShotLocatorCapture(const std::string& error);

  // Reads recorder-owned guided scrape target selections and returns locator
  // bundles as a JSON array string.
  void GetGuidedScrapeSelectionLocatorsForTools(
      base::OnceCallback<void(bool ok,
                              std::string selections_json,
                              std::string error)> callback);
  bool ResetGuidedLoopStepOneForTools(std::string* error_out = nullptr);
  void PopGuidedScrapeSelectionForTools(
      base::OnceCallback<void(bool ok,
                              uint32_t remaining_count,
                              std::string error)> callback);
  bool PopLastManualLoopStepForTools(std::string* removed_action_out = nullptr,
                                     std::string* error_out = nullptr);

  using PageMapCallback = base::OnceCallback<void(base::Value::Dict)>;
  void CapturePageMap(PageMapCallback cb);

  // Query buffered console entries (most recent first). level_filter: 'all', 'error', 'warn', 'log', etc.
  base::Value::List QueryConsole(const std::string& level_filter, int limit) const;

 protected:
  // Reload/navigation-safe teardown: detach before content changes.
  void DidStartNavigation(content::NavigationHandle* navigation_handle) override;
  void DidFinishNavigation(content::NavigationHandle* navigation_handle) override;
  void DidOpenRequestedURL(content::WebContents* new_contents,
                           content::RenderFrameHost* source_render_frame_host,
                           const GURL& url,
                           const content::Referrer& referrer,
                           WindowOpenDisposition disposition,
                           ui::PageTransition transition,
                           bool started_from_context_menu,
                           bool renderer_initiated) override;
  void WebContentsDestroyed() override;
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabStripModelDestroyed(TabStripModel* tab_strip_model) override;
  // Note: console message hook removed for now; DevTools path may be used.

 private:

  friend class InputObserver;
  
  // Input observer to capture mouse clicks at the browser layer and request
  // selector resolution from the renderer resolver.
  class InputObserver : public content::RenderWidgetHost::InputEventObserver,
                        public content::RenderWidgetHostObserver {
   public:
    InputObserver(RecorderController* owner, content::RenderWidgetHost* host);
    ~InputObserver() override;

    void Detach();

   // content::RenderWidgetHost::InputEventObserver
    void OnInputEvent(const content::RenderWidgetHost& host,
                      const blink::WebInputEvent& event,
                      InputEventSource source) override;
    void OnInputEventAck(const content::RenderWidgetHost& host,
                         blink::mojom::InputEventResultSource source,
                         blink::mojom::InputEventResultState state,
                         const blink::WebInputEvent& event) override {}

    // content::RenderWidgetHostObserver
    void RenderWidgetHostDestroyed(content::RenderWidgetHost* widget_host) override;

   private:
    void MaybeHandleKeyboardEvent(const blink::WebInputEvent& event);
    // Swallows page clicks while the recorder is intercepting them and feeds
    // them to OnInputEvent instead, so the page never sees them.
    bool InterceptMouseEvent(const blink::WebMouseEvent& event);

    raw_ptr<RecorderController> owner_ = nullptr;
    raw_ptr<content::RenderWidgetHost> host_ = nullptr;
    content::RenderWidgetHost::MouseEventCallback mouse_event_callback_;
  };

  void AttachInputObserver();
  void DetachInputObserver();
  void EnsureDevToolsAttached();
  // Attach CDP client if needed and enable DOM/Runtime domains (lazy).
  void EnsureCdpDomainsEnabled();
  void DetachDevToolsSession();
  void UpdateInstrumentationState();
  bool ShouldMaintainInstrumentationSession() const;
  bool ShouldObserveInput() const;
  void ObservePreferredTabStrip(BrowserView* browser_view);
  content::WebContents* ResolveTrackedCapturableWebContents(
      BrowserView* preferred_browser_view,
      content::WebContents* avoid_contents) const;
  void SetGuidedLoopFirstClickGuardArmed(bool armed);
  void RefreshOneShotCaptureClickSuppression();
  bool IsGuidedInterceptionPhaseActive() const;
  void MaybeQueueHoverProbe(int x_css, int y_css);
  void RunHoverProbe();
  // DevTools helpers
  void AttachToWebContents(content::WebContents* web_contents);
  void AttachToActiveTab(BrowserView* browser_view);
  void DetachFromTarget();
  void ReattachToTarget();
  void QueueClickFallback(int x_css,
                          int y_css,
                          base::TimeDelta delay = base::Milliseconds(900));
  void CancelClickFallback();
  void RecordDragAction(const gfx::Point& from_point,
                        const gfx::Point& to_point);
  bool TryUpgradeRecentRecordedClick(
      const base::Value::Dict& click,
      const base::Value::List& incoming_locators,
      int click_count,
      const gfx::Point& point,
      const std::string& frame_id,
      const std::string& source);
  bool TryResolveClickFromTrackedState(
      const gfx::Point& point,
      bool require_non_visual,
      const char* source);
  void ResolveOneShotCaptureAtPoint(const gfx::Point& point);
  base::Value::List BuildRecordedLocatorBundleForClick(
      const base::Value::Dict& click) const;
  base::Value::List BuildLocatorBundleForPoint(const gfx::Point& point) const;
  void ToggleGuidedScrapeSelectionAtPoint(const gfx::Point& point);
  void OnGuidedScrapeSelectionCaptureResult(const gfx::Point& point,
                                            base::Value::Dict result);
  void OnGuidedScrapeSelectionPeersResolved(
      base::Value::List locators,
      gfx::Point point,
      dashboard::mojom::RepeatedPeersPtr peers);
  void ClearGuidedScrapeSelections();
  bool ToggleGuidedScrapeSelectionLocators(
      base::Value::List locators,
      GuidedScrapeSelectionDetail detail = GuidedScrapeSelectionDetail());
  void SetGuidedScrapeOverlayEnabled(bool enabled);
  void PopGuidedScrapeOverlaySelection();
  void RebuildGuidedScrapeSelectionModel();
  // Main-frame renderer resolver, rebound when the main frame changes.
  dashboard::mojom::StealthyElementResolver* MainFrameResolver();
  bool ShouldInterceptPageClicks() const;
  void OnPageScrolledBy(double dy);
  void RefreshHighlightOverlay();
  bool HandleGuidedPageArrowShortcut(int windows_key_code);
  base::Value::List SanitizeGuidedScrapeSelectionLocators(
      const base::Value::List& locators,
      bool allow_visual_fallback = false) const;
  std::string GuidedScrapeSelectionSignature(
      const base::Value::List& locators) const;
  void SetOneShotCaptureClickSuppression(bool armed, bool persistent = false);
  void SetGuidedLoopSameTabNavigationGuard(
      bool enabled,
      bool block_all_anchor_navigation = false);
  void CompleteOneShotLocatorCapture(base::Value::Dict result);

  void SendCommand(const std::string& method, base::Value::Dict params);
  int SendCommandExpectResponse(const std::string& method,
                                base::Value::Dict params,
                                base::OnceCallback<void(std::string, std::string)> cb);
  void OnDevToolsResponse(int id, base::Value::Dict result, base::Value::Dict error);
  // Receive DevTools events (method + params)
  void OnDevToolsEvent(const std::string& method, base::Value::Dict params);
  void InstallRecorderScripts();

  void FlushPendingTypeBuffer();
  void ScheduleTypeBufferIdleFlush();
  void HandleBackspaceForTypeBuffer();
  void MaybeAppendCharacterForTypeBuffer(const blink::WebKeyboardEvent& event);
  bool MaybeCollapseRecordedTailIntoLoop();
  void ScheduleScrollCapture();
  void CaptureScrollSnapshotForRecording();
  void MaybeQueueLoopPreview(int x_css, int y_css);
  void RunLoopPreviewProbe();
  void ClearLoopPreview();
  void UpdateLoopPreviewAtPoint(int x_css, int y_css, bool active);
  void OnLoopPreviewResolved(bool active_request,
                             gfx::Point point,
                             dashboard::mojom::RepeatedPeersPtr peers);
  bool BeginManualForEachLoopFromPreview();
  std::optional<size_t> FindLatestManualLoopCaptureActionIndex() const;
  void RemoveRecordedActionsFromIndex(size_t index);
  void EndManualForEachLoopCapture();
  bool TryAppendActionToManualLoop(const base::Value::Dict& action);
  void RebuildRecordedActionSignatureCache();
  void AppendRecordedActionSignature(const base::Value::Dict& action);
  void UpdateRecordedActionSignatureAt(size_t index);
  void NotifyStateChanged();

  // Last-resort capture when the renderer hit test finds nothing usable:
  // nearest PageMap element, else the raw point.
  void ProbeSelectorsAtPoint(int x_css, int y_css);
  bool TryResolveSelectorsViaRenderer(int x_css, int y_css);

  // PageMap capture helpers
  void MaybeCapturePageMap(const std::string& reason);
  void RequestPageMapSnapshot(const std::string& reason, PageMapCallback cb);
  void StartPageMapCapture();
  void EnsurePageMapExecutionContext();
  void RequestPageMapFrameTree();
  void OnPageMapFrameTreeReceived(std::string res_json, std::string err_json);
  void RequestPageMapIsolatedWorld();
  void OnPageMapWorldCreated(std::string res_json, std::string err_json);
  void EvaluatePageMapScript();
  void OnPageMapEvaluated(std::string res_json, std::string err_json);
  void DispatchPageMapResult(base::Value::Dict raw_snapshot);
  void OnPageMapExtractionFinished(base::Value::Dict pagemap);
  void HandlePageMapFailure(const std::string& reason);
  void ResolvePageMapCallbacks(const base::Value::Dict& pagemap);
  void StoreAutoCapturedPageMap(const base::Value::Dict& pagemap);
  void ResetPageMapState();
  void ResetPageMapContextForNavigation();
  std::optional<base::Value::Dict> MaybeCloneRecentPageMapSnapshot(
      const std::string& reason) const;
  bool is_recording_ = false;
  bool explicit_recording_enabled_ = false;
  bool cdp_domains_enabled_ = false;

 public:
  // Temporary public entry to receive simulated renderer clicks (mojo stub).
  void OnUserClickFromRenderer(base::Value::Dict click);
  void AugmentClickParamsFromPageMapSnapshot(base::Value::Dict& params,
                                             const base::Value::Dict& click);

  // Build a compact element report for the given index from the last
  // PageMap snapshot, used by debugging inspect_element tools.
  std::optional<base::Value::Dict> BuildElementReportForIndex(
      int index) const;

  // Target + DevTools
  raw_ptr<content::WebContents> target_web_contents_ = nullptr;  // Not owned.
  scoped_refptr<content::DevToolsAgentHost> agent_host_;
  std::unique_ptr<RecorderDevToolsClient> devtools_client_;
  int next_command_id_ = 1;
  std::unique_ptr<InputObserver> input_observer_;
  raw_ptr<TabStripModel> observed_tab_strip_model_ = nullptr;
  raw_ptr<Browser> observed_browser_ = nullptr;
  int last_capturable_tab_id_ = -1;
  std::string target_main_frame_token_;

  mojo::AssociatedRemote<dashboard::mojom::StealthyElementResolver>
      click_stealth_resolver_;
  mojo::AssociatedRemote<dashboard::mojom::ElementResolver> click_resolver_;
  std::optional<gfx::Point> pending_click_resolve_point_;
  mojo::AssociatedRemote<dashboard::mojom::StealthyElementResolver>
      main_frame_resolver_;
  content::GlobalRenderFrameHostId main_frame_resolver_frame_id_;
  std::unique_ptr<dashboard::RecorderHighlightOverlay> highlight_overlay_;
  std::vector<gfx::Rect> loop_preview_rects_;
  std::u16string loop_preview_tip_;
  gfx::Point loop_preview_tip_anchor_;
  bool guided_scrape_overlay_enabled_ = false;

  // Buffered console entries (ring buffer)
  std::deque<base::Value::Dict> console_entries_;
  static constexpr size_t kMaxConsoleEntries = 500;

  std::map<int, base::OnceCallback<void(std::string, std::string)>> pending_eval_;

  std::u16string pending_type_buffer_;
  base::OneShotTimer type_buffer_idle_flush_timer_;
  base::OneShotTimer scroll_capture_timer_;
  // Scroll distance (CSS px, positive = down) since the last recorded step.
  double pending_scroll_y_ = 0;

  std::optional<gfx::Point> last_user_click_point_;

  base::OneShotTimer pending_click_fallback_timer_;
  std::optional<gfx::Point> pending_click_fallback_point_;
  std::optional<gfx::Point> active_pointer_down_point_;
  std::optional<gfx::Point> active_pointer_last_point_;
  bool active_pointer_dragged_ = false;
  base::OneShotTimer hover_probe_timer_;
  std::optional<gfx::Point> pending_hover_probe_point_;
  base::TimeTicks pending_hover_probe_queued_at_;
  std::optional<gfx::Point> last_hover_probe_point_;
  base::TimeTicks last_hover_probe_enqueued_at_;
  base::OneShotTimer loop_preview_timer_;
  std::optional<gfx::Point> pending_loop_preview_point_;
  std::optional<gfx::Point> last_loop_preview_point_;
  bool loop_preview_modifiers_active_ = false;
  bool loop_preview_key_chord_active_ = false;
  bool loop_preview_visible_ = false;
  std::optional<base::Value::Dict> last_loop_preview_result_;
  bool guided_loop_target_confirmation_pending_ = false;
  std::optional<gfx::Point> guided_loop_target_probe_point_;
  bool guided_loop_first_click_guard_armed_ = false;
  bool manual_loop_capture_active_ = false;
  bool pending_manual_loop_capture_start_ = false;
  GuidedLoopUiState guided_loop_ui_state_;
  std::vector<std::string> guided_scrape_selection_signatures_;
  std::vector<base::Value::List> guided_scrape_selection_locators_;
  std::vector<GuidedScrapeSelectionDetail> guided_scrape_selection_details_;
  std::string guided_scrape_row_locator_;
  int guided_scrape_row_count_ = 0;
  std::string guided_scrape_selection_model_json_;
  bool guided_same_tab_navigation_guard_enabled_ = false;
  bool page_click_interception_armed_ = false;
  std::string guided_loop_origin_url_;
  std::string last_hover_transition_signature_;
  base::TimeTicks last_hover_recorded_at_;
  std::optional<gfx::Point> last_recorded_click_point_;
  base::TimeTicks last_recorded_click_at_;
  int last_recorded_click_count_ = -1;
  std::string last_recorded_click_frame_id_;
  std::string last_recorded_click_source_;
  std::optional<gfx::Point> last_non_hover_click_point_;
  base::TimeTicks last_non_hover_click_at_;

  bool locator_capture_armed_ = false;
  bool locator_capture_ready_ = false;
  bool locator_capture_suppress_recording_side_effects_ = false;
  int locator_capture_retry_count_ = 0;
  std::optional<gfx::Point> post_locator_capture_suppress_point_;
  base::TimeTicks post_locator_capture_suppress_until_;
  std::optional<base::TimeTicks> locator_capture_armed_at_;
  std::optional<base::TimeTicks> locator_capture_last_input_at_;
  std::optional<base::TimeTicks> guided_loop_target_click_started_at_;
  base::OneShotTimer locator_capture_timeout_timer_;
  base::OnceCallback<void(base::Value::Dict)> locator_capture_callback_;

  // Accumulated lightweight action list for recorder consumers.
  base::Value::List recorded_actions_;
  std::vector<std::string> recorded_action_signatures_;
  base::RepeatingCallbackList<void(const RecorderStateSnapshot&)>
      state_changed_callbacks_;

  // Current URL context (updated on url_change); attached to each action.
  std::string current_url_;
  // Track the last URL we recorded as a navigate action to avoid duplicates
  // (e.g., initial attach + subsequent commit, redirects, reloads).
  std::string last_recorded_navigate_url_;
  // Track the navigation entry index so we can infer back vs forward when the
  // user uses browser history navigation controls.
  std::optional<int> last_committed_entry_index_;
  // PageMap capture state --------------------------------------------------
  std::vector<PageMapCallback> pending_page_map_callbacks_;
  std::optional<int> page_map_context_id_;
  std::string page_map_frame_id_;
  bool page_map_frame_request_pending_ = false;
  bool page_map_world_pending_ = false;
  bool page_map_capture_in_progress_ = false;
  std::string page_map_capture_reason_;
  std::optional<base::Value::Dict> last_page_map_snapshot_;
  base::TimeTicks last_page_map_snapshot_at_;
  std::string last_page_map_snapshot_url_;

  base::WeakPtrFactory<RecorderController> weak_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_RECORDER_CONTROLLER_H_

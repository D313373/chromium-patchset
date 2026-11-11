// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_SIMULACHROME_AGENT_HOST_H_
#define CHROME_BROWSER_SIMULACHROME_SIMULACHROME_AGENT_HOST_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "chrome/browser/simulachrome/grounded_command.h"
#include "chrome/browser/simulachrome/planned_action.h"
#include "chrome/browser/simulachrome/planned_action_runner.h"
#include "chrome/browser/simulachrome/viewport_ensure_visible.h"
#include "ui/gfx/geometry/point.h"
#include "services/simulachrome/public/mojom/simulachrome_agent.mojom.h"

namespace content {
class WebContents;
}

namespace viz {
class GpuClient;
}

namespace simulachrome {

class FrameCaptureClient;
class VisualSettleTracker;

// Browser-process orchestrator:
//   Parse the manager command into an operation.
//   The embedded model is asked only where the control is.
//   Scaffolding runs that operation through the script executor.
class SimulachromeAgentHost : public content::WebContentsObserver,
                              public PlannedActionDelegate {
 public:
  using StepCallback =
      base::OnceCallback<void(bool ok, std::string error,
                              mojom::AgentActionPtr action)>;

  explicit SimulachromeAgentHost(content::WebContents* web_contents);
  ~SimulachromeAgentHost() override;

  // The tab this host is bound to, or null once that tab has been destroyed.
  // Callers compare against this instead of caching their own raw_ptr.
  content::WebContents* target() const { return web_contents_; }

  // content::WebContentsObserver:
  void WebContentsDestroyed() override;

  SimulachromeAgentHost(const SimulachromeAgentHost&) = delete;
  SimulachromeAgentHost& operator=(const SimulachromeAgentHost&) = delete;

  void EnsureService();

  // Viewport PNG for a vision model that cannot hold a GPU mailbox (the
  // qwen task completer talks to Ollama over HTTP). Uses the same
  // CopySharedImage mailbox as the embedded model. The utility reads that
  // mailbox; CopyFromSurface runs only when mailbox readback is down.
  using ViewportImageCallback =
      base::OnceCallback<void(bool ok,
                              std::string png_base64,
                              int width,
                              int height,
                              int css_width,
                              int css_height,
                              std::string source,
                              std::string error)>;
  void CaptureViewportImage(ViewportImageCallback done);

  void LoadModel(const base::FilePath& model_path,
                 base::OnceCallback<void(bool, const std::string&)> done);

  // NL instruction on the active tab. Optional |target_css_selector| enables
  // Strategy 2: DOM bounding-rect check + silent scrollIntoView before capture.
  void RunStep(const std::string& task_instruction, StepCallback done);
  void RunStep(const std::string& task_instruction,
               const std::string& target_css_selector,
               StepCallback done);

  // Batch page tasks. The manager composes every step in full (operation,
  // typed text, key, drag destination, click count); the embedded model is only
  // ever asked where a control is, one control at a time, and each action fires
  // as soon as its own targets are resolved. Batches queue FIFO.
  // |on_action| fires after every action, |on_done| once per batch.
  void SubmitPlannedBatch(std::vector<PlannedAction> actions,
                          PlannedBatchOptions options,
                          PlannedActionRunner::ActionCallback on_action,
                          PlannedActionRunner::BatchCallback on_done);
  // Finishes the in-flight operation, then skips everything still queued.
  void CancelPlannedBatches();
  bool has_planned_work() const { return planned_runner_.busy(); }

 private:
  // PlannedActionDelegate:
  void LocateTarget(const PlannedTarget& target, LocateCallback done) override;
  void RefinePoint(const PlannedAction& action,
                   const LocateResult& located,
                   RefineCallback done) override;
  void Dispatch(const PlannedAction& action,
                const std::vector<PlannedPoint>& points,
                DispatchCallback done) override;

  void OnLocateViewportReady(std::string slot,
                             std::string css_selector,
                             std::string prompt,
                             LocateCallback done,
                             ViewportEnsureVisible::Result viewport_result);
  void OnLocateSettled(std::string slot,
                       std::string prompt,
                       LocateCallback done);
  void OnLocateCaptured(std::string slot,
                        std::string prompt,
                        LocateCallback done,
                        mojom::FrameTexturePtr frame,
                        std::string capture_error);
  void OnLocateAgentResult(std::string slot,
                           LocateCallback done,
                           mojom::AgentResultPtr result);

  // Describes the elements stacked at each DIP point (top 3 per point) with
  // document.elementsFromPoint. Read-only: it never dispatches input, so it
  // cannot trip bot detection and it sees through overlays. Empty result means
  // the probe could not run (cross-process frame, navigation, no tab).
  using ProbeCallback =
      base::OnceCallback<void(std::vector<std::vector<std::string>>)>;
  void ProbePoints(const std::vector<gfx::Point>& dip_points,
                   ProbeCallback done);
  void OnRefineProbed(ExpectedHit expect,
                      PlannedPoint raw,
                      std::vector<gfx::Point> points,
                      RefineCallback done,
                      std::vector<std::vector<std::string>> labels);
  void DispatchPlannedAction(mojom::AgentActionPtr action,
                             std::string pre_click_label,
                             DispatchCallback done);
  void OnPlannedApplied(std::string pre_click_label,
                        DispatchCallback done,
                        bool injected,
                        std::string message);

  static constexpr int kMaxVisualScrollIters = 5;

  // Mojo drops in-flight reply callbacks when the utility dies, so every
  // outstanding request is wrapped to self-invoke with a failure instead of
  // silently never resolving the caller's JS promise.
  void OnServiceDisconnected();
  void OnGpuAttached(bool ready);
  void PumpViewportCaptures();
  void StartViewportCapture(ViewportImageCallback done);
  void OnViewportFrame(ViewportImageCallback done,
                       mojom::FrameTexturePtr frame,
                       std::string capture_error);
  void FinishViewportImage(ViewportImageCallback done,
                           bool ok,
                           std::string png_base64,
                           int width,
                           int height,
                           int css_width,
                           int css_height,
                           std::string source,
                           std::string error);

  void BeginSettleAndCapture(const std::string& task_instruction,
                             const std::string& target_css_selector,
                             int visual_iters,
                             StepCallback done);
  void OnViewportReady(const std::string& task_instruction,
                       const std::string& target_css_selector,
                       int visual_iters,
                       StepCallback done,
                       ViewportEnsureVisible::Result viewport_result);
  void OnVisualSettled(const std::string& task_instruction,
                       const std::string& target_css_selector,
                       int visual_iters,
                       StepCallback done);
  void OnFrameCaptured(const std::string& task_instruction,
                       const std::string& target_css_selector,
                       int visual_iters,
                       StepCallback done,
                       mojom::FrameTexturePtr frame,
                       std::string capture_error);
  void OnAgentResult(const std::string& task_instruction,
                     const std::string& target_css_selector,
                     int visual_iters,
                     StepCallback done,
                     mojom::AgentResultPtr result);
  void OnActionApplied(const std::string& task_instruction,
                       const std::string& target_css_selector,
                       int visual_iters,
                       StepCallback done,
                       mojom::AgentActionPtr action,
                       bool injected,
                       std::string inject_error);

  // Scroll, key, and wait never ask the model. Click and type ask only for
  // a point, then this stamps the parsed operation onto that point.
  void DispatchParsedCommand(const std::string& task_instruction,
                             const std::string& target_css_selector,
                             int visual_iters,
                             StepCallback done,
                             mojom::AgentActionPtr located);

  raw_ptr<content::WebContents> web_contents_;
  GroundedCommand pending_command_;
  // RunStep and the planned queue share the capture client and the model, so
  // they must not interleave.
  int legacy_steps_in_flight_ = 0;
  PlannedActionRunner planned_runner_{this};
  mojo::Remote<mojom::SimulachromeAgent> agent_;
  // Keeps the viz.mojom.Gpu pipe alive for the utility. Empty deleter until
  // EnsureService replaces it with the UI-thread deleter from CreateGpuClient.
  std::unique_ptr<viz::GpuClient, base::OnTaskRunnerDeleter> gpu_client_{
      nullptr, base::OnTaskRunnerDeleter(nullptr)};
  std::unique_ptr<VisualSettleTracker> settle_tracker_;
  std::unique_ptr<FrameCaptureClient> capture_client_;
  bool gpu_attach_finished_ = false;
  bool viewport_capture_in_flight_ = false;
  std::vector<ViewportImageCallback> pending_viewport_captures_;

  base::WeakPtrFactory<SimulachromeAgentHost> weak_factory_{this};
};

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_SIMULACHROME_AGENT_HOST_H_

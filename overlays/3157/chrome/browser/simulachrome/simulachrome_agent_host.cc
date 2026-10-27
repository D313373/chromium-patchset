// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/simulachrome_agent_host.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/simulachrome/agent_input_injector.h"
#include "chrome/browser/simulachrome/frame_capture_client.h"
#include "chrome/browser/simulachrome/grounded_command.h"
#include "chrome/browser/simulachrome/viewport_ensure_visible.h"
#include "chrome/browser/simulachrome/visual_settle_tracker.h"
#include "content/public/browser/gpu_client.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

namespace simulachrome {

SimulachromeAgentHost::SimulachromeAgentHost(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      web_contents_(web_contents),
      settle_tracker_(std::make_unique<VisualSettleTracker>(web_contents)),
      capture_client_(std::make_unique<FrameCaptureClient>(web_contents)) {}

SimulachromeAgentHost::~SimulachromeAgentHost() = default;

void SimulachromeAgentHost::WebContentsDestroyed() {
  // RunStep already fails closed on a null target; nulling here keeps the
  // raw_ptr from dangling past the tab it points at.
  web_contents_ = nullptr;
}

void SimulachromeAgentHost::EnsureService() {
  if (agent_.is_bound()) {
    return;
  }
  content::ServiceProcessHost::Launch<mojom::SimulachromeAgent>(
      agent_.BindNewPipeAndPassReceiver(),
      content::ServiceProcessHost::Options()
          .WithDisplayName("Simulachrome Agent (smolvlm2)")
          .Pass());
  // Not reset_on_disconnect(): that installs its own handler and would hide
  // the teardown, leaving no place to log or to drop the held capture.
  gpu_attach_finished_ = false;
  agent_.set_disconnect_handler(
      base::BindOnce(&SimulachromeAgentHost::OnServiceDisconnected,
                     weak_factory_.GetWeakPtr()));

  // WithGpuClient on ServiceProcessHost is passkey-locked to video capture.
  // CreateGpuClient is the supported way to hand a utility a viz.mojom.Gpu
  // pipe. Queued ahead of LoadModel on this same remote, so the raster context
  // exists before the first frame.
  mojo::PendingRemote<viz::mojom::Gpu> gpu_remote;
  gpu_client_ = content::CreateGpuClient(
      gpu_remote.InitWithNewPipeAndPassReceiver(),
      /*enable_extra_handles_validation=*/false);
  agent_->AttachGpu(
      std::move(gpu_remote),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&SimulachromeAgentHost::OnGpuAttached,
                         weak_factory_.GetWeakPtr()),
          false));
}

void SimulachromeAgentHost::OnServiceDisconnected() {
  LOG(ERROR) << "[SimulachromeAgentHost] agent service disconnected"
             << " (model load / inference crash or OOM); failing pending step";
  // Destroying the Remote destroys pending reply callbacks, which fires their
  // wrapped default invocation and unwinds the step back to the caller.
  agent_.reset();
  gpu_client_.reset();
  gpu_attach_finished_ = false;
  viewport_capture_in_flight_ = false;
  if (capture_client_) {
    capture_client_->SetSkipCpuCopy(false);
  }
  std::vector<ViewportImageCallback> dropped =
      std::move(pending_viewport_captures_);
  for (auto& pending : dropped) {
    if (pending) {
      std::move(pending).Run(false, std::string(), 0, 0, 0, 0, std::string(),
                             "agent_service_disconnected");
    }
  }
}

void SimulachromeAgentHost::OnGpuAttached(bool ready) {
  gpu_attach_finished_ = true;
  if (capture_client_) {
    capture_client_->SetSkipCpuCopy(ready);
  }
  if (ready) {
    LOG(INFO) << "[SimulachromeAgentHost] mailbox readback ready; "
                 "CPU frame copy disabled";
  } else {
    LOG(ERROR) << "[SimulachromeAgentHost] mailbox readback unavailable; "
                  "keeping CPU frame copy";
  }
  PumpViewportCaptures();
}

void SimulachromeAgentHost::CaptureViewportImage(ViewportImageCallback done) {
  if (!web_contents_) {
    std::move(done).Run(false, std::string(), 0, 0, 0, 0, std::string(),
                        "no_web_contents");
    return;
  }
  if (planned_runner_.busy() || legacy_steps_in_flight_ > 0) {
    std::move(done).Run(false, std::string(), 0, 0, 0, 0, std::string(),
                        "capture_busy");
    return;
  }
  EnsureService();
  if (!agent_.is_bound()) {
    std::move(done).Run(false, std::string(), 0, 0, 0, 0, std::string(),
                        "agent_service_unavailable");
    return;
  }
  pending_viewport_captures_.push_back(std::move(done));
  if (gpu_attach_finished_) {
    PumpViewportCaptures();
  }
}

void SimulachromeAgentHost::PumpViewportCaptures() {
  if (viewport_capture_in_flight_ || pending_viewport_captures_.empty() ||
      !gpu_attach_finished_ || !capture_client_) {
    return;
  }
  ViewportImageCallback done = std::move(pending_viewport_captures_.front());
  pending_viewport_captures_.erase(pending_viewport_captures_.begin());
  StartViewportCapture(std::move(done));
}

void SimulachromeAgentHost::StartViewportCapture(ViewportImageCallback done) {
  viewport_capture_in_flight_ = true;
  capture_client_->CaptureFrameTexture(base::BindOnce(
      &SimulachromeAgentHost::OnViewportFrame, weak_factory_.GetWeakPtr(),
      std::move(done)));
}

void SimulachromeAgentHost::FinishViewportImage(
    ViewportImageCallback done,
    bool ok,
    std::string png_base64,
    int width,
    int height,
    int css_width,
    int css_height,
    std::string source,
    std::string error) {
  if (capture_client_) {
    capture_client_->ReleasePending();
  }
  viewport_capture_in_flight_ = false;
  if (done) {
    std::move(done).Run(ok, std::move(png_base64), width, height, css_width,
                        css_height, std::move(source), std::move(error));
  }
  PumpViewportCaptures();
}

void SimulachromeAgentHost::OnViewportFrame(ViewportImageCallback done,
                                           mojom::FrameTexturePtr frame,
                                           std::string capture_error) {
  if (!frame || !capture_error.empty()) {
    FinishViewportImage(
        std::move(done), false, std::string(), 0, 0, 0, 0, std::string(),
        capture_error.empty() ? "capture_failed" : std::move(capture_error));
    return;
  }
  const int css_w = frame->viewport_size_dip.width();
  const int css_h = frame->viewport_size_dip.height();
  const int px_w = frame->size_px.width();
  const int px_h = frame->size_px.height();
  if (!frame->rgba_pixels.empty() && px_w > 0 && px_h > 0) {
    const size_t need =
        static_cast<size_t>(px_w) * static_cast<size_t>(px_h) * 4;
    std::optional<std::vector<uint8_t>> png;
    if (frame->rgba_pixels.size() >= need) {
      png = gfx::PNGCodec::Encode(
          frame->rgba_pixels.data(), gfx::PNGCodec::FORMAT_RGBA,
          gfx::Size(px_w, px_h), px_w * 4, /*discard_transparency=*/false,
          {});
    }
    if (!png || png->empty()) {
      FinishViewportImage(std::move(done), false, std::string(), px_w, px_h,
                          css_w, css_h, "cpu", "png_encode_failed");
      return;
    }
    FinishViewportImage(std::move(done), true, base::Base64Encode(*png), px_w,
                        px_h, css_w, css_h, "cpu", std::string());
    return;
  }
  if (!agent_.is_bound() || frame->mailbox.IsZero()) {
    FinishViewportImage(std::move(done), false, std::string(), px_w, px_h,
                        css_w, css_h, std::string(), "no_mailbox_or_pixels");
    return;
  }
  agent_->ReadbackFrame(
      std::move(frame),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(
              [](base::WeakPtr<SimulachromeAgentHost> self,
                 ViewportImageCallback inner, int css_w, int css_h,
                 mojom::FrameImagePtr image) {
                if (!self) {
                  return;
                }
                if (!image || !image->ok || image->png.empty()) {
                  self->FinishViewportImage(
                      std::move(inner), false, std::string(), 0, 0, css_w,
                      css_h, "mailbox",
                      image && !image->error.empty() ? image->error
                                                     : "mailbox_readback_failed");
                  return;
                }
                const int width = image->size_px.width();
                const int height = image->size_px.height();
                self->FinishViewportImage(
                    std::move(inner), true, base::Base64Encode(image->png),
                    width, height, css_w, css_h, "mailbox", std::string());
              },
              weak_factory_.GetWeakPtr(), std::move(done), css_w, css_h),
          nullptr));
}

void SimulachromeAgentHost::LoadModel(
    const base::FilePath& model_path,
    base::OnceCallback<void(bool, const std::string&)> done) {
  EnsureService();
  if (!agent_.is_bound()) {
    std::move(done).Run(false, "agent_service_unavailable");
    return;
  }
  agent_->LoadModel(model_path,
                    mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                        std::move(done), false, "agent_service_disconnected"));
}

void SimulachromeAgentHost::RunStep(const std::string& task_instruction,
                                    StepCallback done) {
  RunStep(task_instruction, /*target_css_selector=*/std::string(),
          std::move(done));
}

void SimulachromeAgentHost::RunStep(const std::string& task_instruction,
                                    const std::string& target_css_selector,
                                    StepCallback done) {
  if (!web_contents_) {
    std::move(done).Run(false, "no_web_contents", nullptr);
    return;
  }
  if (planned_runner_.busy()) {
    std::move(done).Run(false, "planned_batch_in_progress", nullptr);
    return;
  }
  ++legacy_steps_in_flight_;
  done = base::BindOnce(
      [](base::WeakPtr<SimulachromeAgentHost> self, StepCallback inner, bool ok,
         std::string error, mojom::AgentActionPtr action) {
        if (self) {
          --self->legacy_steps_in_flight_;
        }
        std::move(inner).Run(ok, std::move(error), std::move(action));
      },
      weak_factory_.GetWeakPtr(), std::move(done));
  pending_command_ = ParseGroundedCommand(task_instruction);
  if (pending_command_.kind == GroundedCommand::Kind::kType &&
      pending_command_.text.empty()) {
    auto action = mojom::AgentAction::New();
    action->type = mojom::AgentActionType::kFail;
    action->payload = "type command has no text";
    std::move(done).Run(false, "type command has no text", std::move(action));
    return;
  }
  LOG(INFO) << "[SimulachromeAgentHost] command kind="
            << static_cast<int>(pending_command_.kind)
            << " needs_target=" << pending_command_.needs_target
            << " target=\"" << pending_command_.target << "\"";
  if (!pending_command_.needs_target) {
    DispatchParsedCommand(task_instruction, target_css_selector,
                          /*visual_iters=*/kMaxVisualScrollIters,
                          std::move(done), /*located=*/nullptr);
    return;
  }
  EnsureService();
  BeginSettleAndCapture(LocatePromptFor(pending_command_), target_css_selector,
                        /*visual_iters=*/0, std::move(done));
}

void SimulachromeAgentHost::DispatchParsedCommand(
    const std::string& task_instruction,
    const std::string& target_css_selector,
    int visual_iters,
    StepCallback done,
    mojom::AgentActionPtr located) {
  auto action = mojom::AgentAction::New();
  if (located) {
    action->x = located->x;
    action->y = located->y;
    action->confidence = located->confidence;
  }
  switch (pending_command_.kind) {
    case GroundedCommand::Kind::kType:
      action->type = mojom::AgentActionType::kType;
      action->payload = pending_command_.text;
      break;
    case GroundedCommand::Kind::kKey:
      action->type = mojom::AgentActionType::kKeyPress;
      action->payload = pending_command_.key;
      break;
    case GroundedCommand::Kind::kScroll:
      action->type = mojom::AgentActionType::kScroll;
      action->scroll_dy = pending_command_.scroll_dy;
      break;
    case GroundedCommand::Kind::kWait:
      action->type = mojom::AgentActionType::kWait;
      break;
    case GroundedCommand::Kind::kClick:
      action->type = mojom::AgentActionType::kClick;
      action->payload = pending_command_.target;
      break;
  }
  LOG(INFO) << "[SimulachromeAgentHost] scaffold kind="
            << static_cast<int>(action->type) << " norm=(" << action->x << ","
            << action->y << ")";
  mojom::AgentActionPtr copy = action.Clone();
  AgentInputInjector::ApplyAction(
      web_contents_, *copy,
      base::BindOnce(&SimulachromeAgentHost::OnActionApplied,
                     weak_factory_.GetWeakPtr(), task_instruction,
                     target_css_selector, visual_iters, std::move(done),
                     std::move(action)));
}

void SimulachromeAgentHost::BeginSettleAndCapture(
    const std::string& task_instruction,
    const std::string& target_css_selector,
    int visual_iters,
    StepCallback done) {
  // Strategy 2: if Planner supplied a DOM/A11y selector, bring it on-screen
  // before the 200ms settle + mailbox capture.
  ViewportEnsureVisible::Ensure(
      web_contents_, target_css_selector,
      base::BindOnce(&SimulachromeAgentHost::OnViewportReady,
                     weak_factory_.GetWeakPtr(), task_instruction,
                     target_css_selector, visual_iters, std::move(done)));
}

void SimulachromeAgentHost::OnViewportReady(
    const std::string& task_instruction,
    const std::string& target_css_selector,
    int visual_iters,
    StepCallback done,
    ViewportEnsureVisible::Result result) {
  if (result == ViewportEnsureVisible::Result::kNotFound &&
      !target_css_selector.empty()) {
    // Planner named an element that is not in the DOM — surface as
    // kElementNotFound so the Manager can replan (Strategy 3).
    auto action = mojom::AgentAction::New();
    action->type = mojom::AgentActionType::kElementNotFound;
    action->payload = "selector_not_in_dom:" + target_css_selector;
    std::move(done).Run(false, "element_not_found", std::move(action));
    return;
  }

  settle_tracker_->WaitUntilSettled(
      base::Milliseconds(200),
      base::BindOnce(&SimulachromeAgentHost::OnVisualSettled,
                     weak_factory_.GetWeakPtr(), task_instruction,
                     target_css_selector, visual_iters, std::move(done)));
}

void SimulachromeAgentHost::OnVisualSettled(
    const std::string& task_instruction,
    const std::string& target_css_selector,
    int visual_iters,
    StepCallback done) {
  capture_client_->CaptureFrameTexture(base::BindOnce(
      &SimulachromeAgentHost::OnFrameCaptured, weak_factory_.GetWeakPtr(),
      task_instruction, target_css_selector, visual_iters, std::move(done)));
}

void SimulachromeAgentHost::OnFrameCaptured(
    const std::string& task_instruction,
    const std::string& target_css_selector,
    int visual_iters,
    StepCallback done,
    mojom::FrameTexturePtr frame,
    std::string capture_error) {
  if (!frame || !capture_error.empty()) {
    capture_client_->ReleasePending();
    std::move(done).Run(false,
                        capture_error.empty() ? "capture_failed" : capture_error,
                        nullptr);
    return;
  }
  // The remote can go away during the scroll/settle/capture gap above. Do not
  // relaunch here: a fresh service has no weights, so fail the step and let the
  // Manager re-issue load_simulachrome_model.
  if (!agent_.is_bound()) {
    capture_client_->ReleasePending();
    std::move(done).Run(false, "agent_service_unavailable", nullptr);
    return;
  }
  agent_->ExecuteAction(
      task_instruction, std::move(frame),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&SimulachromeAgentHost::OnAgentResult,
                         weak_factory_.GetWeakPtr(), task_instruction,
                         target_css_selector, visual_iters, std::move(done)),
          nullptr));
}

void SimulachromeAgentHost::OnAgentResult(
    const std::string& task_instruction,
    const std::string& target_css_selector,
    int visual_iters,
    StepCallback done,
    mojom::AgentResultPtr result) {
  capture_client_->ReleasePending();

  if (!result || !result->ok || !result->action) {
    std::move(done).Run(false, result ? result->error : "null_result", nullptr);
    return;
  }

  // Strategy 3: model could not locate the target in-frame.
  if (result->action->type == mojom::AgentActionType::kElementNotFound) {
    std::move(done).Run(false, "element_not_found",
                        std::move(result->action));
    return;
  }

  mojom::AgentActionPtr located = std::move(result->action);
  if (located->type != mojom::AgentActionType::kClick) {
    std::move(done).Run(false, "locate_failed", std::move(located));
    return;
  }
  DispatchParsedCommand(task_instruction, target_css_selector, visual_iters,
                        std::move(done), std::move(located));
}

void SimulachromeAgentHost::OnActionApplied(
    const std::string& task_instruction,
    const std::string& target_css_selector,
    int visual_iters,
    StepCallback done,
    mojom::AgentActionPtr action,
    bool injected,
    std::string inject_error) {
  if (!injected) {
    std::move(done).Run(
        false,
        inject_error.empty() ? "input_inject_failed" : inject_error,
        std::move(action));
    return;
  }

  // On a successful click, `inject_error` carries the element that was under
  // the point. Other actions leave it empty.
  std::move(done).Run(true, std::move(inject_error), std::move(action));
}

// ---------------------------------------------------------------------------
// Planned batches: manager composes the whole action, the model only locates.
// ---------------------------------------------------------------------------

namespace {

LocateResult LocateFailure(std::string error) {
  LocateResult r;
  r.ok = false;
  r.error = std::move(error);
  return r;
}

// Mirrors the renderer's DescribeClickedElement label so GradeHit sees the
// same shape whichever side produced it:  tag[ type=..][ role=..][ id=..]["name"]
constexpr char kProbeScript[] = R"((function(pts) {
  function d(e) {
    var t = (e.tagName || '').toLowerCase();
    var s = t;
    if (t === 'input') {
      var ty = e.getAttribute('type');
      if (ty) s += ' type=' + ty.slice(0, 24);
    }
    var r = e.getAttribute('role');
    if (r) s += ' role=' + r.slice(0, 24);
    if (e.id) s += ' id=' + String(e.id).slice(0, 40);
    if (t === 'html' || t === 'body') return s;
    var n = e.getAttribute('aria-label') || e.getAttribute('placeholder') ||
        e.getAttribute('alt') || e.getAttribute('title') ||
        e.getAttribute('name') || (e.textContent || '');
    n = String(n).replace(/\s+/g, ' ').trim().slice(0, 60);
    if (n) s += ' "' + n + '"';
    return s;
  }
  try {
    return pts.map(function(p) {
      var es = document.elementsFromPoint(p[0], p[1]) || [];
      return es.slice(0, 3).map(d);
    });
  } catch (e) {
    return [];
  }
})(%s))";

int GradeRank(ClickGrade g) {
  switch (g) {
    case ClickGrade::kA:
      return 3;
    case ClickGrade::kB:
      return 2;
    case ClickGrade::kC:
      return 1;
    case ClickGrade::kUnknown:
      return 0;
  }
  return 0;
}

bool IsPointerOpForProbe(mojom::AgentActionType type) {
  return type == mojom::AgentActionType::kClick ||
         type == mojom::AgentActionType::kDoubleClick ||
         type == mojom::AgentActionType::kRightClick ||
         type == mojom::AgentActionType::kHover ||
         type == mojom::AgentActionType::kType;
}

}  // namespace

void SimulachromeAgentHost::SubmitPlannedBatch(
    std::vector<PlannedAction> actions,
    PlannedBatchOptions options,
    PlannedActionRunner::ActionCallback on_action,
    PlannedActionRunner::BatchCallback on_done) {
  planned_runner_.Submit(std::move(actions), options, std::move(on_action),
                         std::move(on_done));
}

void SimulachromeAgentHost::CancelPlannedBatches() {
  planned_runner_.CancelAll();
}

void SimulachromeAgentHost::LocateTarget(const PlannedTarget& target,
                                         LocateCallback done) {
  if (!web_contents_) {
    std::move(done).Run(LocateFailure("no_web_contents"));
    return;
  }
  if (legacy_steps_in_flight_ > 0) {
    std::move(done).Run(LocateFailure("step_in_progress"));
    return;
  }
  EnsureService();
  if (!agent_.is_bound()) {
    std::move(done).Run(LocateFailure("agent_service_unavailable"));
    return;
  }
  // The only sentence the model sees: no verb, no text, no tokens.
  GroundedCommand locate;
  locate.kind = GroundedCommand::Kind::kClick;
  locate.target = target.describe;
  locate.needs_target = true;
  ViewportEnsureVisible::Ensure(
      web_contents_, target.css_selector,
      base::BindOnce(&SimulachromeAgentHost::OnLocateViewportReady,
                     weak_factory_.GetWeakPtr(), target.slot,
                     target.css_selector, LocatePromptFor(locate),
                     std::move(done)));
}

void SimulachromeAgentHost::OnLocateViewportReady(
    std::string slot,
    std::string css_selector,
    std::string prompt,
    LocateCallback done,
    ViewportEnsureVisible::Result viewport_result) {
  if (viewport_result == ViewportEnsureVisible::Result::kNotFound &&
      !css_selector.empty()) {
    std::move(done).Run(
        LocateFailure("element_not_found:selector_not_in_dom"));
    return;
  }
  settle_tracker_->WaitUntilSettled(
      base::Milliseconds(200),
      base::BindOnce(&SimulachromeAgentHost::OnLocateSettled,
                     weak_factory_.GetWeakPtr(), std::move(slot),
                     std::move(prompt), std::move(done)));
}

void SimulachromeAgentHost::OnLocateSettled(std::string slot,
                                            std::string prompt,
                                            LocateCallback done) {
  capture_client_->CaptureFrameTexture(
      base::BindOnce(&SimulachromeAgentHost::OnLocateCaptured,
                     weak_factory_.GetWeakPtr(), std::move(slot),
                     std::move(prompt), std::move(done)));
}

void SimulachromeAgentHost::OnLocateCaptured(std::string slot,
                                             std::string prompt,
                                             LocateCallback done,
                                             mojom::FrameTexturePtr frame,
                                             std::string capture_error) {
  if (!frame || !capture_error.empty()) {
    capture_client_->ReleasePending();
    std::move(done).Run(LocateFailure(
        capture_error.empty() ? "capture_failed" : capture_error));
    return;
  }
  // The utility can die during the settle/capture gap. A fresh service has no
  // weights, so fail rather than relaunch; the manager reloads the model.
  if (!agent_.is_bound()) {
    capture_client_->ReleasePending();
    std::move(done).Run(LocateFailure("agent_service_unavailable"));
    return;
  }
  agent_->ExecuteAction(
      prompt, std::move(frame),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&SimulachromeAgentHost::OnLocateAgentResult,
                         weak_factory_.GetWeakPtr(), std::move(slot),
                         std::move(done)),
          nullptr));
}

void SimulachromeAgentHost::OnLocateAgentResult(std::string slot,
                                                LocateCallback done,
                                                mojom::AgentResultPtr result) {
  capture_client_->ReleasePending();
  if (!result || !result->ok || !result->action) {
    std::move(done).Run(
        LocateFailure(result && !result->error.empty() ? result->error
                                                       : "null_result"));
    return;
  }
  if (result->action->type == mojom::AgentActionType::kElementNotFound) {
    std::move(done).Run(LocateFailure("element_not_found"));
    return;
  }
  // The model's grammar only allows click. Anything else is a failed locate.
  if (result->action->type != mojom::AgentActionType::kClick) {
    std::move(done).Run(LocateFailure("locate_failed"));
    return;
  }
  LocateResult out;
  out.ok = true;
  out.point.slot = std::move(slot);
  out.point.x = result->action->x;
  out.point.y = result->action->y;
  out.point.confidence = result->action->confidence;
  std::move(done).Run(std::move(out));
}

void SimulachromeAgentHost::ProbePoints(
    const std::vector<gfx::Point>& dip_points,
    ProbeCallback done) {
  content::RenderFrameHost* rfh =
      web_contents_ ? web_contents_->GetPrimaryMainFrame() : nullptr;
  if (!rfh || !rfh->IsRenderFrameLive() || dip_points.empty()) {
    std::move(done).Run({});
    return;
  }
  base::Value::List pts;
  for (const gfx::Point& p : dip_points) {
    base::Value::List pair;
    pair.Append(p.x());
    pair.Append(p.y());
    pts.Append(std::move(pair));
  }
  std::string json;
  if (!base::JSONWriter::Write(base::Value(std::move(pts)), &json)) {
    std::move(done).Run({});
    return;
  }
  // Avoid StringPrintf: the script contains literal '%' characters.
  std::string js = kProbeScript;
  const size_t pos = js.rfind("%s");
  if (pos == std::string::npos) {
    std::move(done).Run({});
    return;
  }
  js.replace(pos, 2, json);
  rfh->ExecuteJavaScriptForTests(
      base::UTF8ToUTF16(js),
      base::BindOnce(
          [](ProbeCallback done, base::Value result) {
            std::vector<std::vector<std::string>> out;
            if (result.is_list()) {
              for (const base::Value& per_point : result.GetList()) {
                std::vector<std::string> labels;
                if (per_point.is_list()) {
                  for (const base::Value& label : per_point.GetList()) {
                    if (label.is_string()) {
                      labels.push_back(label.GetString());
                    }
                  }
                }
                out.push_back(std::move(labels));
              }
            }
            std::move(done).Run(std::move(out));
          },
          std::move(done)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void SimulachromeAgentHost::RefinePoint(const PlannedAction& action,
                                        const LocateResult& located,
                                        RefineCallback done) {
  RefinedPoint passthrough;
  passthrough.point = located.point;
  int cx = 0;
  int cy = 0;
  if (!web_contents_ ||
      !AgentInputInjector::NormalizedToWidgetDip(
          web_contents_, located.point.x, located.point.y, &cx, &cy)) {
    std::move(done).Run(std::move(passthrough));
    return;
  }
  // The raw point first, then rings of 8 around it. Rings are small on purpose:
  // this corrects a near miss onto the control the model meant; it is not a
  // search for some other control.
  std::vector<gfx::Point> points = {gfx::Point(cx, cy)};
  static constexpr int kRadii[] = {8, 20};
  static constexpr int kDirs[8][2] = {{1, 0},  {-1, 0}, {0, 1},  {0, -1},
                                      {1, 1},  {-1, 1}, {1, -1}, {-1, -1}};
  for (int radius : kRadii) {
    for (const auto& dir : kDirs) {
      const int px = cx + dir[0] * radius;
      const int py = cy + dir[1] * radius;
      if (px >= 0 && py >= 0) {
        points.emplace_back(px, py);
      }
    }
  }
  ProbePoints(points,
              base::BindOnce(&SimulachromeAgentHost::OnRefineProbed,
                             weak_factory_.GetWeakPtr(), action.expect,
                             located.point, points, std::move(done)));
}

void SimulachromeAgentHost::OnRefineProbed(
    ExpectedHit expect,
    PlannedPoint raw,
    std::vector<gfx::Point> points,
    RefineCallback done,
    std::vector<std::vector<std::string>> labels) {
  RefinedPoint out;
  out.point = raw;
  if (labels.size() != points.size() || labels.empty()) {
    // Probe unavailable (e.g. the point is inside a cross-process frame).
    // Report nothing rather than guess; the raw point is used.
    std::move(done).Run(std::move(out));
    return;
  }

  struct Scored {
    ClickGrade grade = ClickGrade::kUnknown;
    std::string label;
  };
  auto score = [&expect](const std::vector<std::string>& stack) {
    Scored best;
    best.label = stack.empty() ? std::string() : stack.front();
    best.grade = GradeHit(expect, best.label);
    for (const std::string& label : stack) {
      const ClickGrade g = GradeHit(expect, label);
      if (GradeRank(g) > GradeRank(best.grade)) {
        best.grade = g;
        best.label = label;
      }
    }
    return best;
  };

  const Scored center = score(labels[0]);
  out.grade = center.grade;
  out.label = center.label;
  size_t chosen = 0;
  // Keep the raw point when it already satisfies the expectation. Otherwise
  // take the closest ring point that does. A partial match (B) at the raw point
  // is only replaced by a full match, never by another partial one.
  if (center.grade != ClickGrade::kA) {
    for (size_t i = 1; i < labels.size(); ++i) {
      const Scored s = score(labels[i]);
      if (s.grade == ClickGrade::kA) {
        chosen = i;
        out.grade = s.grade;
        out.label = s.label;
        break;
      }
      if (center.grade == ClickGrade::kC && s.grade == ClickGrade::kB &&
          chosen == 0) {
        chosen = i;
        out.grade = s.grade;
        out.label = s.label;
        // Keep scanning: a later A beats this B.
      }
    }
  }
  if (chosen != 0) {
    content::RenderWidgetHostView* view =
        web_contents_ ? web_contents_->GetRenderWidgetHostView() : nullptr;
    if (view) {
      const gfx::Size size = view->GetViewBounds().size();
      const float w = static_cast<float>(std::max(1, size.width() - 1));
      const float h = static_cast<float>(std::max(1, size.height() - 1));
      out.point.x = std::clamp(points[chosen].x() / w, 0.f, 1.f);
      out.point.y = std::clamp(points[chosen].y() / h, 0.f, 1.f);
      out.snapped = true;
      LOG(INFO) << "[SimulachromeAgentHost] graded click snapped "
                << "(" << points[0].x() << "," << points[0].y() << ") -> ("
                << points[chosen].x() << "," << points[chosen].y()
                << ") grade=" << ClickGradeName(out.grade);
    }
  }
  std::move(done).Run(std::move(out));
}

void SimulachromeAgentHost::Dispatch(const PlannedAction& action,
                                     const std::vector<PlannedPoint>& points,
                                     DispatchCallback done) {
  auto fail = [&done](const char* error) {
    DispatchResult r;
    r.ok = false;
    r.error = error;
    std::move(done).Run(std::move(r));
  };
  if (!web_contents_) {
    fail("no_web_contents");
    return;
  }

  auto a = mojom::AgentAction::New();
  if (!points.empty()) {
    a->x = points[0].x;
    a->y = points[0].y;
    a->confidence = points[0].confidence;
  }
  switch (action.op) {
    case PlannedOp::kClick:
      a->type = mojom::AgentActionType::kClick;
      break;
    case PlannedOp::kDoubleClick:
      a->type = mojom::AgentActionType::kDoubleClick;
      break;
    case PlannedOp::kRightClick:
      a->type = mojom::AgentActionType::kRightClick;
      break;
    case PlannedOp::kHover:
      a->type = mojom::AgentActionType::kHover;
      break;
    case PlannedOp::kType:
      a->type = mojom::AgentActionType::kType;
      a->payload = action.text;
      if (action.submit) {
        // A trailing newline is the existing "then press Enter" convention
        // shared by the bridge and the injector.
        a->payload += "\n";
      }
      break;
    case PlannedOp::kKey:
      a->type = mojom::AgentActionType::kKeyPress;
      a->payload = action.key;
      break;
    case PlannedOp::kScroll:
      a->type = mojom::AgentActionType::kScroll;
      a->x = 0.5f;
      a->y = 0.5f;
      a->scroll_dy = action.scroll_dy;
      break;
    case PlannedOp::kDrag:
      if (points.size() < 2) {
        fail("drag_missing_target");
        return;
      }
      a->type = mojom::AgentActionType::kDrag;
      a->end_x = points[1].x;
      a->end_y = points[1].y;
      break;
    case PlannedOp::kWait:
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](DispatchCallback done) {
                DispatchResult r;
                r.ok = true;
                std::move(done).Run(std::move(r));
              },
              std::move(done)),
          base::Milliseconds(action.wait_ms));
      return;
  }

  // Harvest what is under the pointer right before it moves. This is the same
  // metadata the recorder and playback targeting want, and it is the grading
  // evidence. It is read-only, so it costs one JS round trip and nothing else.
  if (IsPointerOpForProbe(a->type)) {
    int dx = 0;
    int dy = 0;
    if (AgentInputInjector::NormalizedToWidgetDip(web_contents_, a->x, a->y,
                                                  &dx, &dy)) {
      ProbePoints(
          {gfx::Point(dx, dy)},
          base::BindOnce(
              [](base::WeakPtr<SimulachromeAgentHost> self,
                 mojom::AgentActionPtr action, DispatchCallback done,
                 std::vector<std::vector<std::string>> labels) {
                if (!self) {
                  DispatchResult r;
                  r.error = "host_destroyed";
                  std::move(done).Run(std::move(r));
                  return;
                }
                std::string label;
                if (!labels.empty() && !labels[0].empty()) {
                  label = labels[0][0];
                }
                self->DispatchPlannedAction(std::move(action), std::move(label),
                                            std::move(done));
              },
              weak_factory_.GetWeakPtr(), std::move(a), std::move(done)));
      return;
    }
  }
  DispatchPlannedAction(std::move(a), std::string(), std::move(done));
}

void SimulachromeAgentHost::DispatchPlannedAction(mojom::AgentActionPtr action,
                                                  std::string pre_click_label,
                                                  DispatchCallback done) {
  if (!web_contents_) {
    DispatchResult r;
    r.error = "no_web_contents";
    std::move(done).Run(std::move(r));
    return;
  }
  LOG(INFO) << "[SimulachromeAgentHost] planned dispatch type="
            << static_cast<int>(action->type) << " norm=(" << action->x << ","
            << action->y << ") pre=\"" << pre_click_label << "\"";
  mojom::AgentActionPtr copy = action.Clone();
  AgentInputInjector::ApplyAction(
      web_contents_, *copy,
      base::BindOnce(&SimulachromeAgentHost::OnPlannedApplied,
                     weak_factory_.GetWeakPtr(), std::move(pre_click_label),
                     std::move(done)));
}

void SimulachromeAgentHost::OnPlannedApplied(std::string pre_click_label,
                                             DispatchCallback done,
                                             bool injected,
                                             std::string message) {
  DispatchResult r;
  r.ok = injected;
  if (injected) {
    // The Blink tool path replies with the element it actually clicked. The
    // executor path replies with nothing, so use the pre-dispatch probe.
    r.hit_element = message.empty() ? pre_click_label : message;
  } else {
    r.error = message.empty() ? "input_inject_failed" : message;
    r.hit_element = pre_click_label;
  }
  std::move(done).Run(std::move(r));
}

}  // namespace simulachrome

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_SIMULACHROME_VIZ_FRAME_MAILBOX_CAPTURE_H_
#define COMPONENTS_VIZ_SERVICE_SIMULACHROME_VIZ_FRAME_MAILBOX_CAPTURE_H_

#include <string>

#include "base/functional/callback.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "ui/gfx/geometry/size.h"

namespace viz {

// ---------------------------------------------------------------------------
// INJECTION POINTS (do not call from renderer):
//
// 1) cc::LayerTreeHostImpl::NotifyReadyToActivate / DidDrawAllLayers
//    - Prefer capturing after a frame that had no scroll/animation damage.
//
// 2) viz::Display::DrawAndSwap / DirectRenderer::DrawFrame
//    - After the root pass is drawn, the backbuffer / root SharedImage is
//      available via SkiaOutputSurface / AggregatedFrame.
//
// Recommended production approach:
//   - Create (or reuse) a SharedImage with DISPLAY_READ | GLES2_READ usage.
//   - Copy the root render pass into that SharedImage (CopyOutput / blit).
//   - Produce gpu::Mailbox + gpu::SyncToken and bounce them to the browser
//     process via an existing Viz Host frame sink Mojo extension, then onward
//     to SimulachromeAgentHost.
//
// This header documents the capture API the browser FrameCaptureClient will
// eventually call through a browser↔viz Mojo bridge (not yet landed).
// ---------------------------------------------------------------------------

struct CapturedFrameMailbox {
  gpu::Mailbox mailbox;
  gpu::SyncToken sync_token;
  gfx::Size size_px;
  float device_scale_factor = 1.f;
};

using CaptureMailboxCallback =
    base::OnceCallback<void(bool ok, CapturedFrameMailbox frame, std::string error)>;

// Stub API. Implement against SkiaOutputSurface::MakePromiseSharedImage /
// CopyOutputRequest on the root surface.
class VizFrameMailboxCapture {
 public:
  // Hook to call from viz::Display after DrawAndSwap when a capture is armed.
  static void CaptureRootFrameAfterSwap(CaptureMailboxCallback callback);

  // Call from LayerTreeHostImpl / Display when scroll or CSS transitions run
  // so the browser VisualSettleTracker can reset its 200ms quiet window.
  static void NotifyVisualActivity();
};

}  // namespace viz

#endif  // COMPONENTS_VIZ_SERVICE_SIMULACHROME_VIZ_FRAME_MAILBOX_CAPTURE_H_

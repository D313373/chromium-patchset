// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/simulachrome/viz_frame_mailbox_capture.h"

#include "base/logging.h"

namespace viz {

// static
void VizFrameMailboxCapture::CaptureRootFrameAfterSwap(
    CaptureMailboxCallback callback) {
  // Scaffolding: no SharedImage export yet.
  //
  // Pseudocode for the real hook inside viz::Display::DrawAndSwap:
  //
  //   auto* sii = GetSharedImageInterface();
  //   gpu::SharedImageMetadata meta = ...;  // BGRA, size = surface size
  //   gpu::Mailbox mailbox = sii->CreateSharedImage(...);
  //   // blit / CopyOutput root pass -> mailbox
  //   gpu::SyncToken token = sii->GenVerifiedSyncToken();
  //   callback.Run(true, {mailbox, token, size, dsf}, "");
  //
  CapturedFrameMailbox empty;
  LOG(WARNING) << "[VizFrameMailboxCapture] stub — wire SharedImage export";
  std::move(callback).Run(false, empty, "not_implemented");
}

// static
void VizFrameMailboxCapture::NotifyVisualActivity() {
  // TODO: Mojo ping to browser VisualSettleTracker::NotifyVisualActivity.
}

}  // namespace viz

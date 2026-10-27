// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/visual_settle_tracker.h"

#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace simulachrome {

VisualSettleTracker::VisualSettleTracker(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents) {}

VisualSettleTracker::~VisualSettleTracker() = default;

void VisualSettleTracker::WaitUntilSettled(base::TimeDelta quiet_for,
                                           base::OnceClosure done) {
  quiet_for_ = quiet_for.is_positive() ? quiet_for : base::Milliseconds(200);
  pending_done_ = std::move(done);
  ArmQuietTimer();
}

void VisualSettleTracker::NotifyVisualActivity() {
  if (!pending_done_) {
    return;
  }
  ArmQuietTimer();
}

void VisualSettleTracker::DidFinishLoad(
    content::RenderFrameHost* render_frame_host,
    const GURL& validated_url) {
  (void)render_frame_host;
  (void)validated_url;
  NotifyVisualActivity();
}

void VisualSettleTracker::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  (void)navigation_handle;
  NotifyVisualActivity();
}

void VisualSettleTracker::ArmQuietTimer() {
  quiet_timer_.Stop();
  quiet_timer_.Start(FROM_HERE, quiet_for_,
                     base::BindOnce(&VisualSettleTracker::OnQuietTimerFired,
                                    weak_factory_.GetWeakPtr()));
}

void VisualSettleTracker::OnQuietTimerFired() {
  if (!pending_done_) {
    return;
  }
  std::move(pending_done_).Run();
}

}  // namespace simulachrome

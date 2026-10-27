// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_VISUAL_SETTLE_TRACKER_H_
#define CHROME_BROWSER_SIMULACHROME_VISUAL_SETTLE_TRACKER_H_

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class WebContents;
}

namespace simulachrome {

// Tracks scroll / load / visual activity and fires when quiet for |quiet_for|.
// Default policy: 200ms with no DidFinishLoad churn and no scroll markers.
class VisualSettleTracker : public content::WebContentsObserver {
 public:
  explicit VisualSettleTracker(content::WebContents* web_contents);
  ~VisualSettleTracker() override;

  void WaitUntilSettled(base::TimeDelta quiet_for,
                        base::OnceClosure done);

  // Call from compositor / scroll hooks when motion is observed.
  void NotifyVisualActivity();

  // content::WebContentsObserver:
  void DidFinishLoad(content::RenderFrameHost* render_frame_host,
                     const GURL& validated_url) override;
  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override;

 private:
  void OnQuietTimerFired();
  void ArmQuietTimer();

  base::TimeDelta quiet_for_ = base::Milliseconds(200);
  base::OneShotTimer quiet_timer_;
  base::OnceClosure pending_done_;
  base::WeakPtrFactory<VisualSettleTracker> weak_factory_{this};
};

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_VISUAL_SETTLE_TRACKER_H_

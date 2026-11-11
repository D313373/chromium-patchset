// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/simulachrome_startup.h"

#import <Foundation/Foundation.h>

namespace simulachrome {

void HoldForegroundActivity() {
  // Held for the process lifetime. Releasing the token lets App Nap suspend
  // the browser as soon as every window is covered, which freezes the agent
  // loop until the window is ordered front again.
  static id activity = nil;
  if (activity) {
    return;
  }
  const NSActivityOptions options =
      (NSActivityUserInitiatedAllowingIdleSystemSleep |
       NSActivityLatencyCritical) &
      ~(NSActivitySuddenTerminationDisabled |
        NSActivityAutomaticTerminationDisabled);
  activity = [NSProcessInfo.processInfo beginActivityWithOptions:options
                                                          reason:@"Simulachrome agent"];
}

}  // namespace simulachrome

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_VIEWPORT_ENSURE_VISIBLE_H_
#define CHROME_BROWSER_SIMULACHROME_VIEWPORT_ENSURE_VISIBLE_H_

#include <string>

#include "base/functional/callback.h"

namespace content {
class WebContents;
}

namespace simulachrome {

// Strategy 2 (Planner / DOM pre-flight): before mailbox capture, ensure a
// candidate element is inside the VisualViewport. If its layout box is
// off-screen, silently scrollIntoView({behavior:'instant'}) so smolvlm2
// always sees the target and does not burn inference cycles scrolling.
class ViewportEnsureVisible {
 public:
  enum class Result {
    kNoSelector,   // caller had no DOM/A11y target — skip
    kNotFound,     // selector matched nothing
    kAlreadyVisible,
    kScrolled,     // scrollIntoView ran; caller should re-settle
  };

  using DoneCallback = base::OnceCallback<void(Result result)>;

  // |css_selector| empty ⇒ kNoSelector immediately (no JS).
  // Otherwise queries document.querySelector in the primary main frame.
  static void Ensure(content::WebContents* web_contents,
                     const std::string& css_selector,
                     DoneCallback done);
};

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_VIEWPORT_ENSURE_VISIBLE_H_

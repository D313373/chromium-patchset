// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_AGENT_INPUT_INJECTOR_H_
#define CHROME_BROWSER_SIMULACHROME_AGENT_INPUT_INJECTOR_H_

#include "base/functional/callback.h"
#include "services/simulachrome/public/mojom/simulachrome_agent.mojom.h"

namespace content {
class WebContents;
}

namespace simulachrome {

// Translates normalized [0,1] AgentAction coords into CSS DIPs and synthesizes
// trusted blink input. Clicks go through ChromeRenderFrame.InvokeTool so the
// renderer ClickTool calls WebWidget::HandleInputEvent.
//
// The browser installs an override so grounded steps run through
// ScriptExecutionService instead of this injector. Without an override, the
// injector remains the fallback.
class AgentInputInjector {
 public:
  // |ok| is false when the action was not delivered. |error| is the reason.
  // Clicks complete asynchronously, after the renderer tool replies, so the
  // caller must not block the UI thread waiting on this.
  using ApplyCallback = base::OnceCallback<void(bool ok, std::string error)>;

  // When set, ApplyAction hands the grounded step to this callback and does
  // not inject. The dashboard bridge runs the step on ScriptExecutor.
  using ApplyOverride = base::RepeatingCallback<void(
      content::WebContents* web_contents,
      const mojom::AgentAction& action,
      ApplyCallback done)>;

  static void SetApplyOverride(ApplyOverride override);

  // Rewrites a type payload with the script-executor token catalog before the
  // text is injected. Installed by the dashboard bridge. Empty leaves the
  // payload unchanged.
  using TokenExpander = base::RepeatingCallback<std::string(
      content::WebContents* web_contents,
      const std::string& text)>;
  static void SetTokenExpander(TokenExpander expander);

  static void ApplyAction(content::WebContents* web_contents,
                          const mojom::AgentAction& action,
                          ApplyCallback done);

  // Normalize [0,1] -> widget DIP, accounting for DPR only insofar as the
  // host view's CSS size is already in DIPs (PositionInWidget space).
  static bool NormalizedToWidgetDip(content::WebContents* web_contents,
                                    float norm_x,
                                    float norm_y,
                                    int* out_x,
                                    int* out_y);
};

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_AGENT_INPUT_INJECTOR_H_

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_EMBEDDED_EXECUTOR_BRIDGE_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_EMBEDDED_EXECUTOR_BRIDGE_H_

namespace simulachrome {

// Grounded smolvlm2 steps (click, type, scroll, key) run as a one-shot
// ScriptExecutionService timeline, the same ad-hoc API the task completer
// uses. Tokens in typed text expand there. Call once from browser startup.
void InstallEmbeddedExecutorBridge();

}  // namespace simulachrome

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_EMBEDDED_EXECUTOR_BRIDGE_H_

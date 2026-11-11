// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_SIMULACHROME_STARTUP_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_SIMULACHROME_STARTUP_H_

namespace simulachrome {

// Applies Simulachrome product defaults to the browser process command line
// before the message loop starts. Safe to call multiple times; existing
// explicit switches win.
//
// Defaults (when unset):
// - enable Glic as one window-scoped panel (dockable, not per-tab)
// - disable GlicMultiInstance (per-tab instances)
// - disable occluded-window backgrounding / background timer throttling
// - keep the renderer process at foreground priority
// - on Mac, hold an App Nap assertion so a covered window does not freeze
//   the agent (the window is not ordered front)
// - guest URL chrome://simulachrome-agent/ollama.html (packaged agent UI)
// - apply restart-required switches from global_settings.json when present
// - remote debugging ONLY when subsystem.remoteDebuggingEnabled is true
void ApplyBrowserCommandLineDefaults();

// Keeps this process out of App Nap for its lifetime. No-op off Mac, and a
// no-op if an assertion is already held. Does not activate any window.
void HoldForegroundActivity();

}  // namespace simulachrome

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_SIMULACHROME_STARTUP_H_

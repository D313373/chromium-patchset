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
// - force visible widgets (automation reliability)
// - guest URL chrome://simulachrome-agent/ollama.html (packaged agent UI)
// - apply restart-required switches from global_settings.json when present
// - remote debugging ONLY when subsystem.remoteDebuggingEnabled is true
void ApplyBrowserCommandLineDefaults();

}  // namespace simulachrome

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_SIMULACHROME_STARTUP_H_

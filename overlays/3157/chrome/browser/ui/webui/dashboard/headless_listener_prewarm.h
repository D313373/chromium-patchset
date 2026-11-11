// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_PREWARM_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_PREWARM_H_

#include "base/functional/callback.h"
#include "base/values.h"

class Profile;

namespace dashboard {

// Fire-and-forget: ensure the isolated headless Chromium (:9222) is already
// running with about:blank so the first listener/heal start is not cold.
void MaybePrewarmHeadlessListenerHost(Profile* profile);

// Ensure the C++-owned headless Chromium is up, then run page investigation.
// Never opens tabs in the headed browser. Full investigate automation over
// the native control channel is still being wired; callers may receive a
// structured not-implemented response after a successful warm.
void InvestigatePageOnHeadlessListener(
    Profile* profile,
    base::Value::Dict payload,
    base::OnceCallback<void(base::Value::Dict)> callback);

void ExecuteActionsOnHeadlessListener(
    Profile* profile,
    base::Value::Dict payload,
    base::OnceCallback<void(base::Value::Dict)> callback);

void RunScriptOnHeadlessListener(
    Profile* profile,
    base::Value::Dict payload,
    base::OnceCallback<void(base::Value::Dict)> callback);

void ResumeFromStepOnHeadlessListener(
    Profile* profile,
    base::Value::Dict payload,
    base::OnceCallback<void(base::Value::Dict)> callback);

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_PREWARM_H_

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_CONTROLLER_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_CONTROLLER_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

class Profile;

namespace dashboard {

// Headed-process owner of the isolated --headless-listener Chromium child.
// Launches the sibling process, speaks length-prefixed JSON on a local IPC
// socket (Unix domain on POSIX; Windows named-pipe TBD), and forwards pushed
// trigger events for onMatch playback in this process. Not Mojo and not a
// Chrome Native Messaging Host.
class HeadlessListenerController {
 public:
  using ResponseCallback =
      base::OnceCallback<void(bool ok, base::Value::Dict response)>;
  using TriggerCallback =
      base::RepeatingCallback<void(base::Value::Dict trigger_event)>;

  static HeadlessListenerController* GetForProfile(Profile* profile);

  explicit HeadlessListenerController(Profile* profile);
  ~HeadlessListenerController();

  HeadlessListenerController(const HeadlessListenerController&) = delete;
  HeadlessListenerController& operator=(const HeadlessListenerController&) =
      delete;

  void SetTriggerCallback(TriggerCallback callback);

  // Ensure the headless child is running and the control channel is up.
  void EnsureReady(base::OnceCallback<void(bool ok, std::string error)> done);

  void StartListener(const std::string& script_id, ResponseCallback done);
  void StopListener(const std::string& script_id, ResponseCallback done);
  void ListActive(ResponseCallback done);
  void Warm(ResponseCallback done);
  void ClearPendingTriggers(ResponseCallback done);
  // IPC: investigate_page — payload may include url / actions / probe /
  // key_script_id.
  void InvestigatePage(base::Value::Dict payload, ResponseCallback done);
  // IPC: execute_actions — run click/type/wait/... on a retained target.
  void ExecuteActions(base::Value::Dict payload, ResponseCallback done);
  // IPC: run_script — run a saved script on a retained/new headless target.
  void RunScript(base::Value::Dict payload, ResponseCallback done);
  // IPC: resume_from_step — resume with automatic KEY fallback.
  void ResumeFromStep(base::Value::Dict payload, ResponseCallback done);
  void RefreshListener(const std::string& script_id, ResponseCallback done);
  void CaptureScreenshot(const std::string& script_id, ResponseCallback done);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_CONTROLLER_H_

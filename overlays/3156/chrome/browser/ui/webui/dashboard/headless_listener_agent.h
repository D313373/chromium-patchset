// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_AGENT_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_AGENT_H_

#include <memory>

#include "base/no_destructor.h"

class Profile;

namespace dashboard {

// Runs inside the --headless-listener Chromium process. Accepts the headed
// control socket, owns ListenerManager for full StartListener pipelines
// (navigate / setup / key login / ArmListen), and pushes match triggers back
// to the headed browser for onMatch playback.
class HeadlessListenerAgent {
 public:
  static HeadlessListenerAgent& GetInstance();

  // Start accepting connections once a Profile is ready. No-op unless the
  // process was launched with --headless-listener.
  void MaybeStartForProfile(Profile* profile);

  void Shutdown();

 private:
  friend class base::NoDestructor<HeadlessListenerAgent>;
  HeadlessListenerAgent();
  ~HeadlessListenerAgent();

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_AGENT_H_

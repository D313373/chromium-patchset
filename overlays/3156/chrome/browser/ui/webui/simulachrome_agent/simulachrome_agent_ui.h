// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SIMULACHROME_AGENT_SIMULACHROME_AGENT_UI_H_
#define CHROME_BROWSER_UI_WEBUI_SIMULACHROME_AGENT_SIMULACHROME_AGENT_UI_H_

#include <memory>

#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

namespace chrome {
inline constexpr char kChromeUISimulachromeAgentHost[] = "simulachrome-agent";
inline constexpr char kChromeUISimulachromeAgentURL[] =
    "chrome://simulachrome-agent/";
}

class SimulachromeAgentUI;

class SimulachromeAgentUIConfig
    : public content::DefaultWebUIConfig<SimulachromeAgentUI> {
 public:
  SimulachromeAgentUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUISimulachromeAgentHost) {}
};

// Serves the packaged Glic agent client (ollama.html + assets) from
// Resources/simulachrome_agent or SIMULACHROME_AGENT_DIR.
// Also hosts DashboardUIMessageHandler so the guest can call
// dashboardAgentOllamaChat in-process (no serve.py sidecar).
class SimulachromeAgentUI : public content::WebUIController {
 public:
  explicit SimulachromeAgentUI(content::WebUI* web_ui);
  ~SimulachromeAgentUI() override;

  SimulachromeAgentUI(const SimulachromeAgentUI&) = delete;
  SimulachromeAgentUI& operator=(const SimulachromeAgentUI&) = delete;

 private:
  std::unique_ptr<dashboard::Dashboard> agent_chat_dashboard_;
};

#endif  // CHROME_BROWSER_UI_WEBUI_SIMULACHROME_AGENT_SIMULACHROME_AGENT_UI_H_

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SIMULACHROME_SIMULACHROME_WEB_UI_MESSAGE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_SIMULACHROME_SIMULACHROME_WEB_UI_MESSAGE_HANDLER_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/simulachrome/planned_action_runner.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "services/simulachrome/public/mojom/simulachrome_agent.mojom.h"

class Profile;

namespace content {
class WebContents;
}

namespace simulachrome {
class SimulachromeAgentHost;
}

namespace simulachrome_webui {

// Bridges the custom Simulachrome / Glic-replacement panel JS to C++:
//   chrome.send("execute_simulachrome_action",
//               [instruction, callbackId?, cssSelector?])
//     → Strategy2 scrollIntoView(selector) if provided
//     → capture active tab (SharedImage mailbox) via SimulachromeAgentHost
//     → utility VLM → AgentInputInjector; Strategy1 re-loops on scroll
class SimulachromeWebUIMessageHandler : public content::WebUIMessageHandler {
 public:
  explicit SimulachromeWebUIMessageHandler(Profile* profile);
  ~SimulachromeWebUIMessageHandler() override;

  SimulachromeWebUIMessageHandler(const SimulachromeWebUIMessageHandler&) =
      delete;
  SimulachromeWebUIMessageHandler& operator=(
      const SimulachromeWebUIMessageHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  // JS: chrome.send("execute_simulachrome_action",
  //                 ["Click Submit", callbackId?, "#submit-btn"?])
  void HandleExecuteSimulachromeAction(const base::Value::List& args);

  // Batch page tasks, composed in full by the manager. The embedded model is
  // asked only to locate each control, one at a time; every action fires as
  // soon as its own targets resolve. Batches queue FIFO per target tab.
  // JS: sendWithPromise("submit_simulachrome_batch",
  //       {actions: [{op, target|from+to, args, expect}], stop_on_failure})
  //   resolves {ok, failed_index, results:[{index,id,op,status,error?,
  //             hit_element?,grade?,snapped?,raw_fallback?,warning?,points?}]}
  // Also accepts the batch as a JSON string for bridges that only forward text.
  void HandleSubmitSimulachromeBatch(const base::Value::List& args);
  // JS: chrome.send("cancel_simulachrome_batch", [callbackId?])
  void HandleCancelSimulachromeBatch(const base::Value::List& args);

  void OnBatchComplete(std::string callback_id,
                       simulachrome::PlannedBatchResult result);

  // Optional: preload weights once.
  // JS: chrome.send("load_simulachrome_model", [modelDirPath, callbackName?])
  void HandleLoadSimulachromeModel(const base::Value::List& args);

  // JS: sendWithPromise("capture_simulachrome_frame", {tab_id?})
  // Mailbox capture for a vision model that is not the embedded VLM.
  void HandleCaptureSimulachromeFrame(const base::Value::List& args);

  content::WebContents* ResolveTargetWebContents();
  simulachrome::SimulachromeAgentHost* EnsureAgentHost(
      content::WebContents* target);

  void OnStepComplete(std::string callback_id,
                      bool ok,
                      std::string error,
                      simulachrome::mojom::AgentActionPtr action);

  void ResolveJsCallback(const std::string& callback_id,
                         const base::Value::Dict& payload);

  raw_ptr<Profile> profile_;
  // One host per active target WebContents (recreated if the tab changes).
  std::unique_ptr<simulachrome::SimulachromeAgentHost> agent_host_;

  base::WeakPtrFactory<SimulachromeWebUIMessageHandler> weak_factory_{this};
};

}  // namespace simulachrome_webui

#endif  // CHROME_BROWSER_UI_WEBUI_SIMULACHROME_SIMULACHROME_WEB_UI_MESSAGE_HANDLER_H_

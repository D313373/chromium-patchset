#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_DEVTOOLS_CLIENT_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_DEVTOOLS_CLIENT_H_

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host_client.h"

namespace content {
class DevToolsAgentHost;
class WebContents;
}  // namespace content

namespace dashboard {

class DashboardDevToolsClient : public content::DevToolsAgentHostClient {
 public:
  using CommandCallback =
      base::OnceCallback<void(bool success, const std::string& error)>;
  using CommandResultCallback = base::OnceCallback<void(
      bool success,
      const std::string& error,
      base::Value::Dict result)>;
  using EventCallback =
      base::RepeatingCallback<void(const std::string& method,
                                   const base::Value::Dict& params)>;
  using ScreenshotCallback =
      base::OnceCallback<void(bool success,
                              std::string error,
                              std::string png_base64)>;

  // Hidden/headless tabs often have no compositor surface, so CopyFromSurface
  // returns an empty bitmap. Prefer CDP Page.captureScreenshot.
  static void CapturePngBase64(content::WebContents* web_contents,
                               ScreenshotCallback callback);

  explicit DashboardDevToolsClient(content::WebContents* web_contents);
  ~DashboardDevToolsClient() override;

  DashboardDevToolsClient(const DashboardDevToolsClient&) = delete;
  DashboardDevToolsClient& operator=(const DashboardDevToolsClient&) = delete;

  void Detach();

  int SendCommand(const std::string& method, base::Value::Dict params);
  int SendCommand(const std::string& method,
                  base::Value::Dict params,
                  CommandCallback callback);
  int SendCommandForResult(const std::string& method,
                           base::Value::Dict params,
                           CommandResultCallback callback);
  int AddEventObserver(EventCallback callback);
  void RemoveEventObserver(int observer_id);

  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;

  bool IsTrusted() override;
  bool UsesBinaryProtocol() override;
  std::string GetTypeForMetrics() override;

 private:
  bool EnsureAttached();
  void AttachToDevTools();
  void CompletePendingCommand(int id,
                              bool success,
                              const std::string& error,
                              base::Value::Dict result = base::Value::Dict());
  void FailAllPendingCommands(const std::string& error);

  raw_ptr<content::WebContents> web_contents_ = nullptr;
  scoped_refptr<content::DevToolsAgentHost> agent_host_;
  int next_command_id_ = 1;
  int next_event_observer_id_ = 1;
  std::map<int, CommandCallback> pending_commands_;
  std::map<int, CommandResultCallback> pending_result_commands_;
  std::map<int, EventCallback> event_observers_;

  base::WeakPtrFactory<DashboardDevToolsClient> weak_ptr_factory_{this};
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_DEVTOOLS_CLIENT_H_

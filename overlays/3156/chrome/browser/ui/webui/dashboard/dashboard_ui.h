#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_UI_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_UI_H_

#include <memory>

#include "chrome/browser/ui/webui/dashboard/dashboard.mojom.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

class Profile;

namespace dashboard {
class Dashboard;
}

class DashboardHandler;
class DashboardUIMessageHandler;

class DashboardUI;

class DashboardUIConfig : public content::DefaultWebUIConfig<DashboardUI> {
 public:
  DashboardUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIDashboardHost) {}
};

// The WebUI controller for the chrome://dashboard page.
class DashboardUI : public ui::MojoWebUIController {
 public:
  explicit DashboardUI(content::WebUI* web_ui);
  ~DashboardUI() override;

  DashboardUI(const DashboardUI&) = delete;
  DashboardUI& operator=(const DashboardUI&) = delete;

  void BindInterface(
      mojo::PendingReceiver<dashboard::mojom::DashboardHandler> receiver);

 private:
  // The core data model for the dashboard.
  std::unique_ptr<dashboard::Dashboard> dashboard_;
  std::unique_ptr<DashboardHandler> handler_;
  // Owned by WebUI; outlives this controller.
  raw_ptr<DashboardUIMessageHandler> message_handler_ = nullptr;
  raw_ptr<Profile> profile_ = nullptr;
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_UI_H_


#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_ACTION_BASE_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_ACTION_BASE_H_

#include <random>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_devtools_client.h"
#include "content/public/browser/web_contents.h"
class CdpActionBase : public dashboard::actions::Action {
 public:
  using CdpCallback =
      base::OnceCallback<void(bool success, const std::string& error)>;
  struct SessionState {
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    bool caps_lock_active = false;
    bool shift_active = false;
  };

  explicit CdpActionBase(Profile* profile);
  ~CdpActionBase() override;

 protected:
  void ExecuteCdpCommand(dashboard::DashboardDevToolsClient* client,
                         const std::string& method,
                         base::Value::Dict params,
                         CdpCallback callback);

  dashboard::DashboardDevToolsClient* GetDevToolsClient(
      content::WebContents* web_contents);

  // Mythic Realism Utilities
  int GetRandomRange(int min, int max) const;
  double GetRandomDouble(double min, double max) const;
  double GetRandomGaussian(double mean, double std_dev) const;
  bool RollProbability(double chance_percent) const;

  bool IsCdpInputEnabled() const { return true; }

  SessionState& GetSessionState(content::WebContents* web_contents) const;

 private:
  void OnCdpCommandComplete(CdpCallback callback,
                            bool success,
                            const std::string& error);

  // This engine is required for the GetRandom utilities
  mutable std::mt19937 random_engine_;

  // Required for safe asynchronous CDP callbacks
  base::WeakPtrFactory<CdpActionBase> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_ACTION_BASE_H_

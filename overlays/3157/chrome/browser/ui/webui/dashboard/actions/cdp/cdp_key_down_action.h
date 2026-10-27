#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_KEY_DOWN_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_KEY_DOWN_ACTION_H_

#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_action_base.h"

class CdpKeyDownAction : public CdpActionBase {
 public:
  explicit CdpKeyDownAction(Profile* profile);
  ~CdpKeyDownAction() override;

  // Action implementations
  void Execute(content::WebContents* web_contents, Callback callback) override;
  bool SetParams(const base::Value::Dict& params) override;

 private:
  void OnCdpKeyDownComplete(content::WebContents* web_contents,
                            dashboard::DashboardDevToolsClient* client,
                            Callback callback,
                            bool success,
                            const std::string& error);
  void OnCdpComplete(Callback callback,
                     const std::string& operation,
                     bool success,
                     const std::string& error);
  std::string key_;  // Key character: "c", "v", "f", etc.
  std::vector<std::string> modifiers_;  // "ctrl", "shift", "alt", "meta"
  int key_code_ = 0;  // Optional: explicit key code
  
  // CDP mode options
  bool use_cdp_input_ = true;

  base::WeakPtrFactory<CdpKeyDownAction> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_KEY_DOWN_ACTION_H_

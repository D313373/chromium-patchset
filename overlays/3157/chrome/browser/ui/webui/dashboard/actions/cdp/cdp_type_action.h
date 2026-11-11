#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_TYPE_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_TYPE_ACTION_H_

#include <memory>
#include <string>
#include <vector>

#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"

class ElementReadinessChecker;

namespace content {
class WebContents;
}

namespace dashboard {
class DashboardDevToolsClient;
}

class CdpTypeAction : public CdpActionBase {
 public:
  explicit CdpTypeAction(Profile* profile);
  ~CdpTypeAction() override;

  void Execute(content::WebContents* web_contents, Callback callback) override;
  bool SetParams(const base::Value::Dict& params) override;

 private:
  void OnElementReady(content::WebContents* web_contents,
                      Callback callback,
                      bool ready,
                      const dashboard::ResolvedElement& element,
                      const std::string& error);
  void OnFocusClickComplete(content::WebContents* web_contents,
                            Callback callback,
                            bool success,
                            const std::string& message);
  void StartTypingAfterFocusClick(content::WebContents* web_contents,
                                  Callback callback);
  void InsertExactTextViaCdp(dashboard::DashboardDevToolsClient* client,
                             Callback callback);
  void OnInsertExactTextComplete(Callback callback,
                                 bool success,
                                 const std::string& error);

  void OnFocusMoveComplete(dashboard::DashboardDevToolsClient* client,
                           content::WebContents* web_contents,
                           int target_x,
                           int target_y,
                           Callback callback,
                           bool success,
                           const std::string& error);

  void OnFocusMouseDownComplete(dashboard::DashboardDevToolsClient* client,
                                content::WebContents* web_contents,
                                int target_x,
                                int target_y,
                                Callback callback,
                                bool success,
                                const std::string& error);

  void OnFocusMouseUpComplete(dashboard::DashboardDevToolsClient* client,
                              content::WebContents* web_contents,
                              Callback callback,
                              bool success,
                              const std::string& error);

  void TypeNextCharacter(dashboard::DashboardDevToolsClient* client,
                         content::WebContents* web_contents,
                         size_t index,
                         Callback callback);

  void OnKeyDownComplete(dashboard::DashboardDevToolsClient* client,
                         content::WebContents* web_contents,
                         size_t index,
                         char c,
                         bool needs_shift,
                         Callback callback,
                         bool success,
                         const std::string& error);

  void SendKeyUpAndContinue(dashboard::DashboardDevToolsClient* client,
                            content::WebContents* web_contents,
                            size_t index,
                            char c,
                            bool needs_shift,
                            Callback callback);

  void ToggleCapsLock(dashboard::DashboardDevToolsClient* client,
                      content::WebContents* web_contents,
                      bool active);
  void ToggleShift(dashboard::DashboardDevToolsClient* client,
                   content::WebContents* web_contents,
                   bool active);
  int CountUpcomingConsecutiveCaps(size_t index) const;
  bool IsSymbolRequiringShift(char c) const;
  double GetKeyboardDistance(char key1, char key2) const;
  int GetInterKeyDelayMs(size_t index) const;
  std::string GetDomCodeForChar(char c) const;

  std::string text_to_type_;
  std::vector<dashboard::Locator> locators_;
  std::vector<dashboard::Locator> frame_hint_locators_;
  int wait_timeout_ms_ = 15000;

  // CDP mode options
  bool use_cdp_input_ = true;

  std::unique_ptr<ElementReadinessChecker> checker_;
  std::unique_ptr<dashboard::actions::Action> focus_click_action_;
  base::WeakPtrFactory<CdpTypeAction> weak_ptr_factory_{this};
};

#endif

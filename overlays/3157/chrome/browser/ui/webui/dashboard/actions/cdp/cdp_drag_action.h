#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_DRAG_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_DRAG_ACTION_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"

class ElementReadinessChecker;

class CdpDragAction : public CdpActionBase {
 public:
  explicit CdpDragAction(Profile* profile);
  ~CdpDragAction() override;

  // Action implementations
  void Execute(content::WebContents* web_contents, Callback callback) override;
  bool SetParams(const base::Value::Dict& params) override;

 private:
  void OnFromReady(content::WebContents* web_contents,
                   Callback callback,
                   bool ready,
                   const dashboard::ResolvedElement& element,
                   const std::string& error);

  void OnToReady(content::WebContents* web_contents,
                 Callback callback,
                 bool ready,
                 const dashboard::ResolvedElement& element,
                 const std::string& error);

  void ExecuteCdpDrag(content::WebContents* web_contents,
                      const dashboard::ResolvedElement& from_element,
                      const dashboard::ResolvedElement& to_element,
                      Callback callback);

  void DispatchPressAtStart(dashboard::DashboardDevToolsClient* client,
                            content::WebContents* web_contents,
                            int start_x,
                            int start_y,
                            int target_x,
                            int target_y,
                            Callback callback,
                            bool success,
                            const std::string& error);

  void DispatchLatchMove(dashboard::DashboardDevToolsClient* client,
                         content::WebContents* web_contents,
                         int start_x,
                         int start_y,
                         int target_x,
                         int target_y,
                         Callback callback);

  void DispatchDragStep(dashboard::DashboardDevToolsClient* client,
                        content::WebContents* web_contents,
                        int origin_x,
                        int origin_y,
                        int target_x,
                        int target_y,
                        int step_index,
                        Callback callback);

  void DispatchReleaseAtTarget(dashboard::DashboardDevToolsClient* client,
                               content::WebContents* web_contents,
                               int target_x,
                               int target_y,
                               Callback callback,
                               bool success,
                               const std::string& error);

  void OnDragComplete(Callback callback,
                      bool success,
                      const std::string& error);

  std::vector<dashboard::Locator> from_locators_;
  std::vector<dashboard::Locator> to_locators_;
  bool has_from_coords_ = false;
  bool has_to_coords_ = false;
  int from_x_ = 0;
  int from_y_ = 0;
  int to_x_ = 0;
  int to_y_ = 0;
  int wait_timeout_ms_ = 15000;
  bool use_human_offset_ = true;
  int steps_ = 12;

  // CDP mode options
  bool use_cdp_input_ = true;

  std::unique_ptr<ElementReadinessChecker> from_checker_;
  std::unique_ptr<ElementReadinessChecker> to_checker_;
  std::optional<dashboard::ResolvedElement> resolved_from_element_;

  base::WeakPtrFactory<CdpDragAction> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CDP_DRAG_ACTION_H_

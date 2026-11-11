#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_LOOP_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_LOOP_ACTION_H_

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "base/timer/timer.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_devtools_client.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "url/gurl.h"

class LoopAction : public dashboard::actions::Action {
 public:
  explicit LoopAction(Profile* profile);
  ~LoopAction() override;

  // Action implementations
  void Execute(content::WebContents* web_contents, Callback callback) override;
  bool SetParams(const base::Value::Dict& params) override;

 private:
  class SyntheticClickNavigationWaiter;

  enum class LoopMode {
    kCount,
    kWhileElementExists,
    kForEachElement,
  };

  enum class ForEachStrategy {
    kSnapshotOnce,
    kRequeryEachIteration,
  };

  struct LoopTarget {
    LoopTarget();
    ~LoopTarget();
    LoopTarget(const LoopTarget&);
    LoopTarget& operator=(const LoopTarget&);
    LoopTarget(LoopTarget&&);
    LoopTarget& operator=(LoopTarget&&);

    int x = 0;
    int y = 0;
    std::vector<dashboard::Locator> locators;
  };

  void EvaluateAndRunNextIteration();
  void OnLoopConditionChecked(bool ready, const std::string& error);
  void CollectForEachTargets();
  void StartResolverPageMapFallback(const std::string& reason);
  void OnResolverPageMapFallbackReady(const std::string& reason,
                                      base::Value::Dict page_map);
  void StartNativeLocatorMatchFallback(const std::string& reason);
  void OnNativeLocatorMatchesReady(const std::string& reason,
                                   base::Value::Dict page_map);
  void RunCollectedForEachTargets();
  void OnForEachTargetsCollected(dashboard::UnifiedPageMapResult result);
  bool PopulateForEachTargetsFromPageMap(const base::Value::Dict& page_map,
                                         std::string* error_out);
  std::unique_ptr<dashboard::actions::Action> BuildIterationBoundAction(
      size_t action_index);
  void RunNextAction(size_t action_index);
  void OnActionComplete(size_t action_index, bool success,
                        const std::string& result);
  void OnSyntheticClickNavigationSettled(size_t action_index);
  void OnGuidedReturnRecoveryComplete(size_t action_index,
                                      bool success,
                                      const std::string& result);
  void Complete(bool success, const std::string& message);
  std::string DescribeNestedAction(size_t action_index) const;
  std::string ReserveGuidedRunCsvFileName(const std::string& requested_name);

  std::vector<std::unique_ptr<Action>> nested_actions_;
  std::vector<base::Value::Dict> nested_action_payloads_;
  std::vector<LoopTarget> for_each_targets_;
  std::set<std::string> for_each_seen_target_keys_;
  std::map<std::string, std::string> guided_run_csv_name_overrides_;
  std::unique_ptr<dashboard::actions::Action> iteration_bound_action_;
  std::unique_ptr<dashboard::actions::Action> guided_return_recovery_action_;
  std::unique_ptr<SyntheticClickNavigationWaiter>
      synthetic_click_navigation_waiter_;
  base::Value::List for_each_locators_raw_;
  std::vector<dashboard::Locator> condition_locators_;
  std::unique_ptr<class ElementReadinessChecker> readiness_checker_;
  std::unique_ptr<dashboard::PageMapExtractor> resolver_page_map_extractor_;
  raw_ptr<content::WebContents> web_contents_ = nullptr;

  Callback callback_;
  LoopMode loop_mode_ = LoopMode::kCount;
  ForEachStrategy for_each_strategy_ = ForEachStrategy::kSnapshotOnce;
  bool guided_scrape_loop_mode_ = false;
  bool replay_target_click_ = false;
  int iterations_ = 1;
  int max_iterations_ = 1;
  int condition_timeout_ms_ = 5000;
  int current_iteration_ = 0;
  size_t pending_synthetic_click_resume_index_ = 0;
  std::string loop_variable_;
  GURL origin_url_;
  base::OneShotTimer synthetic_click_navigation_timeout_;

  base::WeakPtrFactory<LoopAction> weak_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_LOOP_ACTION_H_

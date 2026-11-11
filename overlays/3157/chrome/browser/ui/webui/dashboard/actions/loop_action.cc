#include "chrome/browser/ui/webui/dashboard/actions/loop_action.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include "base/functional/bind.h"
#include "base/json/string_escape.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_factory.h"
#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"
#include "chrome/browser/ui/webui/dashboard/actions/native_locator_matches.h"
#include "chrome/browser/ui/webui/dashboard/actions/native_page_map_matcher.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_automation_controller.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/execution_run_context.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/render_frame_host.h"

namespace {

constexpr char kNestedActionPathPrefix[] = "[nested_action_path=";
constexpr char kNestedActionPathSuffix = ']';

std::optional<std::string> ConsumeNestedActionPathToken(std::string* message) {
  if (!message) {
    return std::nullopt;
  }
  std::string text = *message;
  base::TrimWhitespaceASCII(text, base::TRIM_ALL, &text);
  if (!base::StartsWith(text, kNestedActionPathPrefix,
                        base::CompareCase::SENSITIVE)) {
    return std::nullopt;
  }
  const size_t prefix_len = strlen(kNestedActionPathPrefix);
  const size_t suffix_pos = text.find(kNestedActionPathSuffix, prefix_len);
  if (suffix_pos == std::string::npos || suffix_pos <= prefix_len) {
    return std::nullopt;
  }
  std::string path = text.substr(prefix_len, suffix_pos - prefix_len);
  base::TrimWhitespaceASCII(path, base::TRIM_ALL, &path);
  if (path.empty()) {
    return std::nullopt;
  }
  std::string remainder = text.substr(suffix_pos + 1);
  base::TrimWhitespaceASCII(remainder, base::TRIM_ALL, &remainder);
  *message = remainder;
  return path;
}

std::string AddNestedActionPathPrefix(const std::string& local_index,
                                      std::string message) {
  std::optional<std::string> existing = ConsumeNestedActionPathToken(&message);
  std::string nested_path = local_index;
  if (existing.has_value() && !existing->empty()) {
    nested_path.append(".");
    nested_path.append(*existing);
  }
  base::TrimWhitespaceASCII(message, base::TRIM_ALL, &message);
  if (message.empty()) {
    message = "Nested loop action failed.";
  }
  return base::StringPrintf("%s%s] %s", kNestedActionPathPrefix,
                            nested_path.c_str(), message.c_str());
}

bool LooksLikeLegacyGuidedScrapeLoop(const base::Value::List& nested_steps) {
  bool has_scrape_table = false;
  bool has_terminal_click = false;
  size_t step_index = 0;
  for (const auto& step : nested_steps) {
    const base::Value::Dict* step_dict = step.GetIfDict();
    if (!step_dict) {
      ++step_index;
      continue;
    }
    const std::string* action = step_dict->FindString("action");
    if (!action) {
      ++step_index;
      continue;
    }
    if (*action == "scrape_table") {
      has_scrape_table = true;
    }
    if (*action == "click" && step_index + 1 == nested_steps.size()) {
      has_terminal_click = true;
    }
    ++step_index;
  }
  return has_scrape_table && has_terminal_click;
}

}  // namespace

class LoopAction::SyntheticClickNavigationWaiter
    : public content::WebContentsObserver {
 public:
  SyntheticClickNavigationWaiter(content::WebContents* contents,
                                 LoopAction* owner)
      : content::WebContentsObserver(contents), owner_(owner) {}

  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (!owner_ || !navigation || navigation->GetWebContents() != web_contents()) {
      return;
    }
    if (!navigation->IsInPrimaryMainFrame() || !navigation->HasCommitted()) {
      return;
    }
    const GURL committed = navigation->GetURL();
    if (!committed.is_valid() || committed.IsAboutBlank()) {
      return;
    }
    LoopAction* owner = owner_.get();
    owner_ = nullptr;
    owner->synthetic_click_navigation_waiter_.reset();
    owner->synthetic_click_navigation_timeout_.Stop();
    owner->OnSyntheticClickNavigationSettled(
        owner->pending_synthetic_click_resume_index_);
  }

  void WebContentsDestroyed() override {
    if (!owner_) {
      return;
    }
    LoopAction* owner = owner_.get();
    owner_ = nullptr;
    owner->synthetic_click_navigation_waiter_.reset();
    owner->synthetic_click_navigation_timeout_.Stop();
    owner->OnSyntheticClickNavigationSettled(
        owner->pending_synthetic_click_resume_index_);
  }

 private:
  raw_ptr<LoopAction> owner_;
};

LoopAction::LoopTarget::LoopTarget() = default;
LoopAction::LoopTarget::~LoopTarget() = default;
LoopAction::LoopTarget::LoopTarget(const LoopTarget&) = default;
LoopAction::LoopTarget& LoopAction::LoopTarget::operator=(const LoopTarget&) =
    default;
LoopAction::LoopTarget::LoopTarget(LoopTarget&&) = default;
LoopAction::LoopTarget& LoopAction::LoopTarget::operator=(LoopTarget&&) =
    default;

LoopAction::LoopAction(Profile* profile)
    : dashboard::actions::Action(profile) {}

LoopAction::~LoopAction() = default;

bool LoopAction::SetParams(const base::Value::Dict& params) {
  const base::Value::List* nested = params.FindList("steps");
  if (!nested || nested->empty()) {
    return false;
  }

  const std::string* variable = params.FindString("variable");
  loop_variable_ = variable ? *variable : "items";

  const std::string* mode_raw = params.FindString("loopMode");
  std::string loop_mode =
      base::ToLowerASCII(mode_raw ? *mode_raw : std::string("count"));
  if (loop_mode.empty()) {
    loop_mode = "count";
  }
  if (loop_mode == "while_element_exists" || loop_mode == "for_each") {
    loop_mode_ = LoopMode::kWhileElementExists;
    if (loop_mode == "for_each") {
      loop_mode_ = LoopMode::kForEachElement;
      const std::string* strategy_raw = params.FindString("forEachStrategy");
      const std::string strategy = base::ToLowerASCII(
          strategy_raw ? *strategy_raw : std::string("snapshot_once"));
      for_each_strategy_ = (strategy == "requery_each_iteration")
                               ? ForEachStrategy::kRequeryEachIteration
                               : ForEachStrategy::kSnapshotOnce;
    }
    const base::Value::List* locators = params.FindList("locators");
    if (!locators || locators->empty()) {
      return false;
    }
    condition_locators_ = dashboard::ConvertLocators(*locators);
    for_each_locators_raw_ = locators->Clone();
    if (condition_locators_.empty()) {
      return false;
    }
    condition_timeout_ms_ = params.FindInt("waitTimeout").value_or(
        params.FindInt("timeout_ms").value_or(
            params.FindInt("timeout").value_or(5000)));
    condition_timeout_ms_ = std::clamp(condition_timeout_ms_, 1000, 30000);
    max_iterations_ = params.FindInt("maxIterations").value_or(
        (loop_mode_ == LoopMode::kForEachElement ? 200 : 50));
    if (max_iterations_ < 1) {
      return false;
    }
    iterations_ = max_iterations_;
  } else {
    loop_mode_ = LoopMode::kCount;
    condition_timeout_ms_ = 5000;
    iterations_ = params.FindInt("iterations").value_or(
        params.FindInt("maxIterations").value_or(1));
    if (iterations_ < 1) {
      return false;
    }
    max_iterations_ = std::max(iterations_, params.FindInt("maxIterations")
                                                .value_or(iterations_));
  }

  const bool legacy_guided_scrape_loop = LooksLikeLegacyGuidedScrapeLoop(*nested);
  guided_scrape_loop_mode_ = legacy_guided_scrape_loop;
  replay_target_click_ = params.FindBool("replayTargetClick").value_or(false);
  if (!replay_target_click_ && legacy_guided_scrape_loop) {
    replay_target_click_ = true;
    LOG(INFO) << "[LoopAction] Enabling replayTargetClick for legacy guided "
                 "scrape loop";
  }
  if (loop_mode_ == LoopMode::kForEachElement &&
      for_each_strategy_ == ForEachStrategy::kRequeryEachIteration &&
      legacy_guided_scrape_loop) {
    for_each_strategy_ = ForEachStrategy::kSnapshotOnce;
    LOG(INFO) << "[LoopAction] Promoting legacy guided scrape loop strategy "
                 "from requery_each_iteration to snapshot_once";
  }

  // Create a factory to build the nested actions.
  dashboard::actions::ActionFactory factory(profile_);
  nested_actions_ = factory.CreateActionSequence(*nested);
  nested_action_payloads_.clear();
  nested_action_payloads_.reserve(nested->size());
  for (const auto& step : *nested) {
    if (!step.is_dict()) {
      continue;
    }
    nested_action_payloads_.push_back(step.GetDict().Clone());
  }
  if (nested_actions_.empty()) {
    return false;
  }

  SetSerializedAction("loop", params.Clone());
  return true;
}

void LoopAction::Execute(content::WebContents* web_contents, Callback callback) {
  if (!web_contents) {
    std::move(callback).Run(false, "No web contents");
    return;
  }
  if (nested_actions_.empty()) {
    std::move(callback).Run(true, "");
    return;
  }
  web_contents_ = web_contents;
  callback_ = std::move(callback);
  current_iteration_ = 0;
  for_each_targets_.clear();
  for_each_seen_target_keys_.clear();
  guided_run_csv_name_overrides_.clear();
  iteration_bound_action_.reset();
  guided_return_recovery_action_.reset();
  synthetic_click_navigation_waiter_.reset();
  synthetic_click_navigation_timeout_.Stop();
  pending_synthetic_click_resume_index_ = 0;
  resolver_page_map_extractor_.reset();
  origin_url_ = web_contents_->GetLastCommittedURL();
  if (loop_mode_ == LoopMode::kForEachElement) {
    if (for_each_strategy_ == ForEachStrategy::kSnapshotOnce) {
      CollectForEachTargets();
    } else {
      EvaluateAndRunNextIteration();
    }
    return;
  }
  EvaluateAndRunNextIteration();
}

void LoopAction::EvaluateAndRunNextIteration() {
  if (!web_contents_) {
    Complete(false, "No web contents");
    return;
  }
  if (current_iteration_ >= max_iterations_) {
    Complete(true, "");
    return;
  }
  if (loop_mode_ == LoopMode::kCount && current_iteration_ >= iterations_) {
    Complete(true, "");
    return;
  }
  if (loop_mode_ == LoopMode::kForEachElement &&
      for_each_strategy_ == ForEachStrategy::kRequeryEachIteration) {
    CollectForEachTargets();
    return;
  }
  if (loop_mode_ == LoopMode::kForEachElement &&
      current_iteration_ >= static_cast<int>(for_each_targets_.size())) {
    LOG(INFO) << "[LoopAction] No remaining targets at iteration="
              << current_iteration_ << " target_count="
              << for_each_targets_.size();
    Complete(true, "");
    return;
  }

  if (loop_mode_ == LoopMode::kWhileElementExists) {
    readiness_checker_ = std::make_unique<ElementReadinessChecker>(web_contents_);
    readiness_checker_->WaitForElementReadyAsync(
        condition_locators_, dashboard::SearchScope::kAll,
        /*timeout_ms=*/condition_timeout_ms_,
        base::BindOnce(
            [](base::WeakPtr<LoopAction> weak, bool ready,
               const dashboard::ResolvedElement& /*element*/,
               const std::string& error) {
              if (!weak) {
                return;
              }
              weak->OnLoopConditionChecked(ready, error);
            },
            weak_factory_.GetWeakPtr()));
    return;
  }

  RunNextAction(0);
}

void LoopAction::CollectForEachTargets() {
  if (!web_contents_) {
    Complete(false, "No web contents");
    return;
  }
  if (!dashboard::ExecutionRunContext::Get().ShouldUseUnifiedPageMap()) {
    StartResolverPageMapFallback("page_map_disabled_in_script_mode");
    return;
  }
  dashboard::FetchUnifiedPageMapSnapshot(
      web_contents_,
      base::BindOnce(&LoopAction::OnForEachTargetsCollected,
                     weak_factory_.GetWeakPtr()),
      dashboard::mojom::SearchScope::kAll);
}

void LoopAction::StartResolverPageMapFallback(const std::string& reason) {
  if (!web_contents_) {
    Complete(false, "No web contents");
    return;
  }
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[LoopAction] Falling back to resolver page-map (reason="
              << reason << ")";
  }
  resolver_page_map_extractor_ = std::make_unique<dashboard::PageMapExtractor>();
  resolver_page_map_extractor_->Run(
      web_contents_,
      base::BindOnce(&LoopAction::OnResolverPageMapFallbackReady,
                     weak_factory_.GetWeakPtr(), reason),
      /*greedy=*/false, /*is_agent_mode=*/false, /*timeout_ms=*/2500);
}

void LoopAction::OnResolverPageMapFallbackReady(const std::string& reason,
                                                base::Value::Dict page_map) {
  resolver_page_map_extractor_.reset();
  std::string error;
  if (!PopulateForEachTargetsFromPageMap(page_map, &error)) {
    StartNativeLocatorMatchFallback(error.empty() ? reason : error);
    return;
  }
  RunCollectedForEachTargets();
}

// The page maps only carry interactive / accessibility-visible nodes; plain
// list rows or cards need a direct locator sweep in each frame.
void LoopAction::StartNativeLocatorMatchFallback(const std::string& reason) {
  if (!web_contents_) {
    Complete(false, reason);
    return;
  }
  dashboard::CollectNativeLocatorMatches(
      web_contents_, condition_locators_, dashboard::mojom::SearchScope::kAll,
      std::max(max_iterations_, 1) * 4,
      base::BindOnce(&LoopAction::OnNativeLocatorMatchesReady,
                     weak_factory_.GetWeakPtr(), reason));
}

void LoopAction::OnNativeLocatorMatchesReady(const std::string& reason,
                                             base::Value::Dict page_map) {
  std::string error;
  if (!PopulateForEachTargetsFromPageMap(page_map, &error)) {
    Complete(false, reason.empty() ? error : reason);
    return;
  }
  LOG(INFO) << "[LoopAction] Collected " << for_each_targets_.size()
            << " loop targets from native locator sweep";
  RunCollectedForEachTargets();
}

void LoopAction::RunCollectedForEachTargets() {
  if (for_each_strategy_ == ForEachStrategy::kRequeryEachIteration) {
    std::optional<LoopTarget> next_target;
    for (const auto& t : for_each_targets_) {
      const std::string key = base::StringPrintf(
          "%d,%d,%s", t.x, t.y,
          t.locators.empty() ? "" : t.locators.front().value.c_str());
      if (for_each_seen_target_keys_.contains(key)) {
        continue;
      }
      for_each_seen_target_keys_.insert(key);
      next_target = t;
      break;
    }
    if (!next_target.has_value()) {
      Complete(true, "");
      return;
    }
    for_each_targets_.clear();
    for_each_targets_.push_back(*next_target);
    RunNextAction(0);
    return;
  }
  EvaluateAndRunNextIteration();
}

bool LoopAction::PopulateForEachTargetsFromPageMap(const base::Value::Dict& page_map,
                                                   std::string* error_out) {
  for_each_targets_.clear();
  std::vector<dashboard::NativePageMapCandidate> candidates =
      dashboard::FindMatchingPageMapCandidates(page_map, condition_locators_,
                                               max_iterations_);
  for_each_targets_.reserve(candidates.size());
  for (auto& candidate : candidates) {
    LoopTarget target;
    target.x = candidate.x + candidate.width / 2;
    target.y = candidate.y + candidate.height / 2;
    target.locators = std::move(candidate.locators);
    for_each_targets_.push_back(std::move(target));
    if (static_cast<int>(for_each_targets_.size()) >= max_iterations_) {
      break;
    }
  }
  if (!for_each_targets_.empty()) {
    LOG(INFO) << "[LoopAction] Collected " << for_each_targets_.size()
              << " loop targets from page-map";
    return true;
  }
  if (error_out) {
    *error_out = "No loop targets matched the recorded locator bundle";
  }
  return false;
}

void LoopAction::OnForEachTargetsCollected(
    dashboard::UnifiedPageMapResult result) {
  if (!result.ok) {
    StartResolverPageMapFallback(result.error.empty()
                                     ? "Failed to collect native loop targets"
                                     : result.error);
    return;
  }

  std::string error;
  if (!PopulateForEachTargetsFromPageMap(result.page_map, &error)) {
    StartResolverPageMapFallback(error.empty() ? "No unified loop targets"
                                               : error);
    return;
  }

  if (for_each_strategy_ == ForEachStrategy::kRequeryEachIteration) {
    std::optional<LoopTarget> next_target;
    for (const auto& t : for_each_targets_) {
      const std::string key = base::StringPrintf(
          "%d,%d,%s", t.x, t.y,
          t.locators.empty() ? "" : t.locators.front().value.c_str());
      if (for_each_seen_target_keys_.contains(key)) {
        continue;
      }
      for_each_seen_target_keys_.insert(key);
      next_target = t;
      break;
    }
    if (!next_target.has_value()) {
      Complete(true, "");
      return;
    }
    for_each_targets_.clear();
    for_each_targets_.push_back(*next_target);
    RunNextAction(0);
    return;
  }

  if (for_each_targets_.empty()) {
    Complete(true, "");
    return;
  }
  EvaluateAndRunNextIteration();
}

std::unique_ptr<dashboard::actions::Action>
LoopAction::BuildIterationBoundAction(size_t action_index) {
  const int target_index =
      (for_each_strategy_ == ForEachStrategy::kRequeryEachIteration)
          ? 0
          : current_iteration_;
  if (target_index < 0 ||
      target_index >= static_cast<int>(for_each_targets_.size())) {
    return nullptr;
  }
  const LoopTarget& target = for_each_targets_[target_index];
  base::Value::Dict action_dict;
  base::Value::Dict* params = nullptr;
  const bool synthetic_target_click = replay_target_click_ && action_index == 0;
  if (replay_target_click_ && action_index == 0) {
    action_dict.Set("action", "click");
    action_dict.Set("params", base::Value::Dict());
    params = action_dict.FindDict("params");
    if (params) {
      params->Set("clickCount", 1);
    }
  } else {
    const size_t payload_index =
        replay_target_click_ ? action_index - 1 : action_index;
    if (payload_index >= nested_action_payloads_.size()) {
      return nullptr;
    }
    action_dict = nested_action_payloads_[payload_index].Clone();
    if (!action_dict.FindString("action")) {
      return nullptr;
    }
    params = action_dict.FindDict("params");
    if (!params) {
      action_dict.Set("params", base::Value::Dict());
      params = action_dict.FindDict("params");
    }
  }
  if (!params) {
    return nullptr;
  }

  if (synthetic_target_click) {
    base::Value::List locators;
    for (const auto& locator : target.locators) {
      locators.Append(dashboard::LocatorToValue(locator));
    }
    params->Set("locators", std::move(locators));
    params->Set("x", target.x);
    params->Set("y", target.y);
    base::Value::Dict coords;
    coords.Set("x", target.x);
    coords.Set("y", target.y);
    params->Set("coordinates", std::move(coords));
  }

  if (!synthetic_target_click && guided_scrape_loop_mode_) {
    const std::string* action_name = action_dict.FindString("action");
    if (action_name && *action_name == "scrape_table" &&
        params->FindBool("appendCsv").value_or(false)) {
      if (const std::string* csv_name = params->FindString("csvFileName")) {
        params->Set("csvFileName",
                    ReserveGuidedRunCsvFileName(*csv_name));
      }
    }
  }

  dashboard::actions::ActionFactory factory(profile_);
  return factory.CreateAction(action_dict);
}

void LoopAction::OnLoopConditionChecked(bool ready, const std::string& error) {
  readiness_checker_.reset();
  if (!ready) {
    Complete(true, "");
    return;
  }
  if (!error.empty()) {
    LOG(WARNING) << "[LoopAction] while_element_exists readiness warning: "
                 << error;
  }
  RunNextAction(0);
}

void LoopAction::RunNextAction(size_t action_index) {
  const size_t action_count =
      nested_actions_.size() + (replay_target_click_ ? 1u : 0u);
  if (action_index >= action_count) {
    ++current_iteration_;
    EvaluateAndRunNextIteration();
    return;
  }

  if (loop_mode_ == LoopMode::kForEachElement) {
    LOG(INFO) << "[LoopAction] Iteration " << (current_iteration_ + 1) << "/"
              << std::max(1, static_cast<int>(for_each_targets_.size()))
              << " actionIndex=" << action_index;
    EmitProgressDetail(DescribeNestedAction(action_index));
    iteration_bound_action_ = BuildIterationBoundAction(action_index);
    if (iteration_bound_action_) {
      iteration_bound_action_->Execute(
          web_contents_,
          base::BindOnce(&LoopAction::OnActionComplete,
                         weak_factory_.GetWeakPtr(), action_index));
      return;
    }
  }

  const size_t nested_index =
      replay_target_click_ ? action_index - 1 : action_index;
  nested_actions_[nested_index]->Execute(
      web_contents_,
      base::BindOnce(&LoopAction::OnActionComplete, weak_factory_.GetWeakPtr(),
                     action_index));
}

void LoopAction::OnActionComplete(size_t action_index,
                                  bool success,
                                  const std::string& result) {
  if (action_index == 0) {
    iteration_bound_action_.reset();
  }
  const size_t action_count =
      nested_actions_.size() + (replay_target_click_ ? 1u : 0u);
  const bool is_terminal_recorded_click =
      guided_scrape_loop_mode_ && replay_target_click_ &&
      action_index + 1 == action_count;
  if (!success) {
    if (is_terminal_recorded_click) {
      LOG(WARNING) << "[LoopAction] Guided return step failed at iteration "
                   << (current_iteration_ + 1)
                   << ", recovering via navigate to origin URL: "
                   << origin_url_;
      if (!origin_url_.is_valid()) {
        RunNextAction(action_index + 1);
        return;
      }
      base::Value::Dict action_dict;
      action_dict.Set("action", "navigate");
      base::Value::Dict params;
      params.Set("url", origin_url_.spec());
      action_dict.Set("params", std::move(params));
      dashboard::actions::ActionFactory factory(profile_);
      guided_return_recovery_action_ = factory.CreateAction(action_dict);
      if (!guided_return_recovery_action_) {
        RunNextAction(action_index + 1);
        return;
      }
      EmitProgressDetail(base::StringPrintf("loop:return_to_origin (%d/%d)",
                                            current_iteration_ + 1,
                                            std::max(1, static_cast<int>(for_each_targets_.size()))));
      guided_return_recovery_action_->Execute(
          web_contents_,
          base::BindOnce(&LoopAction::OnGuidedReturnRecoveryComplete,
                         weak_factory_.GetWeakPtr(), action_index));
      return;
    }
    std::string message = result.empty() ? "Loop action failed." : result;
    message = AddNestedActionPathPrefix(
        base::NumberToString(static_cast<int>(action_index) + 1),
        std::move(message));
    Complete(false, message);
    return;
  }

  const bool is_synthetic_target_click = replay_target_click_ && action_index == 0;
  if (is_synthetic_target_click && web_contents_) {
    pending_synthetic_click_resume_index_ = action_index + 1;
    synthetic_click_navigation_waiter_ =
        std::make_unique<SyntheticClickNavigationWaiter>(web_contents_, this);
    synthetic_click_navigation_timeout_.Start(
        FROM_HERE, base::Seconds(4),
        base::BindOnce(
            [](base::WeakPtr<LoopAction> weak) {
              if (!weak) {
                return;
              }
              weak->synthetic_click_navigation_waiter_.reset();
              weak->OnSyntheticClickNavigationSettled(
                  weak->pending_synthetic_click_resume_index_);
            },
            weak_factory_.GetWeakPtr()));
    return;
  }

  if (is_terminal_recorded_click && web_contents_ && origin_url_.is_valid()) {
    const GURL current_url = web_contents_->GetLastCommittedURL();
    if (current_url.GetWithoutRef() != origin_url_.GetWithoutRef()) {
      LOG(INFO) << "[LoopAction] Guided return step did not restore origin at "
                << "iteration " << (current_iteration_ + 1)
                << ", navigating back to " << origin_url_;
      base::Value::Dict action_dict;
      action_dict.Set("action", "navigate");
      base::Value::Dict params;
      params.Set("url", origin_url_.spec());
      action_dict.Set("params", std::move(params));
      dashboard::actions::ActionFactory factory(profile_);
      guided_return_recovery_action_ = factory.CreateAction(action_dict);
      if (!guided_return_recovery_action_) {
        RunNextAction(action_index + 1);
        return;
      }
      EmitProgressDetail(base::StringPrintf("loop:return_to_origin (%d/%d)",
                                            current_iteration_ + 1,
                                            std::max(1, static_cast<int>(for_each_targets_.size()))));
      guided_return_recovery_action_->Execute(
          web_contents_,
          base::BindOnce(&LoopAction::OnGuidedReturnRecoveryComplete,
                         weak_factory_.GetWeakPtr(), action_index));
      return;
    }
  }

  RunNextAction(action_index + 1);
}

void LoopAction::OnSyntheticClickNavigationSettled(size_t action_index) {
  synthetic_click_navigation_timeout_.Stop();
  synthetic_click_navigation_waiter_.reset();
  RunNextAction(action_index);
}

void LoopAction::OnGuidedReturnRecoveryComplete(size_t action_index,
                                                bool success,
                                                const std::string& result) {
  guided_return_recovery_action_.reset();
  if (!success) {
    std::string message =
        result.empty() ? "Guided return recovery navigation failed." : result;
    message = AddNestedActionPathPrefix(
        base::NumberToString(static_cast<int>(action_index) + 1),
        std::move(message));
    Complete(false, message);
    return;
  }
  RunNextAction(action_index + 1);
}

void LoopAction::Complete(bool success, const std::string& message) {
  readiness_checker_.reset();
  iteration_bound_action_.reset();
  guided_return_recovery_action_.reset();
  synthetic_click_navigation_waiter_.reset();
  synthetic_click_navigation_timeout_.Stop();
  for_each_targets_.clear();
  for_each_seen_target_keys_.clear();
  web_contents_ = nullptr;
  current_iteration_ = 0;
  origin_url_ = GURL();
  if (!callback_) {
    return;
  }
  std::move(callback_).Run(success, message);
}

std::string LoopAction::DescribeNestedAction(size_t action_index) const {
  const int iteration_total = std::max(1, static_cast<int>(for_each_targets_.size()));
  std::string nested_kind;
  if (replay_target_click_ && action_index == 0) {
    nested_kind = "click";
  } else {
    const size_t nested_index =
        replay_target_click_ ? action_index - 1 : action_index;
    if (nested_index < nested_action_payloads_.size()) {
      if (const std::string* action =
              nested_action_payloads_[nested_index].FindString("action")) {
        nested_kind = *action;
      }
    }
  }
  if (nested_kind.empty()) {
    nested_kind = "unknown";
  }
  return base::StringPrintf("loop:%s (%d/%d)", nested_kind.c_str(),
                            current_iteration_ + 1, iteration_total);
}

std::string LoopAction::ReserveGuidedRunCsvFileName(
    const std::string& requested_name) {
  if (!profile_ || requested_name.empty()) {
    return requested_name;
  }

  const auto found = guided_run_csv_name_overrides_.find(requested_name);
  if (found != guided_run_csv_name_overrides_.end()) {
    return found->second;
  }

  const int64_t run_token =
      base::Time::Now().ToDeltaSinceWindowsEpoch().InMilliseconds();
  const base::FilePath requested_path =
      base::FilePath::FromUTF8Unsafe(requested_name).BaseName();
  const base::FilePath actual_path =
      requested_path.InsertBeforeExtensionASCII(
          "_run" + base::NumberToString(run_token));
  const std::string resolved_name = actual_path.AsUTF8Unsafe();
  guided_run_csv_name_overrides_[requested_name] = resolved_name;
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[LoopAction] Reserved guided CSV file for this run: "
              << requested_name << " -> " << resolved_name;
  }
  return resolved_name;
}

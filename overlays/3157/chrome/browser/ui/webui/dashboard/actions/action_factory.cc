#include "chrome/browser/ui/webui/dashboard/actions/action_factory.h"

#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/authenticate_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_click_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_drag_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_key_down_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_navigate_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_scroll_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/cdp/cdp_type_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/click_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/drag_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/execution_failover_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/glic_prompt_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/history_nav_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/if_condition_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/key_down_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/key_press_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/listen_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/loop_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/navigate_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/scrape_element_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/scrape_table_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/scroll_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/type_action.h"
#include "chrome/browser/ui/webui/dashboard/actions/wait_action.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"

namespace dashboard {
namespace actions {

namespace {

void ApplyRunContextToAction(Action* action,
                             bool is_agent_mode,
                             bool force_native_execution) {
  if (action) {
    action->SetExecutionRunContext(is_agent_mode, force_native_execution);
  }
}

}  // namespace

ActionFactory::ActionFactory(Profile* profile) : profile_(profile) {}
ActionFactory::~ActionFactory() = default;

namespace {

const base::Value::List* PreferTopLevelChildListWhenParamsEmpty(
    const base::Value::Dict& action_data,
    const base::Value::Dict& params,
    const char* key) {
  const base::Value::List* top_level = action_data.FindList(key);
  const base::Value::List* in_params = params.FindList(key);
  if (top_level && !top_level->empty() && (!in_params || in_params->empty())) {
    return top_level;
  }
  return in_params ? in_params : top_level;
}

}  // namespace

bool IsExecutionRoutableKind(const std::string& kind) {
  // listen is intentionally native-only (see CreateAction).
  return kind == "click" || kind == "hover" || kind == "drag" ||
         kind == "type" || kind == "key_down" || kind == "key_press" ||
         kind == "scroll" || kind == "navigate";
}

std::unique_ptr<Action> CreateNativeActionForKind(const std::string& kind,
                                                  Profile* profile) {
  if (kind == "click" || kind == "hover") {
    return std::make_unique<ClickAction>(profile);
  }
  if (kind == "drag") {
    // Native drag synthesizes mouse events that macOS HTML5 DnD still latches
    // to the real cursor. CDP Input.dispatchDragEvent uses the scripted
    // coordinates. Retire native drag until that is fixed.
    return std::make_unique<CdpDragAction>(profile);
  }
  if (kind == "type") {
    return std::make_unique<TypeAction>(profile);
  }
  if (kind == "key_down") {
    return std::make_unique<KeyDownAction>(profile);
  }
  if (kind == "key_press") {
    return std::make_unique<KeyPressAction>(
        profile, KeyPressAction::ExecutionPreference::kPreferNative);
  }
  if (kind == "scroll") {
    return std::make_unique<ScrollAction>(profile);
  }
  if (kind == "navigate") {
    return std::make_unique<NavigateAction>(profile);
  }
  if (kind == "listen") {
    return std::make_unique<ListenAction>(profile);
  }
  return nullptr;
}

std::unique_ptr<Action> CreateCdpActionForKind(const std::string& kind,
                                               Profile* profile) {
  if (kind == "click" || kind == "hover") {
    return std::make_unique<CdpClickAction>(profile);
  }
  if (kind == "drag") {
    return std::make_unique<CdpDragAction>(profile);
  }
  if (kind == "type") {
    return std::make_unique<CdpTypeAction>(profile);
  }
  if (kind == "key_down") {
    return std::make_unique<CdpKeyDownAction>(profile);
  }
  if (kind == "key_press") {
    return std::make_unique<KeyPressAction>(
        profile, KeyPressAction::ExecutionPreference::kPreferCdp);
  }
  if (kind == "scroll") {
    return std::make_unique<CdpScrollAction>(profile);
  }
  if (kind == "navigate") {
    return std::make_unique<CdpNavigateAction>(profile);
  }
  // listen is native-only; do not create a CDP listen action.
  return nullptr;
}

std::unique_ptr<Action> CreateRoutedAction(const std::string& kind,
                                           Profile* profile,
                                           bool force_cdp_execution) {
  dashboard::ActionExecutionRouting routing;
  if (force_cdp_execution) {
    routing.primary = dashboard::ActionExecutionPath::kCdp;
    routing.fallback = dashboard::ActionExecutionPath::kUnset;
  } else {
    routing = dashboard::GetActionExecutionRouting();
  }

  std::unique_ptr<Action> action =
      routing.primary == dashboard::ActionExecutionPath::kCdp
          ? CreateCdpActionForKind(kind, profile)
          : CreateNativeActionForKind(kind, profile);
  if (!action) {
    action = CreateCdpActionForKind(kind, profile);
  }
  if (!action) {
    action = CreateNativeActionForKind(kind, profile);
  }
  if (action) {
    action->SetExecutionPathLabel(std::string(
        dashboard::ToActionExecutionPathString(routing.primary)));
  }
  return action;
}

std::unique_ptr<Action> CreateConfiguredActionForPath(
    const std::string& kind,
    Profile* profile,
    const base::Value::Dict& params,
    dashboard::ActionExecutionPath path) {
  std::unique_ptr<Action> action =
      path == dashboard::ActionExecutionPath::kCdp
          ? CreateCdpActionForKind(kind, profile)
          : CreateNativeActionForKind(kind, profile);
  if (!action) {
    return nullptr;
  }
  action->SetExecutionPathLabel(
      std::string(dashboard::ToActionExecutionPathString(path)));
  if (!action->SetParams(params)) {
    LOG(ERROR) << "[ActionFactory] SetParams failed for kind='" << kind
               << "' path='"
               << dashboard::ToActionExecutionPathString(path) << "'";
    return nullptr;
  }
  return action;
}

std::unique_ptr<Action> ActionFactory::CreateAction(
    const base::Value::Dict& action_data) {
  const std::string* action_name = action_data.FindString("action");
  std::string kind = action_name ? *action_name : std::string();
  if (kind.empty()) {
    return nullptr;
  }

  const base::Value::Dict* params = action_data.FindDict("params");
  if (!params) {
    LOG(ERROR) << "[ActionFactory] Missing params for kind='" << kind << "'";
    return nullptr;
  }
  base::Value::Dict effective_params = params->Clone();
  if (kind == "loop") {
    if (const base::Value::List* steps = PreferTopLevelChildListWhenParamsEmpty(
            action_data, effective_params, "steps")) {
      effective_params.Set("steps", steps->Clone());
    }
  }
  if (kind == "if_condition") {
    if (const base::Value::List* then_actions =
            PreferTopLevelChildListWhenParamsEmpty(action_data,
                                                   effective_params, "then")) {
      effective_params.Set("then", then_actions->Clone());
    }
    if (const base::Value::List* else_actions =
            PreferTopLevelChildListWhenParamsEmpty(action_data,
                                                   effective_params, "else")) {
      effective_params.Set("else", else_actions->Clone());
    }
  }
  if (kind == "hover" && !effective_params.Find("click_count") &&
      !effective_params.Find("clickCount")) {
    effective_params.Set("click_count", 0);
    effective_params.Set("clickCount", 0);
  }

  std::unique_ptr<Action> action;
  // Listen is native-only for now. CDP listen streams are incomplete and were
  // rejecting common conditions; keep one reliable backend.
  if (kind == "listen") {
    action = std::make_unique<ListenAction>(profile_);
    if (!action->SetParams(effective_params)) {
      LOG(ERROR) << "[ActionFactory] SetParams failed for kind='listen'";
      return nullptr;
    }
    action->SetExecutionPathLabel("native");
    ApplyRunContextToAction(action.get(), is_agent_mode_,
                            force_native_execution_);
    LOG(INFO) << "[ActionFactory] Created action kind='listen' path=native";
    return action;
  }

  if (IsExecutionRoutableKind(kind)) {
    dashboard::ActionExecutionRouting routing;
    if (force_native_execution_) {
      routing.primary = dashboard::ActionExecutionPath::kNative;
      routing.fallback = dashboard::ActionExecutionPath::kUnset;
    } else if (force_cdp_execution_) {
      routing.primary = dashboard::ActionExecutionPath::kCdp;
      routing.fallback = dashboard::ActionExecutionPath::kUnset;
    } else {
      routing = dashboard::GetActionExecutionRouting();
    }
    if (kind == "drag") {
      routing.primary = dashboard::ActionExecutionPath::kCdp;
      routing.fallback = dashboard::ActionExecutionPath::kUnset;
    }

    std::unique_ptr<Action> primary_action = CreateConfiguredActionForPath(
        kind, profile_, effective_params, routing.primary);
    std::unique_ptr<Action> fallback_action;
    if (routing.fallback != dashboard::ActionExecutionPath::kUnset) {
      fallback_action = CreateConfiguredActionForPath(
          kind, profile_, effective_params, routing.fallback);
    }

    if (primary_action && fallback_action) {
      ApplyRunContextToAction(primary_action.get(), is_agent_mode_,
                              force_native_execution_);
      ApplyRunContextToAction(fallback_action.get(), is_agent_mode_,
                              force_native_execution_);
      action = std::make_unique<ExecutionFailoverAction>(
          profile_, kind, std::move(primary_action), routing.primary,
          std::move(fallback_action), routing.fallback);
      action->SetExecutionPathLabel(std::string(
          dashboard::ToActionExecutionPathString(routing.primary)));
    } else if (primary_action) {
      action = std::move(primary_action);
    } else if (fallback_action) {
      action = std::move(fallback_action);
    } else {
      action = CreateRoutedAction(kind, profile_, force_cdp_execution_);
      if (action && !action->SetParams(effective_params)) {
        LOG(ERROR) << "[ActionFactory] SetParams failed for kind='" << kind
                   << "'";
        return nullptr;
      }
      ApplyRunContextToAction(action.get(), is_agent_mode_,
                              force_native_execution_);
      return action;
    }
  }

  if (!action && kind == "back") {
    action = std::make_unique<HistoryNavAction>(
        profile_, HistoryNavAction::Direction::kBack);
  } else if (kind == "forward") {
    action = std::make_unique<HistoryNavAction>(
        profile_, HistoryNavAction::Direction::kForward);
  } else if (kind == "scrape_element") {
    action = std::make_unique<ScrapeElementAction>(profile_);
  } else if (kind == "loop") {
    action = std::make_unique<LoopAction>(profile_);
  } else if (kind == "key_press") {
    action = std::make_unique<KeyPressAction>(profile_);
  } else if (kind == "wait") {
    action = std::make_unique<WaitAction>(profile_);
  } else if (kind == "glic_prompt") {
    action = std::make_unique<GlicPromptAction>(profile_);
  } else if (kind == "if_condition") {
    action = std::make_unique<IfConditionAction>(profile_);
  } else if (kind == "authenticate") {
    action = std::make_unique<AuthenticateAction>(profile_);
  } else if (kind == "scrape_table") {
    action = std::make_unique<ScrapeTableAction>(profile_);
  }

  if (!action) {
    LOG(ERROR) << "[ActionFactory] Unknown action kind='" << kind << "'";
    return nullptr;
  }

  if (!IsExecutionRoutableKind(kind) && !action->SetParams(effective_params)) {
    LOG(ERROR) << "[ActionFactory] SetParams failed for kind='" << kind << "'";
    return nullptr;
  }

  LOG(INFO) << "[ActionFactory] Created action kind='" << kind << "'";
  ApplyRunContextToAction(action.get(), is_agent_mode_, force_native_execution_);
  return action;
}

void ActionFactory::SetForceCdpExecution(bool force) {
  force_cdp_execution_ = force;
  if (force) {
    force_native_execution_ = false;
  }
}

void ActionFactory::SetForceNativeExecution(bool force) {
  force_native_execution_ = force;
  if (force) {
    force_cdp_execution_ = false;
  }
}

void ActionFactory::SetAgentMode(bool agent_mode) {
  is_agent_mode_ = agent_mode;
}

std::vector<std::unique_ptr<Action>> ActionFactory::CreateActionSequence(
    const base::Value::List& timeline) {
  std::vector<std::unique_ptr<Action>> sequence;
  for (const auto& action_value : timeline) {
    if (action_value.is_dict()) {
      auto action = CreateAction(action_value.GetDict());
      if (action) {
        sequence.push_back(std::move(action));
      }
    }
  }
  return sequence;
}

}  // namespace actions
}  // namespace dashboard

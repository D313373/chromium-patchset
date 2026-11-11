#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_GLOBAL_SETTINGS_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_GLOBAL_SETTINGS_H_

#include <string_view>

#include "base/values.h"

namespace dashboard {

enum class ActionExecutionPath {
  kUnset,
  kCdp,
  kNative,
};

struct ActionExecutionRouting {
  ActionExecutionPath primary = ActionExecutionPath::kCdp;
  ActionExecutionPath fallback = ActionExecutionPath::kUnset;
};

// Default global settings payload used when settings have not been persisted
// yet or when persisted settings are invalid.
base::Value::Dict DefaultGlobalSettings();

// Process-wide in-memory global settings cache.
base::Value::Dict& GlobalSettingsStore();

// Parse execution routing from a settings payload.
ActionExecutionRouting ReadActionExecutionRouting(
    const base::Value::Dict& settings);

// Convenience wrapper that reads execution routing from GlobalSettingsStore().
ActionExecutionRouting GetActionExecutionRouting();

ActionExecutionPath ParseActionExecutionPath(std::string_view value,
                                             bool allow_unset);
std::string_view ToActionExecutionPathString(ActionExecutionPath path);

// Recording stealth helpers (read GlobalSettingsStore().recording).
// Defaults: stealth on, no page injects, no eager CDP Runtime/DOM enable.
bool RecordingStealthModeEnabled();
bool RecordingAllowPageInjects();
bool RecordingEagerCdpAttach();

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_DASHBOARD_GLOBAL_SETTINGS_H_

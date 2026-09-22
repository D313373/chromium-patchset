#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_SCHEMA_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_SCHEMA_H_

#include <string>

#include "base/time/time.h"
#include "base/values.h"

namespace dashboard {

// Canonicalization/validation utilities for script JSON persisted on disk.
class ScriptSchema {
 public:
  // Produces a canonical script dict suitable for persistence.
  // Ensures:
  //  - schema_version, id, name
  //  - created_at (if absent) and updated_at timestamps (ISO 8601 UTC)
  //  - actions normalized to {action (lowercase), params {}}
  //  - url_contexts present (empty list when absent)
  //  - nested control-flow arrays normalized
  static base::Value::Dict CanonicalizeForSave(base::Value::Dict in,
                                               const std::string& id,
                                               base::Time now);

  // Exposed for helper routines in the same translation unit.
  static void CanonicalizeAction(base::Value::Dict& action);
  static void CanonicalizeActionsList(base::Value::List& list);

  // Schema introspection: Get default/template action for a given type.
  // Returns a canonical action dict with all default params populated.
  static base::Value::Dict GetActionTemplate(const std::string& action_type);

  // Validate that an action has required params for its type.
  // Returns true if valid; sets |error| message if invalid.
  static bool ValidateAction(const base::Value::Dict& action, std::string* error);

  // Get list of all supported action types with metadata.
  // Each entry: {action, display_name, description, params: [{name, type, required, default}]}
  static base::Value::List GetSupportedActions();

  // URL to navigate to before resuming execution at |step_index|. Prefers the
  // action's contextUrl (walking backward if empty), then navigate params.
  static std::string ResolveResumeNavigationUrl(const base::Value::List& timeline,
                                                int step_index);

  // Prefer url_before / context_url / url_after from a run-history step entry.
  static std::string ExtractRunHistoryStepUrl(const base::Value::List* steps,
                                              int step_index);

  static bool ContextUrlsEquivalent(const std::string& left,
                                    const std::string& right);

 private:
  static std::string ToIso8601UTC(base::Time t);
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_SCRIPT_SCHEMA_H_

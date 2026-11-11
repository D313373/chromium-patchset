#include "chrome/browser/ui/webui/dashboard/script_schema.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "url/gurl.h"

namespace dashboard {

namespace {

// ============================================================================
// Action Schema Definitions
// ============================================================================

// Helper to create a base::Value for default params
base::Value MakeDefaultValue(std::string_view type_str) {
  if (type_str == "string") return base::Value("");
  if (type_str == "int") return base::Value(0);
  if (type_str == "double") return base::Value(0.0);
  if (type_str == "bool") return base::Value(false);
  if (type_str == "list") return base::Value(base::Value::List());
  if (type_str == "dict") return base::Value(base::Value::Dict());
  return base::Value();
}

struct ActionParamSpec {
  const char* name;
  const char* type;  // "string", "int", "double", "bool", "list", "dict"
  bool required;
  const char* description;
  const char* default_literal = nullptr;  // Optional override for scalar types.

  base::Value GetDefaultValue() const {
    if (default_literal && *default_literal) {
      const std::string_view type_view(type ? type : "");
      if (type_view == "string") {
        return base::Value(default_literal);
      }
      if (type_view == "int") {
        int v = 0;
        if (base::StringToInt(default_literal, &v))
          return base::Value(v);
        return base::Value(0);
      }
      if (type_view == "double") {
        double v = 0.0;
        if (base::StringToDouble(default_literal, &v))
          return base::Value(v);
        return base::Value(0.0);
      }
      if (type_view == "bool") {
        return base::Value(
            base::EqualsCaseInsensitiveASCII(default_literal, "true"));
      }
    }
    return MakeDefaultValue(type);
  }
};

struct ActionSpec {
  const char* action_name;
  const char* display_name;
  const char* description;
  std::initializer_list<ActionParamSpec> params;
};

std::vector<ActionSpec> GetActionSpecs() {
  std::vector<ActionSpec> s;
  s.push_back({
      "click",
      "Click",
      "Click an element on the page",
      {
          {"locators", "list", false,
           "Array of locator objects to find the element"},
          {"coordinates", "dict", false,
           "Direct click coordinates {x,y}"},
          {"clickCount", "int", false,
           "Number of clicks (0=hover only, 1=single, 2=double, 3=triple)", "1"},
          {"waitTimeout", "int", false,
           "Wait timeout in ms for element readiness", "15000"},
          {"humanOffset", "bool", false,
           "Use human-like click jitter", "true"},
          {"button", "string", false,
           "Mouse button: 'left', 'right', or 'middle'", "left"},
          {"iframePath", "list", false,
           "Path to iframe if element is inside iframe"},
      }});
  s.push_back({
      "hover",
      "Hover",
      "Move cursor over an element without clicking",
      {
          {"locators", "list", false,
           "Array of locator objects to find the element"},
          {"coordinates", "dict", false,
           "Direct hover coordinates {x,y}"},
          {"clickCount", "int", false,
           "Number of clicks (hover-only should stay 0)", "0"},
          {"waitTimeout", "int", false,
           "Wait timeout in ms for element readiness", "15000"},
          {"humanOffset", "bool", false,
           "Use human-like cursor offset", "true"},
          {"button", "string", false,
           "Mouse button: 'left', 'right', or 'middle'", "left"},
          {"iframePath", "list", false,
           "Path to iframe if element is inside iframe"},
      }});
  s.push_back({
      "drag",
      "Drag",
      "Drag from a start target to an end target",
      {
          {"fromLocators", "list", false,
           "Locator list for drag start target"},
          {"toLocators", "list", false,
           "Locator list for drag end target"},
          {"fromCoordinates", "dict", false,
           "Direct start coordinates {x,y}"},
          {"toCoordinates", "dict", false,
           "Direct end coordinates {x,y}"},
          {"waitTimeout", "int", false,
           "Wait timeout in ms for start/end readiness", "15000"},
          {"humanOffset", "bool", false,
           "Use human-like offset/jitter while dragging", "true"},
          {"steps", "int", false,
           "Number of intermediate drag move events", "12"},
      }});
  s.push_back({
    "type",
    "Type Text",
    "Type text into an element",
    {
      {"locators", "list", false,
       "Array of locator objects to find the element (optional; defaults to active element)"},
      {"text", "string", true, "Text to type into the element"},
      {"delay_ms", "int", false, "Delay between keystrokes in milliseconds"},
      {"waitTimeout", "int", false, "Wait timeout in ms for element readiness", "15000"},
    }
  });
  s.push_back({
    "navigate",
    "Navigate",
    "Navigate to a URL",
    {
      {"url", "string", true, "URL to navigate to"}
    }
  });
  s.push_back({
    "back",
    "Back",
    "Navigate back in browser history (one step)",
    {}
  });
  s.push_back({
    "forward",
    "Forward",
    "Navigate forward in browser history (one step)",
    {}
  });
  s.push_back({
    "wait",
    "Wait",
    "Wait for a specified duration",
    {
      {"waitType", "string", true, "'time' or 'element'"},
      {"time", "int", false, "Duration in milliseconds when waitType=='time'"},
      {"locators", "list", false, "Element locators for element wait"},
      {"timeout", "int", false, "Timeout milliseconds when waitType=='element'"},
      {"randomDelay", "bool", false, "Randomized delay between actions"}
    }
  });
  s.push_back({
    "listen",
    "Listen",
    "Wait until a condition becomes true for an element",
    {
      {"locators", "list", true, "Array of locator objects to find the element"},
      {"condition", "string", true,
       "Condition: visible|hidden|text_contains|text_equals|new_child|new_child_contains|attribute_contains|attribute_equals"},
      {"text", "string", false, "Optional text/substring used by text_* / *_contains conditions"},
      {"textUseRegex", "bool", false, "Treat the text field as a regular expression"},
      {"attribute", "string", false, "Attribute name used by attribute_* conditions (e.g. 'class')"},
      {"attributeUseRegex", "bool", false, "Treat text as a regular expression when matching the attribute value"},
      {"captureVariable", "string", false, "Optional token key to receive the captured match value for trigger script expansion"},
      {"timeout_ms", "int", false, "Overall timeout for the listen action (ms)", "60000"},
      {"autoRefresh", "bool", false, "Whether the listener should periodically refresh and re-arm"},
      {"executionFrequencyMs", "int", false, "Refresh cadence in milliseconds when autoRefresh is enabled"},
      {"sampleIntervalMs", "int", false, "How often the listener should re-check between refreshes (ms)", "5000"},
      {"triggerThrottleMs", "int", false, "Minimum time between successive trigger runs (ms)", "5000"},
      {"refreshJitter", "bool", false, "Add randomized jitter before each refresh/listen cycle"},
      {"extremeEvasion", "bool", false, "Enable long-period cadence variance and sleep windows"},
      {"onMatchScriptId", "string", false, "Optional script id to run when the condition is satisfied"},
    }
  });
  s.push_back({
    "glic_prompt",
    "Glic Prompt",
    "Run the autonomous Glic-style agent loop on the current page using snapshot context and action JSON",
    {
      {"focusAreas", "list", false,
       "Optional grouped focus elements/areas. Each entry may contain a label and locators list that tells the agent which page regions to prioritize"},
      {"focusOnlyContext", "bool", false,
       "If true, only focus-area context is sent initially. The agent can request broader context with refresh_pagemap"},
      {"locators", "list", false,
       "Legacy optional focus locator bundle. Converted to a single focus area when loading older scripts"},
      {"prompt", "string", true,
       "Prompt text. Supports smart-input token expansion including listener match variables."},
      {"stayOnCurrentTab", "bool", false,
       "If true, refuse navigate/call_script/open-tab and keep using the script tab"},
      {"skipAutoSave", "bool", false,
       "If true, do not auto-save a nested script when this prompt finishes"},
    }
  });
  s.push_back({
    "call_script",
    "Call Script",
    "Execute another stored dashboard script by id",
    {
      {"scriptId", "string", true, "ID of the script to call"},
      {"waitFor", "string", false,
       "Optional wait mode after call (none|url|selector)"},
      {"checkSelector", "string", false,
       "CSS selector used when waitFor=='selector'"},
    }
  });
  s.push_back({
    "scrape_element",
    "Scrape Element",
    "Extract data from an element",
    {
      {"locators", "list", true, "Array of locator objects to find the element"},
      {"variableName", "string", true, "Variable name to store the scraped value"},
      {"dataType", "string", false, "Data type to scrape ('text','html','attribute_href','attribute_src')"},
      {"download", "bool", false, "Whether to download scraped resource"},
      {"downloadPath", "string", false, "Download path"},
      {"findReplace", "list", false, "Find/replace rules to apply to scraped text"}
    }
  });
  s.push_back({
    "scrape_table",
    "Scrape Table",
    "Extract data from a table",
    {
      {"rowLocators", "list", false, "Locators that match each data row to scrape"},
      {"locators", "list", false, "Legacy alias for rowLocators"},
      {"fields", "list", true, "List of fields with row-relative locators"},
      {"variableName", "string", true, "Variable name to identify scraped table data"},
      {"rowSelectionMode", "string", false, "Row extraction mode: all|index", "all"},
      {"rowIndex", "int", false, "1-based row index when rowSelectionMode=='index'", "1"},
      {"saveCsv", "bool", false, "Persist scraped rows to a CSV file", "true"},
      {"appendCsv", "bool", false, "Append rows to an existing CSV instead of overwriting", "false"},
      {"csvFileName", "string", false, "CSV file name under SimulachromeData/scrapes"}
    }
  });
  s.push_back({
    "authenticate",
    "Authenticate",
    "Login flow with priority-ordered methods",
    {
      {"methods", "list", true,
       "Ordered login methods. Index 0 is the highest priority."}
    }
  });
  s.push_back({
    "if_condition",
    "If Condition",
    "Conditional execution based on element presence or value",
    {
      {"conditionType", "string", true, "'element_exists' or 'text_contains'"},
      {"locators", "list", false, "Locators used when checking element_exists"},
      {"text", "string", false, "Text to match when using text_contains"},
      {"then", "list", false, "Actions to execute if condition is true"},
      {"else", "list", false, "Actions to execute if condition is false"}
    }
  });
  s.push_back({
    "scroll",
    "Scroll",
    "Scroll window or a specific element",
    {
      {"distance", "double", false,
       "Vertical scroll delta in pixels (+down, -up)"},
      {"percentY", "double", false, "Target vertical scroll percent [0-100]", "0"},
      {"percentX", "double", false, "Target horizontal scroll percent [0-100]", "0"},
      {"element", "string", false, "Scroll target: 'window' or 'element'", "window"},
      {"locators", "list", false, "Element locators when element=='element'"},
    }
  });
  s.push_back({
    "key_press",
    "Key Press",
    "Press a named key one or more times",
    {
      {"key", "string", true, "Key name (e.g. Enter, Tab, Escape, ArrowDown)"},
      {"count", "int", false, "Number of times to press the key", "1"},
    }
  });
  s.push_back({
    "key_down",
    "Key Down",
    "Send a key press with optional modifiers",
    {
      {"key", "string", true, "Key name or single character"},
      {"modifiers", "list", false, "Modifiers: ['ctrl','shift','alt','meta']"},
      {"keyCode", "int", false, "Optional explicit key code"},
    }
  });
  s.push_back({
    "loop",
    "Loop",
    "Repeat nested actions with an explicit stop condition",
    {
      {"loopMode", "string", false, "'count', 'for_each', or 'while_element_exists'", "count"},
      {"iterations", "int", false, "Repeat count when loopMode='count'", "1"},
      {"maxIterations", "int", false, "Safety cap for loop iterations", "50"},
      {"waitTimeout", "int", false, "Timeout in ms for while_element_exists / for_each locator resolution", "5000"},
      {"locators", "list", false, "Target locators for for_each/while_element_exists"},
      {"forEachStrategy", "string", false, "'snapshot_once' or 'requery_each_iteration' when loopMode='for_each'", "snapshot_once"},
      {"variable", "string", false, "Legacy optional variable identifier"},
      {"steps", "list", false, "Legacy nested steps (top-level preferred)"}
    }
  });
  return s;
}

const ActionSpec* FindActionSpec(const std::string& action_name) {
  static base::NoDestructor<std::vector<ActionSpec>> specs(GetActionSpecs());
  std::string lower = base::ToLowerASCII(action_name);
  for (const auto& spec : *specs) {
    if (spec.action_name == lower) {
      return &spec;
    }
  }
  return nullptr;
}

// Append a note into the action's "comment" string field.
void AppendComment(base::Value::Dict& action, const std::string& note) {
  std::string existing;
  if (const std::string* c = action.FindString("comment")) existing = *c;
  if (!existing.empty()) existing.append(" | ");
  existing.append(note);
  action.Set("comment", existing);
}

// JSON stringify helper used when moving invalid content into comments.
std::string JsonStringify(const base::Value& v) {
  std::string s; base::JSONWriter::Write(v, &s); return s;
}

void CanonicalizeNested(base::Value::Dict& d, const char* key) {
  if (base::Value* v = d.Find(key)) {
    if (v->is_list()) {
      auto& lst = v->GetList();
      for (auto& it : lst) {
        if (it.is_dict()) {
          base::Value::Dict dd = it.GetDict().Clone();
          ScriptSchema::CanonicalizeAction(dd);
          it = base::Value(std::move(dd));
        } else {
          // Move invalid into comment and drop
        }
      }
    } else {
      AppendComment(d, std::string("invalid ") + key + " moved: " + JsonStringify(*v));
      d.Remove(key);
    }
  }
}

bool IsControlFlowChildListKey(std::string_view key) {
  return key == "steps" || key == "then" || key == "else" ||
         key == "nested";
}

bool HasNonEmptyLocatorList(const base::Value::Dict& params, const char* key) {
  const base::Value::List* list = params.FindList(key);
  return list && !list->empty();
}

bool HasCoordinateDict(const base::Value::Dict& params, const char* key) {
  const base::Value::Dict* coords = params.FindDict(key);
  if (!coords) {
    return false;
  }
  // JSON round-trips often store whole numbers as doubles; accept both.
  if (coords->FindInt("x").has_value() && coords->FindInt("y").has_value()) {
    return true;
  }
  return coords->FindDouble("x").has_value() &&
         coords->FindDouble("y").has_value();
}

bool HasNonEmptyString(const base::Value::Dict& params, const char* key) {
  const std::string* value = params.FindString(key);
  return value && !base::TrimWhitespaceASCII(*value, base::TRIM_ALL).empty();
}

void PromoteControlFlowChildList(base::Value::Dict& action,
                                 base::Value::Dict& params,
                                 const char* key) {
  const base::Value::List* action_list = action.FindList(key);
  base::Value* params_value = params.Find(key);
  if (!params_value) {
    return;
  }

  if (!params_value->is_list()) {
    AppendComment(action, std::string("invalid params.") + key +
                              " moved: " + JsonStringify(*params_value));
    params.Remove(key);
    return;
  }

  base::Value::List promoted = params_value->GetList().Clone();
  ScriptSchema::CanonicalizeActionsList(promoted);
  const bool action_missing_or_empty = !action_list || action_list->empty();
  if (action_missing_or_empty && !promoted.empty()) {
    action.Set(key, std::move(promoted));
  }
  params.Remove(key);
}

std::string ReadCanonicalActionContextUrl(const base::Value::Dict& action) {
  std::string context_url;
  if (const std::string* context_value = action.FindString("contextUrl")) {
    context_url = *context_value;
  } else if (const std::string* legacy_context_value =
                 action.FindString("context_url")) {
    context_url = *legacy_context_value;
  }
  base::TrimWhitespaceASCII(context_url, base::TRIM_ALL, &context_url);
  return context_url;
}

std::string ReadCanonicalNavigateUrl(const base::Value::Dict& action) {
  const std::string* action_name = action.FindString("action");
  if (!action_name ||
      !base::EqualsCaseInsensitiveASCII(*action_name, "navigate")) {
    return std::string();
  }
  const base::Value::Dict* params = action.FindDict("params");
  if (!params) {
    return std::string();
  }
  std::string navigate_url;
  if (const std::string* value = params->FindString("url")) {
    navigate_url = *value;
  }
  base::TrimWhitespaceASCII(navigate_url, base::TRIM_ALL, &navigate_url);
  return navigate_url;
}

base::Value::List FlattenLegacyUrlContexts(const base::Value::List& ctxs) {
  base::Value::List flat;
  for (const auto& c : ctxs) {
    const base::Value::Dict* ctx = c.GetIfDict();
    if (!ctx) {
      continue;
    }
    if (const base::Value::List* acts = ctx->FindList("actions")) {
      for (const auto& action : *acts) {
        flat.Append(action.Clone());
      }
    }
  }
  return flat;
}

base::Value::List DeriveUrlContextsFromActions(const base::Value::List& actions,
                                               const std::string& start_url) {
  base::Value::List contexts;
  std::string current_url = start_url;
  base::TrimWhitespaceASCII(current_url, base::TRIM_ALL, &current_url);
  for (const auto& entry : actions) {
    const base::Value::Dict* action = entry.GetIfDict();
    if (!action) {
      continue;
    }
    const std::string explicit_context_url =
        ReadCanonicalActionContextUrl(*action);
    const std::string navigate_url = ReadCanonicalNavigateUrl(*action);
    const std::string context_url =
        !explicit_context_url.empty() ? explicit_context_url
        : !navigate_url.empty()      ? navigate_url
                                     : current_url;
    bool needs_new_context = true;
    if (!contexts.empty()) {
      const base::Value::Dict* last = contexts.back().GetIfDict();
      const std::string* last_url = last ? last->FindString("url") : nullptr;
      needs_new_context = !last_url || *last_url != context_url;
    }
    if (needs_new_context) {
      base::Value::Dict ctx;
      ctx.Set("url", context_url);
      ctx.Set("actions", base::Value::List());
      contexts.Append(std::move(ctx));
    }
    contexts.back().GetDict().FindList("actions")->Append(entry.Clone());
    if (!navigate_url.empty()) {
      current_url = navigate_url;
    } else if (!explicit_context_url.empty()) {
      current_url = explicit_context_url;
    }
  }
  return contexts;
}

std::string CanonicalizeLegacyActionName(const std::string& raw_name,
                                         const base::Value::Dict& params) {
  std::string name = base::ToLowerASCII(raw_name);
  if (name == "unknown") {
    if (params.FindString("url")) {
      return "navigate";
    }
    if (params.FindString("key")) {
      return "key_press";
    }
    if ((params.FindList("fromLocators") && !params.FindList("fromLocators")->empty()) ||
        (params.FindList("toLocators") && !params.FindList("toLocators")->empty()) ||
        params.FindDict("fromCoordinates") || params.FindDict("fromCoords") ||
        params.FindDict("toCoordinates") || params.FindDict("toCoords")) {
      return "drag";
    }
    if (params.FindString("conditionType") || params.FindList("then") ||
        params.FindList("else")) {
      return "if_condition";
    }
    if (params.FindList("fields") || params.FindList("rowLocators") ||
        params.FindString("variableName")) {
      if (params.FindList("fields") || params.FindList("rowLocators")) {
        return "scrape_table";
      }
      if (params.FindList("locators")) {
        return "scrape_element";
      }
    }
    if (params.FindString("condition") || params.FindString("captureVariable") ||
        params.FindString("onMatchScriptId")) {
      return "listen";
    }
    if (params.FindString("waitType") || params.Find("time") ||
        params.Find("timeout")) {
      return "wait";
    }
    if (params.Find("distance") || params.Find("percentX") ||
        params.Find("percentY")) {
      return "scroll";
    }
    const std::string* text = params.FindString("text");
    const bool has_locator_list =
        params.FindList("locators") && !params.FindList("locators")->empty();
    const bool has_single_locator = params.FindDict("locator") != nullptr;
    if (text && (has_locator_list || has_single_locator)) {
      return "type";
    }
    if (has_locator_list || has_single_locator) {
      const int click_count = params.FindInt("clickCount")
                                  .value_or(params.FindInt("click_count")
                                                .value_or(1));
      const bool hover_only = params.FindBool("hoverOnly").value_or(false);
      if (hover_only || click_count == 0) {
        return "hover";
      }
      return "click";
    }
  }

  if (name == "cdp_drag")
    return "drag";
  if (name == "cdp_type")
    return "type";
  if (name == "cdp_key_down")
    return "key_down";

  return name;
}

}  // namespace

base::Value::Dict ScriptSchema::CanonicalizeForSave(base::Value::Dict in,
                                                    const std::string& id,
                                                    base::Time now) {
  // id/name/schema_version
  in.Set("id", id);
  if (!in.FindString("name")) in.Set("name", "Untitled");
  in.Set("schema_version", "1.0.0");

  // timestamps
  const std::string now_iso = ToIso8601UTC(now);
  if (!in.FindString("created_at")) in.Set("created_at", now_iso);
  in.Set("updated_at", now_iso);

  // Primary timeline actions.
  if (base::Value* actions_any = in.Find("actions")) {
    if (actions_any->is_list()) {
      CanonicalizeActionsList(actions_any->GetList());
    } else {
      AppendComment(in, std::string("invalid actions moved: ") +
                            JsonStringify(*actions_any));
      in.Set("actions", base::Value::List());
    }
  } else {
    in.Set("actions", base::Value::List());
  }

  // Optional legacy URL contexts. Import them only when flat actions are
  // absent, then regenerate compatibility metadata from the canonical actions.
  if (base::Value* ctxs_any = in.Find("url_contexts"); ctxs_any &&
                              ctxs_any->is_list()) {
    auto& ctxs = ctxs_any->GetList();
    for (auto& c : ctxs) {
      if (!c.is_dict()) continue;
      base::Value::Dict ctx = c.GetDict().Clone();
      if (base::Value* acts_any = ctx.Find("actions")) {
        if (acts_any->is_list()) {
          auto& lst = acts_any->GetList();
          for (auto& it : lst) {
            if (it.is_dict()) {
              base::Value::Dict d = it.GetDict().Clone();
              CanonicalizeAction(d);
              it = base::Value(std::move(d));
            } else {
              AppendComment(ctx, std::string("invalid action moved: ") + JsonStringify(it));
              it = base::Value(base::Value::Dict());
            }
          }
        } else {
          AppendComment(ctx, std::string("invalid actions moved: ") + JsonStringify(*acts_any));
          ctx.Set("actions", base::Value::List());
        }
      } else {
        ctx.Set("actions", base::Value::List());
      }
      c = base::Value(ctx.Clone());
    }
  } else {
    in.Set("url_contexts", base::Value::List());
  }

  base::Value::List* actions = in.FindList("actions");
  base::Value::List* url_contexts = in.FindList("url_contexts");
  if (actions && actions->empty() && url_contexts && !url_contexts->empty()) {
    *actions = FlattenLegacyUrlContexts(*url_contexts);
    CanonicalizeActionsList(*actions);
  }

  const std::string* start_url_value = in.FindString("startUrl");
  const std::string start_url =
      start_url_value ? *start_url_value : std::string();
  if (actions) {
    in.Set("url_contexts", DeriveUrlContextsFromActions(*actions, start_url));
  }

  return in;
}

void ScriptSchema::CanonicalizeActionsList(base::Value::List& list) {
  for (auto& it : list) {
    if (!it.is_dict()) continue;
    base::Value::Dict d = it.GetDict().Clone();
    CanonicalizeAction(d);
    it = base::Value(std::move(d));
  }
}

void ScriptSchema::CanonicalizeAction(base::Value::Dict& action) {
  // Canonical form only accepts `params` as a dict.
  base::Value* params_any = action.Find("params");
  base::Value::Dict params;
  if (params_any && params_any->is_dict()) {
    params = params_any->GetDict().Clone();
  }

  // Canonical form is strict: action kind must be provided via `action`.
  const std::string* action_name = action.FindString("action");
  std::string name =
      action_name ? CanonicalizeLegacyActionName(*action_name, params) : "";
  if (name.empty())
    name = "__invalid__";
  action.Set("action", name);

  // Apply schema-based normalization
  const ActionSpec* spec = FindActionSpec(name);
  if (spec) {
    // Apply defaults for missing params
    for (const auto& param_spec : spec->params) {
      if (!params.Find(param_spec.name) &&
          !IsControlFlowChildListKey(param_spec.name)) {
        params.Set(param_spec.name, param_spec.GetDefaultValue());
      }
    }

    // Do not migrate unknown params into comments anymore. Unknowns are
    // tolerated and may be ignored by action SetParams.
  }

  // Normalize nested control-flow arrays
  CanonicalizeNested(action, "steps");
  CanonicalizeNested(action, "then");
  CanonicalizeNested(action, "else");
  CanonicalizeNested(action, "nested");
  PromoteControlFlowChildList(action, params, "steps");
  PromoteControlFlowChildList(action, params, "then");
  PromoteControlFlowChildList(action, params, "else");
  PromoteControlFlowChildList(action, params, "nested");

  // Canonical optional per-action URL context.
  std::string context_url = ReadCanonicalActionContextUrl(action);
  if (context_url.empty()) {
    action.Remove("contextUrl");
  } else {
    action.Set("contextUrl", context_url);
  }
  action.Remove("context_url");

  // Replace
  action.Set("params", std::move(params));
}

std::string ScriptSchema::ToIso8601UTC(base::Time t) {
  base::Time::Exploded ex;
  t.UTCExplode(&ex);
  // Format as YYYY-MM-DDTHH:MM:SSZ
  return base::StringPrintf("%04d-%02d-%02dT%02d:%02d:%02dZ",
                            ex.year, ex.month, ex.day_of_month,
                            ex.hour, ex.minute, ex.second);
}

// ============================================================================
// Schema Introspection API
// ============================================================================

base::Value::Dict ScriptSchema::GetActionTemplate(const std::string& action_type) {
  base::Value::Dict action;
  std::string lower = base::ToLowerASCII(action_type);
  action.Set("action", lower);

  base::Value::Dict params;
  const ActionSpec* spec = FindActionSpec(lower);
  if (spec) {
    for (const auto& param : spec->params) {
      params.Set(param.name, param.GetDefaultValue());
    }
  }

  action.Set("params", std::move(params));
  // Ensure template is normalized the same way persisted actions are.
  CanonicalizeAction(action);
  if (!action.FindString("comment"))
    action.Set("comment", "");
  return action;
}

bool ScriptSchema::ValidateAction(const base::Value::Dict& action, std::string* error) {
  const std::string* action_name = action.FindString("action");
  if (!action_name) {
    if (error) *error = "Missing 'action' field";
    return false;
  }

  const ActionSpec* spec = FindActionSpec(*action_name);
  if (!spec) {
    if (error) *error = "Unknown action type: " + *action_name;
    return false;
  }

  const base::Value::Dict* params = action.FindDict("params");
  if (!params) {
    if (error) *error = "Missing 'params' field";
    return false;
  }

  // Check required params
  for (const auto& param_spec : spec->params) {
    if (!param_spec.required)
      continue;

    const base::Value* value = params->Find(param_spec.name);
    if (!value) {
      if (error) {
        *error = std::string("Missing required parameter '") + param_spec.name +
                 "' for action '" + spec->action_name + "'";
      }
      return false;
    }

    const std::string_view type = param_spec.type ? param_spec.type : "";
    if (type == "string") {
      const std::string* text = value->GetIfString();
      if (!text ||
          base::TrimWhitespaceASCII(*text, base::TRIM_ALL).empty()) {
        if (error) {
          *error = std::string("Required parameter '") + param_spec.name +
                   "' for action '" + spec->action_name + "' cannot be empty";
        }
        return false;
      }
    } else if (type == "list") {
      const base::Value::List* list = value->GetIfList();
      if (!list || list->empty()) {
        if (error) {
          *error = std::string("Required parameter '") + param_spec.name +
                   "' for action '" + spec->action_name + "' cannot be empty";
        }
        return false;
      }
    } else if (type == "dict") {
      if (!value->is_dict()) {
        if (error) {
          *error = std::string("Required parameter '") + param_spec.name +
                   "' for action '" + spec->action_name + "' must be an object";
        }
        return false;
      }
    }
  }

  // Additional cross-field validation for specific actions.
  if (*action_name == "click" || *action_name == "hover") {
    // ClickAction requires locators or direct coordinates.
    bool has_locators = HasNonEmptyLocatorList(*params, "locators");
    bool has_coords = HasCoordinateDict(*params, "coordinates");
    if (!has_locators && !has_coords) {
      if (error) {
        *error = (*action_name == "hover")
                     ? "hover requires 'locators' or 'coordinates'"
                     : "click requires 'locators' or 'coordinates'";
      }
      return false;
    }
  }

  if (*action_name == "scroll") {
    const std::string* element = params->FindString("element");
    const std::string element_type = element ? *element : "window";
    if (element_type == "element") {
      const bool has_locators = HasNonEmptyLocatorList(*params, "locators");
      if (!has_locators) {
        if (error) {
          *error = "scroll requires 'locators' when element=='element'";
        }
        return false;
      }
    }
  }

  if (*action_name == "drag") {
    const bool has_from_locs = HasNonEmptyLocatorList(*params, "fromLocators");
    const bool has_to_locs = HasNonEmptyLocatorList(*params, "toLocators");
    const bool has_from_coords = HasCoordinateDict(*params, "fromCoordinates");
    const bool has_to_coords = HasCoordinateDict(*params, "toCoordinates");

    if (!has_from_locs && !has_from_coords) {
      if (error)
        *error = "drag requires 'fromLocators' or 'fromCoordinates'";
      return false;
    }
    if (!has_to_locs && !has_to_coords) {
      if (error)
        *error = "drag requires 'toLocators' or 'toCoordinates'";
      return false;
    }
  }

  if (*action_name == "loop") {
    const base::Value::List* steps = action.FindList("steps");
    if (!steps) {
      steps = params->FindList("steps");
    }
    if (!steps || steps->empty()) {
      if (error)
        *error = "loop requires at least one nested step";
      return false;
    }

    const std::string* mode_raw = params->FindString("loopMode");
    const std::string mode =
        base::ToLowerASCII(mode_raw ? *mode_raw : std::string("count"));
    if (mode == "count" || mode.empty()) {
      const int iterations = params->FindInt("iterations").value_or(
          params->FindInt("maxIterations").value_or(0));
      if (iterations < 1) {
        if (error)
          *error = "count loop requires iterations >= 1";
        return false;
      }
    } else if (mode == "for_each" || mode == "while_element_exists") {
      const bool has_locators = HasNonEmptyLocatorList(*params, "locators");
      if (!has_locators) {
        if (error)
          *error = (mode == "for_each")
                       ? "for_each loop requires at least one locator"
                       : "while_element_exists loop requires at least one locator";
        return false;
      }
      const int max_iterations =
          params->FindInt("maxIterations").value_or(mode == "for_each" ? 200 : 50);
      if (max_iterations < 1) {
        if (error)
          *error = (mode == "for_each")
                       ? "for_each loop requires maxIterations >= 1"
                       : "while_element_exists loop requires maxIterations >= 1";
        return false;
      }
      if (mode == "for_each") {
        const std::string* strategy_raw = params->FindString("forEachStrategy");
        const std::string strategy = base::ToLowerASCII(
            strategy_raw ? *strategy_raw : std::string("snapshot_once"));
        if (strategy != "snapshot_once" &&
            strategy != "requery_each_iteration") {
          if (error) {
            *error =
                "forEachStrategy must be 'snapshot_once' or 'requery_each_iteration'";
          }
          return false;
        }
      }
    } else {
      if (error) {
        *error =
            "loopMode must be 'count', 'for_each', or 'while_element_exists'";
      }
      return false;
    }
  }

  if (*action_name == "wait") {
    const std::string* wait_type_raw = params->FindString("waitType");
    if (!wait_type_raw) {
      wait_type_raw = params->FindString("wait_type");
    }
    std::string wait_type =
        base::ToLowerASCII(wait_type_raw ? *wait_type_raw : std::string());
    if (wait_type.empty()) {
      wait_type =
          (HasNonEmptyLocatorList(*params, "locators") ||
           params->Find("timeout") != nullptr)
              ? "element"
              : "time";
    }
    if (wait_type == "time") {
      const int time_ms = params->FindInt("time").value_or(0);
      if (time_ms <= 0) {
        if (error)
          *error = "waitType 'time' requires time > 0";
        return false;
      }
    } else if (wait_type == "element") {
      if (!HasNonEmptyLocatorList(*params, "locators")) {
        if (error)
          *error = "waitType 'element' requires at least one locator";
        return false;
      }
      const int timeout_ms = params->FindInt("timeout").value_or(0);
      if (timeout_ms <= 0) {
        if (error)
          *error = "waitType 'element' requires timeout > 0";
        return false;
      }
    } else {
      if (error)
        *error = "waitType must be 'time' or 'element'";
      return false;
    }
  }

  if (*action_name == "if_condition") {
    const std::string* cond_raw = params->FindString("conditionType");
    if (!cond_raw) {
      cond_raw = params->FindString("condition_type");
    }
    std::string condition =
        base::ToLowerASCII(cond_raw ? *cond_raw : std::string());
    if (condition.empty()) {
      if (HasNonEmptyString(*params, "text")) {
        condition = "text_contains";
      } else if (HasNonEmptyLocatorList(*params, "locators")) {
        condition = "element_exists";
      }
    }
    if (condition.empty()) {
      if (error)
        *error = "if_condition requires conditionType";
      return false;
    }
    if (condition == "element_exists") {
      if (!HasNonEmptyLocatorList(*params, "locators")) {
        if (error)
          *error = "if_condition element_exists requires at least one locator";
        return false;
      }
    } else if (condition == "text_contains") {
      if (!HasNonEmptyString(*params, "text")) {
        if (error)
          *error = "if_condition text_contains requires non-empty text";
        return false;
      }
    } else {
      if (error)
        *error = "if_condition conditionType must be 'element_exists' or 'text_contains'";
      return false;
    }
  }

  if (*action_name == "authenticate") {
    const base::Value::List* methods = params->FindList("methods");
    if (!methods) {
      if (error)
        *error = "authenticate requires methods";
      return false;
    }
    for (const base::Value& method_value : *methods) {
      const base::Value::Dict* method = method_value.GetIfDict();
      const std::string* id = method ? method->FindString("id") : nullptr;
      if (!method || !id || id->empty()) {
        if (error)
          *error = "authenticate methods require an id";
        return false;
      }
    }
  }

  if (*action_name == "listen") {
    if (!HasNonEmptyLocatorList(*params, "locators")) {
      if (error)
        *error = "listen requires at least one locator";
      return false;
    }
    const std::string* cond_raw = params->FindString("condition");
    const std::string condition = base::ToLowerASCII(
        cond_raw ? *cond_raw : std::string());
    if (condition.empty()) {
      if (error)
        *error = "listen requires condition";
      return false;
    }
    const bool needs_text = condition == "text_contains" ||
                            condition == "text_equals" ||
                            condition == "new_child_contains" ||
                            condition == "attribute_contains" ||
                            condition == "attribute_equals";
    const bool needs_attribute = condition == "attribute_contains" ||
                                 condition == "attribute_equals";
    if (needs_text && !HasNonEmptyString(*params, "text")) {
      if (error)
        *error = "listen condition requires non-empty text";
      return false;
    }
    if (needs_attribute && !HasNonEmptyString(*params, "attribute")) {
      if (error)
        *error = "listen attribute condition requires non-empty attribute";
      return false;
    }
  }

  return true;
}

base::Value::List ScriptSchema::GetSupportedActions() {
  base::Value::List result;
  static base::NoDestructor<std::vector<ActionSpec>> specs(GetActionSpecs());

  for (const auto& spec : *specs) {
    base::Value::Dict action_info;
    action_info.Set("action", spec.action_name);
    action_info.Set("display_name", spec.display_name);
    action_info.Set("description", spec.description);

    base::Value::List params_list;
    for (const auto& param : spec.params) {
      base::Value::Dict param_info;
      param_info.Set("name", param.name);
      param_info.Set("type", param.type);
      param_info.Set("required", param.required);
      param_info.Set("default", param.GetDefaultValue());
      param_info.Set("description", param.description);
      params_list.Append(std::move(param_info));
    }
    action_info.Set("params", std::move(params_list));

    result.Append(std::move(action_info));
  }

  return result;
}

// static
std::string ScriptSchema::ResolveResumeNavigationUrl(
    const base::Value::List& timeline,
    int step_index) {
  if (step_index < 0 || timeline.empty()) {
    return std::string();
  }
  const size_t start =
      std::min(static_cast<size_t>(step_index), timeline.size() - 1);

  // Prefer the resume step's contextUrl; if empty (e.g. scrape with no
  // recorded URL), walk backward so we still land on the right page.
  for (size_t i = start + 1; i > 0; --i) {
    const base::Value::Dict* action = timeline[i - 1].GetIfDict();
    if (!action) {
      continue;
    }
    std::string url = ReadCanonicalActionContextUrl(*action);
    if (url.empty()) {
      url = ReadCanonicalNavigateUrl(*action);
    }
    if (!url.empty()) {
      const GURL gurl(url);
      if (gurl.is_valid() && !gurl.SchemeIs("about") &&
          gurl.spec() != "about:blank") {
        return gurl.GetWithoutRef().spec();
      }
    }
  }
  return std::string();
}

// static
std::string ScriptSchema::ExtractRunHistoryStepUrl(
    const base::Value::List* steps,
    int step_index) {
  if (!steps || step_index < 0) {
    return std::string();
  }
  for (const auto& it : *steps) {
    const base::Value::Dict* sd = it.GetIfDict();
    if (!sd) {
      continue;
    }
    const std::optional<int> si = sd->FindInt("stepIndex");
    if (!si || *si != step_index) {
      continue;
    }
    std::string url;
    if (const std::string* u = sd->FindString("url_before")) {
      url = *u;
    }
    if (url.empty()) {
      if (const std::string* u = sd->FindString("observed_context_url")) {
        url = *u;
      }
    }
    if (url.empty()) {
      if (const std::string* u = sd->FindString("context_url")) {
        url = *u;
      }
    }
    if (url.empty()) {
      if (const std::string* u = sd->FindString("url_after")) {
        url = *u;
      }
    }
    base::TrimWhitespaceASCII(url, base::TRIM_ALL, &url);
    const GURL gurl(url);
    if (gurl.is_valid() && !gurl.SchemeIs("about") &&
        gurl.spec() != "about:blank") {
      return gurl.GetWithoutRef().spec();
    }
    return std::string();
  }
  return std::string();
}

// static
bool ScriptSchema::ContextUrlsEquivalent(const std::string& left,
                                         const std::string& right) {
  std::string l = left;
  std::string r = right;
  base::TrimWhitespaceASCII(l, base::TRIM_ALL, &l);
  base::TrimWhitespaceASCII(r, base::TRIM_ALL, &r);
  if (l.empty() || r.empty()) {
    return l == r;
  }
  const GURL left_url(l);
  const GURL right_url(r);
  if (left_url.is_valid() && right_url.is_valid()) {
    return left_url.GetWithoutRef() == right_url.GetWithoutRef();
  }
  return l == r;
}

}  // namespace dashboard

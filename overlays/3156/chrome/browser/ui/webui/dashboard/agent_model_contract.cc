// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/agent_model_contract.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"

namespace dashboard {
namespace {

bool IsSafeAgentLocatorType(std::string_view type) {
  return !base::EqualsCaseInsensitiveASCII(type, "VISUAL_COORDS");
}

base::Value::List SanitizeLocators(const base::Value::List& locators) {
  base::Value::List sanitized;
  for (const base::Value& locator_value : locators) {
    const base::Value::Dict* locator = locator_value.GetIfDict();
    const std::string* type = locator ? locator->FindString("type") : nullptr;
    const std::string* value = locator ? locator->FindString("value") : nullptr;
    if (!type || !value || value->empty() || !IsSafeAgentLocatorType(*type)) {
      continue;
    }
    base::Value::Dict out;
    out.Set("type", *type);
    out.Set("value", *value);
    sanitized.Append(std::move(out));
  }
  return sanitized;
}

void EnsureCssLocatorFromSelectorAliases(base::Value::Dict& params) {
  // Prefer explicit shorthand over any pre-existing locator list so surgical
  // step edits like {"do":"update_step","id":"username"} rewrite the target.
  if (const std::string* id = params.FindString("id"); id && !id->empty()) {
    base::Value::List locators;
    base::Value::Dict locator;
    locator.Set("type", "ID");
    locator.Set("value", *id);
    locators.Append(std::move(locator));
    params.Set("locators", std::move(locators));
    params.Remove("id");
    return;
  }

  if (const std::string* sel = params.FindString("sel");
      sel && !sel->empty() && !params.FindString("selector")) {
    params.Set("selector", *sel);
    params.Remove("sel");
  }

  const char* keys[] = {"selector", "css", "cssSelector", "css_selector"};
  for (const char* key : keys) {
    if (const std::string* selector = params.FindString(key);
        selector && !selector->empty()) {
      base::Value::List locators;
      base::Value::Dict locator;
      locator.Set("type", "CSS_SELECTOR");
      locator.Set("value", *selector);
      locators.Append(std::move(locator));
      params.Set("locators", std::move(locators));
      params.Remove(key);
      return;
    }
  }

  if (const std::string* xpath = params.FindString("xpath");
      xpath && !xpath->empty()) {
    base::Value::List locators;
    base::Value::Dict locator;
    locator.Set("type", "XPATH");
    locator.Set("value", *xpath);
    locators.Append(std::move(locator));
    params.Set("locators", std::move(locators));
    params.Remove("xpath");
  }
}

void PromoteTopLevelFieldsIntoParams(base::Value::Dict& step,
                                     base::Value::Dict& params) {
  // Playwright-ish / loose model shapes often put url/text/selector on the
  // step root instead of inside params.
  if (!params.FindString("url")) {
    if (const std::string* url = step.FindString("url"); url && !url->empty()) {
      params.Set("url", *url);
    }
  }
  if (!params.FindString("text")) {
    if (const std::string* text = step.FindString("text"); text) {
      params.Set("text", *text);
    } else if (const std::string* value = step.FindString("value"); value) {
      // Prefer explicit text; fall back to value for type/fill.
      params.Set("text", *value);
    }
  }
  for (const char* key : {"selector", "sel", "css", "cssSelector", "css_selector",
                          "xpath", "timeout", "timeout_ms", "time", "duration",
                          "ms", "waitType", "wait_type", "id", "el"}) {
    if (params.Find(key)) {
      continue;
    }
    if (const base::Value* v = step.Find(key)) {
      params.Set(key, v->Clone());
    }
  }
}

std::string AliasActionName(std::string_view raw) {
  const std::string lower = base::ToLowerASCII(raw);
  if (lower == "fill" || lower == "input" || lower == "enter_text") {
    return "type";
  }
  if (lower == "click_element" || lower == "clickelement") {
    return "click";
  }
  if (lower == "get_element_text" || lower == "gettext" ||
      lower == "get_text") {
    return "scrape_element";
  }
  if (lower == "wait_for_element" || lower == "waitforelement" ||
      lower == "wait_for" || lower == "waitfor") {
    return "wait";
  }
  if (lower == "goto" || lower == "open" || lower == "open_url") {
    return "navigate";
  }
  if (lower == "keypress" || lower == "press_key" || lower == "press") {
    return "key_press";
  }
  if (lower == "pause" || lower == "sleep" || lower == "delay") {
    return "wait";
  }
  return lower;
}

void NormalizeLocatorList(base::Value::Dict& params) {
  if (const std::string* as_string = params.FindString("locators");
      as_string && !as_string->empty()) {
    base::Value::List locators;
    base::Value::Dict locator;
    locator.Set("type", "CSS_SELECTOR");
    locator.Set("value", *as_string);
    locators.Append(std::move(locator));
    params.Set("locators", std::move(locators));
    return;
  }
  base::Value::List* locators = params.FindList("locators");
  if (!locators) {
    return;
  }
  base::Value::List normalized;
  for (base::Value& entry : *locators) {
    if (entry.is_string() && !entry.GetString().empty()) {
      base::Value::Dict locator;
      locator.Set("type", "CSS_SELECTOR");
      locator.Set("value", entry.GetString());
      normalized.Append(std::move(locator));
      continue;
    }
    if (base::Value::Dict* locator = entry.GetIfDict()) {
      if (!locator->FindString("type") || locator->FindString("type")->empty()) {
        locator->Set("type", "CSS_SELECTOR");
      }
      if (const std::string* value = locator->FindString("value");
          !value || value->empty()) {
        if (const std::string* sel = locator->FindString("selector");
            sel && !sel->empty()) {
          locator->Set("value", *sel);
          locator->Remove("selector");
        }
      }
      normalized.Append(std::move(*locator));
    }
  }
  params.Set("locators", std::move(normalized));
}

void StringifyTargetIdAliases(base::Value::Dict& params) {
  const char* keys[] = {"el",        "n",          "target",     "target_id",
                        "targetId",  "element",    "element_id", "elementId",
                        "snapshot_id", "snapshotId", "id"};
  if (params.FindString("id") && !params.FindString("id")->empty()) {
    return;
  }
  for (const char* key : keys) {
    if (const std::string* as_string = params.FindString(key);
        as_string && !as_string->empty()) {
      params.Set("id", *as_string);
      return;
    }
    if (std::optional<int> as_int = params.FindInt(key)) {
      params.Set("id", base::NumberToString(*as_int));
      return;
    }
    if (std::optional<double> as_double = params.FindDouble(key)) {
      params.Set("id", base::NumberToString(static_cast<int>(*as_double)));
      return;
    }
  }
}

std::string AliasCommandName(std::string_view raw) {
  const std::string lower = base::ToLowerASCII(raw);
  if (lower == "look" || lower == "refresh" || lower == "pagemap" ||
      lower == "refresh_pagemap") {
    return "refresh_pagemap";
  }
  if (lower == "more" || lower == "next" || lower == "more_context" ||
      lower == "request_more_context" || lower == "request_full_context") {
    return "request_more_context";
  }
  if (lower == "see" || lower == "photo" || lower == "screenshot" ||
      lower == "inspect_image" || lower == "request_screenshot") {
    return "request_screenshot";
  }
  if (lower == "done" || lower == "finish" || lower == "stop" ||
      lower == "complete" || lower == "agent_finish") {
    return "agent_finish";
  }
  if (lower == "reload" || lower == "reload_page") {
    return "reload_page";
  }
  if (lower == "investigate" || lower == "investigate_page" ||
      lower == "dashboard_investigate_page") {
    return "dashboard_investigate_page";
  }
  if (lower == "list_scripts" || lower == "dashboard_list_scripts") {
    return "dashboard_list_scripts";
  }
  if (lower == "list_collections" || lower == "dashboard_list_collections") {
    return "dashboard_list_collections";
  }
  if (lower == "get_script" || lower == "read_script" ||
      lower == "dashboard_get_script_json") {
    return "dashboard_get_script_json";
  }
  if (lower == "get_step" || lower == "read_step" || lower == "inspect_step" ||
      lower == "dashboard_get_script_step") {
    return "dashboard_get_script_step";
  }
  if (lower == "update_step" || lower == "edit_step" || lower == "patch_step" ||
      lower == "dashboard_update_script_step") {
    return "dashboard_update_script_step";
  }
  if (lower == "delete_step" || lower == "dashboard_delete_script_step") {
    return "dashboard_delete_script_step";
  }
  if (lower == "save_script" || lower == "write_script" ||
      lower == "dashboard_save_script_json") {
    return "dashboard_save_script_json";
  }
  if (lower == "run_script" || lower == "dashboard_run_script") {
    return "dashboard_run_script";
  }
  if (lower == "select_script" || lower == "dashboard_select_script") {
    return "dashboard_select_script";
  }
  if (lower == "resume_script" ||
      lower == "dashboard_resume_script_from_step") {
    return "dashboard_resume_script_from_step";
  }
  if (lower == "set_view" || lower == "dashboard_set_view") {
    return "dashboard_set_view";
  }
  if (lower == "set_setting" || lower == "dashboard_set_global_setting") {
    return "dashboard_set_global_setting";
  }
  if (lower == "listener_match" || lower == "configure_listener" ||
      lower == "dashboard_configure_listener_match") {
    return "dashboard_configure_listener_match";
  }
  if (lower == "record_action" || lower == "dashboard_record_agent_action") {
    return "dashboard_record_agent_action";
  }
  if (lower == "heal" || lower == "queue_heal" ||
      lower == "dashboard_queue_auto_heal") {
    return "dashboard_queue_auto_heal";
  }
  if (lower == "process_heal" ||
      lower == "dashboard_process_auto_heal_queue") {
    return "dashboard_process_auto_heal_queue";
  }
  if (lower == "list_failures" ||
      lower == "dashboard_list_timeline_failures") {
    return "dashboard_list_timeline_failures";
  }
  if (lower == "note" || lower == "remember" || lower == "store_memento") {
    return "store_memento";
  }
  if (lower == "read_note" || lower == "read_memento") {
    return "read_memento";
  }
  if (lower == "timeline" || lower == "run_timeline") {
    return "run_timeline";
  }
  if (lower == "save_recent_actions_as_script" ||
      lower == "promote_span_to_macro" || lower == "save_task_script") {
    return "save_task_script";
  }
  if (lower == "call_macro") {
    return "call_macro";
  }
  return lower;
}

bool IsMetaCommand(std::string_view command) {
  return command == "call_action" || command == "run_timeline" ||
         command == "refresh_pagemap" || command == "request_more_context" ||
         command == "request_more_controls" ||
         command == "request_primary_options" ||
         command == "request_screenshot" || command == "reload_page" ||
         command == "inspect_element" || command == "save_task_script" ||
         command == "call_macro" || command == "agent_finish" ||
         command == "store_memento" || command == "read_memento" ||
         command == "reset_agent" || base::StartsWith(command, "dashboard_");
}

void FoldSiblingsIntoPayload(base::Value::Dict& cmd) {
  if (cmd.FindList("payload")) {
    return;
  }
  if (!cmd.FindDict("payload")) {
    cmd.Set("payload", base::Value::Dict());
  }
  base::Value::Dict* payload = cmd.FindDict("payload");
  if (!payload) {
    return;
  }
  std::vector<std::string> move_keys;
  for (const auto item : cmd) {
    if (item.first == "command" || item.first == "do" ||
        item.first == "payload") {
      continue;
    }
    if (payload->contains(item.first)) {
      continue;
    }
    move_keys.push_back(item.first);
  }
  for (const std::string& key : move_keys) {
    if (const base::Value* value = cmd.Find(key)) {
      payload->Set(key, value->Clone());
      cmd.Remove(key);
    }
  }
}

base::Value::Dict CoerceLooseCommand(const base::Value::Dict& raw) {
  base::Value::Dict out = raw.Clone();
  std::string command;
  if (const std::string* existing = out.FindString("command");
      existing && !existing->empty()) {
    command = *existing;
  } else if (const std::string* do_name = out.FindString("do");
             do_name && !do_name->empty()) {
    command = *do_name;
    out.Remove("do");
  } else if (const std::string* action = out.FindString("action");
             action && !action->empty()) {
    command = "call_action";
  }
  if (command.empty()) {
    return out;
  }
  command = AliasCommandName(command);
  out.Set("command", command);
  FoldSiblingsIntoPayload(out);

  if (command == "agent_finish") {
    if (base::Value::Dict* payload = out.FindDict("payload")) {
      if (!payload->FindString("summary")) {
        if (const std::string* why = payload->FindString("why")) {
          payload->Set("summary", *why);
        } else if (const std::string* reason = payload->FindString("reason")) {
          payload->Set("summary", *reason);
        }
      }
      if (!payload->FindString("status")) {
        payload->Set("status", "solved");
      }
    }
  }

  if (command == "dashboard_get_script_step" ||
      command == "dashboard_update_script_step" ||
      command == "dashboard_delete_script_step" ||
      command == "dashboard_resume_script_from_step") {
    if (base::Value::Dict* payload = out.FindDict("payload")) {
      if (!payload->Find("step_index") && payload->Find("step")) {
        if (std::optional<int> step = payload->FindInt("step")) {
          payload->Set("step_index", *step);
        } else if (const std::string* step_s = payload->FindString("step")) {
          int parsed = 0;
          if (base::StringToInt(*step_s, &parsed)) {
            payload->Set("step_index", parsed);
          }
        } else if (std::optional<double> step_d = payload->FindDouble("step")) {
          payload->Set("step_index", static_cast<int>(*step_d));
        }
        payload->Remove("step");
      }
    }
  }

  if (base::Value::Dict* payload = out.FindDict("payload")) {
    if (const std::string* sel = payload->FindString("sel");
        sel && !payload->FindString("selector")) {
      payload->Set("selector", *sel);
      payload->Remove("sel");
    }
    StringifyTargetIdAliases(*payload);
    if (std::optional<int> ms = payload->FindInt("ms")) {
      if (!payload->Find("timeout_ms") && !payload->Find("time")) {
        payload->Set("timeout_ms", *ms);
      }
      payload->Remove("ms");
    }
  }
  return out;
}

void NormalizeWaitParams(base::Value::Dict& params) {
  if (const std::string* wait_type = params.FindString("waitType");
      wait_type && !wait_type->empty()) {
    const std::string wt = base::ToLowerASCII(*wait_type);
    if (wt == "visible" || wt == "attached" || wt == "present" ||
        wt == "selector" || wt == "domcontentloaded") {
      params.Set("waitType", "element");
    } else if (wt == "timeout" || wt == "delay" || wt == "ms" ||
               wt == "milliseconds") {
      params.Set("waitType", "time");
    } else {
      params.Set("waitType", wt);
    }
  } else if (const std::string* wait_type_alt = params.FindString("wait_type");
             wait_type_alt && !wait_type_alt->empty()) {
    params.Set("waitType", base::ToLowerASCII(*wait_type_alt));
    params.Remove("wait_type");
    NormalizeWaitParams(params);
    return;
  } else {
    params.Set("waitType", "time");
  }

  const std::string* normalized_type = params.FindString("waitType");
  if (normalized_type && *normalized_type == "time") {
    if (!params.Find("time")) {
      if (std::optional<int> timeout_ms = params.FindInt("timeout_ms")) {
        params.Set("time", *timeout_ms);
        params.Remove("timeout_ms");
      } else if (std::optional<int> ms = params.FindInt("ms")) {
        params.Set("time", *ms);
        params.Remove("ms");
      } else if (std::optional<double> duration = params.FindDouble("duration")) {
        // Treat small numbers as seconds, large as ms (same heuristic as
        // hosted client).
        const double d = *duration;
        params.Set("time", d > 1000.0 ? static_cast<int>(d)
                                      : static_cast<int>(d * 1000.0));
        params.Remove("duration");
      } else if (std::optional<int> timeout = params.FindInt("timeout")) {
        params.Set("time", *timeout);
      }
    }
  } else if (normalized_type && *normalized_type == "element") {
    if (!params.Find("timeout")) {
      if (std::optional<int> timeout_ms = params.FindInt("timeout_ms")) {
        params.Set("timeout", *timeout_ms);
        params.Remove("timeout_ms");
      } else if (std::optional<int> time = params.FindInt("time")) {
        params.Set("timeout", *time);
      } else {
        params.Set("timeout", 5000);
      }
    }
  }
}

}  // namespace

// static
std::string AgentModelContract::NormalizeLoopProfile(
    std::string_view profile) {
  const std::string raw = base::ToLowerASCII(profile);
  if (raw == kLoopProfileGlicTask) {
    return kLoopProfileGlicTask;
  }
  if (raw == kLoopProfileInvestigator) {
    return kLoopProfileInvestigator;
  }
  return kLoopProfileDashboardAssistant;
}

// static
std::string AgentModelContract::BuildActionShapePromptBlock() {
  // Deliberately tiny: the adapter expands shorthand. Local CPU models pay for
  // every token of schema pedantry.
  return R"(Targeting: prefer {"id":"<from context>"} or {"sel":"css"}.
Adapter expands shorthand. Do not invent long locator arrays.)";
}

// static
std::string AgentModelContract::BuildCoreCommandSchemaPromptBlock() {
  return R"(One JSON object per turn (markdown optional):
{"do":"click","id":"..."}
{"do":"type","id":"...","text":"..."}
{"do":"wait","ms":1000}
{"do":"goto","url":"https://..."}
{"do":"look"}
{"do":"more"}
{"do":"see"}
{"do":"done","why":"..."}
Verbs: click type wait goto look more see done. One move only.)";
}

// static
std::string AgentModelContract::BuildDashboardCommandSchemaPromptBlock() {
  return BuildCoreCommandSchemaPromptBlock() + R"(
Script tools:
{"do":"list_scripts"}
{"do":"get_step","step":2}
{"do":"update_step","step":2,"id":"username"}
{"do":"get_script"}
{"do":"save_script","script_json":{...}}
{"do":"run_script"}
{"do":"investigate","url":"..."}
Prefer get_step/update_step for one failed action. Do not call heal while healing.)";
}

// static
std::string AgentModelContract::BuildScriptJsonReferencePromptBlock() {
  return R"(Saved script shape: {"name":"...","actions":[{"action":"navigate","params":{"url":"..."}},{"action":"click","params":{"sel":"button"}}]}
Shorthand sel/id is fine; adapter canonicalizes.)";
}

// static
std::string AgentModelContract::BuildSystemPromptForLoopProfile(
    std::string_view profile) {
  const std::string normalized = NormalizeLoopProfile(profile);

  if (normalized == kLoopProfileGlicTask) {
    return base::StrCat({
        "Browser agent. Complete the user goal with the fewest moves.\n"
        "Context is a trimmed accessibility list (not full DOM). After goto, "
        "call look if login/reward controls are missing from CURRENT context "
        "before typing. Never type credentials without a visible login "
        "control in context. Between username/password/submit, keep acting "
        "without requesting more context unless blocked.\n"
        "Credentials: type Dashboard variable/secret tokens from the goal and "
        "token list (e.g. {Email}/{SITE_PW}). Never hardcode a site password "
        "name. Stop with done when solved or blocked.\n\n",
        BuildCoreCommandSchemaPromptBlock(),
        "\n",
        BuildActionShapePromptBlock(),
    });
  }

  if (normalized == kLoopProfileInvestigator) {
    return base::StrCat({
        "Investigator. Diagnose page/script failures with the fewest tools.\n"
        "Prefer investigate -> get_step -> done. Do not rewrite whole scripts "
        "unless asked.\n\n"
        "{\"do\":\"investigate\",\"url\":\"...\"}\n"
        "{\"do\":\"get_step\",\"step\":N}\n"
        "{\"do\":\"done\",\"why\":\"...\"}\n",
        BuildActionShapePromptBlock(),
    });
  }

  return base::StrCat({
      "Dashboard assistant. Fix/build scripts with short tool calls.\n"
      "Prefer investigate -> small edit -> run. Plain text OK when no tool "
      "needed.\n\n",
      BuildDashboardCommandSchemaPromptBlock(),
      "\n",
      BuildActionShapePromptBlock(),
      "\n",
      BuildScriptJsonReferencePromptBlock(),
  });
}

// static
base::Value::Dict AgentModelContract::NormalizeModelActionPayload(
    std::string_view action_or_command,
    const base::Value::Dict* payload) {
  base::Value::Dict wrapper;
  std::string action_name = AliasActionName(action_or_command);
  base::Value::Dict params = payload ? payload->Clone() : base::Value::Dict();

  // If the model nested the real action inside payload.action, prefer that.
  if (const std::string* nested = params.FindString("action");
      nested && !nested->empty() &&
      (action_name == "call_action" || action_name.empty())) {
    action_name = AliasActionName(*nested);
    if (const base::Value::Dict* nested_params = params.FindDict("params")) {
      params = nested_params->Clone();
    } else {
      params.Remove("action");
    }
  }

  if (action_name == "scrape_element") {
    if (!params.Find("variableName")) {
      params.Set("variableName", "__agent_scraped_text");
    }
    if (!params.Find("dataType")) {
      params.Set("dataType", "text");
    }
  }

  if (action_name == "wait") {
    NormalizeWaitParams(params);
  }

  if (const std::string* sel = params.FindString("sel");
      sel && !params.FindString("selector")) {
    params.Set("selector", *sel);
    params.Remove("sel");
  }
  StringifyTargetIdAliases(params);
  EnsureCssLocatorFromSelectorAliases(params);
  NormalizeLocatorList(params);

  // Single locator object → locators array.
  if (!params.FindList("locators")) {
    if (const base::Value::Dict* locator = params.FindDict("locator")) {
      base::Value::List locators;
      locators.Append(locator->Clone());
      params.Set("locators", std::move(locators));
      params.Remove("locator");
      NormalizeLocatorList(params);
    }
  }

  wrapper.Set("action", action_name);
  wrapper.Set("params", std::move(params));
  ScriptSchema::CanonicalizeAction(wrapper);
  return wrapper;
}

// static
std::optional<base::Value::Dict> AgentModelContract::NormalizeScriptAction(
    const base::Value::Dict& step) {
  base::Value::Dict working = step.Clone();

  std::string action_name;
  if (const std::string* action = working.FindString("action");
      action && !action->empty()) {
    action_name = *action;
  } else if (const std::string* type = working.FindString("type");
             type && !type->empty()) {
    // Playwright-ish: {"type":"goto","url":"..."}
    action_name = *type;
    working.Remove("type");
  }
  if (action_name.empty()) {
    return std::nullopt;
  }

  base::Value::Dict params;
  if (const base::Value::Dict* existing = working.FindDict("params")) {
    params = existing->Clone();
  }
  PromoteTopLevelFieldsIntoParams(working, params);

  base::Value::Dict normalized =
      NormalizeModelActionPayload(action_name, &params);
  const std::string* out_action = normalized.FindString("action");
  if (!out_action || out_action->empty() || *out_action == "__invalid__") {
    return std::nullopt;
  }
  return normalized;
}

// static
void AgentModelContract::NormalizeScriptActionsList(
    base::Value::List& actions) {
  base::Value::List normalized;
  for (base::Value& entry : actions) {
    base::Value::Dict* dict = entry.GetIfDict();
    if (!dict) {
      continue;
    }
    std::optional<base::Value::Dict> step = NormalizeScriptAction(*dict);
    if (step) {
      normalized.Append(std::move(*step));
    }
  }
  actions = std::move(normalized);
}

// static
void AgentModelContract::SanitizeAgentActionParams(
    std::string_view action_name,
    base::Value::Dict* params) {
  if (!params) {
    return;
  }
  if (base::Value::List* locators = params->FindList("locators")) {
    base::Value::List sanitized = SanitizeLocators(*locators);
    if (sanitized.empty()) {
      params->Remove("locators");
    } else {
      params->Set("locators", std::move(sanitized));
    }
  }
  if (base::EqualsCaseInsensitiveASCII(action_name, "click") ||
      base::EqualsCaseInsensitiveASCII(action_name, "hover")) {
    const base::Value::List* locators = params->FindList("locators");
    const bool has_locators = locators && !locators->empty();
    // Keep screenshot / grid clicks (humanOffset=false). Strip APC pixel
    // hints only when locators are the real target.
    const std::optional<bool> human_offset = params->FindBool("humanOffset");
    const bool screenshot_point =
        human_offset.has_value() && !*human_offset &&
        (params->FindDict("coordinates") || params->FindDict("coordinate") ||
         params->Find("x") || params->Find("y"));
    if (has_locators && !screenshot_point) {
      params->Remove("x");
      params->Remove("y");
      params->Remove("coordinates");
      params->Remove("coordinate");
    }
    if (screenshot_point) {
      params->Remove("locators");
    }
  }
}

// static
base::Value::Dict AgentModelContract::NormalizeAgentCommandForExecution(
    const base::Value::Dict& command) {
  base::Value::Dict coerced = CoerceLooseCommand(command);
  const std::string* command_name = coerced.FindString("command");
  if (!command_name || command_name->empty()) {
    return coerced;
  }

  auto normalize_action_dict = [](base::Value::Dict* action) {
    if (!action) {
      return;
    }
    const std::string* action_name = action->FindString("action");
    std::string name = action_name ? *action_name : std::string();
    // Support Playwright-ish timeline entries inside run_timeline.
    if (name.empty()) {
      if (const std::string* type = action->FindString("type");
          type && !type->empty()) {
        name = *type;
      }
    }
    if (name.empty()) {
      return;
    }
    std::optional<base::Value::Dict> normalized =
        AgentModelContract::NormalizeScriptAction(*action);
    if (!normalized) {
      return;
    }
    *action = std::move(*normalized);
    if (base::Value::Dict* params = action->FindDict("params")) {
      SanitizeAgentActionParams(
          action->FindString("action") ? *action->FindString("action") : "",
          params);
    }
  };

  if (IsMetaCommand(*command_name)) {
    base::Value::Dict normalized = std::move(coerced);
    if (*command_name == "call_macro") {
      normalized.Set("command", "call_action");
      base::Value::Dict* payload = normalized.FindDict("payload");
      if (!payload) {
        normalized.Set("payload", base::Value::Dict());
        payload = normalized.FindDict("payload");
      }
      std::string script_id;
      if (const std::string* existing = payload->FindString("scriptId")) {
        script_id = *existing;
      }
      payload->Set("action", "call_script");
      base::Value::Dict params;
      if (!script_id.empty()) {
        params.Set("scriptId", script_id);
      }
      payload->Set("params", std::move(params));
    }

    const std::string* normalized_command = normalized.FindString("command");
    if (normalized_command && *normalized_command == "call_action") {
      if (base::Value::Dict* payload = normalized.FindDict("payload")) {
        normalize_action_dict(payload);
      }
    } else if (normalized_command && *normalized_command == "run_timeline") {
      base::Value::List* top_level = normalized.FindList("payload");
      base::Value::List* timeline = nullptr;
      if (top_level) {
        timeline = top_level;
      } else if (base::Value::Dict* payload = normalized.FindDict("payload")) {
        timeline = payload->FindList("timeline");
        if (!timeline) {
          timeline = payload->FindList("actions");
        }
      }
      if (timeline) {
        for (base::Value& action_value : *timeline) {
          normalize_action_dict(action_value.GetIfDict());
        }
      }
    }
    return normalized;
  }

  // Bare action verb as command → call_action.
  base::Value::Dict normalized;
  normalized.Set("command", "call_action");
  normalized.Set(
      "payload",
      NormalizeModelActionPayload(*command_name, coerced.FindDict("payload")));
  if (base::Value::Dict* payload = normalized.FindDict("payload")) {
    if (base::Value::Dict* params = payload->FindDict("params")) {
      SanitizeAgentActionParams(
          payload->FindString("action") ? *payload->FindString("action") : "",
          params);
    }
  }
  return normalized;
}

}  // namespace dashboard

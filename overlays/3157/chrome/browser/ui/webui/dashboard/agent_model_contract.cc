// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/agent_model_contract.h"

#include <array>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace dashboard {
namespace {

constexpr std::array<const char*, 19> kCanonicalActions = {
    "navigate", "back", "forward", "click", "hover", "drag", "type",
    "key_down", "key_press", "scrape_element", "scrape_table", "scroll",
    "loop", "if_condition", "authenticate", "wait", "listen", "glic_prompt",
    "call_script",
};

constexpr std::array<const char*, 33> kCanonicalCommands = {
    "call_action",
    "run_timeline",
    "refresh_pagemap",
    "request_more_context",
    "request_more_controls",
    "request_primary_options",
    "request_screenshot",
    "reload_page",
    "inspect_element",
    "save_task_script",
    "agent_finish",
    "store_memento",
    "read_memento",
    "reset_agent",
    "dashboard_handoff_to_assistant",
    "dashboard_investigate_page",
    "dashboard_list_scripts",
    "dashboard_list_collections",
    "dashboard_get_script_json",
    "dashboard_get_script_step",
    "dashboard_update_script_step",
    "dashboard_delete_script_step",
    "dashboard_save_script_json",
    "dashboard_run_script",
    "dashboard_select_script",
    "dashboard_resume_script_from_step",
    "dashboard_set_view",
    "dashboard_set_global_setting",
    "dashboard_configure_listener_match",
    "dashboard_record_agent_action",
    "dashboard_queue_auto_heal",
    "dashboard_process_auto_heal_queue",
    "dashboard_list_timeline_failures",
};

template <size_t N>
bool IsListed(const std::array<const char*, N>& list, std::string_view name) {
  for (const char* entry : list) {
    if (name == entry) {
      return true;
    }
  }
  return false;
}

bool IsCanonicalActionName(std::string_view name) {
  return IsListed(kCanonicalActions, name);
}

bool IsCanonicalCommandName(std::string_view name) {
  return IsListed(kCanonicalCommands, name);
}

bool HasShorthandParam(const base::Value::Dict& params) {
  for (const char* key :
       {"sel", "selector", "css", "cssSelector", "css_selector", "xpath", "el",
        "id"}) {
    const base::Value* value = params.Find(key);
    if (!value || value->is_none()) {
      continue;
    }
    if (value->is_string() && value->GetString().empty()) {
      continue;
    }
    return true;
  }
  return false;
}

bool LocatorsLookCanonical(const base::Value::Dict& params) {
  for (const char* key :
       {"locators", "fromLocators", "toLocators", "rowLocators"}) {
    const base::Value* value = params.Find(key);
    if (!value) {
      continue;
    }
    const base::Value::List* list = value->GetIfList();
    if (!list) {
      return false;
    }
    for (const base::Value& entry : *list) {
      const base::Value::Dict* locator = entry.GetIfDict();
      const std::string* type = locator ? locator->FindString("type") : nullptr;
      const std::string* locator_value =
          locator ? locator->FindString("value") : nullptr;
      if (!type || type->empty() || !locator_value || locator_value->empty()) {
        return false;
      }
    }
  }
  return true;
}

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

bool ListContainsString(const base::Value::List* list, std::string_view key) {
  if (!list) {
    return false;
  }
  for (const base::Value& item : *list) {
    if (const std::string* text = item.GetIfString(); text && *text == key) {
      return true;
    }
  }
  return false;
}

void RememberLowering(base::Value::List* lowered, std::string_view key) {
  if (!ListContainsString(lowered, key)) {
    lowered->Append(key);
  }
}

constexpr const char* kOpenAIStrictStrippedKeys[] = {
    "minimum", "maximum", "minLength", "maxLength",
    "minItems", "maxItems", "pattern", "format",
};

bool IsOpenAIStrictStrippedKey(std::string_view key) {
  for (const char* stripped : kOpenAIStrictStrippedKeys) {
    if (key == stripped) {
      return true;
    }
  }
  return false;
}

base::Value LowerSchemaForOpenAIStrict(const base::Value& node,
                                       base::Value::List* lowered) {
  const base::Value::Dict* in = node.GetIfDict();
  if (!in) {
    return node.Clone();
  }
  base::Value::Dict out;
  for (const auto [key, value] : *in) {
    if (IsOpenAIStrictStrippedKey(key)) {
      RememberLowering(lowered, key);
      continue;
    }
    if (key == "properties" || key == "items" || key == "anyOf" ||
        key == "oneOf" || key == "required" || key == "additionalProperties") {
      continue;
    }
    out.Set(key, value.Clone());
  }
  if (const base::Value::List* any_of = in->FindList("anyOf")) {
    base::Value::List next;
    for (const base::Value& item : *any_of) {
      next.Append(LowerSchemaForOpenAIStrict(item, lowered));
    }
    out.Set("anyOf", std::move(next));
  }
  if (const base::Value::List* one_of = in->FindList("oneOf")) {
    base::Value::List next;
    for (const base::Value& item : *one_of) {
      next.Append(LowerSchemaForOpenAIStrict(item, lowered));
    }
    out.Set("oneOf", std::move(next));
  }
  if (const base::Value* items = in->Find("items")) {
    out.Set("items", LowerSchemaForOpenAIStrict(*items, lowered));
  }
  if (const base::Value::Dict* properties = in->FindDict("properties")) {
    const base::Value::List* required = in->FindList("required");
    base::Value::Dict next_properties;
    base::Value::List next_required;
    bool made_nullable = false;
    for (const auto [key, value] : *properties) {
      base::Value lowered_prop = LowerSchemaForOpenAIStrict(value, lowered);
      if (!ListContainsString(required, key)) {
        base::Value::List union_types;
        union_types.Append(std::move(lowered_prop));
        base::Value::Dict null_type;
        null_type.Set("type", "null");
        union_types.Append(std::move(null_type));
        base::Value::Dict wrapped;
        wrapped.Set("anyOf", std::move(union_types));
        next_properties.Set(key, std::move(wrapped));
        made_nullable = true;
      } else {
        next_properties.Set(key, std::move(lowered_prop));
      }
      next_required.Append(key);
    }
    if (made_nullable) {
      RememberLowering(lowered, "optional_to_nullable");
    }
    out.Set("properties", std::move(next_properties));
    out.Set("required", std::move(next_required));
    out.Set("additionalProperties", false);
  } else if (in->Find("additionalProperties")) {
    out.Set("additionalProperties", false);
  }
  return base::Value(std::move(out));
}

base::Value::Dict WrapResultObject(base::Value schema,
                                   base::Value::List* lowered) {
  if (const base::Value::Dict* existing = schema.GetIfDict()) {
    const base::Value::Dict* properties = existing->FindDict("properties");
    if (properties && properties->Find("result") &&
        ListContainsString(existing->FindList("required"), "result")) {
      return existing->Clone();
    }
  }
  RememberLowering(lowered, "root_result_wrapper");
  base::Value::Dict root;
  root.Set("type", "object");
  root.Set("additionalProperties", false);
  base::Value::List required;
  required.Append("result");
  root.Set("required", std::move(required));
  base::Value::Dict properties;
  properties.Set("result", std::move(schema));
  root.Set("properties", std::move(properties));
  return root;
}

std::string SchemaFingerprint(const base::Value::Dict& schema) {
  std::string json;
  base::JSONWriter::Write(schema, &json);
  uint32_t hash = 5381u;
  for (unsigned char c : json) {
    hash = ((hash << 5) + hash) + c;
  }
  return base::NumberToString(hash);
}

const base::Value::Dict* CanonicalSchemaFromFormat(
    const base::Value::Dict* format) {
  if (!format) {
    return nullptr;
  }
  const base::Value::Dict* json_schema = format->FindDict("json_schema");
  if (!json_schema) {
    return nullptr;
  }
  return json_schema->FindDict("schema");
}

std::string SchemaNameFromFormat(const base::Value::Dict* format) {
  if (!format) {
    return "simulachrome_agent_action";
  }
  const base::Value::Dict* json_schema = format->FindDict("json_schema");
  if (!json_schema) {
    return "simulachrome_agent_action";
  }
  const std::string* name = json_schema->FindString("name");
  return name && !name->empty() ? *name : "simulachrome_agent_action";
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
  return std::string();
}

// static
std::string AgentModelContract::BuildCoreCommandSchemaPromptBlock() {
  return std::string();
}

// static
std::string AgentModelContract::BuildDashboardCommandSchemaPromptBlock() {
  return std::string();
}

// static
std::string AgentModelContract::BuildScriptJsonReferencePromptBlock() {
  return std::string();
}

// static
std::string AgentModelContract::BuildSystemPromptForLoopProfile(
    std::string_view profile) {
  const std::string normalized = NormalizeLoopProfile(profile);

  if (normalized == kLoopProfileGlicTask) {
    return "Browser agent. A user goal is a compressed summary of outcomes, "
           "not a step list and not a particular site. The current page "
           "decides the path. A short goal is often many moves. Issue ONE "
           "action object per turn. A click is {\"action\":\"click\","
           "\"target_id\":1} or {\"action\":\"click\",\"coordinate\":"
           "{\"x\":120,\"y\":80}}. A type is {\"action\":\"type\","
           "\"target_id\":2,\"text\":\"{TokenName}\"}. A navigate is "
           "{\"action\":\"navigate\",\"url\":\"https://example.com/\"}. "
           "A scroll is {\"action\":\"scroll\",\"direction\":\"down\","
           "\"distance\":600}. Also {\"action\":\"key_press\",\"key\":"
           "\"Enter\"}, {\"action\":\"wait\",\"timeout\":2}, "
           "{\"action\":\"request_screenshot\"}, or {\"action\":\"complete\","
           "\"status\":\"solved\",\"summary\":\"done\"}. Each action includes "
           "only its own fields. Never pack a whole clause into one move. "
           "Complete only when the page shows the outcome is done, not when "
           "you have covered each clause of the prompt.\n"
           "Context is a trimmed accessibility list (not full DOM). After a "
           "navigation, look again if the control the next action needs is "
           "missing from the current context. Never type a stored value "
           "unless its field is visible. Keep acting through the page in "
           "front of you without requesting more context unless blocked. "
           "Overlays already present stay valid until the URL changes. If a "
           "later look returns Context status: UNCHANGED, do not re-plan "
           "from scratch.\n"
           "Stored values: type Dashboard variable and secret tokens from the "
           "goal and token list as {TokenName}. Never invent a literal for a "
           "stored value. Finish only when the page shows the outcome, or "
           "when you are blocked.";
  }

  if (normalized == kLoopProfileInvestigator) {
    return "Investigator. Drive the native headless instance (C++ control "
           "channel / :9222). Never open headed tabs. Each turn is one action "
           "object such as {\"action\":\"click\",\"target_id\":1}, "
           "{\"action\":\"type\",\"target_id\":2,\"text\":\"{TokenName}\"}, "
           "{\"action\":\"navigate\",\"url\":\"https://example.com/\"}, "
           "{\"action\":\"scroll\",\"direction\":\"down\",\"distance\":600}, "
           "{\"action\":\"request_screenshot\"}, or {\"action\":\"complete\","
           "\"status\":\"solved\",\"summary\":\"done\"}. Each action includes "
           "only its own fields.\n"
           "Prefer a saved script or KEY script when it covers the next "
           "segment. If the script fully solves the goal, finish. Otherwise "
           "keep going from the page in front of you, one action at a time.\n"
           "Investigate navigates, waits, then returns inputs, buttons, "
           "tables, and per-locator probes. Never trust a blank page when an "
           "http(s) url was requested. Reuse the existing target. Screenshots "
           "stay off unless you need one.\n"
           "When a locator fails, compare that locator's probe row to the "
           "suggested row selector and the input ids.\n"
           "When the diagnosis is ready for a script edit or a user "
           "explanation, hand off. Do not rewrite the script yourself.";
  }

  return "Dashboard assistant. Fix and build scripts with one action object "
         "per turn, such as {\"action\":\"navigate\",\"url\":"
         "\"https://example.com/\"}, {\"action\":\"click\",\"target_id\":1}, "
         "or {\"action\":\"complete\",\"status\":\"solved\",\"summary\":"
         "\"done\"}. Each action includes only its own fields.\n"
         "Prefer investigate, then a small edit, then run. Plain text is "
         "fine when no tool is needed.\n"
         "If the user message includes Findings from an investigator handoff, "
         "apply that edit, explain briefly, verify, then finish. Do not "
         "re-explore unless verify fails. Do not call heal while a heal is "
         "already running.";
}

// static
base::Value::Dict AgentModelContract::BuildAgentTurnResponseFormat() {
  static const base::NoDestructor<base::Value::Dict> format([] {
    constexpr char kSchemaJson[] =
#include "chrome/browser/ui/webui/dashboard/agent_turn_schema.inc"
        ;
    std::optional<base::Value> parsed = base::JSONReader::Read(
        kSchemaJson, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!parsed || !parsed->is_dict()) {
      return base::Value::Dict();
    }
    return std::move(parsed->GetDict());
  }());
  return format->Clone();
}

// static
base::Value::Dict AgentModelContract::BuildScriptReviewResponseFormat() {
  static const base::NoDestructor<base::Value::Dict> format([] {
    constexpr char kSchemaJson[] =
#include "chrome/browser/ui/webui/dashboard/script_review_schema.inc"
        ;
    std::optional<base::Value> parsed = base::JSONReader::Read(
        kSchemaJson, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!parsed || !parsed->is_dict()) {
      return base::Value::Dict();
    }
    return std::move(parsed->GetDict());
  }());
  return format->Clone();
}

// static
base::Value::Dict AgentModelContract::CompileStructuredOutput(
    std::string_view provider,
    std::string_view transport,
    const base::Value::Dict* canonical_format) {
  base::Value::Dict owned_canonical;
  if (!canonical_format || !CanonicalSchemaFromFormat(canonical_format)) {
    owned_canonical = BuildAgentTurnResponseFormat();
    canonical_format = &owned_canonical;
  }
  const base::Value::Dict* canonical_schema =
      CanonicalSchemaFromFormat(canonical_format);
  base::Value::Dict plan;
  base::Value::Dict diagnostics;
  diagnostics.Set("provider", provider);
  diagnostics.Set("transport", transport);
  diagnostics.Set("verified", false);
  diagnostics.Set("schema_hash",
                  canonical_schema ? SchemaFingerprint(*canonical_schema)
                                   : "0");
  base::Value::List lowered;
  const std::string schema_name = SchemaNameFromFormat(canonical_format);

  auto finish = [&](std::string mode, bool strict,
                    std::string fallback_reason) {
    diagnostics.Set("mode", mode);
    diagnostics.Set("strict", strict);
    diagnostics.Set("fallback_reason", fallback_reason);
    diagnostics.Set("lowered", std::move(lowered));
    plan.Set("diagnostics", std::move(diagnostics));
    return std::move(plan);
  };

  if (!canonical_schema || transport == "prompt_only" || transport.empty()) {
    return finish("prompt_only", false, "no_structured_output_transport");
  }

  if (transport == "ollama_format") {
    plan.Set("format", canonical_schema->Clone());
    return finish("schema_supported_non_strict", false,
                  "ollama_format_enforcement_unverified");
  }

  if (transport == "openai_response_format") {
    const bool openai_strict = provider == "openai";
    base::Value schema = openai_strict
                             ? LowerSchemaForOpenAIStrict(
                                   base::Value(canonical_schema->Clone()),
                                   &lowered)
                             : base::Value(canonical_schema->Clone());
    base::Value::Dict wrapped = WrapResultObject(std::move(schema), &lowered);
    base::Value::Dict response_format;
    response_format.Set("type", "json_schema");
    base::Value::Dict json_schema;
    json_schema.Set("name", schema_name);
    json_schema.Set("strict", openai_strict);
    json_schema.Set("schema", std::move(wrapped));
    response_format.Set("json_schema", std::move(json_schema));
    plan.Set("response_format", std::move(response_format));
    if (openai_strict) {
      return finish("strict_schema_requested", true, "");
    }
    return finish("schema_supported_non_strict", false,
                  "strict_subset_unverified");
  }

  if (transport == "anthropic_output_config") {
    base::Value::List ignored;
    base::Value::Dict wrapped =
        WrapResultObject(base::Value(canonical_schema->Clone()), &lowered);
    base::Value::Dict format;
    format.Set("type", "json_schema");
    format.Set("schema", std::move(wrapped));
    base::Value::Dict output_config;
    output_config.Set("format", std::move(format));
    plan.Set("output_config", std::move(output_config));
    return finish("schema_supported_non_strict", false,
                  "anthropic_structured_outputs_unverified");
  }

  if (transport == "gemini_response_json_schema") {
    base::Value::Dict wrapped =
        WrapResultObject(base::Value(canonical_schema->Clone()), &lowered);
    base::Value::Dict generation;
    generation.Set("responseMimeType", "application/json");
    generation.Set("responseJsonSchema", std::move(wrapped));
    plan.Set("generation_config", std::move(generation));
    return finish("schema_supported_non_strict", false,
                  "gemini_response_json_schema_unverified");
  }

  return finish("unsupported", false, "unknown_structured_output_transport");
}

// static
base::Value::Dict AgentModelContract::UnwrapStructuredAction(
    const base::Value::Dict& parsed) {
  if (const std::string* action = parsed.FindString("action");
      action && !action->empty()) {
    return parsed.Clone();
  }
  if (const base::Value::Dict* result = parsed.FindDict("result")) {
    if (const std::string* action = result->FindString("action");
        action && !action->empty()) {
      return result->Clone();
    }
  }
  return parsed.Clone();
}

// static
void AgentModelContract::AdaptResponseFormatForProvider(
    base::Value::Dict& format,
    std::string_view provider) {
  const base::Value::Dict plan = CompileStructuredOutput(
      provider, "openai_response_format", &format);
  if (const base::Value::Dict* compiled = plan.FindDict("response_format")) {
    format = compiled->Clone();
  }
}

// static
base::Value::Dict AgentModelContract::NormalizeModelActionPayload(
    std::string_view action_or_command,
    const base::Value::Dict* payload) {
  base::Value::Dict rejected;
  rejected.Set("action", "");
  rejected.Set("params", base::Value::Dict());
  if (!IsCanonicalActionName(action_or_command) || !payload) {
    return rejected;
  }
  if (HasShorthandParam(*payload) || !LocatorsLookCanonical(*payload)) {
    return rejected;
  }
  base::Value::Dict wrapper;
  wrapper.Set("action", std::string(action_or_command));
  wrapper.Set("params", payload->Clone());
  return wrapper;
}

// static
std::optional<base::Value::Dict> AgentModelContract::NormalizeScriptAction(
    const base::Value::Dict& step) {
  if (step.FindString("type")) {
    return std::nullopt;
  }
  const std::string* action = step.FindString("action");
  const base::Value::Dict* params = step.FindDict("params");
  if (!action || !params) {
    return std::nullopt;
  }
  base::Value::Dict normalized = NormalizeModelActionPayload(*action, params);
  const std::string* out_action = normalized.FindString("action");
  if (!out_action || out_action->empty()) {
    return std::nullopt;
  }
  return normalized;
}

// static
void AgentModelContract::NormalizeScriptActionsList(base::Value::List& actions) {
  base::Value::List kept;
  for (base::Value& entry : actions) {
    base::Value::Dict* dict = entry.GetIfDict();
    if (!dict) {
      continue;
    }
    std::optional<base::Value::Dict> step = NormalizeScriptAction(*dict);
    if (step) {
      kept.Append(std::move(*step));
    }
  }
  actions = std::move(kept);
}

// static
void AgentModelContract::SanitizeAgentActionParams(std::string_view action_name,
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
  (void)action_name;
}

constexpr std::array<const char*, 9> kFlatAgentActions = {
    "click", "type", "navigate", "scroll", "key_press", "wait",
    "request_screenshot", "complete", "save_task_script",
};

template <size_t N>
bool KeysAreAllowed(const base::Value::Dict& turn,
                    const std::array<const char*, N>& allowed) {
  for (const auto [key, unused_value] : turn) {
    (void)unused_value;
    if (!IsListed(allowed, key)) {
      return false;
    }
  }
  return true;
}

bool IsPixel(std::optional<int> value) {
  return value.has_value() && *value >= 0 && *value <= 4000;
}

base::Value::Dict FlatActionToCommand(const base::Value::Dict& turn) {
  base::Value::Dict rejected;
  rejected.Set("command", "");
  const std::string* action = turn.FindString("action");
  if (!action || !IsListed(kFlatAgentActions, *action)) {
    return rejected;
  }
  if (*action == "click" &&
      !KeysAreAllowed(
          turn, std::array<const char*, 3>{"action", "target_id", "coordinate"})) {
    return rejected;
  }
  if (*action == "type" &&
      !KeysAreAllowed(turn,
                      std::array<const char*, 3>{"action", "target_id", "text"})) {
    return rejected;
  }
  if (*action == "navigate" &&
      !KeysAreAllowed(turn, std::array<const char*, 2>{"action", "url"})) {
    return rejected;
  }
  if (*action == "scroll" &&
      !KeysAreAllowed(turn, std::array<const char*, 5>{
                                "action", "direction", "distance", "scope",
                                "target_id"})) {
    return rejected;
  }
  if (*action == "key_press" &&
      !KeysAreAllowed(turn, std::array<const char*, 2>{"action", "key"})) {
    return rejected;
  }
  if (*action == "wait" &&
      !KeysAreAllowed(turn, std::array<const char*, 2>{"action", "timeout"})) {
    return rejected;
  }
  if (*action == "request_screenshot") {
    if (!KeysAreAllowed(turn, std::array<const char*, 1>{"action"})) {
      return rejected;
    }
    base::Value::Dict out;
    out.Set("command", "request_screenshot");
    out.Set("payload", base::Value::Dict());
    return out;
  }
  if (*action == "save_task_script") {
    if (!KeysAreAllowed(
            turn, std::array<const char*, 3>{"action", "indexes", "name"})) {
      return rejected;
    }
    const base::Value::List* indexes = turn.FindList("indexes");
    const std::string* name = turn.FindString("name");
    if (!indexes || indexes->empty() || !name) {
      return rejected;
    }
    for (const base::Value& index : *indexes) {
      if (!index.is_int() || index.GetInt() < 0) {
        return rejected;
      }
    }
    base::Value::Dict payload;
    payload.Set("indexes", indexes->Clone());
    payload.Set("name", *name);
    base::Value::Dict out;
    out.Set("command", "save_task_script");
    out.Set("payload", std::move(payload));
    return out;
  }
  if (*action == "complete") {
    if (!KeysAreAllowed(
            turn, std::array<const char*, 3>{"action", "status", "summary"})) {
      return rejected;
    }
    const std::string* status = turn.FindString("status");
    const std::string* summary = turn.FindString("summary");
    if (!status || (*status != "solved" && *status != "blocked") || !summary) {
      return rejected;
    }
    base::Value::Dict payload;
    payload.Set("status", *status);
    payload.Set("summary", *summary);
    base::Value::Dict out;
    out.Set("command", "agent_finish");
    out.Set("payload", std::move(payload));
    return out;
  }
  if (!IsCanonicalActionName(*action)) {
    return rejected;
  }
  base::Value::Dict params;
  if (turn.Find("target_id")) {
    std::optional<int> target_id = turn.FindInt("target_id");
    if (!target_id || *target_id < 0) {
      return rejected;
    }
    params.Set("target_id", *target_id);
  }
  if (const std::string* text = turn.FindString("text")) {
    params.Set("text", *text);
  }
  if (const std::string* url = turn.FindString("url"); url && !url->empty()) {
    params.Set("url", *url);
  }
  if (turn.Find("coordinate")) {
    const base::Value::Dict* coordinate = turn.FindDict("coordinate");
    if (!coordinate || !IsPixel(coordinate->FindInt("x")) ||
        !IsPixel(coordinate->FindInt("y"))) {
      return rejected;
    }
    base::Value::Dict point;
    point.Set("x", *coordinate->FindInt("x"));
    point.Set("y", *coordinate->FindInt("y"));
    params.Set("coordinates", std::move(point));
    params.Set("humanOffset", false);
  }
  if (turn.Find("distance")) {
    if (!IsPixel(turn.FindInt("distance"))) {
      return rejected;
    }
    params.Set("distance", *turn.FindInt("distance"));
  }
  if (const std::string* direction = turn.FindString("direction")) {
    if (*direction != "up" && *direction != "down") {
      return rejected;
    }
    params.Set("direction", *direction);
  }
  if (const std::string* scope = turn.FindString("scope")) {
    if (*scope != "page" && *scope != "iframe") {
      return rejected;
    }
    params.Set("scope", *scope);
  } else if (turn.Find("scope")) {
    return rejected;
  }
  if (const std::string* key = turn.FindString("key"); key && !key->empty()) {
    params.Set("key", *key);
  }
  if (turn.Find("timeout")) {
    std::optional<int> timeout = turn.FindInt("timeout");
    if (!timeout || *timeout < 0) {
      return rejected;
    }
    params.Set("time", *timeout);
    params.Set("waitType", "time");
  }
  if (*action == "type" &&
      (!params.Find("target_id") || !params.FindString("text"))) {
    return rejected;
  }
  if (*action == "navigate" && !params.FindString("url")) {
    return rejected;
  }
  if (*action == "scroll" &&
      (!params.Find("distance") || !params.FindString("direction"))) {
    return rejected;
  }
  if (*action == "key_press" && !params.FindString("key")) {
    return rejected;
  }
  if (*action == "wait" && !params.Find("time")) {
    return rejected;
  }
  base::Value::Dict step;
  step.Set("action", *action);
  step.Set("params", std::move(params));
  std::optional<base::Value::Dict> normalized =
      AgentModelContract::NormalizeScriptAction(step);
  if (!normalized) {
    return rejected;
  }
  if (base::Value::Dict* out_params = normalized->FindDict("params")) {
    AgentModelContract::SanitizeAgentActionParams(*action, out_params);
  }
  base::Value::Dict out;
  out.Set("command", "call_action");
  out.Set("payload", std::move(*normalized));
  return out;
}

// static
base::Value::Dict AgentModelContract::NormalizeAgentCommandForExecution(
    const base::Value::Dict& command) {
  base::Value::Dict rejected;
  rejected.Set("command", "");
  if (command.Find("do")) {
    return rejected;
  }
  const std::string* flat_action = command.FindString("action");
  const std::string* command_name = command.FindString("command");
  if (flat_action && !flat_action->empty() && !command_name &&
      !command.Find("payload")) {
    return FlatActionToCommand(command);
  }
  if (!command_name || !IsCanonicalCommandName(*command_name)) {
    return rejected;
  }
  base::Value::Dict out = command.Clone();
  out.Set("command", *command_name);

  auto accept_action = [](base::Value::Dict* action) -> bool {
    if (!action) {
      return false;
    }
    std::optional<base::Value::Dict> normalized = NormalizeScriptAction(*action);
    if (!normalized) {
      return false;
    }
    if (base::Value::Dict* params = normalized->FindDict("params")) {
      const std::string* name = normalized->FindString("action");
      SanitizeAgentActionParams(name ? *name : "", params);
    }
    *action = std::move(*normalized);
    return true;
  };

  if (*command_name == "call_action") {
    base::Value::Dict* payload = out.FindDict("payload");
    if (!accept_action(payload)) {
      return rejected;
    }
  } else if (*command_name == "run_timeline") {
    base::Value::Dict* payload = out.FindDict("payload");
    base::Value::List* timeline = payload ? payload->FindList("timeline") : nullptr;
    if (!timeline) {
      return rejected;
    }
    for (base::Value& action_value : *timeline) {
      if (!accept_action(action_value.GetIfDict())) {
        return rejected;
      }
    }
  }
  return out;
}

}  // namespace dashboard

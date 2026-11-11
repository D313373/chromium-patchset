// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_AGENT_MODEL_CONTRACT_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_AGENT_MODEL_CONTRACT_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"

namespace dashboard {

// Canonical model <-> Simulachrome agent contract.
//
// Guided decoding (action_output_schema.js, sent as response_format) produces
// one closed action object from an anyOf union. These methods map that object
// onto the executor and reject alias, shorthand, and Playwright forms. They
// do not rewrite aliases. An internal choice-mode envelope without a
// top-level "action" is still accepted.
//
// Keep the WebUI twin in sync:
//   chrome/browser/resources/dashboard/agent/model_prompt_contract.js
//   chrome/browser/resources/dashboard/agent/action_output_schema.js
class AgentModelContract {
 public:
  static constexpr char kLoopProfileDashboardAssistant[] =
      "dashboard_assistant";
  static constexpr char kLoopProfileGlicTask[] = "glic_task";
  static constexpr char kLoopProfileInvestigator[] = "investigator";

  // Normalize a loop-profile id to one of the known constants.
  static std::string NormalizeLoopProfile(std::string_view profile);

  // Shared prompt fragments (action shape + command lists).
  static std::string BuildActionShapePromptBlock();
  static std::string BuildCoreCommandSchemaPromptBlock();
  static std::string BuildDashboardCommandSchemaPromptBlock();

  // Full system prompt for a given loop profile.
  static std::string BuildSystemPromptForLoopProfile(std::string_view profile);

  // Compact script JSON reference used when the model must emit a script.
  static std::string BuildScriptJsonReferencePromptBlock();

  // OpenAI / xAI Grok chat-completions response_format object.
  // The schema inside is the canonical action union (root anyOf).
  static base::Value::Dict BuildAgentTurnResponseFormat();

  // Post-task script review: save_task_script with indexes, or complete.
  static base::Value::Dict BuildScriptReviewResponseFormat();

  // Compile the canonical schema for one provider transport.
  // `transport` is openai_response_format, ollama_format,
  // anthropic_output_config, gemini_response_json_schema, or prompt_only.
  // The result's diagnostics.mode is never strict_schema_verified; that label
  // is reserved for a live test that proved enforcement.
  static base::Value::Dict CompileStructuredOutput(
      std::string_view provider,
      std::string_view transport,
      const base::Value::Dict* canonical_format);

  // Drop a provider wrapper {"result": {action...}} before canonical
  // validation. A bare action object is unchanged.
  static base::Value::Dict UnwrapStructuredAction(
      const base::Value::Dict& parsed);

  // OpenAI-compatible response_format mutation. Prefer
  // CompileStructuredOutput at request-construction sites.
  static void AdaptResponseFormatForProvider(base::Value::Dict& format,
                                             std::string_view provider);

  // Accept a canonical {"action","params"}. Reject aliases and shorthand.
  static base::Value::Dict NormalizeModelActionPayload(
      std::string_view action_or_command,
      const base::Value::Dict* payload);

  // Accept a canonical script step. Returns nullopt for alias or shorthand.
  static std::optional<base::Value::Dict> NormalizeScriptAction(
      const base::Value::Dict& step);

  // Normalize every entry in an actions list in place. Drops unsavable steps.
  static void NormalizeScriptActionsList(base::Value::List& actions);

  // Accept a canonical command envelope. Rejects {"do": ...} and aliases.
  static base::Value::Dict NormalizeAgentCommandForExecution(
      const base::Value::Dict& command);

  // Light param sanitize used after normalize (safe locator types, drop
  // coordinate shorthand for click/hover).
  static void SanitizeAgentActionParams(std::string_view action_name,
                                        base::Value::Dict* params);
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_AGENT_MODEL_CONTRACT_H_

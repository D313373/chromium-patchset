// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/agent_model_contract.h"

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace dashboard {
namespace {

TEST(AgentModelContractTest, RejectsPlaywrightNavigateStep) {
  base::Value::Dict step;
  step.Set("type", "goto");
  step.Set("url", "https://example.com/table");

  EXPECT_FALSE(AgentModelContract::NormalizeScriptAction(step));
}

TEST(AgentModelContractTest, RejectsSelectorShorthand) {
  base::Value::Dict params;
  params.Set("sel", "#courses_table");
  auto wrapper =
      AgentModelContract::NormalizeModelActionPayload("click", &params);
  EXPECT_EQ("", *wrapper.FindString("action"));
}

TEST(AgentModelContractTest, AcceptsFlatClick) {
  base::Value::Dict coordinate;
  coordinate.Set("x", 120);
  coordinate.Set("y", 80);
  base::Value::Dict turn;
  turn.Set("action", "click");
  turn.Set("coordinate", std::move(coordinate));

  auto normalized =
      AgentModelContract::NormalizeAgentCommandForExecution(turn);
  EXPECT_EQ("call_action", *normalized.FindString("command"));
  const base::Value::Dict* action = normalized.FindDict("payload");
  ASSERT_TRUE(action);
  EXPECT_EQ("click", *action->FindString("action"));
  const base::Value::Dict* params = action->FindDict("params");
  ASSERT_TRUE(params);
  const base::Value::Dict* point = params->FindDict("coordinates");
  ASSERT_TRUE(point);
  EXPECT_EQ(120, point->FindInt("x"));
  EXPECT_EQ(80, point->FindInt("y"));
}

TEST(AgentModelContractTest, AcceptsScriptReviewSave) {
  base::Value::List indexes;
  indexes.Append(0);
  indexes.Append(2);
  base::Value::Dict turn;
  turn.Set("action", "save_task_script");
  turn.Set("indexes", std::move(indexes));
  turn.Set("name", "Daily bonus");
  auto normalized =
      AgentModelContract::NormalizeAgentCommandForExecution(turn);
  EXPECT_EQ("save_task_script", *normalized.FindString("command"));
  const base::Value::Dict* payload = normalized.FindDict("payload");
  ASSERT_TRUE(payload);
  EXPECT_EQ(2u, payload->FindList("indexes")->size());

  const base::Value::Dict review =
      AgentModelContract::BuildScriptReviewResponseFormat();
  const base::Value::Dict* schema =
      review.FindDict("json_schema")->FindDict("schema");
  ASSERT_TRUE(schema);
  EXPECT_EQ(2u, schema->FindList("anyOf")->size());
}

TEST(AgentModelContractTest, OpenAIStrictWrapperDoesNotClaimVerified) {
  const base::Value::Dict canonical =
      AgentModelContract::BuildAgentTurnResponseFormat();
  const base::Value::Dict plan = AgentModelContract::CompileStructuredOutput(
      "openai", "openai_response_format", &canonical);
  const base::Value::Dict* diagnostics = plan.FindDict("diagnostics");
  ASSERT_TRUE(diagnostics);
  EXPECT_EQ("strict_schema_requested", *diagnostics->FindString("mode"));
  EXPECT_FALSE(diagnostics->FindBool("verified").value_or(true));
  EXPECT_TRUE(diagnostics->FindBool("strict").value_or(false));
  EXPECT_NE("strict_schema_verified", *diagnostics->FindString("mode"));

  const base::Value::Dict* format = plan.FindDict("response_format");
  ASSERT_TRUE(format);
  const base::Value::Dict* json_schema = format->FindDict("json_schema");
  ASSERT_TRUE(json_schema);
  EXPECT_TRUE(json_schema->FindBool("strict").value_or(false));
  const base::Value::Dict* schema = json_schema->FindDict("schema");
  ASSERT_TRUE(schema);
  EXPECT_EQ("object", *schema->FindString("type"));
  EXPECT_FALSE(schema->FindBool("additionalProperties").value_or(true));
  const base::Value::List* required = schema->FindList("required");
  ASSERT_TRUE(required);
  ASSERT_EQ(1u, required->size());
  EXPECT_EQ("result", *(*required)[0].GetIfString());
  const base::Value::Dict* result =
      schema->FindDict("properties")->FindDict("result");
  ASSERT_TRUE(result);
  ASSERT_EQ(8u, result->FindList("anyOf")->size());

  bool saw_nullable_target = false;
  for (const base::Value& branch_value : *result->FindList("anyOf")) {
    const base::Value::Dict* branch = branch_value.GetIfDict();
    ASSERT_TRUE(branch);
    const base::Value::Dict* properties = branch->FindDict("properties");
    ASSERT_TRUE(properties);
    const base::Value::List* action =
        properties->FindDict("action")->FindList("enum");
    ASSERT_TRUE(action);
    if (*(*action)[0].GetIfString() == "click") {
      EXPECT_FALSE(properties->Find("text"));
      EXPECT_FALSE(properties->Find("url"));
      const base::Value::Dict* target = properties->FindDict("target_id");
      ASSERT_TRUE(target);
      EXPECT_TRUE(target->FindList("anyOf"));
      const base::Value::Dict* coordinate = properties->FindDict("coordinate");
      ASSERT_TRUE(coordinate);
      const base::Value::List* coordinate_union = coordinate->FindList("anyOf");
      ASSERT_TRUE(coordinate_union);
      const base::Value::Dict* coordinate_object =
          (*coordinate_union)[0].GetIfDict();
      ASSERT_TRUE(coordinate_object);
      const base::Value::Dict* x = coordinate_object->FindDict("properties")
                                       ->FindDict("x");
      ASSERT_TRUE(x);
      EXPECT_EQ("integer", *x->FindString("type"));
      EXPECT_FALSE(x->Find("minimum"));
      EXPECT_FALSE(x->Find("maximum"));
      saw_nullable_target = true;
    }
  }
  EXPECT_TRUE(saw_nullable_target);

  base::Value::Dict grok_plan = AgentModelContract::CompileStructuredOutput(
      "grok", "openai_response_format", &canonical);
  EXPECT_EQ("schema_supported_non_strict",
            *grok_plan.FindDict("diagnostics")->FindString("mode"));
  EXPECT_FALSE(grok_plan.FindDict("response_format")
                   ->FindDict("json_schema")
                   ->FindBool("strict")
                   .value_or(true));
}

TEST(AgentModelContractTest, ProviderTransportsStayDistinct) {
  const base::Value::Dict canonical =
      AgentModelContract::BuildAgentTurnResponseFormat();
  const base::Value::Dict ollama = AgentModelContract::CompileStructuredOutput(
      "ollama", "ollama_format", &canonical);
  const base::Value::Dict* ollama_schema = ollama.FindDict("format");
  ASSERT_TRUE(ollama_schema);
  EXPECT_TRUE(ollama_schema->FindList("anyOf"));
  EXPECT_FALSE(ollama_schema->Find("properties"));
  EXPECT_FALSE(ollama.Find("response_format"));

  const base::Value::Dict anthropic =
      AgentModelContract::CompileStructuredOutput(
          "anthropic", "anthropic_output_config", &canonical);
  const base::Value::Dict* output_config = anthropic.FindDict("output_config");
  ASSERT_TRUE(output_config);
  EXPECT_EQ("json_schema",
            *output_config->FindDict("format")->FindString("type"));
  EXPECT_EQ("object", *output_config->FindDict("format")
                           ->FindDict("schema")
                           ->FindString("type"));
  EXPECT_FALSE(anthropic.Find("response_format"));
  EXPECT_EQ("schema_supported_non_strict",
            *anthropic.FindDict("diagnostics")->FindString("mode"));

  const base::Value::Dict gemini = AgentModelContract::CompileStructuredOutput(
      "gemini", "gemini_response_json_schema", &canonical);
  const base::Value::Dict* generation = gemini.FindDict("generation_config");
  ASSERT_TRUE(generation);
  EXPECT_EQ("application/json", *generation->FindString("responseMimeType"));
  EXPECT_EQ("object",
            *generation->FindDict("responseJsonSchema")->FindString("type"));
  EXPECT_FALSE(gemini.Find("response_format"));
}

TEST(AgentModelContractTest, UnwrapsResultWrapperOnly) {
  base::Value::Dict wrapped;
  base::Value::Dict action;
  action.Set("action", "navigate");
  action.Set("url", "https://example.com/");
  wrapped.Set("result", std::move(action));
  base::Value::Dict unwrapped =
      AgentModelContract::UnwrapStructuredAction(wrapped);
  EXPECT_EQ("navigate", *unwrapped.FindString("action"));
  EXPECT_EQ("https://example.com/", *unwrapped.FindString("url"));

  base::Value::Dict bare;
  bare.Set("action", "wait");
  bare.Set("timeout", 1);
  base::Value::Dict kept = AgentModelContract::UnwrapStructuredAction(bare);
  EXPECT_EQ("wait", *kept.FindString("action"));
  EXPECT_EQ(1, kept.FindInt("timeout").value_or(-1));
}

TEST(AgentModelContractTest, RejectsClickCarryingTypeFields) {
  base::Value::Dict turn;
  turn.Set("action", "click");
  turn.Set("target_id", 1);
  turn.Set("text", "hello");
  turn.Set("url", "https://example.com/");
  EXPECT_EQ("", *AgentModelContract::NormalizeAgentCommandForExecution(turn)
                     .FindString("command"));
}

TEST(AgentModelContractTest, RejectsNestedCommandEnvelopeFromTheModel) {
  base::Value::Dict command;
  command.Set("command", "call_action");
  command.Set("action", "click");
  base::Value::Dict payload;
  payload.Set("action", "click");
  command.Set("payload", std::move(payload));
  EXPECT_EQ("", *AgentModelContract::NormalizeAgentCommandForExecution(command)
                     .FindString("command"));
}

TEST(AgentModelContractTest, RejectsLooseDoDialect) {
  base::Value::Dict click;
  click.Set("do", "click");
  click.Set("id", 12);
  EXPECT_EQ("", *AgentModelContract::NormalizeAgentCommandForExecution(click)
                     .FindString("command"));

  base::Value::Dict done;
  done.Set("do", "done");
  done.Set("why", "form submitted");
  EXPECT_EQ("", *AgentModelContract::NormalizeAgentCommandForExecution(done)
                     .FindString("command"));

  base::Value::Dict look;
  look.Set("do", "look");
  EXPECT_EQ("", *AgentModelContract::NormalizeAgentCommandForExecution(look)
                     .FindString("command"));
}

TEST(AgentModelContractTest, AcceptsCanonicalFinish) {
  base::Value::Dict command;
  command.Set("command", "agent_finish");
  base::Value::Dict payload;
  payload.Set("status", "solved");
  payload.Set("summary", "form submitted");
  command.Set("payload", std::move(payload));

  auto normalized =
      AgentModelContract::NormalizeAgentCommandForExecution(command);
  EXPECT_EQ("agent_finish", *normalized.FindString("command"));
  const base::Value::Dict* out = normalized.FindDict("payload");
  ASSERT_TRUE(out);
  EXPECT_EQ("form submitted", *out->FindString("summary"));
}

TEST(AgentModelContractTest, SystemPromptHasNoFormatInstructions) {
  const std::string glic = AgentModelContract::BuildSystemPromptForLoopProfile(
      AgentModelContract::kLoopProfileGlicTask);
  EXPECT_EQ(std::string::npos, glic.find("{\"do\""));
  EXPECT_EQ(std::string::npos, glic.find("params.locators"));
  EXPECT_NE(std::string::npos, glic.find("{\"action\":\"click\"}"));
  EXPECT_EQ(std::string::npos, glic.find("\"command\""));

  const std::string dash = AgentModelContract::BuildSystemPromptForLoopProfile(
      AgentModelContract::kLoopProfileDashboardAssistant);
  EXPECT_EQ(std::string::npos, dash.find("{\"do\""));
  EXPECT_NE(std::string::npos, dash.find("Findings"));

  const std::string inv = AgentModelContract::BuildSystemPromptForLoopProfile(
      AgentModelContract::kLoopProfileInvestigator);
  EXPECT_EQ(std::string::npos, inv.find("{\"do\""));
  EXPECT_NE(std::string::npos, inv.find("hand off"));
  EXPECT_NE(std::string::npos, inv.find("headless"));

  const base::Value::Dict format =
      AgentModelContract::BuildAgentTurnResponseFormat();
  EXPECT_EQ("json_schema", *format.FindString("type"));
  const base::Value::Dict* json_schema = format.FindDict("json_schema");
  ASSERT_TRUE(json_schema);
  EXPECT_TRUE(json_schema->FindBool("strict").value_or(false));
  const base::Value::Dict* schema = json_schema->FindDict("schema");
  ASSERT_TRUE(schema);
  const base::Value::List* branches = schema->FindList("anyOf");
  ASSERT_TRUE(branches);
  EXPECT_EQ(8u, branches->size());
  EXPECT_FALSE(schema->Find("properties"));
  bool saw_click = false;
  for (const base::Value& branch_value : *branches) {
    const base::Value::Dict* branch = branch_value.GetIfDict();
    ASSERT_TRUE(branch);
    EXPECT_EQ("object", *branch->FindString("type"));
    EXPECT_FALSE(branch->FindBool("additionalProperties").value_or(true));
    const base::Value::Dict* properties = branch->FindDict("properties");
    ASSERT_TRUE(properties);
    EXPECT_FALSE(properties->Find("command"));
    EXPECT_FALSE(properties->Find("payload"));
    const base::Value::Dict* action = properties->FindDict("action");
    ASSERT_TRUE(action);
    const base::Value::List* names = action->FindList("enum");
    ASSERT_TRUE(names);
    ASSERT_EQ(1u, names->size());
    const std::string* name = (*names)[0].GetIfString();
    ASSERT_TRUE(name);
    if (*name == "click") {
      saw_click = true;
      EXPECT_FALSE(properties->Find("text"));
      EXPECT_FALSE(properties->Find("url"));
      EXPECT_FALSE(properties->Find("distance"));
      const base::Value::Dict* coordinate = properties->FindDict("coordinate");
      ASSERT_TRUE(coordinate);
      const base::Value::Dict* coordinate_props =
          coordinate->FindDict("properties");
      ASSERT_TRUE(coordinate_props);
      const base::Value::Dict* x = coordinate_props->FindDict("x");
      ASSERT_TRUE(x);
      EXPECT_EQ("integer", *x->FindString("type"));
      EXPECT_EQ(0, x->FindInt("minimum"));
      EXPECT_EQ(4000, x->FindInt("maximum"));
    }
    if (*name == "type") {
      EXPECT_FALSE(properties->Find("coordinate"));
      EXPECT_FALSE(properties->Find("distance"));
    }
  }
  EXPECT_TRUE(saw_click);
}

}  // namespace
}  // namespace dashboard

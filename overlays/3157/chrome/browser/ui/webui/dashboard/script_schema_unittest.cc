// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/script_schema.h"

#include <set>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/simulachrome/action_vocabulary_generated.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace dashboard {

class ScriptSchemaTest : public testing::Test {
 protected:
  void SetUp() override {}
};

// Drift guard: the script schema and the shared action vocabulary
// (simulachrome/action_vocabulary/action_vocabulary.json) must name exactly the
// same actions, and every alias must land on one of them.
TEST_F(ScriptSchemaTest, ActionNamesMatchSharedVocabulary) {
  std::set<std::string> schema_actions;
  for (const base::Value& entry : ScriptSchema::GetSupportedActions()) {
    schema_actions.insert(*entry.GetDict().FindString("action"));
  }
  std::set<std::string> vocab_actions;
  for (std::string_view name : simulachrome::vocabulary::kCanonicalActions) {
    vocab_actions.emplace(name);
  }
  EXPECT_EQ(schema_actions, vocab_actions)
      << "Update action_vocabulary.json (and regenerate the JS twin) when "
         "adding or removing a script action.";
  for (const auto& entry : simulachrome::vocabulary::kActionAliases) {
    EXPECT_TRUE(vocab_actions.contains(std::string(entry.canonical)))
        << entry.alias << " -> " << entry.canonical;
  }
}

// Test GetActionTemplate returns valid structure
TEST_F(ScriptSchemaTest, GetActionTemplate_Click) {
  auto tmpl = ScriptSchema::GetActionTemplate("click");
  
  EXPECT_EQ("click", *tmpl.FindString("action"));
  EXPECT_TRUE(tmpl.FindDict("params"));
  
  const auto* params = tmpl.FindDict("params");
  EXPECT_TRUE(params->FindList("locators"));
  EXPECT_EQ(1, params->FindInt("click_count").value_or(0));
  const std::string* button = params->FindString("button");
  EXPECT_EQ("left", button ? *button : "");
}

TEST_F(ScriptSchemaTest, GetActionTemplate_Navigate) {
  auto tmpl = ScriptSchema::GetActionTemplate("navigate");
  
  EXPECT_EQ("navigate", *tmpl.FindString("action"));
  const auto* params = tmpl.FindDict("params");
  EXPECT_TRUE(params->FindString("url"));
}

// Test CanonicalizeAction normalizes action names
TEST_F(ScriptSchemaTest, CanonicalizeAction_LowercasesName) {
  base::Value::Dict action;
  action.Set("action", "CLICK");
  action.Set("params", base::Value::Dict());
  
  ScriptSchema::CanonicalizeAction(action);
  
  EXPECT_EQ("click", *action.FindString("action"));
}

// Test CanonicalizeAction handles legacy "type" field
TEST_F(ScriptSchemaTest, CanonicalizeAction_HandlesTypeField) {
  base::Value::Dict action;
  action.Set("type", "Navigate");
  action.Set("params", base::Value::Dict());
  
  ScriptSchema::CanonicalizeAction(action);
  
  EXPECT_EQ("navigate", *action.FindString("action"));
}

// Test CanonicalizeAction applies defaults
TEST_F(ScriptSchemaTest, CanonicalizeAction_AppliesDefaults) {
  base::Value::Dict action;
  action.Set("action", "click");
  base::Value::Dict params;
  params.Set("locators", base::Value::List());
  action.Set("params", std::move(params));
  
  ScriptSchema::CanonicalizeAction(action);
  
  const auto* result_params = action.FindDict("params");
  EXPECT_EQ(1, result_params->FindInt("click_count").value_or(0));
  EXPECT_EQ("left", *result_params->FindString("button"));
}

// Test CanonicalizeAction clamps click_count
TEST_F(ScriptSchemaTest, CanonicalizeAction_ClampsClickCount) {
  base::Value::Dict action;
  action.Set("action", "click");
  base::Value::Dict params;
  params.Set("click_count", 5);  // Too high
  action.Set("params", std::move(params));
  
  ScriptSchema::CanonicalizeAction(action);
  
  const auto* result_params = action.FindDict("params");
  EXPECT_EQ(3, result_params->FindInt("click_count").value_or(0));
  
  // Check comment was added
  const std::string* comment = action.FindString("comment");
  EXPECT_TRUE(comment && comment->find("clamped") != std::string::npos);
}

// Test CanonicalizeAction tolerates unknown params (forward-compat)
TEST_F(ScriptSchemaTest, CanonicalizeAction_ToleratesUnknownParams) {
  base::Value::Dict action;
  action.Set("action", "click");
  base::Value::Dict params;
  params.Set("locators", base::Value::List());
  params.Set("unknown_param", "some_value");
  action.Set("params", std::move(params));

  ScriptSchema::CanonicalizeAction(action);

  const auto* result_params = action.FindDict("params");
  EXPECT_TRUE(result_params->Find("unknown_param"));
}

TEST_F(ScriptSchemaTest, CanonicalizeAction_PromotesLoopStepsToTopLevel) {
  base::Value::Dict action;
  action.Set("action", "loop");

  base::Value::Dict params;
  params.Set("loopMode", "count");
  params.Set("iterations", 1);
  params.Set("maxIterations", 1);
  base::Value::List steps;
  {
    base::Value::Dict child;
    child.Set("action", "wait");
    base::Value::Dict child_params;
    child_params.Set("waitType", "time");
    child_params.Set("time", 150);
    child.Set("params", std::move(child_params));
    steps.Append(std::move(child));
  }
  params.Set("steps", std::move(steps));
  action.Set("params", std::move(params));

  ScriptSchema::CanonicalizeAction(action);

  const base::Value::List* top_level_steps = action.FindList("steps");
  ASSERT_TRUE(top_level_steps);
  ASSERT_EQ(1u, top_level_steps->size());
  const base::Value::Dict* result_params = action.FindDict("params");
  ASSERT_TRUE(result_params);
  EXPECT_FALSE(result_params->Find("steps"));
}

// Test ValidateAction accepts valid actions
TEST_F(ScriptSchemaTest, ValidateAction_AcceptsValid) {
  base::Value::Dict action;
  action.Set("action", "click");
  base::Value::Dict params;
  base::Value::List locators;
  base::Value::Dict l;
  l.Set("type", "css");
  l.Set("value", "button#submit");
  locators.Append(std::move(l));
  params.Set("locators", std::move(locators));
  action.Set("params", std::move(params));
  
  std::string error;
  bool valid = ScriptSchema::ValidateAction(action, &error);
  
  EXPECT_TRUE(valid);
  EXPECT_TRUE(error.empty());
}

TEST_F(ScriptSchemaTest, ValidateAction_HoverAcceptsCoordinates) {
  base::Value::Dict action;
  action.Set("action", "hover");
  base::Value::Dict params;
  base::Value::Dict coords;
  coords.Set("x", 100);
  coords.Set("y", 200);
  params.Set("coordinates", std::move(coords));
  action.Set("params", std::move(params));

  std::string error;
  bool valid = ScriptSchema::ValidateAction(action, &error);

  EXPECT_TRUE(valid);
  EXPECT_TRUE(error.empty());
}

TEST_F(ScriptSchemaTest, ValidateAction_DragAcceptsLocatorPairs) {
  base::Value::Dict action;
  action.Set("action", "drag");
  base::Value::Dict params;
  base::Value::List from_locs;
  base::Value::Dict from;
  from.Set("type", "css_selector");
  from.Set("value", ".drag-source");
  from_locs.Append(std::move(from));
  base::Value::List to_locs;
  base::Value::Dict to;
  to.Set("type", "css_selector");
  to.Set("value", ".drop-target");
  to_locs.Append(std::move(to));
  params.Set("fromLocators", std::move(from_locs));
  params.Set("toLocators", std::move(to_locs));
  action.Set("params", std::move(params));

  std::string error;
  const bool valid = ScriptSchema::ValidateAction(action, &error);
  EXPECT_TRUE(valid);
  EXPECT_TRUE(error.empty());
}

TEST_F(ScriptSchemaTest, ValidateAction_DragRejectsMissingEndPoint) {
  base::Value::Dict action;
  action.Set("action", "drag");
  base::Value::Dict params;
  base::Value::Dict from_coords;
  from_coords.Set("x", 120);
  from_coords.Set("y", 180);
  params.Set("fromCoordinates", std::move(from_coords));
  action.Set("params", std::move(params));

  std::string error;
  const bool valid = ScriptSchema::ValidateAction(action, &error);
  EXPECT_FALSE(valid);
  EXPECT_TRUE(error.find("toLocators") != std::string::npos ||
              error.find("toCoordinates") != std::string::npos);
}

// Test ValidateAction rejects missing action field
TEST_F(ScriptSchemaTest, ValidateAction_RejectsMissingAction) {
  base::Value::Dict action;
  action.Set("params", base::Value::Dict());
  
  std::string error;
  bool valid = ScriptSchema::ValidateAction(action, &error);
  
  EXPECT_FALSE(valid);
  EXPECT_FALSE(error.empty());
}

// Test ValidateAction rejects missing required params
TEST_F(ScriptSchemaTest, ValidateAction_RejectsMissingRequiredParams) {
  base::Value::Dict action;
  action.Set("action", "type");
  base::Value::Dict params;
  // Missing required "text" param
  action.Set("params", std::move(params));
  
  std::string error;
  bool valid = ScriptSchema::ValidateAction(action, &error);
  
  EXPECT_FALSE(valid);
  EXPECT_TRUE(error.find("text") != std::string::npos);
}

TEST_F(ScriptSchemaTest, ValidateAction_ScrollElementRequiresLocators) {
  base::Value::Dict action;
  action.Set("action", "scroll");
  base::Value::Dict params;
  params.Set("element", "element");
  action.Set("params", std::move(params));

  std::string error;
  bool valid = ScriptSchema::ValidateAction(action, &error);

  EXPECT_FALSE(valid);
  EXPECT_TRUE(error.find("locators") != std::string::npos);
}

TEST_F(ScriptSchemaTest, ValidateAction_ScrollDistanceAccepted) {
  base::Value::Dict action;
  action.Set("action", "scroll");
  base::Value::Dict params;
  params.Set("distance", 600);
  action.Set("params", std::move(params));

  std::string error;
  bool valid = ScriptSchema::ValidateAction(action, &error);

  EXPECT_TRUE(valid);
  EXPECT_TRUE(error.empty());
}

TEST_F(ScriptSchemaTest, ValidateAction_LoopCountAccepted) {
  base::Value::Dict action;
  action.Set("action", "loop");
  base::Value::Dict params;
  params.Set("loopMode", "count");
  params.Set("iterations", 3);
  action.Set("params", std::move(params));
  base::Value::List steps;
  base::Value::Dict step;
  step.Set("action", "wait");
  base::Value::Dict step_params;
  step_params.Set("waitType", "time");
  step_params.Set("time", 50);
  step.Set("params", std::move(step_params));
  steps.Append(std::move(step));
  action.Set("steps", std::move(steps));

  std::string error;
  bool valid = ScriptSchema::ValidateAction(action, &error);

  EXPECT_TRUE(valid);
  EXPECT_TRUE(error.empty());
}

TEST_F(ScriptSchemaTest, ValidateAction_LoopRejectsMissingSteps) {
  base::Value::Dict action;
  action.Set("action", "loop");
  base::Value::Dict params;
  params.Set("loopMode", "count");
  params.Set("iterations", 2);
  action.Set("params", std::move(params));

  std::string error;
  bool valid = ScriptSchema::ValidateAction(action, &error);

  EXPECT_FALSE(valid);
  EXPECT_TRUE(error.find("nested step") != std::string::npos);
}

TEST_F(ScriptSchemaTest, ValidateAction_LoopForEachRequiresLocators) {
  base::Value::Dict action;
  action.Set("action", "loop");
  base::Value::Dict params;
  params.Set("loopMode", "for_each");
  params.Set("maxIterations", 10);
  action.Set("params", std::move(params));
  base::Value::List steps;
  base::Value::Dict step;
  step.Set("action", "click");
  base::Value::Dict step_params;
  base::Value::List click_locs;
  base::Value::Dict loc;
  loc.Set("type", "css_selector");
  loc.Set("value", "button");
  click_locs.Append(std::move(loc));
  step_params.Set("locators", std::move(click_locs));
  step.Set("params", std::move(step_params));
  steps.Append(std::move(step));
  action.Set("steps", std::move(steps));

  std::string error;
  const bool valid = ScriptSchema::ValidateAction(action, &error);
  EXPECT_FALSE(valid);
  EXPECT_TRUE(error.find("locator") != std::string::npos);
}

TEST_F(ScriptSchemaTest, ValidateAction_LoopForEachRejectsBadStrategy) {
  base::Value::Dict action;
  action.Set("action", "loop");
  base::Value::Dict params;
  params.Set("loopMode", "for_each");
  params.Set("forEachStrategy", "bad_mode");
  base::Value::List locs;
  base::Value::Dict loc;
  loc.Set("type", "css_selector");
  loc.Set("value", ".item");
  locs.Append(std::move(loc));
  params.Set("locators", std::move(locs));
  action.Set("params", std::move(params));
  base::Value::List steps;
  base::Value::Dict step;
  step.Set("action", "wait");
  base::Value::Dict step_params;
  step_params.Set("waitType", "time");
  step_params.Set("time", 10);
  step.Set("params", std::move(step_params));
  steps.Append(std::move(step));
  action.Set("steps", std::move(steps));

  std::string error;
  const bool valid = ScriptSchema::ValidateAction(action, &error);
  EXPECT_FALSE(valid);
  EXPECT_TRUE(error.find("forEachStrategy") != std::string::npos);
}

// Test GetSupportedActions returns all action types
TEST_F(ScriptSchemaTest, GetSupportedActions_ReturnsAll) {
  auto actions = ScriptSchema::GetSupportedActions();
  
  EXPECT_GT(actions.size(), 0u);
  
  // Check that click action is present
  bool found_click = false;
  for (const auto& action_value : actions) {
    if (const auto* action_dict = action_value.GetIfDict()) {
      if (const std::string* name = action_dict->FindString("action")) {
        if (*name == "click") {
          found_click = true;
          EXPECT_TRUE(action_dict->FindString("display_name"));
          EXPECT_TRUE(action_dict->FindString("description"));
          EXPECT_TRUE(action_dict->FindList("params"));
          break;
        }
      }
    }
  }
  EXPECT_TRUE(found_click);
}

// Test CanonicalizeForSave creates complete script
TEST_F(ScriptSchemaTest, CanonicalizeForSave_CreatesCompleteScript) {
  base::Value::Dict script;
  script.Set("name", "Test Script");
  base::Value::List actions;
  base::Value::Dict action;
  action.Set("action", "wait");
  action.Set("params", base::Value::Dict());
  actions.Append(std::move(action));
  script.Set("actions", std::move(actions));
  
  auto result = ScriptSchema::CanonicalizeForSave(
      std::move(script), "test-id", base::Time::Now());
  
  EXPECT_EQ("test-id", *result.FindString("id"));
  EXPECT_EQ("Test Script", *result.FindString("name"));
  EXPECT_EQ("1.0.0", *result.FindString("schema_version"));
  EXPECT_TRUE(result.FindString("created_at"));
  EXPECT_TRUE(result.FindString("updated_at"));
  EXPECT_TRUE(result.FindList("url_contexts"));
}

// Test CanonicalizeForSave builds url_contexts from flat actions
TEST_F(ScriptSchemaTest, CanonicalizeForSave_BuildsUrlContexts) {
  base::Value::Dict script;
  base::Value::List actions;
  
  // Add navigate action
  base::Value::Dict nav;
  nav.Set("action", "navigate");
  base::Value::Dict nav_params;
  nav_params.Set("url", "https://example.com");
  nav.Set("params", std::move(nav_params));
  actions.Append(std::move(nav));
  
  // Add click action
  base::Value::Dict click;
  click.Set("action", "click");
  click.Set("params", base::Value::Dict());
  actions.Append(std::move(click));
  
  script.Set("actions", std::move(actions));
  
  auto result = ScriptSchema::CanonicalizeForSave(
      std::move(script), "test-id", base::Time::Now());
  
  const auto* url_contexts = result.FindList("url_contexts");
  EXPECT_TRUE(url_contexts);
  EXPECT_EQ(1u, url_contexts->size());
  
  const auto* first_context = (*url_contexts)[0].GetIfDict();
  EXPECT_EQ("https://example.com", *first_context->FindString("url"));
  
  const auto* context_actions = first_context->FindList("actions");
  EXPECT_EQ(2u, context_actions->size());
}

}  // namespace dashboard

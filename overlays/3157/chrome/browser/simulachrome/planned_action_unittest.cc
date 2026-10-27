// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/planned_action.h"

#include <optional>
#include <string>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace simulachrome {
namespace {

base::Value::Dict Dict(const char* json) {
  std::optional<base::Value> v = base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  CHECK(v && v->is_dict()) << json;
  return std::move(*v).TakeDict();
}

TEST(PlannedActionTest, ParsesClickWithShorthandTarget) {
  std::string error;
  auto a = ParsePlannedAction(
      Dict(R"({"op":"click","target":"the Log in button"})"), &error);
  ASSERT_TRUE(a) << error;
  EXPECT_EQ(PlannedOp::kClick, a->op);
  ASSERT_EQ(1u, a->targets.size());
  EXPECT_EQ("primary", a->targets[0].slot);
  EXPECT_EQ("the Log in button", a->targets[0].describe);
  EXPECT_EQ(ClickStrategy::kRaw, a->strategy);
}

TEST(PlannedActionTest, AliasesResolveThroughSharedVocabulary) {
  std::string error;
  auto a = ParsePlannedAction(
      Dict(R"({"op":"dblclick","target":"row 3"})"), &error);
  ASSERT_TRUE(a) << error;
  EXPECT_EQ(PlannedOp::kDoubleClick, a->op);
  a = ParsePlannedAction(
      Dict(R"({"op":"Fill","target":"email","args":{"text":"{Email}"}})"),
      &error);
  ASSERT_TRUE(a) << error;
  EXPECT_EQ(PlannedOp::kType, a->op);
  EXPECT_EQ("{Email}", a->text);
}

TEST(PlannedActionTest, DragNeedsFromAndTo) {
  std::string error;
  EXPECT_FALSE(ParsePlannedAction(
      Dict(R"({"op":"drag","from":"card"})"), &error));
  EXPECT_NE(std::string::npos, error.find("from,to")) << error;

  auto a = ParsePlannedAction(
      Dict(R"({"op":"drag","from":"card","to":"Done column"})"), &error);
  ASSERT_TRUE(a) << error;
  ASSERT_EQ(2u, a->targets.size());
  EXPECT_EQ("from", a->targets[0].slot);
  EXPECT_EQ("to", a->targets[1].slot);
}

TEST(PlannedActionTest, TargetlessOpsRejectTargets) {
  std::string error;
  EXPECT_FALSE(ParsePlannedAction(
      Dict(R"({"op":"key","target":"box","args":{"key":"enter"}})"), &error));
  auto a = ParsePlannedAction(
      Dict(R"({"op":"key","args":{"key":"Enter"}})"), &error);
  ASSERT_TRUE(a) << error;
  EXPECT_TRUE(a->targets.empty());
  EXPECT_EQ("Enter", a->key);
}

TEST(PlannedActionTest, TypeNeedsTextOrSubmit) {
  std::string error;
  EXPECT_FALSE(ParsePlannedAction(
      Dict(R"({"op":"type","target":"box"})"), &error));
  EXPECT_TRUE(ParsePlannedAction(
      Dict(R"({"op":"type","target":"box","args":{"submit":true}})"), &error));
}

TEST(PlannedActionTest, UnknownOpListsValidOps) {
  std::string error;
  EXPECT_FALSE(ParsePlannedAction(Dict(R"({"op":"teleport"})"), &error));
  EXPECT_NE(std::string::npos, error.find("double_click")) << error;
}

TEST(PlannedActionTest, ScrollDirectionAndAmount) {
  std::string error;
  auto a = ParsePlannedAction(
      Dict(R"({"op":"scroll","args":{"direction":"up","amount_viewport":0.5}})"),
      &error);
  ASSERT_TRUE(a) << error;
  EXPECT_FLOAT_EQ(-0.5f, a->scroll_dy);
  EXPECT_FALSE(ParsePlannedAction(
      Dict(R"({"op":"scroll","args":{"direction":"sideways"}})"), &error));
}

TEST(PlannedActionTest, ExpectSelectsGradedStrategyByDefault) {
  std::string error;
  auto a = ParsePlannedAction(
      Dict(R"({"op":"click","target":"Sign in",
              "expect":{"role_any":["Button","link"],"name_contains":"Sign In"}})"),
      &error);
  ASSERT_TRUE(a) << error;
  EXPECT_EQ(ClickStrategy::kGraded, a->strategy);
  ASSERT_EQ(2u, a->expect.role_any.size());
  EXPECT_EQ("button", a->expect.role_any[0]);
  EXPECT_EQ("sign in", a->expect.name_contains);

  a = ParsePlannedAction(
      Dict(R"({"op":"click","target":"x","strategy":"raw",
              "expect":{"tag":"button"}})"),
      &error);
  ASSERT_TRUE(a) << error;
  EXPECT_EQ(ClickStrategy::kRaw, a->strategy);
}

TEST(PlannedActionTest, BatchIsValidatedWholeAndIdsAssigned) {
  base::Value::List list;
  list.Append(Dict(R"({"op":"click","target":"a"})"));
  list.Append(Dict(R"({"op":"key","args":{"key":"tab"}})"));
  std::vector<PlannedAction> out;
  std::string error;
  ASSERT_TRUE(ParsePlannedBatch(list, &out, &error)) << error;
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ("a0", out[0].id);
  EXPECT_EQ("a1", out[1].id);

  list.Append(Dict(R"({"op":"drag","from":"only"})"));
  EXPECT_FALSE(ParsePlannedBatch(list, &out, &error));
  EXPECT_EQ(0u, error.find("actions[2]: ")) << error;
}

TEST(PlannedActionTest, BatchRejectsDuplicateIdsAndOversize) {
  base::Value::List dup;
  dup.Append(Dict(R"({"id":"x","op":"click","target":"a"})"));
  dup.Append(Dict(R"({"id":"x","op":"click","target":"b"})"));
  std::vector<PlannedAction> out;
  std::string error;
  EXPECT_FALSE(ParsePlannedBatch(dup, &out, &error));
  EXPECT_NE(std::string::npos, error.find("duplicate id")) << error;

  base::Value::List big;
  for (size_t i = 0; i <= kMaxPlannedBatchSize; ++i) {
    big.Append(Dict(R"({"op":"wait"})"));
  }
  EXPECT_FALSE(ParsePlannedBatch(big, &out, &error));
}

TEST(GradeHitTest, UnknownWithoutExpectation) {
  EXPECT_EQ(ClickGrade::kUnknown, GradeHit(ExpectedHit(), "button \"Go\""));
}

TEST(GradeHitTest, MatchesRoleTagAndName) {
  ExpectedHit expect;
  expect.role_any = {"button"};
  expect.name_contains = "log in";
  EXPECT_EQ(ClickGrade::kA, GradeHit(expect, "button \"Log in\""));
  EXPECT_EQ(ClickGrade::kA,
            GradeHit(expect, "div role=button id=go \"Log In now\""));
  // Right kind of control, wrong label.
  EXPECT_EQ(ClickGrade::kB, GradeHit(expect, "button \"Cancel\""));
  EXPECT_EQ(ClickGrade::kC, GradeHit(expect, "span \"Welcome\""));
}

TEST(GradeHitTest, ImplicitRoles) {
  ExpectedHit expect;
  expect.role_any = {"link"};
  EXPECT_EQ(ClickGrade::kA, GradeHit(expect, "a \"Pricing\""));
  // "a" must not match inside "textarea".
  expect.tag = "a";
  EXPECT_EQ(ClickGrade::kC, GradeHit(expect, "textarea id=msg"));
  expect = ExpectedHit();
  expect.role_any = {"textbox"};
  EXPECT_EQ(ClickGrade::kA, GradeHit(expect, "input type=email id=e"));
  EXPECT_EQ(ClickGrade::kC, GradeHit(expect, "input type=submit \"Go\""));
}

TEST(GradeHitTest, EmptySpaceIsAlwaysAMiss) {
  ExpectedHit expect;
  expect.name_contains = "x";
  EXPECT_EQ(ClickGrade::kC, GradeHit(expect, "(no element)"));
  EXPECT_EQ(ClickGrade::kC, GradeHit(expect, "body"));
  EXPECT_EQ(ClickGrade::kC, GradeHit(expect, "html"));
  EXPECT_EQ(ClickGrade::kC, GradeHit(expect, "body id=x \"x\""));
}

}  // namespace
}  // namespace simulachrome

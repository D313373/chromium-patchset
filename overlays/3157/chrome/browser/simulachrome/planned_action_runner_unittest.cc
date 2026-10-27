// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/planned_action_runner.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace simulachrome {
namespace {

// Records the exact order of delegate calls. Each call is answered
// synchronously, which is the harshest case for the runner's ordering.
class FakeDelegate : public PlannedActionDelegate {
 public:
  void LocateTarget(const PlannedTarget& target,
                    LocateCallback done) override {
    events.push_back("locate:" + target.describe);
    ++in_flight;
    max_in_flight = std::max(max_in_flight, in_flight);
    LocateResult r;
    if (fail_locate_for == target.describe) {
      r.ok = false;
      r.error = "element_not_found";
    } else {
      r.ok = true;
      r.point.x = 0.5f;
      r.point.y = 0.25f;
      r.point.confidence = 0.5f;
    }
    --in_flight;
    std::move(done).Run(std::move(r));
  }

  void RefinePoint(const PlannedAction& action,
                   const LocateResult& located,
                   RefineCallback done) override {
    events.push_back("refine:" + action.id);
    RefinedPoint r;
    r.point = located.point;
    r.grade = refine_grade;
    r.snapped = refine_snaps;
    if (refine_snaps) {
      r.point.x = 0.6f;
    }
    r.label = "button \"Go\"";
    std::move(done).Run(std::move(r));
  }

  void Dispatch(const PlannedAction& action,
                const std::vector<PlannedPoint>& points,
                DispatchCallback done) override {
    events.push_back("dispatch:" + action.id + ":" +
                     std::to_string(points.size()));
    last_points = points;
    DispatchResult r;
    r.ok = fail_dispatch_for != action.id;
    r.error = r.ok ? "" : "input_inject_failed";
    r.hit_element = hit_element;
    std::move(done).Run(std::move(r));
  }

  std::vector<std::string> events;
  std::vector<PlannedPoint> last_points;
  std::string fail_locate_for;
  std::string fail_dispatch_for;
  std::string hit_element;
  ClickGrade refine_grade = ClickGrade::kA;
  bool refine_snaps = false;
  int in_flight = 0;
  int max_in_flight = 0;
};

std::vector<PlannedAction> Batch(const char* json) {
  std::optional<base::Value> v = base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  CHECK(v && v->is_list()) << json;
  std::vector<PlannedAction> out;
  std::string error;
  CHECK(ParsePlannedBatch(v->GetList(), &out, &error)) << error;
  return out;
}

class PlannedActionRunnerTest : public testing::Test {
 protected:
  base::test::SingleThreadTaskEnvironment task_environment_;
};

TEST_F(PlannedActionRunnerTest, ActionFiresRightAfterItsTargetResolves) {
  FakeDelegate delegate;
  PlannedActionRunner runner(&delegate);
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"u","op":"type","target":"email","args":{"text":"{Email}"}},
                {"id":"k","op":"key","args":{"key":"enter"}},
                {"id":"b","op":"click","target":"Log in"}])"),
      PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  task_environment_.RunUntilIdle();

  ASSERT_TRUE(done);
  EXPECT_TRUE(done->ok);
  // Locate -> dispatch per action, strictly in order. The key press never asks
  // the model. Nothing for action N+1 happens before action N's dispatch.
  EXPECT_EQ((std::vector<std::string>{"locate:email", "dispatch:u:1",
                                      "dispatch:k:0", "locate:Log in",
                                      "dispatch:b:1"}),
            delegate.events);
}

TEST_F(PlannedActionRunnerTest, DragLocatesBothEndsThenFiresOnce) {
  FakeDelegate delegate;
  PlannedActionRunner runner(&delegate);
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"d","op":"drag","from":"card","to":"Done"}])"),
      PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(done);
  EXPECT_EQ((std::vector<std::string>{"locate:card", "locate:Done",
                                      "dispatch:d:2"}),
            delegate.events);
  ASSERT_EQ(2u, delegate.last_points.size());
  EXPECT_EQ("from", delegate.last_points[0].slot);
  EXPECT_EQ("to", delegate.last_points[1].slot);
}

TEST_F(PlannedActionRunnerTest, StopsAndSkipsAfterFailure) {
  FakeDelegate delegate;
  delegate.fail_locate_for = "missing";
  PlannedActionRunner runner(&delegate);
  std::vector<std::string> per_action;
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"a","op":"click","target":"ok"},
                {"id":"b","op":"click","target":"missing"},
                {"id":"c","op":"click","target":"never"}])"),
      PlannedBatchOptions(),
      base::BindRepeating(
          [](std::vector<std::string>* log, const PlannedActionResult& r) {
            log->push_back(r.id + "=" + r.ToValue().FindString("status")->c_str());
          },
          &per_action),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  task_environment_.RunUntilIdle();

  ASSERT_TRUE(done);
  EXPECT_FALSE(done->ok);
  EXPECT_EQ(1, done->failed_index);
  EXPECT_EQ((std::vector<std::string>{"a=ok", "b=failed", "c=skipped"}),
            per_action);
  EXPECT_EQ("element_not_found", done->results[1].error);
  EXPECT_EQ("skipped_after_failure", done->results[2].error);
  // "never" was never located.
  for (const std::string& e : delegate.events) {
    EXPECT_NE("locate:never", e);
  }
}

TEST_F(PlannedActionRunnerTest, ContinueOnFailureKeepsGoing) {
  FakeDelegate delegate;
  delegate.fail_locate_for = "missing";
  PlannedActionRunner runner(&delegate);
  PlannedBatchOptions options;
  options.stop_on_failure = false;
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"b","op":"click","target":"missing"},
                {"id":"c","op":"click","target":"fine"}])"),
      options, PlannedActionRunner::ActionCallback(),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(done);
  EXPECT_FALSE(done->ok);
  EXPECT_EQ(PlannedActionResult::Status::kOk, done->results[1].status);
}

TEST_F(PlannedActionRunnerTest, BatchesRunFifoOneAtATime) {
  FakeDelegate delegate;
  PlannedActionRunner runner(&delegate);
  std::vector<std::string> finished;
  auto record = [](std::vector<std::string>* out, std::string name,
                   PlannedBatchResult) { out->push_back(name); };
  runner.Submit(Batch(R"([{"id":"a1","op":"click","target":"one"}])"),
                PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
                base::BindOnce(record, &finished, "first"));
  runner.Submit(Batch(R"([{"id":"b1","op":"click","target":"two"}])"),
                PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
                base::BindOnce(record, &finished, "second"));
  EXPECT_EQ(2u, runner.queued_batches());
  task_environment_.RunUntilIdle();
  EXPECT_EQ((std::vector<std::string>{"first", "second"}), finished);
  EXPECT_EQ((std::vector<std::string>{"locate:one", "dispatch:a1:1",
                                      "locate:two", "dispatch:b1:1"}),
            delegate.events);
  EXPECT_FALSE(runner.busy());
  EXPECT_EQ(1, delegate.max_in_flight);
}

TEST_F(PlannedActionRunnerTest, GradedClickUsesSnappedPointAndReportsIt) {
  FakeDelegate delegate;
  delegate.refine_snaps = true;
  delegate.refine_grade = ClickGrade::kA;
  PlannedActionRunner runner(&delegate);
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"g","op":"click","target":"Go",
                 "expect":{"role_any":["button"]}}])"),
      PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(done);
  EXPECT_EQ((std::vector<std::string>{"locate:Go", "refine:g", "dispatch:g:1"}),
            delegate.events);
  ASSERT_EQ(1u, delegate.last_points.size());
  EXPECT_FLOAT_EQ(0.6f, delegate.last_points[0].x);
  EXPECT_TRUE(done->results[0].snapped);
  EXPECT_FALSE(done->results[0].raw_fallback);
}

TEST_F(PlannedActionRunnerTest, GradedClickFallsBackToRawPoint) {
  FakeDelegate delegate;
  delegate.refine_snaps = false;
  delegate.refine_grade = ClickGrade::kC;
  PlannedActionRunner runner(&delegate);
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"g","op":"click","target":"Go",
                 "expect":{"tag":"button"}}])"),
      PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(done);
  EXPECT_TRUE(done->ok);  // The raw click still goes out.
  ASSERT_EQ(1u, delegate.last_points.size());
  EXPECT_FLOAT_EQ(0.5f, delegate.last_points[0].x);
  EXPECT_TRUE(done->results[0].raw_fallback);
}

TEST_F(PlannedActionRunnerTest, RawStrategySkipsRefine) {
  FakeDelegate delegate;
  PlannedActionRunner runner(&delegate);
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"r","op":"click","target":"Go"}])"),
      PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  task_environment_.RunUntilIdle();
  EXPECT_EQ((std::vector<std::string>{"locate:Go", "dispatch:r:1"}),
            delegate.events);
}

TEST_F(PlannedActionRunnerTest, PostClickGradeWarnsButDoesNotFail) {
  FakeDelegate delegate;
  delegate.hit_element = "div \"Banner\"";
  PlannedActionRunner runner(&delegate);
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"w","op":"click","target":"Go","strategy":"raw",
                 "expect":{"role_any":["button"]}}])"),
      PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(done);
  EXPECT_TRUE(done->ok);
  EXPECT_EQ(ClickGrade::kC, done->results[0].grade);
  EXPECT_EQ("clicked_unexpected_element", done->results[0].warning);
}

TEST_F(PlannedActionRunnerTest, CancelSkipsEverythingQueued) {
  FakeDelegate delegate;
  PlannedActionRunner runner(&delegate);
  std::optional<PlannedBatchResult> done;
  runner.Submit(
      Batch(R"([{"id":"a","op":"click","target":"one"},
                {"id":"b","op":"click","target":"two"}])"),
      PlannedBatchOptions(), PlannedActionRunner::ActionCallback(),
      base::BindOnce([](std::optional<PlannedBatchResult>* out,
                        PlannedBatchResult r) { *out = std::move(r); },
                     &done));
  runner.CancelAll();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(done);
  EXPECT_TRUE(done->cancelled);
  EXPECT_FALSE(done->ok);
  EXPECT_TRUE(delegate.events.empty());
  EXPECT_EQ("cancelled", done->results[0].error);
}

}  // namespace
}  // namespace simulachrome

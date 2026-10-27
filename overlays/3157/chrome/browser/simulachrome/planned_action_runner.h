// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_PLANNED_ACTION_RUNNER_H_
#define CHROME_BROWSER_SIMULACHROME_PLANNED_ACTION_RUNNER_H_

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/simulachrome/planned_action.h"

namespace simulachrome {

// A point the embedded model located, normalized to [0,1] in the viewport.
struct PlannedPoint {
  std::string slot;
  float x = 0.f;
  float y = 0.f;
  float confidence = 0.f;
};

struct LocateResult {
  bool ok = false;
  std::string error;  // "element_not_found", "capture_failed", ...
  PlannedPoint point;
};

struct RefinedPoint {
  PlannedPoint point;
  ClickGrade grade = ClickGrade::kUnknown;
  std::string label;   // element description at |point|, before the click
  bool snapped = false;  // point moved off the raw model point
};

struct DispatchResult {
  bool ok = false;
  std::string error;
  std::string hit_element;  // element under the pointer, when the backend knows
};

struct PlannedActionResult {
  enum class Status { kOk, kFailed, kSkipped };

  PlannedActionResult();
  ~PlannedActionResult();
  PlannedActionResult(const PlannedActionResult&);
  PlannedActionResult& operator=(const PlannedActionResult&);
  PlannedActionResult(PlannedActionResult&&);
  PlannedActionResult& operator=(PlannedActionResult&&);

  size_t index = 0;
  std::string id;
  std::string op;
  Status status = Status::kSkipped;
  std::string error;
  std::string warning;
  std::string hit_element;
  ClickGrade grade = ClickGrade::kUnknown;
  bool snapped = false;
  // Graded click found nothing matching nearby and used the model's raw point.
  bool raw_fallback = false;
  std::vector<PlannedPoint> points;

  base::Value::Dict ToValue() const;
};

struct PlannedBatchResult {
  PlannedBatchResult();
  ~PlannedBatchResult();
  PlannedBatchResult(const PlannedBatchResult&);
  PlannedBatchResult& operator=(const PlannedBatchResult&);
  PlannedBatchResult(PlannedBatchResult&&);
  PlannedBatchResult& operator=(PlannedBatchResult&&);

  bool ok = false;
  bool cancelled = false;
  int failed_index = -1;
  std::vector<PlannedActionResult> results;

  base::Value::Dict ToValue() const;
};

// Everything the runner needs from the browser. SimulachromeAgentHost
// implements this; tests use a fake. Each method must run its callback exactly
// once, possibly synchronously.
class PlannedActionDelegate {
 public:
  using LocateCallback = base::OnceCallback<void(LocateResult)>;
  using RefineCallback = base::OnceCallback<void(RefinedPoint)>;
  using DispatchCallback = base::OnceCallback<void(DispatchResult)>;

  virtual ~PlannedActionDelegate() = default;

  // Ask the embedded model where one control is. This is the only call that
  // uses the model.
  virtual void LocateTarget(const PlannedTarget& target,
                            LocateCallback done) = 0;

  // Graded click: probe around |located| and return the best point to click,
  // or |located| itself (grade kC, snapped=false) when nothing matches.
  virtual void RefinePoint(const PlannedAction& action,
                           const LocateResult& located,
                           RefineCallback done) = 0;

  // Perform the operation. |points| has one entry per target slot, in order.
  virtual void Dispatch(const PlannedAction& action,
                        const std::vector<PlannedPoint>& points,
                        DispatchCallback done) = 0;
};

// Serial queue for batch page tasks.
//
// Exactly one operation is in flight at a time, across all batches: either one
// embedded-model locate or one dispatch. An action fires the moment its last
// target is resolved. The next action's screenshot is taken only after the
// previous action has finished, because that action changes the page the model
// must look at; that is why locates are not pipelined ahead of dispatches.
class PlannedActionRunner {
 public:
  using ActionCallback =
      base::RepeatingCallback<void(const PlannedActionResult&)>;
  using BatchCallback = base::OnceCallback<void(PlannedBatchResult)>;

  explicit PlannedActionRunner(PlannedActionDelegate* delegate);
  ~PlannedActionRunner();

  PlannedActionRunner(const PlannedActionRunner&) = delete;
  PlannedActionRunner& operator=(const PlannedActionRunner&) = delete;

  // Batches run first-in first-out. |on_action| (optional) fires after each
  // action; |on_done| fires once when the batch has finished or been cancelled.
  void Submit(std::vector<PlannedAction> actions,
              PlannedBatchOptions options,
              ActionCallback on_action,
              BatchCallback on_done);

  // Lets the in-flight operation finish, then skips everything still queued.
  void CancelAll();

  bool busy() const { return !batches_.empty(); }
  size_t queued_batches() const { return batches_.size(); }

 private:
  struct Batch {
    Batch();
    ~Batch();
    std::vector<PlannedAction> actions;
    PlannedBatchOptions options;
    ActionCallback on_action;
    BatchCallback on_done;
    PlannedBatchResult result;
    size_t next_index = 0;
    bool started = false;
  };

  void PostPump();
  void Pump();
  void StartAction();
  void LocateNextSlot();
  void OnLocated(LocateResult located);
  void OnRefined(PlannedPoint raw, RefinedPoint refined);
  void DispatchNow();
  void OnDispatched(DispatchResult dispatched);
  void FinishAction(PlannedActionResult::Status status, std::string error);
  void SkipRemaining(Batch& batch, const std::string& reason);
  void FinishFrontBatch();

  const raw_ptr<PlannedActionDelegate> delegate_;
  std::deque<std::unique_ptr<Batch>> batches_;
  bool in_flight_ = false;
  bool cancel_requested_ = false;
  bool pump_posted_ = false;

  // State of the action currently in flight (front batch, next_index - 1).
  PlannedActionResult current_;
  std::vector<PlannedPoint> points_;
  size_t slot_index_ = 0;

  base::WeakPtrFactory<PlannedActionRunner> weak_factory_{this};
};

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_PLANNED_ACTION_RUNNER_H_

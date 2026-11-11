// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/planned_action_runner.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"

namespace simulachrome {
namespace {

bool IsPointerOp(PlannedOp op) {
  switch (op) {
    case PlannedOp::kClick:
    case PlannedOp::kDoubleClick:
    case PlannedOp::kRightClick:
    case PlannedOp::kHover:
    case PlannedOp::kType:
      return true;
    case PlannedOp::kKey:
    case PlannedOp::kScroll:
    case PlannedOp::kDrag:
    case PlannedOp::kWait:
      return false;
  }
  return false;
}

std::string_view StatusName(PlannedActionResult::Status status) {
  switch (status) {
    case PlannedActionResult::Status::kOk:
      return "ok";
    case PlannedActionResult::Status::kFailed:
      return "failed";
    case PlannedActionResult::Status::kSkipped:
      return "skipped";
  }
  return "failed";
}

}  // namespace

PlannedActionResult::PlannedActionResult() = default;
PlannedActionResult::~PlannedActionResult() = default;
PlannedActionResult::PlannedActionResult(const PlannedActionResult&) = default;
PlannedActionResult& PlannedActionResult::operator=(
    const PlannedActionResult&) = default;
PlannedActionResult::PlannedActionResult(PlannedActionResult&&) = default;
PlannedActionResult& PlannedActionResult::operator=(PlannedActionResult&&) =
    default;

PlannedBatchResult::PlannedBatchResult() = default;
PlannedBatchResult::~PlannedBatchResult() = default;
PlannedBatchResult::PlannedBatchResult(const PlannedBatchResult&) = default;
PlannedBatchResult& PlannedBatchResult::operator=(const PlannedBatchResult&) =
    default;
PlannedBatchResult::PlannedBatchResult(PlannedBatchResult&&) = default;
PlannedBatchResult& PlannedBatchResult::operator=(PlannedBatchResult&&) =
    default;

base::Value::Dict PlannedActionResult::ToValue() const {
  base::Value::Dict d;
  d.Set("index", static_cast<int>(index));
  d.Set("id", id);
  d.Set("op", op);
  d.Set("status", std::string(StatusName(status)));
  d.Set("ok", status == Status::kOk);
  if (!error.empty()) {
    d.Set("error", error);
  }
  if (!warning.empty()) {
    d.Set("warning", warning);
  }
  if (!hit_element.empty()) {
    d.Set("hit_element", hit_element);
  }
  if (grade != ClickGrade::kUnknown) {
    d.Set("grade", std::string(ClickGradeName(grade)));
  }
  if (snapped) {
    d.Set("snapped", true);
  }
  if (raw_fallback) {
    d.Set("raw_fallback", true);
  }
  base::Value::List pts;
  for (const PlannedPoint& p : points) {
    base::Value::Dict pd;
    pd.Set("slot", p.slot);
    pd.Set("x", p.x);
    pd.Set("y", p.y);
    pd.Set("confidence", p.confidence);
    pts.Append(std::move(pd));
  }
  if (!pts.empty()) {
    d.Set("points", std::move(pts));
  }
  return d;
}

base::Value::Dict PlannedBatchResult::ToValue() const {
  base::Value::Dict d;
  d.Set("ok", ok);
  if (cancelled) {
    d.Set("cancelled", true);
  }
  d.Set("failed_index", failed_index);
  base::Value::List list;
  for (const PlannedActionResult& r : results) {
    list.Append(r.ToValue());
  }
  d.Set("results", std::move(list));
  return d;
}

PlannedActionRunner::Batch::Batch() = default;
PlannedActionRunner::Batch::~Batch() = default;

PlannedActionRunner::PlannedActionRunner(PlannedActionDelegate* delegate)
    : delegate_(delegate) {}

PlannedActionRunner::~PlannedActionRunner() {
  // The host (and with it this queue) goes away when its tab does. Resolve
  // every pending batch rather than stranding the caller's promise.
  weak_factory_.InvalidateWeakPtrs();
  if (in_flight_ && !batches_.empty()) {
    Batch& batch = *batches_.front();
    current_.status = PlannedActionResult::Status::kFailed;
    current_.error = "host_destroyed";
    batch.result.results.push_back(current_);
    if (batch.result.failed_index < 0) {
      batch.result.failed_index = static_cast<int>(current_.index);
    }
    in_flight_ = false;
  }
  while (!batches_.empty()) {
    Batch& batch = *batches_.front();
    batch.result.cancelled = true;
    SkipRemaining(batch, "host_destroyed");
    FinishFrontBatch();
  }
}

void PlannedActionRunner::Submit(std::vector<PlannedAction> actions,
                                 PlannedBatchOptions options,
                                 ActionCallback on_action,
                                 BatchCallback on_done) {
  auto batch = std::make_unique<Batch>();
  batch->actions = std::move(actions);
  batch->options = options;
  batch->on_action = std::move(on_action);
  batch->on_done = std::move(on_done);
  batches_.push_back(std::move(batch));
  PostPump();
}

void PlannedActionRunner::CancelAll() {
  cancel_requested_ = true;
  // Idle (between actions) is the only state where nothing will notice the
  // flag, so wake the pump to flush the queue.
  if (!in_flight_) {
    PostPump();
  }
}

void PlannedActionRunner::PostPump() {
  if (pump_posted_) {
    return;
  }
  pump_posted_ = true;
  // Always hop through the task runner: delegate callbacks may be synchronous
  // and a batch of waits would otherwise recurse once per action.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&PlannedActionRunner::Pump,
                                weak_factory_.GetWeakPtr()));
}

void PlannedActionRunner::Pump() {
  pump_posted_ = false;
  if (in_flight_) {
    return;
  }
  while (!batches_.empty()) {
    Batch& batch = *batches_.front();
    if (cancel_requested_) {
      batch.result.cancelled = true;
      SkipRemaining(batch, "cancelled");
      FinishFrontBatch();
      continue;
    }
    if (batch.next_index >= batch.actions.size()) {
      FinishFrontBatch();
      continue;
    }
    StartAction();
    return;
  }
  // Queue drained: a later Submit starts fresh.
  cancel_requested_ = false;
}

void PlannedActionRunner::StartAction() {
  Batch& batch = *batches_.front();
  batch.started = true;
  const size_t index = batch.next_index++;
  const PlannedAction& action = batch.actions[index];

  in_flight_ = true;
  current_ = PlannedActionResult();
  current_.index = index;
  current_.id = action.id;
  current_.op = std::string(PlannedOpName(action.op));
  points_.clear();
  slot_index_ = 0;
  LOG(INFO) << "[PlannedActionRunner] start id=" << action.id
            << " op=" << current_.op << " targets=" << action.targets.size();
  LocateNextSlot();
}

void PlannedActionRunner::LocateNextSlot() {
  Batch& batch = *batches_.front();
  const PlannedAction& action = batch.actions[current_.index];
  if (slot_index_ >= action.targets.size()) {
    DispatchNow();
    return;
  }
  delegate_->LocateTarget(
      action.targets[slot_index_],
      base::BindOnce(&PlannedActionRunner::OnLocated,
                     weak_factory_.GetWeakPtr()));
}

void PlannedActionRunner::OnLocated(LocateResult located) {
  Batch& batch = *batches_.front();
  const PlannedAction& action = batch.actions[current_.index];
  if (!located.ok) {
    FinishAction(PlannedActionResult::Status::kFailed,
                 located.error.empty() ? "locate_failed" : located.error);
    return;
  }
  located.point.slot = action.targets[slot_index_].slot;

  const bool refine = action.strategy == ClickStrategy::kGraded &&
                      slot_index_ == 0 && IsPointerOp(action.op);
  if (!refine) {
    points_.push_back(located.point);
    ++slot_index_;
    LocateNextSlot();
    return;
  }
  const PlannedPoint raw = located.point;
  delegate_->RefinePoint(
      action, located,
      base::BindOnce(&PlannedActionRunner::OnRefined,
                     weak_factory_.GetWeakPtr(), raw));
}

void PlannedActionRunner::OnRefined(PlannedPoint raw, RefinedPoint refined) {
  current_.grade = refined.grade;
  current_.snapped = refined.snapped;
  current_.hit_element = refined.label;
  // Nothing near the model's point looked right: keep the model's point. This
  // is the fallback the strategy promises, and it is reported, not hidden.
  current_.raw_fallback = refined.grade == ClickGrade::kC && !refined.snapped;
  PlannedPoint chosen = refined.snapped ? refined.point : raw;
  chosen.slot = raw.slot;
  chosen.confidence = raw.confidence;
  points_.push_back(chosen);
  ++slot_index_;
  LocateNextSlot();
}

void PlannedActionRunner::DispatchNow() {
  Batch& batch = *batches_.front();
  const PlannedAction& action = batch.actions[current_.index];
  current_.points = points_;
  delegate_->Dispatch(
      action, points_,
      base::BindOnce(&PlannedActionRunner::OnDispatched,
                     weak_factory_.GetWeakPtr()));
}

void PlannedActionRunner::OnDispatched(DispatchResult dispatched) {
  Batch& batch = *batches_.front();
  const PlannedAction& action = batch.actions[current_.index];
  if (!dispatched.hit_element.empty()) {
    current_.hit_element = dispatched.hit_element;
  }
  if (!dispatched.ok) {
    FinishAction(PlannedActionResult::Status::kFailed,
                 dispatched.error.empty() ? "dispatch_failed"
                                          : dispatched.error);
    return;
  }
  // The click already happened, so the post-click label is graded only to
  // tell the manager what was really under the pointer.
  if (!action.expect.empty() && IsPointerOp(action.op) &&
      !dispatched.hit_element.empty()) {
    current_.grade = GradeHit(action.expect, dispatched.hit_element);
    if (current_.grade == ClickGrade::kC) {
      current_.warning = "clicked_unexpected_element";
    }
  }
  FinishAction(PlannedActionResult::Status::kOk, std::string());
}

void PlannedActionRunner::FinishAction(PlannedActionResult::Status status,
                                       std::string error) {
  Batch& batch = *batches_.front();
  current_.status = status;
  current_.error = std::move(error);
  LOG(INFO) << "[PlannedActionRunner] done id=" << current_.id
            << " status=" << StatusName(status) << " err=" << current_.error
            << " grade=" << ClickGradeName(current_.grade)
            << " hit=" << current_.hit_element;

  batch.result.results.push_back(current_);
  if (status != PlannedActionResult::Status::kOk &&
      batch.result.failed_index < 0) {
    batch.result.failed_index = static_cast<int>(current_.index);
  }
  if (batch.on_action) {
    batch.on_action.Run(batch.result.results.back());
  }
  in_flight_ = false;

  if (status != PlannedActionResult::Status::kOk &&
      batch.options.stop_on_failure) {
    SkipRemaining(batch, "skipped_after_failure");
  }
  PostPump();
}

void PlannedActionRunner::SkipRemaining(Batch& batch,
                                        const std::string& reason) {
  while (batch.next_index < batch.actions.size()) {
    const size_t index = batch.next_index++;
    PlannedActionResult skipped;
    skipped.index = index;
    skipped.id = batch.actions[index].id;
    skipped.op = std::string(PlannedOpName(batch.actions[index].op));
    skipped.status = PlannedActionResult::Status::kSkipped;
    skipped.error = reason;
    batch.result.results.push_back(skipped);
    if (batch.on_action) {
      batch.on_action.Run(batch.result.results.back());
    }
  }
}

void PlannedActionRunner::FinishFrontBatch() {
  std::unique_ptr<Batch> batch = std::move(batches_.front());
  batches_.pop_front();
  bool all_ok = !batch->result.cancelled;
  for (const PlannedActionResult& r : batch->result.results) {
    if (r.status != PlannedActionResult::Status::kOk) {
      all_ok = false;
    }
  }
  batch->result.ok = all_ok;
  if (batch->on_done) {
    std::move(batch->on_done).Run(std::move(batch->result));
  }
}

}  // namespace simulachrome

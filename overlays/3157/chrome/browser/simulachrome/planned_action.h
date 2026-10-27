// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_PLANNED_ACTION_H_
#define CHROME_BROWSER_SIMULACHROME_PLANNED_ACTION_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "chrome/browser/simulachrome/action_vocabulary_generated.h"

namespace simulachrome {

using PlannedOp = vocabulary::PlannedOp;

inline constexpr size_t kMaxPlannedBatchSize = 12;

// One thing the embedded model must find. The only text the model ever sees is
// |describe|; it never sees the operation, typed text or tokens.
struct PlannedTarget {
  std::string slot;           // "primary" | "from" | "to"
  std::string describe;       // "the blue Log in button"
  std::string css_selector;   // optional scroll-into-view hint
};

// What the element under the point should look like once located. Used to
// grade the click, never to change what gets clicked except by nudging to a
// better-matching neighboring point.
struct ExpectedHit {
  ExpectedHit();
  ~ExpectedHit();
  ExpectedHit(const ExpectedHit&);
  ExpectedHit& operator=(const ExpectedHit&);
  ExpectedHit(ExpectedHit&&);
  ExpectedHit& operator=(ExpectedHit&&);

  std::vector<std::string> role_any;  // lowercase ARIA roles
  std::string tag;                    // lowercase
  std::string name_contains;          // lowercase substring

  bool empty() const {
    return role_any.empty() && tag.empty() && name_contains.empty();
  }
};

enum class ClickGrade { kUnknown, kA, kB, kC };

enum class ClickStrategy {
  // Click exactly where the embedded model pointed.
  kRaw,
  // Probe the neighborhood of the point, click the best-graded sample, and
  // fall back to the raw point when nothing matches.
  kGraded,
};

// Everything about a step except where its targets are. Composed by the
// manager and validated here, so the embedded model is asked only to locate.
struct PlannedAction {
  PlannedAction();
  ~PlannedAction();
  PlannedAction(const PlannedAction&);
  PlannedAction& operator=(const PlannedAction&);
  PlannedAction(PlannedAction&&);
  PlannedAction& operator=(PlannedAction&&);

  std::string id;
  PlannedOp op = PlannedOp::kClick;
  std::vector<PlannedTarget> targets;  // one per slot the op needs
  std::string text;                    // kType; may be a {Token}
  bool submit = false;                 // kType: press Enter afterwards
  std::string key;                     // kKey
  float scroll_dy = 0.8f;              // kScroll, signed viewport fraction
  int wait_ms = 0;                     // kWait
  ExpectedHit expect;
  ClickStrategy strategy = ClickStrategy::kRaw;
};

struct PlannedBatchOptions {
  // Stop at the first failed action and mark the rest skipped.
  bool stop_on_failure = true;
};

std::string_view PlannedOpName(PlannedOp op);
const vocabulary::PlannedOpSpec& SpecFor(PlannedOp op);

// |error| is a human/manager-readable reason, e.g. "drag needs targets from,to".
std::optional<PlannedAction> ParsePlannedAction(const base::Value::Dict& dict,
                                                std::string* error);

// Validates the whole list up front so nothing runs when a later step is bad.
// |error| is prefixed with "actions[N]: ".
bool ParsePlannedBatch(const base::Value::List& list,
                       std::vector<PlannedAction>* out,
                       std::string* error);

// Grades an element description (the renderer's "tag type=.. role=.. id=..
// \"name\"" label) against |expect|. kUnknown when |expect| is empty.
ClickGrade GradeHit(const ExpectedHit& expect, std::string_view hit_label);
std::string_view ClickGradeName(ClickGrade grade);

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_PLANNED_ACTION_H_

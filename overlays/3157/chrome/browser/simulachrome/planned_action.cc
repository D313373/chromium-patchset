// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/planned_action.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace simulachrome {

PlannedAction::PlannedAction() = default;
PlannedAction::~PlannedAction() = default;
PlannedAction::PlannedAction(const PlannedAction&) = default;
PlannedAction& PlannedAction::operator=(const PlannedAction&) = default;
PlannedAction::PlannedAction(PlannedAction&&) = default;
PlannedAction& PlannedAction::operator=(PlannedAction&&) = default;

ExpectedHit::ExpectedHit() {}
ExpectedHit::~ExpectedHit() {}
ExpectedHit::ExpectedHit(const ExpectedHit&) {}
ExpectedHit& ExpectedHit::operator=(const ExpectedHit&) { return *this; }
ExpectedHit::ExpectedHit(ExpectedHit&&) {}
ExpectedHit& ExpectedHit::operator=(ExpectedHit&&) { return *this; }

namespace {

constexpr size_t kMaxDescribeLen = 200;
constexpr size_t kMaxTextLen = 2000;
constexpr size_t kMaxKeyLen = 32;
constexpr size_t kMaxIdLen = 64;

bool IsSafeKeyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-' ||
         c == ' ';
}

// Reads `name` from `args` first, then from the action's top level, so both
// {"op":"type","args":{"text":"x"}} and {"op":"type","text":"x"} work.
const base::Value* FindArg(const base::Value::Dict& dict,
                           const base::Value::Dict* args,
                           const char* name) {
  if (args) {
    if (const base::Value* v = args->Find(name)) {
      return v;
    }
  }
  return dict.Find(name);
}

std::string TrimmedString(const base::Value* v) {
  if (!v || !v->is_string()) {
    return std::string();
  }
  return std::string(base::TrimWhitespaceASCII(v->GetString(), base::TRIM_ALL));
}

void ParseExpect(const base::Value::Dict& dict, ExpectedHit* expect) {
  const base::Value::Dict* e = dict.FindDict("expect");
  if (!e) {
    return;
  }
  if (const base::Value::List* roles = e->FindList("role_any")) {
    for (const base::Value& r : *roles) {
      if (r.is_string() && !r.GetString().empty()) {
        expect->role_any.push_back(base::ToLowerASCII(r.GetString()));
      }
    }
  } else if (const std::string* role = e->FindString("role")) {
    if (!role->empty()) {
      expect->role_any.push_back(base::ToLowerASCII(*role));
    }
  }
  if (const std::string* tag = e->FindString("tag")) {
    expect->tag = base::ToLowerASCII(*tag);
  }
  if (const std::string* name = e->FindString("name_contains")) {
    expect->name_contains = base::ToLowerASCII(*name);
  }
}

// Collects target descriptions from any of:
//   "targets":[{"slot":"from","describe":"..."}, "plain string", ...]
//   "target":"..."                     (primary)
//   "from":"..." / "to":"..."          (drag)
bool ParseTargets(const base::Value::Dict& dict,
                  const vocabulary::PlannedOpSpec& spec,
                  std::vector<PlannedTarget>* out,
                  std::string* error) {
  std::vector<PlannedTarget> found;
  if (const base::Value::List* list = dict.FindList("targets")) {
    for (const base::Value& item : *list) {
      PlannedTarget t;
      if (item.is_string()) {
        t.describe = std::string(
            base::TrimWhitespaceASCII(item.GetString(), base::TRIM_ALL));
      } else if (const base::Value::Dict* d = item.GetIfDict()) {
        t.describe = TrimmedString(d->Find("describe"));
        if (t.describe.empty()) {
          t.describe = TrimmedString(d->Find("target"));
        }
        t.slot = TrimmedString(d->Find("slot"));
        t.css_selector = TrimmedString(d->Find("css_selector"));
      }
      found.push_back(std::move(t));
    }
  } else {
    for (const char* key : {"target", "from", "to"}) {
      const std::string text = TrimmedString(dict.Find(key));
      if (text.empty()) {
        continue;
      }
      PlannedTarget t;
      t.describe = text;
      t.slot = std::string(key) == "target" ? "primary" : key;
      found.push_back(std::move(t));
    }
  }

  const base::span<const std::string_view> slot_names(spec.target_slots);
  if (spec.target_count < 0 ||
      static_cast<size_t>(spec.target_count) > slot_names.size()) {
    *error = "internal: planned op slot count is out of range";
    return false;
  }
  if (static_cast<int>(found.size()) != spec.target_count) {
    std::string slots;
    for (int i = 0; i < spec.target_count; ++i) {
      if (i) {
        slots += ",";
      }
      slots += std::string(slot_names.at(static_cast<size_t>(i)));
    }
    *error = spec.target_count == 0
                 ? base::StrCat({std::string(spec.name),
                                 " takes no target (the model is not asked)"})
                 : base::StrCat({std::string(spec.name), " needs ",
                                 base::NumberToString(spec.target_count),
                                 " target(s): ", slots});
    return false;
  }
  for (int i = 0; i < spec.target_count; ++i) {
    PlannedTarget& t = found[i];
    const std::string expected_slot(slot_names[static_cast<size_t>(i)]);
    // A slot written by the manager must agree with its position.
    if (!t.slot.empty() && t.slot != expected_slot) {
      *error = base::StrCat({std::string(spec.name), " target ",
                             base::NumberToString(i), " must be slot ",
                             expected_slot, ", got ", t.slot});
      return false;
    }
    t.slot = expected_slot;
    if (t.describe.empty()) {
      *error = base::StrCat({expected_slot, " target has no describe text"});
      return false;
    }
    if (t.describe.size() > kMaxDescribeLen) {
      t.describe.resize(kMaxDescribeLen);
    }
  }
  *out = std::move(found);
  return true;
}

bool LabelHasToken(std::string_view label, std::string_view token) {
  // label looks like: tag[ type=..][ role=..][ id=..][ "name"][ inside <label>]
  // Match whole words so tag "a" does not match "textarea".
  size_t pos = 0;
  while ((pos = label.find(token, pos)) != std::string_view::npos) {
    const bool left_ok = pos == 0 || label[pos - 1] == ' ' || label[pos - 1] == '=';
    const size_t end = pos + token.size();
    const bool right_ok =
        end == label.size() || label[end] == ' ' || label[end] == '"';
    if (left_ok && right_ok) {
      return true;
    }
    pos = end;
  }
  return false;
}

bool ImplicitRoleMatches(std::string_view label, std::string_view role) {
  const size_t space = label.find(' ');
  const std::string_view tag =
      space == std::string_view::npos ? label : label.substr(0, space);
  if (role == "link") {
    return tag == "a";
  }
  if (role == "button") {
    return tag == "button" ||
           (tag == "input" && (label.find("type=submit") != std::string_view::npos ||
                               label.find("type=button") != std::string_view::npos));
  }
  if (role == "textbox") {
    return tag == "textarea" ||
           (tag == "input" &&
            label.find("type=checkbox") == std::string_view::npos &&
            label.find("type=radio") == std::string_view::npos &&
            label.find("type=submit") == std::string_view::npos &&
            label.find("type=button") == std::string_view::npos);
  }
  if (role == "checkbox") {
    return tag == "input" && label.find("type=checkbox") != std::string_view::npos;
  }
  if (role == "combobox") {
    return tag == "select";
  }
  if (role == "img") {
    return tag == "img";
  }
  return false;
}

}  // namespace

std::string_view PlannedOpName(PlannedOp op) {
  return SpecFor(op).name;
}

const vocabulary::PlannedOpSpec& SpecFor(PlannedOp op) {
  for (const vocabulary::PlannedOpSpec& spec : vocabulary::kPlannedOps) {
    if (spec.op == op) {
      return spec;
    }
  }
  return vocabulary::kPlannedOps[0];
}

std::optional<PlannedAction> ParsePlannedAction(const base::Value::Dict& dict,
                                                std::string* error) {
  std::string scratch;
  if (!error) {
    error = &scratch;
  }
  const std::string raw_op = base::ToLowerASCII(TrimmedString(dict.Find("op")));
  if (raw_op.empty()) {
    *error = "missing op";
    return std::nullopt;
  }
  const std::string_view canonical = vocabulary::ResolvePlannedOpName(raw_op);
  const vocabulary::PlannedOpSpec* spec = nullptr;
  for (const vocabulary::PlannedOpSpec& s : vocabulary::kPlannedOps) {
    if (s.name == canonical) {
      spec = &s;
      break;
    }
  }
  if (!spec) {
    std::string names;
    for (const vocabulary::PlannedOpSpec& s : vocabulary::kPlannedOps) {
      if (!names.empty()) {
        names += ", ";
      }
      names += std::string(s.name);
    }
    *error = base::StrCat({"unknown op \"", raw_op, "\" (use: ", names, ")"});
    return std::nullopt;
  }

  PlannedAction action;
  action.op = spec->op;
  action.id = TrimmedString(dict.Find("id"));
  if (action.id.size() > kMaxIdLen) {
    action.id.resize(kMaxIdLen);
  }

  if (!ParseTargets(dict, *spec, &action.targets, error)) {
    return std::nullopt;
  }

  const base::Value::Dict* args = dict.FindDict("args");
  switch (action.op) {
    case PlannedOp::kType: {
      const base::Value* text = FindArg(dict, args, "text");
      if (text && text->is_string()) {
        action.text = text->GetString();
      }
      if (const base::Value* submit = FindArg(dict, args, "submit")) {
        action.submit = submit->is_bool() && submit->GetBool();
      }
      if (action.text.empty() && !action.submit) {
        *error = "type needs args.text (use a {Token} for stored values) or submit";
        return std::nullopt;
      }
      if (action.text.size() > kMaxTextLen) {
        *error = "type text too long";
        return std::nullopt;
      }
      break;
    }
    case PlannedOp::kKey: {
      action.key = TrimmedString(FindArg(dict, args, "key"));
      if (action.key.empty() || action.key.size() > kMaxKeyLen ||
          !std::all_of(action.key.begin(), action.key.end(), IsSafeKeyChar)) {
        *error = "key needs args.key such as enter, tab, escape, down";
        return std::nullopt;
      }
      break;
    }
    case PlannedOp::kScroll: {
      const std::string direction =
          base::ToLowerASCII(TrimmedString(FindArg(dict, args, "direction")));
      if (!direction.empty() && direction != "up" && direction != "down") {
        *error = "scroll direction must be up or down";
        return std::nullopt;
      }
      double amount = 0.8;
      if (const base::Value* a = FindArg(dict, args, "amount_viewport")) {
        if (a->is_double() || a->is_int()) {
          amount = a->GetDouble();
        }
      }
      amount = std::clamp(amount, 0.1, 3.0);
      action.scroll_dy =
          static_cast<float>(direction == "up" ? -amount : amount);
      break;
    }
    case PlannedOp::kWait: {
      double ms = 1000;
      if (const base::Value* v = FindArg(dict, args, "ms")) {
        if (v->is_double() || v->is_int()) {
          ms = v->GetDouble();
        }
      }
      action.wait_ms = static_cast<int>(std::clamp(ms, 0.0, 30000.0));
      break;
    }
    case PlannedOp::kClick:
    case PlannedOp::kDoubleClick:
    case PlannedOp::kRightClick:
    case PlannedOp::kHover:
    case PlannedOp::kDrag:
      break;
  }

  ParseExpect(dict, &action.expect);
  // Grading costs one JS round trip. Pay it only when the manager gave us
  // something to grade against, unless it asked for a strategy explicitly.
  const std::string strategy =
      base::ToLowerASCII(TrimmedString(dict.Find("strategy")));
  if (strategy == "graded") {
    action.strategy = ClickStrategy::kGraded;
  } else if (strategy == "raw") {
    action.strategy = ClickStrategy::kRaw;
  } else if (strategy.empty()) {
    action.strategy = action.expect.empty() ? ClickStrategy::kRaw
                                            : ClickStrategy::kGraded;
  } else {
    *error = "strategy must be raw or graded";
    return std::nullopt;
  }
  return action;
}

bool ParsePlannedBatch(const base::Value::List& list,
                       std::vector<PlannedAction>* out,
                       std::string* error) {
  std::string scratch;
  if (!error) {
    error = &scratch;
  }
  if (list.empty()) {
    *error = "actions is empty";
    return false;
  }
  if (list.size() > kMaxPlannedBatchSize) {
    *error = base::StrCat({"too many actions (max ",
                           base::NumberToString(kMaxPlannedBatchSize), ")"});
    return false;
  }
  std::vector<PlannedAction> parsed;
  parsed.reserve(list.size());
  for (size_t i = 0; i < list.size(); ++i) {
    const base::Value::Dict* dict = list[i].GetIfDict();
    std::string item_error;
    std::optional<PlannedAction> action;
    if (dict) {
      action = ParsePlannedAction(*dict, &item_error);
    } else {
      item_error = "not an object";
    }
    if (!action) {
      *error = base::StrCat(
          {"actions[", base::NumberToString(i), "]: ", item_error});
      return false;
    }
    if (action->id.empty()) {
      action->id = base::StrCat({"a", base::NumberToString(i)});
    }
    for (const PlannedAction& prior : parsed) {
      if (prior.id == action->id) {
        *error = base::StrCat({"actions[", base::NumberToString(i),
                               "]: duplicate id ", action->id});
        return false;
      }
    }
    parsed.push_back(std::move(*action));
  }
  *out = std::move(parsed);
  return true;
}

ClickGrade GradeHit(const ExpectedHit& expect, std::string_view hit_label) {
  if (expect.empty()) {
    return ClickGrade::kUnknown;
  }
  const std::string label = base::ToLowerASCII(hit_label);
  // Empty space, the page background and "no element" never satisfy an
  // expectation. A click here is the classic miss.
  if (label.empty() || label == "(no element)" || label == "document" ||
      label == "html" || label == "body" ||
      base::StartsWith(label, "html ") || base::StartsWith(label, "body ")) {
    return ClickGrade::kC;
  }

  int total = 0;
  int matched = 0;
  if (!expect.tag.empty()) {
    ++total;
    const size_t space = label.find(' ');
    const std::string_view first =
        space == std::string::npos ? std::string_view(label)
                                   : std::string_view(label).substr(0, space);
    if (first == expect.tag || LabelHasToken(label, expect.tag)) {
      ++matched;
    }
  }
  if (!expect.role_any.empty()) {
    ++total;
    for (const std::string& role : expect.role_any) {
      if (label.find("role=" + role) != std::string::npos ||
          ImplicitRoleMatches(label, role)) {
        ++matched;
        break;
      }
    }
  }
  if (!expect.name_contains.empty()) {
    ++total;
    if (label.find(expect.name_contains) != std::string::npos) {
      ++matched;
    }
  }
  if (total == 0) {
    return ClickGrade::kUnknown;
  }
  if (matched == total) {
    return ClickGrade::kA;
  }
  return matched > 0 ? ClickGrade::kB : ClickGrade::kC;
}

std::string_view ClickGradeName(ClickGrade grade) {
  switch (grade) {
    case ClickGrade::kA:
      return "A";
    case ClickGrade::kB:
      return "B";
    case ClickGrade::kC:
      return "C";
    case ClickGrade::kUnknown:
      break;
  }
  return "unknown";
}

}  // namespace simulachrome

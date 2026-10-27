// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/glic/agent/unified_agent_types.h"

#include "base/json/json_writer.h"

namespace glic {

namespace {

std::string WriteJson(const base::Value& value) {
  std::string out;
  base::JSONWriter::Write(value, &out);
  return out;
}

std::string WriteJson(const base::Value::List& value) {
  return WriteJson(base::Value(value.Clone()));
}

std::string WriteJson(const base::Value::Dict& value) {
  return WriteJson(base::Value(value.Clone()));
}

mojom::ElementRefSummaryPtr BuildMojomElementRefSummary(
    const UnifiedAgentElementRefRecord& record) {
  auto out = mojom::ElementRefSummary::New();
  out->element_ref = record.element_ref;
  out->role = record.role;
  out->label = record.label;
  out->short_text = record.short_text;
  out->candidate_kind = record.candidate_kind;
  out->frame_ref = record.frame_ref;
  out->score = record.score;
  return out;
}

mojom::UnifiedToolCallPtr BuildMojomToolCall(const UnifiedToolCallRecord& call) {
  auto out = mojom::UnifiedToolCall::New();
  out->tool = call.tool;
  out->payload_json = call.payload_json;
  return out;
}

mojom::UnifiedToolResultPtr BuildMojomToolResult(
    const UnifiedToolResultRecord& result) {
  auto out = mojom::UnifiedToolResult::New();
  out->ok = result.ok;
  out->tool = result.tool;
  out->failure_stage = result.failure_stage;
  out->error_class = result.error_class;
  out->retryable = result.retryable;
  out->duration_ms = result.duration_ms;
  out->started_at = result.started_at;
  out->finished_at = result.finished_at;
  out->summary = result.summary;
  out->artifacts_json = result.artifacts_json;
  out->verification_summary = result.verification_summary;
  return out;
}

}  // namespace

UnifiedAgentElementRefRecord::UnifiedAgentElementRefRecord() = default;
UnifiedAgentElementRefRecord::UnifiedAgentElementRefRecord(
    UnifiedAgentElementRefRecord&&) noexcept = default;
UnifiedAgentElementRefRecord& UnifiedAgentElementRefRecord::operator=(
    UnifiedAgentElementRefRecord&&) noexcept = default;
UnifiedAgentElementRefRecord::~UnifiedAgentElementRefRecord() = default;

UnifiedAgentContextSnapshotRecord::UnifiedAgentContextSnapshotRecord() =
    default;
UnifiedAgentContextSnapshotRecord::UnifiedAgentContextSnapshotRecord(
    UnifiedAgentContextSnapshotRecord&&) noexcept = default;
UnifiedAgentContextSnapshotRecord&
UnifiedAgentContextSnapshotRecord::operator=(
    UnifiedAgentContextSnapshotRecord&&) noexcept = default;
UnifiedAgentContextSnapshotRecord::~UnifiedAgentContextSnapshotRecord() =
    default;

UnifiedToolResultRecord::UnifiedToolResultRecord() = default;
UnifiedToolResultRecord::UnifiedToolResultRecord(
    UnifiedToolResultRecord&&) noexcept = default;
UnifiedToolResultRecord& UnifiedToolResultRecord::operator=(
    UnifiedToolResultRecord&&) noexcept = default;
UnifiedToolResultRecord::~UnifiedToolResultRecord() = default;

UnifiedAgentRunRecord::UnifiedAgentRunRecord() = default;
UnifiedAgentRunRecord::UnifiedAgentRunRecord(
    UnifiedAgentRunRecord&&) noexcept = default;
UnifiedAgentRunRecord& UnifiedAgentRunRecord::operator=(
    UnifiedAgentRunRecord&&) noexcept = default;
UnifiedAgentRunRecord::~UnifiedAgentRunRecord() = default;

std::string AgentProfileToString(mojom::AgentProfile profile) {
  switch (profile) {
    case mojom::AgentProfile::kBrowserOperator:
      return "browser_operator";
    case mojom::AgentProfile::kManager:
      return "manager";
    case mojom::AgentProfile::kScriptBuilder:
      return "script_builder";
    case mojom::AgentProfile::kScriptDebugger:
      return "script_debugger";
    case mojom::AgentProfile::kAutoHeal:
      return "auto_heal";
  }
}

std::string AgentContextPolicyToString(mojom::AgentContextPolicy policy) {
  switch (policy) {
    case mojom::AgentContextPolicy::kAuto:
      return "auto";
    case mojom::AgentContextPolicy::kRefreshIfStale:
      return "refresh_if_stale";
    case mojom::AgentContextPolicy::kForceRefresh:
      return "force_refresh";
  }
}

std::string AgentRunStateToString(mojom::AgentRunState state) {
  switch (state) {
    case mojom::AgentRunState::kIdle:
      return "idle";
    case mojom::AgentRunState::kRunning:
      return "running";
    case mojom::AgentRunState::kCompleted:
      return "completed";
    case mojom::AgentRunState::kFailed:
      return "failed";
    case mojom::AgentRunState::kCanceled:
      return "canceled";
  }
}

std::string AgentFailureStageToString(mojom::AgentFailureStage stage) {
  switch (stage) {
    case mojom::AgentFailureStage::kNone:
      return "none";
    case mojom::AgentFailureStage::kPlanning:
      return "planning";
    case mojom::AgentFailureStage::kGrounding:
      return "grounding";
    case mojom::AgentFailureStage::kDispatch:
      return "dispatch";
    case mojom::AgentFailureStage::kExecution:
      return "execution";
    case mojom::AgentFailureStage::kVerification:
      return "verification";
    case mojom::AgentFailureStage::kScriptIo:
      return "script_io";
    case mojom::AgentFailureStage::kSettings:
      return "settings";
    case mojom::AgentFailureStage::kHealing:
      return "healing";
    case mojom::AgentFailureStage::kFallback:
      return "fallback";
  }
}

mojom::ContextSnapshotPtr BuildMojomContextSnapshot(
    const UnifiedAgentContextSnapshotRecord& snapshot) {
  auto out = mojom::ContextSnapshot::New();
  out->snapshot_id = snapshot.snapshot_id;
  out->url = snapshot.url;
  out->title = snapshot.title;
  out->profile = snapshot.profile;
  out->captured_at = snapshot.captured_at;
  for (const auto& element : snapshot.elements) {
    out->elements.push_back(BuildMojomElementRefSummary(element));
  }
  out->scripts_summary_json = snapshot.scripts_summary_json;
  out->recent_failures_json = snapshot.recent_failures_json;
  out->settings_summary_json = snapshot.settings_summary_json;
  return out;
}

mojom::ElementRefBundlePtr BuildMojomElementRefBundle(
    const UnifiedAgentElementRefRecord& record) {
  auto out = mojom::ElementRefBundle::New();
  out->element_ref = record.element_ref;
  out->locator_bundle_json = WriteJson(record.locator_bundle);
  out->page_map_metadata_json = WriteJson(record.page_map_metadata);
  out->source = record.source;
  out->staleness_epoch = record.staleness_epoch;
  return out;
}

mojom::AgentRunTraceSummaryPtr BuildMojomRunTraceSummary(
    const UnifiedAgentRunRecord& run) {
  auto out = mojom::AgentRunTraceSummary::New();
  out->run_id = run.run_id;
  out->profile = run.profile;
  out->goal = run.goal;
  out->surface_origin = run.surface_origin;
  out->run_state = run.run_state;
  out->failure_stage = run.failure_stage;
  out->error_class = run.error_class;
  out->retry_count = run.retry_count;
  out->context_refresh_count = run.context_refresh_count;
  out->used_actor_fallback = run.used_actor_fallback;
  out->used_executor = run.used_executor;
  out->final_outcome = run.final_outcome;
  out->started_at = run.started_at;
  out->updated_at = run.updated_at;
  return out;
}

mojom::AgentRunTracePtr BuildMojomRunTrace(const UnifiedAgentRunRecord& run) {
  auto out = mojom::AgentRunTrace::New();
  out->summary = BuildMojomRunTraceSummary(run);
  for (const auto& snapshot : run.context_snapshots) {
    out->context_snapshot_ids.push_back(snapshot.snapshot_id);
    out->context_snapshots.push_back(BuildMojomContextSnapshot(snapshot));
  }
  for (const auto& call : run.issued_commands) {
    out->issued_commands.push_back(BuildMojomToolCall(call));
  }
  for (const auto& result : run.command_results) {
    out->command_results.push_back(BuildMojomToolResult(result));
  }
  return out;
}

}  // namespace glic

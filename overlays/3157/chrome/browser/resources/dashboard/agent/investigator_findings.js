// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Structured handoff from investigator (diagnose/reproduce on headless) to
 * dashboard_assistant (edit scripts + explain to the user).
 *
 * Keep this small for local models. Twin guidance lives in
 * agent_model_contract.cc profile prompts.
 */

import {slimInvestigateResult} from './investigate_result_slim.js';

/** @typedef {{
 *   script_id?: string,
 *   step_index?: number,
 *   diagnosis?: string,
 *   proposed_edit?: Object,
 *   evidence?: Object,
 *   user_summary?: string,
 *   status?: string,
 *   target_id?: string,
 * }} InvestigatorFindings
 */

/**
 * @param {Object|null|undefined} raw
 * @return {InvestigatorFindings|null}
 */
export function normalizeInvestigatorFindings(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) {
    return null;
  }
  // Accept nested {findings:{...}} or flat payload from handoff/done.
  const src = (raw.findings && typeof raw.findings === 'object' &&
               !Array.isArray(raw.findings)) ?
    raw.findings :
    raw;

  const scriptId = String(
      src.script_id || src.scriptId || raw.script_id || raw.scriptId || '')
      .trim();
  let stepIndex = null;
  if (src.step_index != null || src.stepIndex != null || src.step != null) {
    stepIndex = Number(src.step_index ?? src.stepIndex ?? src.step);
  } else if (raw.step_index != null || raw.stepIndex != null || raw.step != null) {
    stepIndex = Number(raw.step_index ?? raw.stepIndex ?? raw.step);
  }
  if (!Number.isFinite(stepIndex)) {
    stepIndex = undefined;
  }

  const diagnosis = String(
      src.diagnosis || src.why || src.summary || src.reason || '')
      .trim()
      .slice(0, 600);
  const userSummary = String(
      src.user_summary || src.userSummary || src.explain || diagnosis)
      .trim()
      .slice(0, 600);

  let proposedEdit = null;
  if (src.proposed_edit && typeof src.proposed_edit === 'object') {
    proposedEdit = src.proposed_edit;
  } else if (src.proposedEdit && typeof src.proposedEdit === 'object') {
    proposedEdit = src.proposedEdit;
  } else if (src.edit && typeof src.edit === 'object') {
    proposedEdit = src.edit;
  } else if (src.sel || src.id || src.selector) {
    proposedEdit = {};
    if (src.sel) proposedEdit.sel = String(src.sel);
    if (src.id) proposedEdit.id = String(src.id);
    if (src.selector) proposedEdit.selector = String(src.selector);
  }

  let evidence = null;
  if (src.evidence && typeof src.evidence === 'object') {
    evidence = slimInvestigateResult(src.evidence) || {...src.evidence};
  } else if (src.probe || src.url || src.target_id || src.targetId) {
    evidence = slimInvestigateResult(src) || {
      url: src.url,
      target_id: src.target_id || src.targetId,
      locator_probe: src.locator_probe || src.locatorProbe,
    };
  }

  const status = String(src.status || raw.status || 'handoff').trim() ||
      'handoff';
  const targetId = String(
      src.target_id || src.targetId || evidence?.target_id || '')
      .trim();

  if (!diagnosis && !proposedEdit && stepIndex == null && !scriptId) {
    return null;
  }

  const out = {
    script_id: scriptId || undefined,
    step_index: stepIndex,
    diagnosis: diagnosis || undefined,
    proposed_edit: proposedEdit || undefined,
    evidence: evidence || undefined,
    user_summary: userSummary || undefined,
    status,
    target_id: targetId || undefined,
  };
  for (const key of Object.keys(out)) {
    if (out[key] === undefined) {
      delete out[key];
    }
  }
  return out;
}

/**
 * Pull findings from a normalized agent command (handoff / done).
 * @param {Object|null} command
 * @return {InvestigatorFindings|null}
 */
export function findingsFromAgentCommand(command) {
  if (!command || typeof command !== 'object') {
    return null;
  }
  const name = String(command.command || '').toLowerCase();
  const payload =
      (command.payload && typeof command.payload === 'object') ?
        command.payload :
        {};
  if (name === 'dashboard_handoff_to_assistant' || name === 'handoff' ||
      name === 'pass_to_assistant') {
    return normalizeInvestigatorFindings(payload);
  }
  if (name === 'agent_finish') {
    const status = String(payload.status || '').toLowerCase();
    if (status === 'handoff' || status === 'diagnosed' ||
        payload.findings || payload.diagnosis) {
      return normalizeInvestigatorFindings(payload);
    }
  }
  return null;
}

/**
 * User-turn brief for the dashboard_assistant phase.
 * @param {InvestigatorFindings} findings
 * @param {Object} [extra]
 * @return {string}
 */
export function formatFindingsBriefForAssistant(findings, extra = {}) {
  const card = {
    ...(findings && typeof findings === 'object' ? findings : {}),
    ...(extra && typeof extra === 'object' ? extra : {}),
  };
  let json = '';
  try {
    json = JSON.stringify(card);
  } catch (_) {
    json = '{"diagnosis":"unavailable"}';
  }
  if (json.length > 4500) {
    json = json.slice(0, 4500);
  }
  return [
    'Assistant handoff. Apply a surgical script fix from the findings, explain',
    'briefly to the user, verify with resume_script or run_script if needed, then complete.',
    'Do not re-explore the page unless verify fails. Use update_step for the fix.',
    `Findings:${json}`,
  ].join('\n');
}

/**
 * System prompt for investigator diagnose phase (heal / support).
 * Uses the same core verbs as headed glic via buildSystemPromptForLoopProfile.
 * @param {function(string): string} buildSystemPromptForLoopProfile
 * @return {string}
 */
export function buildInvestigatorDiagnoseSystemPrompt(
    buildSystemPromptForLoopProfile) {
  const base = typeof buildSystemPromptForLoopProfile === 'function' ?
    buildSystemPromptForLoopProfile('investigator') :
    '';
  return [
    base,
    'Diagnose phase only. Reproduce with the same engine/locators as the script.',
    'When you know the fix (or are blocked), hand off. Do not edit scripts yourself.',
    'Include the diagnosis, the step, and the evidence you already have. Fewest moves.',
  ].filter(Boolean).join('\n');
}

/**
 * System prompt for assistant apply/explain phase.
 * @param {function(string): string} buildSystemPromptForLoopProfile
 * @return {string}
 */
export function buildAssistantApplySystemPrompt(buildSystemPromptForLoopProfile) {
  const base = typeof buildSystemPromptForLoopProfile === 'function' ?
    buildSystemPromptForLoopProfile('dashboard_assistant') :
    '';
  return [
    base,
    'You received investigator Findings. Apply the surgical edit, explain to the user,',
    'optionally verify by running the script, then finish.',
    'If verify fails with a new locator issue, finish as blocked.',
    'Do not open headed tabs for page work; script verify runs on headless when configured.',
  ].filter(Boolean).join('\n');
}

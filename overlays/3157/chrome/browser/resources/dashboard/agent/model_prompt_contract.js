// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Canonical model <-> Simulachrome agent contract (WebUI twin).
 *
 * Keep in sync with:
 *   chrome/browser/ui/webui/dashboard/agent_model_contract.h
 *   chrome/browser/ui/webui/dashboard/agent_model_contract.cc
 *
 * Guided decoding owns the JSON shape (action_output_schema.js). These
 * prompts are behavioral only. Model output is accepted only when it already
 * matches the canonical command and action names.
 */

import {ACTION_METADATA} from '../actions/types.js';
import {DASHBOARD_AGENT_ACTIONS, FLAT_AGENT_ACTIONS} from './action_output_schema.js';
import {ACTION_ALIASES} from './action_vocabulary.js';

export const AI_LOOP_PROFILES = Object.freeze({
  DASHBOARD_ASSISTANT: 'dashboard_assistant',
  GLIC_TASK: 'glic_task',
  INVESTIGATOR: 'investigator',
});

export function normalizeAiLoopProfile(profile) {
  const raw = String(profile || '').trim().toLowerCase();
  if (raw === AI_LOOP_PROFILES.GLIC_TASK) {
    return AI_LOOP_PROFILES.GLIC_TASK;
  }
  if (raw === AI_LOOP_PROFILES.INVESTIGATOR) {
    return AI_LOOP_PROFILES.INVESTIGATOR;
  }
  return AI_LOOP_PROFILES.DASHBOARD_ASSISTANT;
}

export function buildActionShapePromptBlock() {
  return '';
}

export function buildCoreCommandSchemaPromptBlock() {
  return '';
}

export function buildDashboardCommandSchemaPromptBlock() {
  return '';
}

export function buildScriptJsonReferencePromptBlock() {
  return '';
}

export function buildSystemPromptForLoopProfile(loopProfile) {
  const profile = normalizeAiLoopProfile(loopProfile);

  if (profile === AI_LOOP_PROFILES.GLIC_TASK) {
    return `Browser agent. A user goal is a compressed summary of outcomes, not a step list and not a particular site. The current page decides the path. A short goal is often many moves. Issue ONE action object per turn. A click is {"action":"click","target_id":1} or {"action":"click","coordinate":{"x":120,"y":80}}. A type is {"action":"type","target_id":2,"text":"{TokenName}"}. A navigate is {"action":"navigate","url":"https://example.com/"}. A scroll is {"action":"scroll","direction":"down","distance":600}. Also {"action":"key_press","key":"Enter"}, {"action":"wait","timeout":2}, {"action":"request_screenshot"}, or {"action":"complete","status":"solved","summary":"done"}. Each action includes only its own fields. Never pack a whole clause into one move. Complete only when the page shows the outcome is done, not when you have covered each clause of the prompt.
Context is a trimmed accessibility list (not full DOM). After a navigation, look again if the control the next action needs is missing from the current context. Never type a stored value unless its field is visible. Keep acting through the page in front of you without requesting more context unless blocked. Overlays already present stay valid until the URL changes. If a later look returns Context status: UNCHANGED, do not re-plan from scratch.
Stored values: type Dashboard variable and secret tokens from the goal and token list as {TokenName}. Never invent a literal for a stored value. Finish only when the page shows the outcome, or when you are blocked.`;
  }

  if (profile === AI_LOOP_PROFILES.INVESTIGATOR) {
    return `Investigator. Drive the native headless instance (C++ control channel / :9222). Never open headed tabs. Each turn is one action object such as {"action":"investigate","url":"https://example.com/"}, {"action":"click","locators":[{"type":"CSS_SELECTOR","value":"#login"}]}, {"action":"type","locators":[{"type":"CSS_SELECTOR","value":"#email"}],"text":"{TokenName}"}, {"action":"get_step","script_id":"...","step_index":3}, {"action":"handoff",...}, or {"action":"complete","status":"blocked","summary":"..."}. Each action includes only its own fields.
Prefer a saved script or KEY script when it covers the next segment. If the script fully solves the goal, finish. Otherwise keep going from the page in front of you, one action at a time.
Investigate navigates, waits, then returns inputs, buttons, tables, and per-locator probes. Never trust a blank page when an http(s) url was requested. Reuse the existing target. Screenshots stay off unless you need one.
When a locator fails, compare that locator's probe row to the suggested row selector and the input ids.
When the diagnosis is ready for a script edit or a user explanation, hand off. Do not rewrite the script yourself.`;
  }

  return `Dashboard assistant. Fix and build scripts with one action object per turn, such as {"action":"update_step","script_id":"...","step_index":3,"locators":[{"type":"CSS_SELECTOR","value":"#login"}]}, {"action":"resume_script","script_id":"...","step_index":3}, or {"action":"complete","status":"solved","summary":"done"}. Each action includes only its own fields.
Prefer investigate, then a small edit, then run. Plain text is fine when no tool is needed.
If the user message includes Findings from an investigator handoff, apply that edit, explain briefly, verify, then finish. Do not re-explore unless verify fails. Do not call heal while a heal is already running.`;
}

function isCanonicalAction(name) {
  if (!Object.prototype.hasOwnProperty.call(ACTION_METADATA, name)) {
    return false;
  }
  const alias = ACTION_ALIASES.get(name);
  return alias == null || alias === name;
}

function locatorsAreCanonical(params) {
  if (!params || typeof params !== 'object') {
    return false;
  }
  const lists = ['locators', 'fromLocators', 'toLocators', 'rowLocators'];
  for (const key of lists) {
    if (!Object.prototype.hasOwnProperty.call(params, key) || params[key] == null) {
      continue;
    }
    if (!Array.isArray(params[key])) {
      return false;
    }
    for (const entry of params[key]) {
      if (!entry || typeof entry !== 'object') {
        return false;
      }
      if (typeof entry.type !== 'string' || !entry.type.trim()) {
        return false;
      }
      if (entry.value == null || String(entry.value).trim() === '') {
        return false;
      }
    }
  }
  return true;
}

const SHORTHAND_PARAM_KEYS = [
  'sel', 'selector', 'css', 'cssSelector', 'css_selector', 'xpath', 'el',
];

function paramsUseShorthand(actionName, params) {
  if (!params || typeof params !== 'object') {
    return false;
  }
  if (SHORTHAND_PARAM_KEYS.some((key) => params[key] != null && params[key] !== '')) {
    return true;
  }
  const meta = ACTION_METADATA[actionName];
  const declaresId = !!(meta && meta.params && meta.params.id);
  return !declaresId && params.id != null && params.id !== '';
}

/** Accept a canonical {action, params}. Reject aliases and shorthand. */
export function normalizeModelActionPayload(actionOrCommand, payload) {
  const actionName = String(actionOrCommand || '').trim();
  const params =
      (payload && typeof payload === 'object' && !Array.isArray(payload)) ?
        payload :
        {};
  if (!isCanonicalAction(actionName) || paramsUseShorthand(actionName, params) ||
      !locatorsAreCanonical(params)) {
    return {action: '', params: {}};
  }
  return {action: actionName, params};
}

/** Accept a canonical script step. Do not salvage Playwright or alias forms. */
export function normalizeScriptAction(step) {
  if (!step || typeof step !== 'object' || Array.isArray(step)) {
    return null;
  }
  const actionName = typeof step.action === 'string' ? step.action.trim() : '';
  if (!actionName || typeof step.type === 'string') {
    return null;
  }
  const params = (step.params && typeof step.params === 'object' &&
                  !Array.isArray(step.params)) ? step.params : null;
  if (!params) {
    return null;
  }
  const normalized = normalizeModelActionPayload(actionName, params);
  if (!normalized.action) {
    return null;
  }
  return normalized;
}

export function normalizeScriptActionsList(actions) {
  if (!Array.isArray(actions)) {
    return [];
  }
  return actions.map((step) => normalizeScriptAction(step)).filter(Boolean);
}

export function sanitizeAgentActionParams(actionName, params) {
  if (!params || typeof params !== 'object') {
    return params || {};
  }
  const out = {...params};
  if (Array.isArray(out.locators)) {
    out.locators = out.locators.filter((loc) => {
      if (!loc || typeof loc !== 'object') {
        return false;
      }
      return String(loc.type || '').toUpperCase() !== 'VISUAL_COORDS' &&
          String(loc.value || '').trim().length > 0;
    });
    if (!out.locators.length) {
      delete out.locators;
    }
  }
  return out;
}

function acceptActionDict(action) {
  const normalized = normalizeScriptAction(action);
  if (!normalized) {
    return null;
  }
  normalized.params = sanitizeAgentActionParams(normalized.action, normalized.params);
  return normalized;
}

const ACTION_KEYS = Object.freeze({
  click: ['action', 'target_id', 'coordinate', 'locators'],
  type: ['action', 'target_id', 'locators', 'text'],
  navigate: ['action', 'url'],
  scroll: ['action', 'direction', 'distance', 'scope', 'target_id'],
  key_press: ['action', 'key'],
  wait: ['action', 'timeout'],
  request_screenshot: ['action'],
  complete: ['action', 'status', 'summary'],
  investigate: ['action', 'url'],
  get_script: ['action', 'script_id'],
  get_step: ['action', 'script_id', 'step_index'],
  update_step: ['action', 'script_id', 'step_index', 'locators'],
  run_script: ['action', 'script_id'],
  resume_script: ['action', 'script_id', 'step_index'],
  handoff: [
    'action', 'script_id', 'step_index', 'diagnosis', 'user_summary',
    'confidence', 'evidence', 'proposed_locators',
  ],
});

function keysBelongToAction(turn, action) {
  const allowed = ACTION_KEYS[action];
  if (!allowed) {
    return false;
  }
  return Object.keys(turn).every((key) => allowed.includes(key));
}

function pixel(value) {
  return Number.isInteger(value) && value >= 0 && value <= 4000;
}

function stepIndex(value) {
  return Number.isInteger(value) && value >= 0;
}

function nonEmptyString(value) {
  return typeof value === 'string' && value.trim().length > 0;
}

function validLocators(value) {
  return Array.isArray(value) && value.length > 0 &&
      value.every((loc) => loc && typeof loc === 'object' &&
                   nonEmptyString(loc.type) && nonEmptyString(loc.value));
}

const REJECTED = Object.freeze({command: ''});

function dashboardActionToCommand(action, turn) {
  switch (action) {
    case 'investigate':
      return {
        command: 'dashboard_investigate_page',
        payload: nonEmptyString(turn.url) ? {url: turn.url} : {},
      };
    case 'get_script':
      if (!nonEmptyString(turn.script_id)) return REJECTED;
      return {command: 'dashboard_get_script_json', payload: {script_id: turn.script_id}};
    case 'get_step':
      if (!nonEmptyString(turn.script_id) || !stepIndex(turn.step_index)) return REJECTED;
      return {
        command: 'dashboard_get_script_step',
        payload: {script_id: turn.script_id, step_index: turn.step_index},
      };
    case 'update_step':
      if (!nonEmptyString(turn.script_id) || !stepIndex(turn.step_index) ||
          !validLocators(turn.locators)) {
        return REJECTED;
      }
      return {
        command: 'dashboard_update_script_step',
        payload: {
          script_id: turn.script_id,
          step_index: turn.step_index,
          locators: turn.locators,
        },
      };
    case 'run_script':
      if (!nonEmptyString(turn.script_id)) return REJECTED;
      return {command: 'dashboard_run_script', payload: {script_id: turn.script_id}};
    case 'resume_script':
      if (!nonEmptyString(turn.script_id) || !stepIndex(turn.step_index)) return REJECTED;
      return {
        command: 'dashboard_resume_script_from_step',
        payload: {script_id: turn.script_id, step_index: turn.step_index},
      };
    case 'handoff': {
      if (!nonEmptyString(turn.script_id) || !stepIndex(turn.step_index) ||
          !nonEmptyString(turn.diagnosis)) {
        return REJECTED;
      }
      const payload = {
        status: 'handoff',
        script_id: turn.script_id,
        step_index: turn.step_index,
        diagnosis: turn.diagnosis,
        user_summary: typeof turn.user_summary === 'string' ? turn.user_summary : '',
        confidence: turn.confidence,
        evidence: {note: typeof turn.evidence === 'string' ? turn.evidence : ''},
      };
      if (turn.proposed_locators != null) {
        if (!validLocators(turn.proposed_locators)) return REJECTED;
        payload.proposed_edit = {locators: turn.proposed_locators};
      }
      return {command: 'dashboard_handoff_to_assistant', payload};
    }
    default:
      return REJECTED;
  }
}

function flatActionToCommand(turn) {
  const action = String(turn.action || '').trim();
  if (!keysBelongToAction(turn, action)) {
    return REJECTED;
  }
  if (DASHBOARD_AGENT_ACTIONS.includes(action) && !FLAT_AGENT_ACTIONS.includes(action)) {
    return dashboardActionToCommand(action, turn);
  }
  if (!FLAT_AGENT_ACTIONS.includes(action)) {
    return REJECTED;
  }
  if (action === 'request_screenshot') {
    return {command: 'request_screenshot', payload: {}};
  }
  if (action === 'complete') {
    if (turn.status !== 'solved' && turn.status !== 'blocked') {
      return REJECTED;
    }
    if (typeof turn.summary !== 'string') {
      return REJECTED;
    }
    return {
      command: 'agent_finish',
      payload: {status: turn.status, summary: turn.summary},
    };
  }
  const params = {};
  if (Number.isInteger(turn.target_id) && turn.target_id >= 0) {
    params.target_id = turn.target_id;
  } else if (turn.target_id != null) {
    return REJECTED;
  }
  if (turn.locators != null) {
    if (!validLocators(turn.locators)) {
      return REJECTED;
    }
    params.locators = turn.locators;
  }
  if (typeof turn.text === 'string') {
    params.text = turn.text;
  }
  if (typeof turn.url === 'string' && turn.url) {
    params.url = turn.url;
  }
  if (turn.coordinate != null) {
    const point = turn.coordinate;
    if (!point || typeof point !== 'object' || !pixel(point.x) || !pixel(point.y)) {
      return REJECTED;
    }
    params.coordinates = {x: point.x, y: point.y};
    params.humanOffset = false;
  }
  if (turn.distance != null) {
    if (!pixel(turn.distance)) {
      return REJECTED;
    }
    params.distance = turn.distance;
  }
  if (typeof turn.direction === 'string' && turn.direction) {
    if (turn.direction !== 'up' && turn.direction !== 'down') {
      return REJECTED;
    }
    params.direction = turn.direction;
  }
  if (turn.scope === 'page' || turn.scope === 'iframe') {
    params.scope = turn.scope;
  } else if (turn.scope != null) {
    return REJECTED;
  }
  if (typeof turn.key === 'string' && turn.key) {
    params.key = turn.key;
  }
  if (turn.timeout != null) {
    if (!Number.isInteger(turn.timeout) || turn.timeout < 0) {
      return REJECTED;
    }
    params.time = turn.timeout;
    params.waitType = 'time';
  }
  const hasTarget = params.target_id != null || params.locators != null;
  if (action === 'type' && (!hasTarget || typeof params.text !== 'string')) {
    return REJECTED;
  }
  if (action === 'navigate' && !params.url) {
    return REJECTED;
  }
  if (action === 'scroll' && (params.distance == null || !params.direction)) {
    return REJECTED;
  }
  if (action === 'key_press' && !params.key) {
    return REJECTED;
  }
  if (action === 'wait' && params.time == null) {
    return REJECTED;
  }
  const accepted = acceptActionDict({action, params});
  if (!accepted) {
    return REJECTED;
  }
  return {command: 'call_action', payload: accepted};
}

const INTERNAL_COMMANDS = new Set([
  'call_action', 'run_timeline', 'refresh_pagemap', 'request_more_context',
  'request_more_controls', 'request_primary_options', 'request_screenshot',
  'reload_page', 'inspect_element', 'save_task_script', 'agent_finish',
  'store_memento', 'read_memento', 'reset_agent',
  'dashboard_handoff_to_assistant', 'dashboard_investigate_page',
  'dashboard_list_scripts', 'dashboard_list_collections',
  'dashboard_get_script_json', 'dashboard_get_script_step',
  'dashboard_update_script_step', 'dashboard_delete_script_step',
  'dashboard_save_script_json', 'dashboard_run_script',
  'dashboard_select_script', 'dashboard_resume_script_from_step',
  'dashboard_set_view', 'dashboard_set_global_setting',
  'dashboard_configure_listener_match', 'dashboard_configure_listener_regex',
  'dashboard_record_agent_action', 'dashboard_queue_auto_heal',
  'dashboard_process_auto_heal_queue', 'dashboard_list_timeline_failures',
]);

/**
 * Accept an executor envelope built inside the dashboard. Model output never
 * reaches here; it goes through normalizeAgentCommandForExecution first.
 */
export function acceptInternalAgentCommand(command) {
  if (!command || typeof command !== 'object' || Array.isArray(command)) {
    return REJECTED;
  }
  const name = typeof command.command === 'string' ? command.command.trim() : '';
  if (!INTERNAL_COMMANDS.has(name) ||
      Object.prototype.hasOwnProperty.call(command, 'action')) {
    return REJECTED;
  }
  const out = {...command, command: name};
  if (name === 'call_action') {
    const accepted = acceptActionDict(command.payload);
    if (!accepted) {
      return REJECTED;
    }
    out.payload = accepted;
  } else if (name === 'run_timeline') {
    const timeline = command.payload && Array.isArray(command.payload.timeline) ?
      command.payload.timeline :
      null;
    if (!timeline) {
      return REJECTED;
    }
    const accepted = timeline.map((step) => acceptActionDict(step));
    if (accepted.some((step) => !step)) {
      return REJECTED;
    }
    out.payload = {...command.payload, timeline: accepted};
  }
  return out;
}

/** Accept one flat {"action":...} object and map it onto the executor. */
export function normalizeAgentCommandForExecution(command) {
  if (!command || typeof command !== 'object' || Array.isArray(command)) {
    return {command: ''};
  }
  if (Object.prototype.hasOwnProperty.call(command, 'command') ||
      Object.prototype.hasOwnProperty.call(command, 'payload') ||
      command.do) {
    return {command: ''};
  }
  if (typeof command.action === 'string' && command.action.trim()) {
    return flatActionToCommand(command);
  }
  return {command: ''};
}

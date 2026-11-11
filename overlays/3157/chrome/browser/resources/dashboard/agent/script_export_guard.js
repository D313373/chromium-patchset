import {
  flattenActionsFromUrlContexts,
  getKeyDownNestedList,
  getLoopStepsList,
  getNestedActionList,
} from './dashboard_script_state.js';

const TOKEN_ONLY_RE = /^\{[^{}]+\}$/;
const EMAIL_RE = /[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}/i;
const PHONE_RE =
    /(?:\+?1[-.\s]?)?(?:\(?\d{3}\)?[-.\s]?)\d{3}[-.\s]?\d{4}\b/;
const SSN_RE = /\b\d{3}-\d{2}-\d{4}\b/;
const API_KEY_RE =
    /\b(?:sk-[A-Za-z0-9]{10,}|ghp_[A-Za-z0-9]{20,}|AIza[0-9A-Za-z\-_]{20,}|xox[baprs]-[A-Za-z0-9-]{10,})\b/;
const HIGH_ENTROPY_RE = /\b[A-Za-z0-9+/=_-]{24,}\b/;

function normalizeActionKind(action) {
  const kind = String(action?.action || action?.type || '').trim().toLowerCase();
  if (kind === 'cdp_type') {
    return 'type';
  }
  return kind;
}

function readActionParams(action) {
  if (action?.params && typeof action.params === 'object') {
    return action.params;
  }
  return action || {};
}

function isTokenReference(text) {
  return TOKEN_ONLY_RE.test(String(text || '').trim());
}

function locatorsSuggestPassword(locators) {
  for (const locator of locators || []) {
    if (!locator || typeof locator !== 'object') {
      continue;
    }
    const type = String(locator.type || '').toUpperCase();
    const value = String(locator.value || '').toLowerCase();
    if (!value) {
      continue;
    }
    if (value.includes('password') || value.includes('passwd') ||
        value.includes('credential') || value.includes('api_key') ||
        value.includes('apikey') || value.includes('secret') ||
        value.includes('token')) {
      return true;
    }
    if ((type === 'ID' || type === 'NAME') &&
        (value.includes('pass') || value.includes('pwd'))) {
      return true;
    }
  }
  return false;
}

function locatorHintName(locators) {
  for (const locator of locators || []) {
    const value = String(locator?.value || '').trim();
    if (!value) {
      continue;
    }
    const cleaned = value
        .replace(/[^A-Za-z0-9]+/g, ' ')
        .trim()
        .split(/\s+/)
        .filter(Boolean)
        .map((part) => part.charAt(0).toUpperCase() + part.slice(1).toLowerCase())
        .join('');
    if (cleaned) {
      return cleaned;
    }
  }
  return '';
}

function toSecretKeyBase(raw) {
  const cleaned = String(raw || '')
      .replace(/[^A-Za-z0-9]+/g, ' ')
      .trim()
      .split(/\s+/)
      .filter(Boolean)
      .map((part) => part.charAt(0).toUpperCase() + part.slice(1).toLowerCase())
      .join('');
  return cleaned || 'Secret';
}

function redactPreview(text) {
  const value = String(text || '');
  if (!value) {
    return '';
  }
  if (value.length <= 4) {
    return '****';
  }
  return `${value.slice(0, 2)}…${value.slice(-1)}`;
}

function classifySensitiveTypeText(text, locators) {
  const value = String(text || '');
  if (!value.trim() || isTokenReference(value)) {
    return null;
  }

  const reasons = [];
  if (EMAIL_RE.test(value)) {
    reasons.push('email');
  }
  if (PHONE_RE.test(value)) {
    reasons.push('phone_number');
  }
  if (SSN_RE.test(value)) {
    reasons.push('ssn');
  }
  if (API_KEY_RE.test(value)) {
    reasons.push('api_key');
  }
  if (HIGH_ENTROPY_RE.test(value) && !EMAIL_RE.test(value)) {
    reasons.push('high_entropy_secret');
  }
  if (locatorsSuggestPassword(locators)) {
    reasons.push('password_field_literal');
  }

  if (!reasons.length) {
    return null;
  }

  return {
    reasons,
    textPreview: redactPreview(value),
    suggestion:
        'Replace this literal with a secret token like {MyPassword} and store ' +
        'the value in Data > Secrets.',
  };
}

export function suggestSecretName(finding, usedNames = new Set()) {
  const reasons = finding?.reasons || [];
  const locators = finding?.locators || [];
  let base = 'Secret';

  if (reasons.includes('password_field_literal')) {
    base = locatorHintName(locators) || 'Password';
    if (!base.toLowerCase().includes('password') &&
        !base.toLowerCase().includes('pass')) {
      base = `${base}Password`;
    }
  } else if (reasons.includes('email')) {
    base = locatorHintName(locators) ? `${locatorHintName(locators)}Email` : 'Email';
  } else if (reasons.includes('phone_number')) {
    base = 'PhoneNumber';
  } else if (reasons.includes('ssn')) {
    base = 'Ssn';
  } else if (reasons.includes('api_key') || reasons.includes('high_entropy_secret')) {
    base = 'ApiKey';
  }

  base = toSecretKeyBase(base);
  let candidate = base;
  let suffix = 2;
  while (usedNames.has(candidate)) {
    candidate = `${base}${suffix++}`;
  }
  usedNames.add(candidate);
  return candidate;
}

function getScriptActionList(script) {
  if (!script || typeof script !== 'object') {
    return [];
  }
  if (Array.isArray(script.actions) && script.actions.length) {
    return script.actions;
  }
  return flattenActionsFromUrlContexts(script.url_contexts || []);
}

function getNestedListForBranch(action, branch) {
  if (branch === 'steps') {
    return getLoopStepsList(action);
  }
  if (branch === 'nested') {
    return getKeyDownNestedList(action);
  }
  return getNestedActionList(action, branch);
}

function ensureScriptUsesActionsArray(script) {
  if (!script || typeof script !== 'object') {
    return;
  }
  if (Array.isArray(script.actions) && script.actions.length) {
    return;
  }
  if (Array.isArray(script.url_contexts) && script.url_contexts.length) {
    script.actions = flattenActionsFromUrlContexts(script.url_contexts);
    delete script.url_contexts;
  }
}

export function getActionAtPath(script, actionPath) {
  if (!Array.isArray(actionPath) || !actionPath.length) {
    return null;
  }
  ensureScriptUsesActionsArray(script);
  const actions = getScriptActionList(script);
  let current = actions[actionPath[0]];
  for (let i = 1; i < actionPath.length; i += 2) {
    const branch = actionPath[i];
    const index = actionPath[i + 1];
    const nested = getNestedListForBranch(current, branch);
    if (!Array.isArray(nested) || !Number.isInteger(index)) {
      return null;
    }
    current = nested[index];
  }
  return current || null;
}

function setTypeTextOnAction(action, newText) {
  if (!action || typeof action !== 'object') {
    return;
  }
  if (action.params && typeof action.params === 'object') {
    action.params.text = newText;
    return;
  }
  action.text = newText;
}

export function applySecretReplacementsToScript(script, replacements) {
  const clone = JSON.parse(JSON.stringify(script || {}));
  ensureScriptUsesActionsArray(clone);
  for (const replacement of replacements || []) {
    const action = getActionAtPath(clone, replacement.actionPath);
    if (!action) {
      continue;
    }
    setTypeTextOnAction(action, `{${replacement.secretName}}`);
  }
  return clone;
}

export function buildSecretMigrationPlan(findings, existingSecretKeys = []) {
  const usedNames = new Set(
      (existingSecretKeys || []).map((key) => String(key || '').trim())
          .filter(Boolean));
  const byLiteral = new Map();
  const plan = [];

  for (const finding of findings || []) {
    const literal = String(finding.originalText || '');
    if (!literal) {
      continue;
    }
    let entry = byLiteral.get(literal);
    if (!entry) {
      entry = {
        secretName: suggestSecretName(finding, usedNames),
        literal,
        findings: [],
      };
      byLiteral.set(literal, entry);
      plan.push(entry);
    }
    entry.findings.push(finding);
  }

  return plan.map((entry) => ({
    secretName: entry.secretName,
    literal: entry.literal,
    token: `{${entry.secretName}}`,
    findings: entry.findings,
    actionPaths: entry.findings.map((finding) => finding.actionPath),
  }));
}

function visitNestedActions(action, visitFn) {
  const kind = normalizeActionKind(action);
  if (kind === 'if_condition') {
    for (const [index, child] of (getNestedActionList(action, 'then') || [])
             .entries()) {
      visitFn(child, 'then', index);
    }
    for (const [index, child] of (getNestedActionList(action, 'else') || [])
             .entries()) {
      visitFn(child, 'else', index);
    }
    return;
  }
  if (kind === 'loop') {
    for (const [index, child] of (getLoopStepsList(action) || []).entries()) {
      visitFn(child, 'steps', index);
    }
    return;
  }
  if (kind === 'key_down' || kind === 'key') {
    for (const [index, child] of (getKeyDownNestedList(action) || []).entries()) {
      visitFn(child, 'nested', index);
    }
    return;
  }
  if (kind === 'authenticate') {
    const methods = action.params && Array.isArray(action.params.methods) ?
      action.params.methods : [];
    for (const method of methods) {
      if (!method || method.custom !== true || !Array.isArray(method.actions)) {
        continue;
      }
      for (const [index, child] of method.actions.entries()) {
        visitFn(child, method.id, index);
      }
    }
  }
}

export function scanScriptForExportRisks(script) {
  const findings = [];
  const actions = getScriptActionList(script);

  const visit = (action, actionPath, pathLabel) => {
    if (!action || typeof action !== 'object') {
      return;
    }
    const kind = normalizeActionKind(action);
    const params = readActionParams(action);

    if (kind === 'type') {
      const text = String(params.text || action.text || '');
      const locators = params.locators || action.locators || [];
      const classified = classifySensitiveTypeText(text, locators);
      if (classified) {
        findings.push({
          action: 'type',
          actionPath,
          path: pathLabel,
          originalText: text,
          locators,
          ...classified,
        });
      }
    }

    visitNestedActions(action, (child, branch, index) => {
      visit(
          child,
          [...actionPath, branch, index],
          `${pathLabel} > ${normalizeActionKind(child) || 'action'}`);
    });
  };

  actions.forEach((action, index) => {
    const label = `step ${index + 1}: ${normalizeActionKind(action) || 'action'}`;
    visit(action, [index], label);
  });

  return {
    findings,
    hasRisks: findings.length > 0,
  };
}

export function extractTokenReferencesFromValue(value, out = new Set()) {
  if (value == null) {
    return out;
  }
  if (typeof value === 'string') {
    const matches = value.matchAll(/\{([^{}]+)\}/g);
    for (const match of matches) {
      const token = String(match[1] || '').trim();
      if (token) {
        out.add(token);
      }
    }
    return out;
  }
  if (Array.isArray(value)) {
    for (const item of value) {
      extractTokenReferencesFromValue(item, out);
    }
    return out;
  }
  if (typeof value === 'object') {
    for (const nested of Object.values(value)) {
      extractTokenReferencesFromValue(nested, out);
    }
  }
  return out;
}

export function extractTokenReferencesFromScript(script) {
  const clone = JSON.parse(JSON.stringify(script || {}));
  delete clone.__localUnsavedChanges;
  delete clone.__agent_next_insert_index;
  return Array.from(extractTokenReferencesFromValue(clone)).sort((a, b) =>
    a.localeCompare(b));
}

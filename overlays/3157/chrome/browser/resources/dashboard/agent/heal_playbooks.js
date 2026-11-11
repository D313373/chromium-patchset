// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Adaptive auto-heal playbooks.
 *
 * Healing exists because the script's own targeting no longer works, so the
 * detective agent must be able to proceed with locators broken, partial or
 * absent. This module turns whatever evidence the incident carries into a short,
 * ordered investigation plan for that agent:
 *
 *   classifyHealFailure()      what kind of failure is this?
 *   buildAdjacentStepContext() what do the neighboring steps tell us?
 *   buildEvidenceProfile()     how much do we actually know?
 *   buildHealPlaybook()        which checks, in which order, for this case?
 *
 * The key observation: a broken step is usually a bridge between two steps that
 * worked. Where the previous step left the browser, and where the next step
 * expects to be, constrain the answer even when the step's own locators are
 * gone. All functions are pure and never touch the DOM.
 */

export const HEAL_CATEGORIES = Object.freeze({
  CONTEXT_MISMATCH: 'context_mismatch',
  FRAME_LOST: 'frame_lost',
  AUTH_WALL: 'auth_wall',
  BLOCKED: 'blocked_or_challenge',
  NOT_INTERACTABLE: 'not_interactable',
  NAVIGATION_TIMEOUT: 'navigation_timeout',
  WAIT_TIMEOUT: 'wait_timeout',
  LOCATOR_NOT_FOUND: 'locator_not_found',
  INVALID_INPUT: 'invalid_input',
  TIMEOUT: 'timeout',
  UNKNOWN: 'unknown',
});

const TOKEN_ONLY_RE = /^\{[^{}]+\}$/;
const NAVIGATING_KINDS = new Set(['navigate', 'history', 'back', 'forward']);
const WAITING_KINDS = new Set(['wait', 'listen']);

function str(v) {
  return v === undefined || v === null ? '' : String(v);
}

function actionKind(action) {
  const raw = str(action && (action.action || action.type || action.kind))
                  .toLowerCase().trim();
  if (raw === 'cdp_click') return 'click';
  if (raw === 'cdp_type') return 'type';
  if (raw === 'cdp_key_down') return 'key_down';
  if (raw === 'cdp_drag') return 'drag';
  return raw || 'unknown';
}

function paramsOf(action) {
  return (action && typeof action.params === 'object' && action.params) || {};
}

/** origin + path only: query strings and fragments often carry session data. */
export function urlForPrompt(url) {
  const raw = str(url).trim();
  if (!raw) return '';
  try {
    const u = new URL(raw);
    return `${u.origin}${u.pathname}`;
  } catch (_) {
    return raw.split(/[?#]/)[0].slice(0, 200);
  }
}

/**
 * A step description that is safe to put in a prompt. Typed text is kept only
 * when it is already a {Token}; any literal is replaced by its length, because
 * older recordings can contain cleartext credentials.
 */
export function summarizeActionSafe(action) {
  if (!action || typeof action !== 'object') return null;
  const kind = actionKind(action);
  const p = paramsOf(action);
  const out = {kind};
  const locators = Array.isArray(p.locators) ? p.locators : [];
  if (locators.length) {
    out.locators = locators.slice(0, 6).map((l) => ({
      type: str(l && l.type),
      value: str(l && l.value).slice(0, 80),
    }));
  }
  if (p.coordinates && typeof p.coordinates === 'object') {
    out.coordinates = {x: p.coordinates.x, y: p.coordinates.y};
  }
  for (const key of ['ax_name', 'ax_role', 'role']) {
    if (str(p[key])) out[key] = str(p[key]).slice(0, 80);
  }
  const text = p.text !== undefined ? str(p.text) : '';
  if (text) {
    out.text = TOKEN_ONLY_RE.test(text.trim()) ? text.trim() : `<literal len=${text.length}>`;
  }
  if (str(p.key)) out.key = str(p.key).slice(0, 24);
  if (str(p.url)) out.url = urlForPrompt(p.url);
  if (p.clickCount !== undefined) out.clickCount = p.clickCount;
  const ctx = str(action.contextUrl || action.context_url);
  if (ctx) out.context_url = urlForPrompt(ctx);
  return out;
}

function stepIndexOf(entry) {
  const n = Number(entry && (entry.stepIndex ?? entry.step_index ?? -1));
  return Number.isFinite(n) ? n : -1;
}

/**
 * What the neighbors of the failed step say. Sources, in order of trust:
 * the run history (what actually happened) then the saved script (what was
 * intended). Either may be missing.
 */
export function buildAdjacentStepContext({runDetails, scriptActions, failedStep}) {
  const idx = Number(failedStep);
  const steps = Array.isArray(runDetails && runDetails.steps) ? runDetails.steps : [];
  const script = Array.isArray(scriptActions) ? scriptActions : [];
  const out = {prev: null, failed: null, next: null};
  if (!Number.isFinite(idx) || idx < 0) return out;

  const runStep = (i) => steps.find((s) => stepIndexOf(s) === i) || null;
  const describe = (i) => {
    const run = runStep(i);
    const scripted = script[i] || null;
    const action = (run && run.action) || scripted;
    if (!run && !scripted) return null;
    return {
      index: i,
      action: summarizeActionSafe(action),
      status: run ? str(run.status) : 'not_run',
      url_before: urlForPrompt(run && (run.url_before || run.urlBefore)),
      url_after: urlForPrompt(run && (run.url_after || run.urlAfter)),
      context_url: urlForPrompt(
          (action && (action.contextUrl || action.context_url)) ||
          (run && (run.expected_context_url || run.context_url))),
    };
  };
  out.prev = idx > 0 ? describe(idx - 1) : null;
  out.failed = describe(idx);
  out.next = describe(idx + 1);

  // Where the browser should be once the failed step has done its job.
  const nextAction = out.next && out.next.action;
  let expectedAfter = '';
  if (out.next) {
    expectedAfter = out.next.url_before || out.next.context_url ||
        (nextAction && nextAction.kind === 'navigate' ? nextAction.url : '') || '';
  }
  out.expected_url_after = expectedAfter;
  const before = (out.failed && (out.failed.context_url || out.failed.url_before)) ||
      (out.prev && out.prev.url_after) || '';
  out.url_before_failed = before;
  out.step_probably_navigates =
      !!(expectedAfter && before && urlForPrompt(expectedAfter) !== urlForPrompt(before));
  return out;
}

const PATTERNS = [
  [HEAL_CATEGORIES.FRAME_LOST,
   /web\s?contents (was )?(destroyed|lost)|no render frame|frame went away|target.*destroyed|tab (was )?closed|no frame/i],
  [HEAL_CATEGORIES.AUTH_WALL,
   /login wall|login method|every enabled login|authenticate|key script|session (expired|ended)|not logged in/i],
  [HEAL_CATEGORIES.BLOCKED,
   /captcha|challenge|access denied|forbidden|\b403\b|\b429\b|rate.?limit|verify (that )?you are human|unusual traffic|blocked/i],
  [HEAL_CATEGORIES.NOT_INTERACTABLE,
   /not (visible|clickable|interactable|enabled|focusable|editable)|obscured|covered|disabled|intercept|off-?screen|click suppressed|clicksuppressed|readiness/i],
  [HEAL_CATEGORIES.INVALID_INPUT,
   /invalid (action|regex|param|selector|text)|schema|parse error/i],
];

const LOCATOR_RE =
    /not found|no[_ ]match|could not (find|resolve)|unable to (find|resolve)|resolve|no element|0 elements|element_not_found|no_element|resolver/i;
const TIMEOUT_RE = /time[ds]?[ -]?out|timeout/i;

/**
 * Classifies a failure from whatever text and structure is available. The
 * executor's own taxonomy is a coarse substring match; this one also looks at
 * the action type and the URL context, because "timed out" means something
 * different for a navigate, a wait and a click.
 */
export function classifyHealFailure({error, failedAction, contextUrlMismatch}) {
  const message = str(error);
  const kind = actionKind(failedAction);
  const signals = [];
  const done = (category, confidence) =>
      ({category, confidence, signals, action_kind: kind});

  if (contextUrlMismatch || /context url mismatch/i.test(message)) {
    signals.push('context_url_mismatch');
    return done(HEAL_CATEGORIES.CONTEXT_MISMATCH, 0.9);
  }
  if (kind === 'authenticate') {
    signals.push('action=authenticate');
    return done(HEAL_CATEGORIES.AUTH_WALL, 0.8);
  }
  for (const [category, re] of PATTERNS) {
    const m = message.match(re);
    if (m) {
      signals.push(`message~${m[0].toLowerCase()}`);
      return done(category, 0.75);
    }
  }
  if (TIMEOUT_RE.test(message)) {
    signals.push('timeout');
    if (NAVIGATING_KINDS.has(kind)) {
      return done(HEAL_CATEGORIES.NAVIGATION_TIMEOUT, 0.8);
    }
    if (WAITING_KINDS.has(kind)) {
      return done(HEAL_CATEGORIES.WAIT_TIMEOUT, 0.8);
    }
    // A click/type that "timed out" almost always timed out waiting for its
    // element, so treat it as a locator problem unless the text says otherwise.
    if (LOCATOR_RE.test(message) || kind === 'click' || kind === 'type' ||
        kind === 'hover' || kind === 'drag') {
      signals.push('element_wait_timeout');
      return done(HEAL_CATEGORIES.LOCATOR_NOT_FOUND, 0.6);
    }
    return done(HEAL_CATEGORIES.TIMEOUT, 0.5);
  }
  if (LOCATOR_RE.test(message)) {
    signals.push('locator_resolution');
    return done(HEAL_CATEGORIES.LOCATOR_NOT_FOUND, 0.7);
  }
  if (!message) signals.push('no_error_text');
  return done(HEAL_CATEGORIES.UNKNOWN, 0.2);
}

/**
 * How much the step itself can still tell us. "sparse" is a normal and
 * expected input here, not an error: it just changes which checks come first.
 */
export function buildEvidenceProfile({failedAction, adjacent, probe}) {
  const kind = actionKind(failedAction);
  const p = paramsOf(failedAction);
  const locators = Array.isArray(p.locators) ? p.locators.filter(
      (l) => l && str(l.value)) : [];
  const hasCoordinates = !!(p.coordinates &&
      Number.isFinite(Number(p.coordinates.x)) &&
      Number.isFinite(Number(p.coordinates.y)));
  const hasName = !!(str(p.ax_name) || str(p.text) || str(p.name));
  const probeRows = probe && Array.isArray(probe.per_locator) ? probe.per_locator : [];
  const survivingLocators = probeRows.filter(
      (r) => r && Number(r.matches ?? r.count ?? 0) > 0).length;
  let level = 'sparse';
  if (locators.length && hasName) level = 'rich';
  else if (locators.length || (hasCoordinates && hasName)) level = 'partial';
  return {
    kind,
    evidence_level: level,
    locator_count: locators.length,
    locator_types: [...new Set(locators.map((l) => str(l.type)))].slice(0, 8),
    locators_probed: probeRows.length,
    locators_still_matching: survivingLocators,
    has_coordinates: hasCoordinates,
    has_name_hint: hasName,
    has_prev_step: !!(adjacent && adjacent.prev),
    has_next_step: !!(adjacent && adjacent.next),
    expected_url_after: (adjacent && adjacent.expected_url_after) || '',
    step_probably_navigates: !!(adjacent && adjacent.step_probably_navigates),
  };
}

const HANDOFF_COMMON = [
  'Hand off with {"action":"handoff",...} including: diagnosis, proposed_locators when a locator fix is known, "confidence" (high|medium|low), and "evidence" (what you actually observed). Say so plainly when you are inferring.',
];

function clickLikeChecks(profile, adj) {
  const steps = [];
  if (profile.step_probably_navigates && profile.expected_url_after) {
    steps.push(
        `Bridge hint: step ${adj.next ? adj.next.index : '?'} runs on ${profile.expected_url_after}, ` +
        `but this step starts on ${adj.url_before_failed || '(unknown)'}. This step most likely navigates between them. ` +
        'List anchors on the page whose href equals that URL; then the same path ignoring query/hash; then the same origin with the longest shared path prefix; then <form action> and [data-href]/[onclick] targets. ' +
        'Among matches, pick the one whose visible text or aria-label best resembles the old step.');
  }
  if (profile.locator_count) {
    steps.push(
        `Probe every saved locator (${profile.locator_types.join(', ') || 'unknown types'}) and record which match 0, 1 or many. ` +
        'The survivors show what drifted: id churn, hashed class names, changed text, moved into an iframe or shadow root.');
  }
  if (profile.has_name_hint) {
    steps.push('Search by role + accessible name, then by visible text (trimmed, case-insensitive, partial), inside the same landmark or dialog.');
  }
  if (profile.evidence_level === 'sparse' || (!profile.locator_count && profile.has_coordinates)) {
    steps.push(
        profile.has_coordinates ?
            'Only coordinates survive. Find the interactive elements nearest those coordinates (same viewport size if recorded, else 1280x800), list the 5 closest by role and text, and rank them against what the previous step left on screen.' :
            'No locators or coordinates survive. Work from the neighbors: what was on screen after the previous step, and what the next step needs to be true.');
  }
  steps.push('Rule out: the element is inside an iframe, shadow root or dialog; it appears only after scrolling or hover; a cookie banner or modal is covering it; the page has not finished rendering.');
  if (adj.next) {
    steps.push('Confirm a candidate by effect, not by looks: after clicking it, the URL should become the next step\'s page, or the next step\'s target should now exist.');
  }
  return steps;
}

function typeChecks(profile) {
  const steps = [];
  steps.push('Find editable fields by label, placeholder, name, autocomplete and input type. Prefer the field whose label matches the old locator text or ax_name.');
  steps.push('If the typed text is a {Token}, check the token has a value before blaming the locator: an unresolved token is a data problem, not a targeting problem.');
  steps.push('Check whether the field was already filled by the browser (:autofill). If so the step may be redundant, and typing over it can break the form.');
  steps.push('Check the field is inside the same form, frame or dialog as before, and that focus can actually land on it (not disabled, readonly or covered).');
  if (profile.locator_count) {
    steps.push(`Probe the saved locators (${profile.locator_types.join(', ')}) to see which still match.`);
  }
  return steps;
}

function authChecks(adj) {
  const steps = [];
  steps.push('Decide whether the session is logged in. LOGGED OUT looks like: a password field and an identity field and a Log in / Sign in button, and no Log out / avatar control. Check ALL frames and shadow roots, because Auth0/Okta-style logins are cross-origin iframes.');
  steps.push('Check autofill on the identity and password fields (:autofill). Autofilled fields mean the user\'s browser already supplies credentials; do not plan to type over them.');
  steps.push('If logged out: the fix is usually to run the key script / authenticate method before this step, then resume. If logged in already: this step is redundant or the page differs; compare with the previous step\'s resulting URL.');
  steps.push('Judge login success by all three together, after a short wait: the URL left the login path, a session indicator is present, and the login form is gone. A single snapshot without the form can be a slow page, not a success.');
  steps.push('A captcha or one-time-code prompt is not something to solve: report it as human_required in the handoff.');
  if (adj.prev && adj.prev.url_after) {
    steps.push(`The previous step left the browser on ${adj.prev.url_after}; check whether that URL is itself a login redirect.`);
  }
  return steps;
}

/**
 * The ordered investigation plan for one incident.
 */
export function buildHealPlaybook({classification, profile, adjacent}) {
  const adj = adjacent || {prev: null, failed: null, next: null};
  const cat = classification.category;
  const kind = profile.kind;
  let title = '';
  let steps = [];
  const avoid = [
    'Do not edit the script yourself; hand off findings.',
    'Do not click through destructive actions (submit, purchase, delete) to test a theory.',
  ];

  switch (cat) {
    case HEAL_CATEGORIES.CONTEXT_MISMATCH:
      title = 'The step ran on the wrong page. Fix navigation before touching locators.';
      steps = [
        `Compare where the previous step left the browser (${adj.prev && adj.prev.url_after || 'unknown'}) with the action's contextUrl (${adj.failed && adj.failed.context_url || 'unknown'}).`,
        'Decide which is stale: a site redirect or URL change (update contextUrl) or a missing/failed navigation (add or repair a navigate step).',
        'Load the expected page directly and confirm the original target exists there.',
      ];
      avoid.push('Do not change locators until the page is right.');
      break;
    case HEAL_CATEGORIES.FRAME_LOST:
      title = 'The tab or frame disappeared mid-step.';
      steps = [
        'Check whether the previous step opened a popup/new tab (target=_blank, window.open) or navigated the page away mid-action.',
        'If a new tab opened, the fix is to wait for it and act there. If the page navigated, the fix is to wait for the navigation to finish before this step.',
        'Reproduce once; if it does not recur, report it as a timing flake with a suggested wait.',
      ];
      break;
    case HEAL_CATEGORIES.AUTH_WALL:
      title = 'A login wall is in the way. Establish the session state first.';
      steps = authChecks(adj);
      break;
    case HEAL_CATEGORIES.BLOCKED:
      title = 'The site is blocking or challenging the run.';
      steps = [
        'Identify the block: captcha, 403/429, "unusual traffic", or a consent/age wall.',
        'Check whether it is specific to headless, to this URL, or to request rate.',
        'Report human_required for captchas. Suggest slower pacing or a different entry URL when it is rate-based. Do not loop retries.',
      ];
      avoid.push('Do not attempt to defeat a captcha or bot check.');
      break;
    case HEAL_CATEGORIES.NOT_INTERACTABLE:
      title = 'The target exists but could not be used.';
      steps = [
        'Identify what is on top of the target: cookie/consent banner, modal, sticky header, loading overlay.',
        'Check disabled/readonly state and whether it becomes enabled after some other field or a delay.',
        'Check it is scrolled into view and not mid-animation.',
        'Proposed fixes: a dismiss step before this one, a scroll-into-view, or a wait-for-enabled.',
      ];
      break;
    case HEAL_CATEGORIES.NAVIGATION_TIMEOUT:
      title = 'Navigation never completed.';
      steps = [
        'Load the URL and record the final URL, redirects, and status. Look for login, consent or geo redirects.',
        'If the page is usable but never "finishes" (SPA, long polling), the fix is to wait for a specific element instead of load.',
        'If it needs auth, switch to the auth checks.',
      ];
      break;
    case HEAL_CATEGORIES.WAIT_TIMEOUT:
      title = 'A wait or listen condition never became true.';
      steps = [
        'Work out what the wait was for (its locators, condition and text) and check whether that state ever occurs on the page.',
        'Distinguish never-happens (changed text/selector, different page) from happens-slowly (increase the timeout).',
        'Check for an intervening redirect, consent wall or login wall.',
        'Proposed fixes: longer timeout, looser condition, or a different element to wait on.',
      ];
      break;
    case HEAL_CATEGORIES.LOCATOR_NOT_FOUND:
      title = 'The target could not be found.';
      steps = (kind === 'type') ? typeChecks(profile) : clickLikeChecks(profile, adj);
      break;
    case HEAL_CATEGORIES.INVALID_INPUT:
      title = 'The action itself is malformed.';
      steps = [
        'Read the step with get_step and compare its params with the action schema.',
        'Fix the specific param (regex, selector syntax, missing field). This is usually not a page problem.',
      ];
      break;
    default:
      title = 'Cause unclear. Reproduce it, then narrow it down.';
      steps = [
        `Load the expected page (${profile.expected_url_after || adj.url_before_failed || 'run history url'}) and attempt the step's intent by hand.`,
        'Compare the page with what the previous step left behind. Check for login walls, overlays and redirects.',
        ...(kind === 'type' ? typeChecks(profile) : clickLikeChecks(profile, adj)),
      ];
  }

  const notes = [];
  if (profile.evidence_level === 'sparse') {
    notes.push('Evidence is sparse: the step\'s own targeting is gone. That is expected. Infer from the neighbors, keep your claims proportional, and say what you could not verify.');
  } else if (profile.evidence_level === 'partial') {
    notes.push('Evidence is partial. Use what survives, but verify it against the live page.');
  }
  return {
    category: cat,
    title,
    steps,
    avoid,
    notes,
    handoff: HANDOFF_COMMON,
  };
}

/** Plain-text lines for the heal prompt. Kept short for local models. */
export function renderHealPlaybookLines({classification, profile, adjacent, playbook}) {
  const adj = adjacent || {};
  const lines = [];
  lines.push(`Failure type: ${classification.category} (confidence ${classification.confidence}; signals: ${classification.signals.join(', ') || 'none'}). Failed action: ${profile.kind}.`);
  lines.push(`Evidence: ${profile.evidence_level} (locators=${profile.locator_count}${profile.locators_probed ? `, still matching=${profile.locators_still_matching}/${profile.locators_probed}` : ''}, coordinates=${profile.has_coordinates ? 'yes' : 'no'}).`);
  if (adj.prev) {
    lines.push(`Previous step #${adj.prev.index} ${adj.prev.action ? adj.prev.action.kind : '?'} ${adj.prev.status}; left the browser on ${adj.prev.url_after || '(unknown)'}.`);
  }
  if (adj.next) {
    lines.push(`Next step #${adj.next.index} ${adj.next.action ? adj.next.action.kind : '?'} expects ${profile.expected_url_after || '(unknown page)'}.`);
  }
  lines.push(playbook.title);
  playbook.steps.forEach((s, i) => lines.push(`${i + 1}. ${s}`));
  for (const n of playbook.notes) lines.push(`Note: ${n}`);
  for (const a of playbook.avoid) lines.push(`Avoid: ${a}`);
  for (const h of playbook.handoff) lines.push(h);
  return lines;
}

/** One call for the prompt builder. */
export function planHealInvestigation({
  error, failedAction, contextUrlMismatch, runDetails, scriptActions, failedStep, probe,
}) {
  const classification =
      classifyHealFailure({error, failedAction, contextUrlMismatch});
  const adjacent = buildAdjacentStepContext({runDetails, scriptActions, failedStep});
  const profile = buildEvidenceProfile({failedAction, adjacent, probe});
  const playbook = buildHealPlaybook({classification, profile, adjacent});
  return {
    classification,
    adjacent,
    profile,
    playbook,
    lines: renderHealPlaybookLines({classification, profile, adjacent, playbook}),
  };
}

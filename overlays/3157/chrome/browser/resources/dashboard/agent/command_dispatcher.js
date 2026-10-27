import {
  acceptInternalAgentCommand,
  normalizeAgentCommandForExecution,
  normalizeScriptAction,
  sanitizeAgentActionParams,
} from './model_prompt_contract.js';
import {
  applyInvestigateContextDiff,
  normalizeInvestigateRequestPayload,
  slimInvestigateResult,
} from './investigate_result_slim.js';
import {
  collectionOfScriptId,
  formatCollectionRef,
  getTopLevelAction,
  isCollectionRef,
  isValidCollectionName,
  isValidScriptId,
  listCollectionsFromScripts,
  listScriptsInCollection,
  parseCollectionRef,
} from './script_collections.js';

function normalizeCommandForDispatch(command) {
  if (command && typeof command === 'object' &&
      Object.prototype.hasOwnProperty.call(command, 'command')) {
    return acceptInternalAgentCommand(command);
  }
  return normalizeAgentCommandForExecution(command);
}

export {createDashboardAutoHealManager} from './dashboard_auto_heal.js';
export {
  COLLECTION_REF_PREFIX,
  formatCollectionRef,
  isCollectionRef,
  parseCollectionRef,
  collectionOfScriptId,
  listScriptsInCollection,
} from './script_collections.js';

function isInvestigatorLoopProfile(windowRef) {
  const raw = String(
      windowRef?.__DASH_LOOP_PROFILE ||
      windowRef?.__DASHBOARD_SETTINGS?.agent?.loopProfile ||
      '')
      .trim()
      .toLowerCase();
  return raw === 'investigator';
}

/** Heal/support assistant phase still verifies scripts on headless. */
function shouldUseHeadlessScriptRuns(windowRef) {
  return isInvestigatorLoopProfile(windowRef) ||
      !!windowRef?.__DASH_HEADLESS_SCRIPT_RUNS;
}

function rememberInvestigateTarget(windowRef, result) {
  const tid = result && (result.target_id || result.targetId);
  if (tid) {
    windowRef.__DASH_INVESTIGATE_TARGET_ID = String(tid);
  }
}

function currentInvestigateTargetId(windowRef, payload) {
  return String(
      payload?.target_id || payload?.targetId ||
      windowRef.__DASH_INVESTIGATE_TARGET_ID ||
      '')
      .trim();
}

function resolveWindowApi(windowRef, name) {
  if (!windowRef || typeof windowRef !== 'object') {
    return null;
  }
  const fn = windowRef[name];
  return (typeof fn === 'function') ? fn : null;
}

function formatError(errorLike) {
  return String(errorLike && errorLike.message ? errorLike.message : errorLike);
}

function classifyCommandFailure(commandName, normalized) {
  const error = String(normalized?.error || '').trim().toLowerCase();
  if (!error) {
    return {
      stage: normalized?.ok ? 'completed' : 'unknown',
      class: normalized?.ok ? 'ok' : 'unknown_failure',
      retryable: !normalized?.ok,
    };
  }
  if (error.includes('dashboard_api_unavailable')) {
    return {stage: 'dispatch', class: 'tool_unavailable', retryable: false};
  }
  if (error.includes('unknown_command')) {
    return {stage: 'dispatch', class: 'unknown_command', retryable: false};
  }
  if (error.includes('missing_action') || error.includes('missing_command')) {
    return {stage: 'planning', class: 'invalid_tool_payload', retryable: false};
  }
  if (error.includes('investigate_')) {
    return {stage: 'investigate', class: 'headless_investigation_failed', retryable: true};
  }
  if (error.includes('runtime_eval') || error.includes('eval')) {
    return {stage: 'execution', class: 'runtime_eval_failed', retryable: true};
  }
  if (error.includes('refreshpagemap') || error.includes('refresh_pagemap') ||
      error.includes('pagemap')) {
    return {stage: 'grounding', class: 'context_refresh_failed', retryable: true};
  }
  if (commandName === 'call_action') {
    return {stage: 'execution', class: 'action_execution_failed', retryable: true};
  }
  if (commandName === 'dashboard_save_script_json' ||
      commandName === 'dashboard_get_script_json') {
    return {stage: 'script_io', class: 'script_io_failed', retryable: true};
  }
  return {stage: 'execution', class: 'tool_execution_failed', retryable: true};
}

function normalizeCommandResult(command, result, startedAt) {
  const normalized = (result && typeof result === 'object') ?
    {...result} :
    {ok: false, error: 'empty_command_response'};
  const durationMs = Math.max(0, Date.now() - startedAt);
  const commandName = String(command?.command || '');
  const failure = classifyCommandFailure(commandName, normalized);
  normalized.command = commandName;
  normalized.duration_ms = durationMs;
  normalized.failure_stage = failure.stage;
  normalized.error_class = failure.class;
  normalized.retryable = failure.retryable;
  return normalized;
}

function sanitizeRegexInput(raw) {
  let value = String(raw || '').trim();
  if (!value) return '';
  if ((value.startsWith('"') && value.endsWith('"')) ||
      (value.startsWith('\'') && value.endsWith('\''))) {
    value = value.slice(1, -1).trim();
  }
  return value;
}

function escapeRegexLiteral(value) {
  return String(value || '').replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function extractDomainFromInput(raw) {
  const cleaned = sanitizeRegexInput(raw);
  if (!cleaned) return '';
  const tryUrl = (candidate) => {
    try {
      const parsed = new URL(candidate);
      const host = String(parsed.hostname || '').trim().toLowerCase();
      return host ? host : '';
    } catch (_) {
      return '';
    }
  };

  const fromAbsolute = tryUrl(cleaned);
  if (fromAbsolute) return fromAbsolute;

  const withProto = tryUrl(`https://${cleaned}`);
  if (withProto) return withProto;

  const hostMatch = cleaned.match(/[a-z0-9-]+(?:\.[a-z0-9-]+)+/i);
  return hostMatch ? hostMatch[0].toLowerCase() : '';
}

function buildDomainUrlRegex(domain) {
  const normalized = String(domain || '').trim().toLowerCase();
  if (!normalized) return '';
  const escaped = escapeRegexLiteral(normalized);
  return `https?:\\/\\/(?:[^"\\s]*\\.)?${escaped}(?:[\\/:?#][^"\\s]*)?`;
}

function toOptionalBool(value) {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return value !== 0;
  if (typeof value === 'string') {
    const v = value.trim().toLowerCase();
    if (!v) return null;
    if (v === 'true' || v === '1' || v === 'yes' || v === 'on') return true;
    if (v === 'false' || v === '0' || v === 'no' || v === 'off') return false;
  }
  return null;
}

function resolveListenerMatchSpec(payload = {}) {
  const explicitRegex = sanitizeRegexInput(
      payload?.regex || payload?.pattern || payload?.regexp || '');
  const explicitLiteral = sanitizeRegexInput(
      payload?.literal || payload?.contains || payload?.text || '');
  const genericValue = sanitizeRegexInput(
      payload?.value || payload?.match || payload?.query || payload?.filter || '');
  const explicitUseRegex = toOptionalBool(
      payload?.use_regex ?? payload?.useRegex ?? payload?.regex_enabled ??
      payload?.regexEnabled ?? null);

  let value = explicitRegex || explicitLiteral || genericValue;
  let useRegex = explicitUseRegex;

  if (!value) {
    const explicitDomain = sanitizeRegexInput(payload?.domain || '');
    const exampleValue = sanitizeRegexInput(
        payload?.sample || payload?.example || payload?.example_value || '');
    const resolvedDomain = explicitDomain || extractDomainFromInput(exampleValue);
    if (resolvedDomain) {
      value = buildDomainUrlRegex(resolvedDomain);
      if (useRegex === null) useRegex = true;
    }
  }

  if (!value) return {ok: false, error: 'missing_match_value'};
  if (useRegex === null) {
    useRegex = !!explicitRegex;
  }
  return {ok: true, value, useRegex};
}

function collectListenActionRefs(scriptJson) {
  const refs = [];
  const pushActionRef = (action, path) => {
    if (!action || typeof action !== 'object') return;
    if (String(action.action || '').toLowerCase() !== 'listen') return;
    refs.push({action, path});
  };

  const topActions = Array.isArray(scriptJson?.actions) ? scriptJson.actions : [];
  for (let i = 0; i < topActions.length; i++) {
    pushActionRef(topActions[i], `actions[${i}]`);
  }

  const contexts = Array.isArray(scriptJson?.url_contexts) ? scriptJson.url_contexts : [];
  for (let c = 0; c < contexts.length; c++) {
    const actions = Array.isArray(contexts[c]?.actions) ? contexts[c].actions : [];
    for (let a = 0; a < actions.length; a++) {
      pushActionRef(actions[a], `url_contexts[${c}].actions[${a}]`);
    }
  }

  return refs;
}

function ensurePrimaryListenAction(scriptJson) {
  const existing = collectListenActionRefs(scriptJson);
  if (existing.length) {
    return {refs: existing, created: false};
  }
  const kind = String(scriptJson?.kind || 'main').toLowerCase();
  if (kind !== 'listener') {
    return {refs: existing, created: false};
  }
  if (!Array.isArray(scriptJson.actions)) {
    scriptJson.actions = [];
  }
  const listenAction = {
    action: 'listen',
    params: {
      condition: 'text_contains',
      text: '',
      timeout_ms: 15000,
      locators: [
        {type: 'CSS_SELECTOR', value: 'body'},
      ],
    },
  };
  scriptJson.actions.unshift(listenAction);
  return {
    refs: [{action: listenAction, path: 'actions[0]'}],
    created: true,
  };
}

export function installDashboardAgentApi({
  windowRef = window,
  documentRef = document,
  appState,
  dashboardHandler,
  chromeSend = (name, args) => {
    if (windowRef?.chrome?.send) {
      windowRef.chrome.send(name, args);
      return;
    }
    throw new Error('chrome_send_unavailable');
  },
  normalizeScriptsDict,
  renderAll,
  switchView,
  scriptHasDraftActions = () => false,
  updateAutoHealSettings = () => {},
  processAutoHealQueue = () => ({ok: true}),
  getAutoHealState = () => ({autoQueueEnabled: false, queue: []}),
}) {
  if (!windowRef || typeof windowRef !== 'object') {
    return null;
  }
  if (!appState || typeof appState !== 'object') {
    return null;
  }

  const countScriptActions = (script) => {
    if (!script || typeof script !== 'object') {
      return 0;
    }
    const top = Array.isArray(script.actions) ? script.actions.length : 0;
    const contexts = Array.isArray(script.url_contexts) ? script.url_contexts : [];
    let nested = 0;
    for (const ctx of contexts) {
      nested += Array.isArray(ctx?.actions) ? ctx.actions.length : 0;
    }
    return top + nested;
  };

  const mergeScriptsPreservingDrafts = (normalizedScripts) => {
    const incoming = (normalizedScripts && typeof normalizedScripts === 'object') ?
      normalizedScripts :
      {};
    const existing = (appState.scripts && typeof appState.scripts === 'object') ?
      appState.scripts :
      {};
    const merged = {...incoming};
    let preservedDraftCount = 0;

    for (const [scriptId, script] of Object.entries(existing)) {
      const preserveLocalUnsaved =
        !!(script && typeof script === 'object' &&
           script.__localUnsavedChanges === true);
      if (!scriptHasDraftActions(script) && !preserveLocalUnsaved) {
        continue;
      }
      merged[scriptId] = script;
      preservedDraftCount += 1;
    }

    appState.scripts = merged;
    if (!appState.currentScriptId || !appState.scripts[appState.currentScriptId]) {
      const firstId = Object.keys(appState.scripts)[0];
      if (firstId) appState.currentScriptId = firstId;
    }
    return preservedDraftCount;
  };

  windowRef.DashboardScriptsChanged = async function(scriptsPayload = null) {
    try {
      let scriptsRaw = null;
      if (scriptsPayload && typeof scriptsPayload === 'object' &&
          !Array.isArray(scriptsPayload)) {
        scriptsRaw = (scriptsPayload.scripts &&
                      typeof scriptsPayload.scripts === 'object') ?
          scriptsPayload.scripts :
          scriptsPayload;
      }
      if (!scriptsRaw) {
        const res = await dashboardHandler.getScripts();
        scriptsRaw = (res && res.scripts) || {};
      }
      const normalizedScripts = normalizeScriptsDict(scriptsRaw);
      const preservedDraftCount = mergeScriptsPreservingDrafts(normalizedScripts);
      if (preservedDraftCount > 0) {
        console.log(
            '[DashboardUI] DashboardScriptsChanged: merged backend refresh while preserving draft scripts',
            {preservedDraftCount});
      }
      if (typeof renderAll === 'function') {
        renderAll();
      }
    } catch (e) {
      console.error('[DashboardUI] DashboardScriptsChanged refresh failed', e);
    }
  };

  windowRef.DashboardListenersChanged = function() {
    try {
      if (appState.currentView !== 'debugging') {
        return;
      }
      const dbgView = documentRef.getElementById('dashboard-debugging-view');
      if (!dbgView || !dbgView.classList.contains('active')) {
        return;
      }
      dbgView.dispatchEvent(
          new CustomEvent('dbg-refresh-listeners', {bubbles: true}));
    } catch (e) {
      console.error('[DashboardUI] DashboardListenersChanged refresh failed', e);
    }
  };

  windowRef.dashboardAgentListScripts = function(_payload = {}) {
    try {
      const scripts = appState.scripts || {};
      const currentScriptId = String(appState.currentScriptId || '');
      const out = Object.entries(scripts).map(([id, script]) => ({
        id,
        name: script?.name || id,
        kind: script?.kind || 'main',
        collection: collectionOfScriptId(id) ||
            String(script?.collection || '').trim(),
        is_current: id === currentScriptId,
        action_count: countScriptActions(script),
      }));
      return {
        ok: true,
        current_script_id: currentScriptId,
        current_view: appState.currentView || 'dashboard',
        collections: listCollectionsFromScripts(scripts),
        scripts: out,
      };
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentListCollections = function(_payload = {}) {
    try {
      const scripts = appState.scripts || {};
      const collections = listCollectionsFromScripts(scripts).map((name) => ({
        name,
        ref: formatCollectionRef(name),
        script_count: listScriptsInCollection(scripts, name).length,
      }));
      return {ok: true, collections};
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentSetView = function(viewName) {
    try {
      const view = String(viewName || '').toLowerCase();
      const allowed = new Set(['dashboard', 'debugging', 'data', 'settings']);
      if (!allowed.has(view)) {
        return {ok: false, error: `invalid_view:${viewName}`};
      }
      if (typeof switchView === 'function') {
        switchView(view);
      }
      return {ok: true, view};
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentSelectScript = function(scriptId) {
    try {
      const id = String(scriptId || '').trim();
      if (!id || !appState.scripts?.[id]) {
        return {ok: false, error: `unknown_script:${id || '(empty)'}`};
      }
      appState.currentScriptId = id;
      // Keep the user on Debugging/Settings/etc. Listener triggers and
      // auto-heal select_script used to force switchView('dashboard'), which
      // yanked the in-page nav back to the main Dashboard tab.
      if (typeof renderAll === 'function') {
        renderAll();
      }
      return {ok: true, script_id: id};
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentRunScript = async function(payload = '') {
    try {
      const options = (payload && typeof payload === 'object') ? payload : {};
      const id = String(
          options.script_id ||
          options.id ||
          payload ||
          appState.currentScriptId ||
          '').trim();
      if (!id || !appState.scripts?.[id]) {
        return {ok: false, error: `unknown_script:${id || '(empty)'}`};
      }
      const waitForCompletion = options.wait_for_completion === true ||
        options.waitForCompletion === true;
      const playback = await dashboardHandler.playScript(id, {
        waitForCompletion,
      });
      return {
        ok: playback?.ok !== false,
        script_id: id,
        playback,
      };
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentResumeScriptFromStep = async function(payload = {}) {
    try {
      const scriptId = String(
          payload?.script_id ||
          payload?.id ||
          appState.currentScriptId ||
          '').trim();
      const stepIndex = Number(payload?.step_index ?? payload?.stepIndex ?? -1);
      const runId = String(payload?.run_id || payload?.runId || '').trim();
      if (!scriptId) {
        return {ok: false, error: 'missing_script_id'};
      }
      if (!Number.isFinite(stepIndex) || stepIndex < 0) {
        return {ok: false, error: 'invalid_step_index'};
      }
      const res = await dashboardHandler.resumeScriptFromStep(
          scriptId, stepIndex, runId);
      return {
        ok: !!res?.success,
        success: !!res?.success,
        script_id: scriptId,
        step_index: stepIndex,
        run_id: String(res?.runId || ''),
        error: String(res?.error || ''),
      };
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentGetScriptJson = async function(payload = {}) {
    try {
      const id = String(
          payload?.script_id ||
          payload?.id ||
          appState.currentScriptId ||
          '').trim();
      if (!id) {
        return {ok: false, error: 'missing_script_id'};
      }
      const script = appState.scripts?.[id];
      if (!script || typeof script !== 'object') {
        return {ok: false, error: `unknown_script:${id}`};
      }
      const scriptJson = JSON.parse(JSON.stringify(script));
      return {ok: true, script_id: id, script_json: scriptJson};
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentSaveScriptJson = async function(payload = {}) {
    try {
      const requestedId = String(payload?.script_id || payload?.id || '').trim();
      let scriptJson =
          payload?.script_json ?? payload?.script ?? payload?.json ?? payload?.data;
      if (typeof scriptJson === 'string') {
        scriptJson = JSON.parse(scriptJson);
      }
      if (!scriptJson || typeof scriptJson !== 'object' || Array.isArray(scriptJson)) {
        return {ok: false, error: 'invalid_script_json'};
      }
      const toSave = JSON.parse(JSON.stringify(scriptJson));
      const id = String(requestedId || toSave.id || '').trim();
      if (!id) {
        return {ok: false, error: 'missing_script_id'};
      }
      if (!isValidScriptId(id)) {
        return {ok: false, error: `invalid_script_id:${id}`};
      }
      toSave.id = id;
      const collection = collectionOfScriptId(id);
      if (collection) {
        toSave.collection = collection;
      }

      await dashboardHandler.saveScript(id, toSave);
      const normalizedSaved =
        normalizeScriptsDict({[id]: toSave})[id] || JSON.parse(JSON.stringify(toSave));
      appState.scripts = {
        ...(appState.scripts && typeof appState.scripts === 'object' ?
          appState.scripts :
          {}),
        [id]: normalizedSaved,
      };
      if (!appState.currentScriptId || !appState.scripts[appState.currentScriptId]) {
        appState.currentScriptId = id;
      }
      if (typeof renderAll === 'function') {
        renderAll();
      }
      return {
        ok: true,
        script_id: id,
        script_json: appState.scripts[id] ?
            JSON.parse(JSON.stringify(appState.scripts[id])) :
            null,
      };
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  const resolveScriptIdFromPayload = (payload = {}) => String(
      payload?.script_id ||
      payload?.id ||
      appState.currentScriptId ||
      '').trim();

  const persistCurrentScript = async (scriptId, script) => {
    const toSave = JSON.parse(JSON.stringify(script));
    toSave.id = scriptId;
    const collection = collectionOfScriptId(scriptId);
    if (collection) {
      toSave.collection = collection;
    }
    delete toSave.__localUnsavedChanges;
    delete toSave.__agent_next_insert_index;
    await dashboardHandler.saveScript(scriptId, toSave);
    const normalizedSaved =
      normalizeScriptsDict({[scriptId]: toSave})[scriptId] || toSave;
    appState.scripts = {
      ...(appState.scripts && typeof appState.scripts === 'object' ?
        appState.scripts :
        {}),
      [scriptId]: normalizedSaved,
    };
    if (typeof renderAll === 'function') {
      renderAll();
    }
    return normalizedSaved;
  };

  windowRef.dashboardAgentGetScriptStep = async function(payload = {}) {
    try {
      const id = resolveScriptIdFromPayload(payload);
      const stepIndex = Number(payload?.step_index ?? payload?.stepIndex ?? -1);
      if (!id) {
        return {ok: false, error: 'missing_script_id'};
      }
      const script = appState.scripts?.[id];
      if (!script || typeof script !== 'object') {
        return {ok: false, error: `unknown_script:${id}`};
      }
      const got = getTopLevelAction(script, stepIndex);
      if (!got.ok) {
        return {ok: false, script_id: id, ...got};
      }
      return {
        ok: true,
        script_id: id,
        step_index: got.step_index,
        action_count: got.action_count,
        action: JSON.parse(JSON.stringify(got.action)),
      };
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentUpdateScriptStep = async function(payload = {}) {
    try {
      const id = resolveScriptIdFromPayload(payload);
      const stepIndex = Number(payload?.step_index ?? payload?.stepIndex ?? -1);
      if (!id) {
        return {ok: false, error: 'missing_script_id'};
      }
      const script = appState.scripts?.[id];
      if (!script || typeof script !== 'object') {
        return {ok: false, error: `unknown_script:${id}`};
      }
      if (!Array.isArray(script.actions)) {
        script.actions = [];
      }
      const got = getTopLevelAction(script, stepIndex);
      if (!got.ok) {
        return {ok: false, script_id: id, ...got};
      }

      let nextAction =
          payload?.action ?? payload?.step ?? payload?.action_json ?? null;
      if (typeof nextAction === 'string') {
        nextAction = JSON.parse(nextAction);
      }
      if (!nextAction || typeof nextAction !== 'object' || Array.isArray(nextAction)) {
        // Allow patching only params/locators onto the existing action.
        const existing = JSON.parse(JSON.stringify(got.action));
        const patchParams =
            payload?.params && typeof payload.params === 'object' ?
              payload.params :
              null;
        const patchLocators = Array.isArray(payload?.locators) ?
          payload.locators :
          null;
        if (!patchParams && !patchLocators && payload?.id == null &&
            payload?.sel == null && payload?.selector == null &&
            payload?.text == null) {
          return {ok: false, error: 'missing_action_or_params'};
        }
        const merged = {
          action: existing.action,
          comment: existing.comment,
          params: {
            ...(existing.params && typeof existing.params === 'object' ?
              existing.params :
              {}),
            ...(patchParams || {}),
          },
        };
        if (patchLocators) {
          merged.params.locators = patchLocators;
        }
        if (payload?.id != null || payload?.sel != null ||
            payload?.selector != null || payload?.text != null) {
          const shorthand = {};
          if (payload.id != null) shorthand.id = payload.id;
          if (payload.sel != null) shorthand.sel = payload.sel;
          if (payload.selector != null) shorthand.selector = payload.selector;
          if (payload.text != null) shorthand.text = payload.text;
          Object.assign(merged.params, shorthand);
        }
        nextAction = merged;
      }

      const normalized = normalizeScriptAction(nextAction) || nextAction;
      if (normalized && typeof normalized === 'object') {
        normalized.params = sanitizeAgentActionParams(
            normalized.action, normalized.params || {});
        if (got.action?.comment && !normalized.comment) {
          normalized.comment = got.action.comment;
        }
      }
      script.actions[stepIndex] = normalized;
      script.__localUnsavedChanges = true;
      const saved = await persistCurrentScript(id, script);
      return {
        ok: true,
        script_id: id,
        step_index: stepIndex,
        action: JSON.parse(JSON.stringify(saved?.actions?.[stepIndex] || normalized)),
        action_count: Array.isArray(saved?.actions) ? saved.actions.length : 0,
      };
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentDeleteScriptStep = async function(payload = {}) {
    try {
      const id = resolveScriptIdFromPayload(payload);
      const stepIndex = Number(payload?.step_index ?? payload?.stepIndex ?? -1);
      if (!id) {
        return {ok: false, error: 'missing_script_id'};
      }
      const script = appState.scripts?.[id];
      if (!script || typeof script !== 'object') {
        return {ok: false, error: `unknown_script:${id}`};
      }
      const got = getTopLevelAction(script, stepIndex);
      if (!got.ok) {
        return {ok: false, script_id: id, ...got};
      }
      const removed = JSON.parse(JSON.stringify(got.action));
      script.actions.splice(stepIndex, 1);
      script.__localUnsavedChanges = true;
      const saved = await persistCurrentScript(id, script);
      return {
        ok: true,
        script_id: id,
        step_index: stepIndex,
        removed,
        action_count: Array.isArray(saved?.actions) ? saved.actions.length : 0,
      };
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentResolveTriggerTarget = function(payload = {}) {
    try {
      const raw = String(
          payload?.target ||
          payload?.onMatchScriptId ||
          payload?.script_id ||
          payload?.id ||
          '').trim();
      if (!raw) {
        return {ok: false, error: 'missing_target'};
      }
      if (isCollectionRef(raw)) {
        const collection = parseCollectionRef(raw);
        if (!isValidCollectionName(collection)) {
          return {ok: false, error: `invalid_collection:${collection}`};
        }
        const scripts = listScriptsInCollection(appState.scripts, collection);
        return {
          ok: true,
          kind: 'collection',
          collection,
          ref: formatCollectionRef(collection),
          scripts,
        };
      }
      if (!appState.scripts?.[raw]) {
        return {ok: false, error: `unknown_script:${raw}`};
      }
      return {
        ok: true,
        kind: 'script',
        script_id: raw,
        collection: collectionOfScriptId(raw),
      };
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentSetGlobalSetting = async function(key, value) {
    try {
      const settingKey = String(key || '').trim();
      if (!settingKey) {
        return {ok: false, error: 'missing_key'};
      }
      const currentRes = await dashboardHandler.getEffectiveSettings('');
      const currentSettings = (currentRes && currentRes.settings) ? currentRes.settings : {};
      const nextSettings = {...currentSettings, [settingKey]: value};
      const setRes = await dashboardHandler.setGlobalSettings(nextSettings);
      if (!setRes || !setRes.success) {
        return {ok: false, error: String(setRes?.error || 'set_global_settings_failed')};
      }
      try {
        // Settings mutations must not drain the auto-heal queue.
        updateAutoHealSettings(nextSettings || {});
      } catch (_) {}
      return {ok: true, key: settingKey, value};
    } catch (e) {
      return {ok: false, error: formatError(e)};
    }
  };

  windowRef.dashboardAgentInvestigatePage = async function(payload = {}) {
    return new Promise((resolve) => {
      const cbId = `dashInvestigate_${Date.now()}_${Math.random().toString(36).slice(2)}`;
      try {
        windowRef[cbId] = (res) => {
          try {
            delete windowRef[cbId];
          } catch (_) {}
          const normalized = (res && typeof res === 'object') ?
            res :
            {ok: false, error: 'investigate_empty_response'};
          if (normalized && normalized.target_id) {
            windowRef.__DASH_INVESTIGATE_TARGET_ID = String(normalized.target_id);
          }
          resolve(normalized);
        };
        chromeSend('dashboardAgentInvestigatePage', [
          cbId,
          normalizeInvestigateRequestPayload(payload || {}),
        ]);
      } catch (e) {
        try {
          delete windowRef[cbId];
        } catch (_) {}
        resolve({ok: false, error: formatError(e)});
      }
    });
  };

  windowRef.dashboardAgentExecuteActionsHeadless = async function(payload = {}) {
    return new Promise((resolve) => {
      const cbId = `dashExecAct_${Date.now()}_${Math.random().toString(36).slice(2)}`;
      try {
        windowRef[cbId] = (res) => {
          try {
            delete windowRef[cbId];
          } catch (_) {}
          const normalized = (res && typeof res === 'object') ?
            res :
            {ok: false, error: 'execute_actions_empty_response'};
          if (normalized && normalized.target_id) {
            windowRef.__DASH_INVESTIGATE_TARGET_ID = String(normalized.target_id);
          }
          resolve(applyInvestigateContextDiff(normalized));
        };
        const body = {...(payload || {})};
        if (!body.target_id && !body.targetId &&
            windowRef.__DASH_INVESTIGATE_TARGET_ID) {
          body.target_id = windowRef.__DASH_INVESTIGATE_TARGET_ID;
        }
        chromeSend('dashboardAgentExecuteActions', [cbId, body]);
      } catch (e) {
        try {
          delete windowRef[cbId];
        } catch (_) {}
        resolve({ok: false, error: formatError(e)});
      }
    });
  };

  windowRef.dashboardAgentRunScriptHeadless = async function(payload = {}) {
    return new Promise((resolve) => {
      const cbId = `dashRunHL_${Date.now()}_${Math.random().toString(36).slice(2)}`;
      try {
        windowRef[cbId] = (res) => {
          try {
            delete windowRef[cbId];
          } catch (_) {}
          const normalized = (res && typeof res === 'object') ?
            res :
            {ok: false, error: 'run_script_empty_response'};
          if (normalized && normalized.target_id) {
            windowRef.__DASH_INVESTIGATE_TARGET_ID = String(normalized.target_id);
          }
          resolve(applyInvestigateContextDiff(normalized));
        };
        const body = {...(payload || {})};
        if (!body.target_id && !body.targetId &&
            windowRef.__DASH_INVESTIGATE_TARGET_ID) {
          body.target_id = windowRef.__DASH_INVESTIGATE_TARGET_ID;
        }
        chromeSend('dashboardAgentRunScriptHeadless', [cbId, body]);
      } catch (e) {
        try {
          delete windowRef[cbId];
        } catch (_) {}
        resolve({ok: false, error: formatError(e)});
      }
    });
  };

  windowRef.dashboardAgentResumeFromStepHeadless = async function(payload = {}) {
    return new Promise((resolve) => {
      const cbId = `dashResumeHL_${Date.now()}_${Math.random().toString(36).slice(2)}`;
      try {
        windowRef[cbId] = (res) => {
          try {
            delete windowRef[cbId];
          } catch (_) {}
          const normalized = (res && typeof res === 'object') ?
            res :
            {ok: false, error: 'resume_empty_response'};
          if (normalized && normalized.target_id) {
            windowRef.__DASH_INVESTIGATE_TARGET_ID = String(normalized.target_id);
          }
          resolve(applyInvestigateContextDiff(normalized));
        };
        const body = {...(payload || {})};
        if (!body.target_id && !body.targetId &&
            windowRef.__DASH_INVESTIGATE_TARGET_ID) {
          body.target_id = windowRef.__DASH_INVESTIGATE_TARGET_ID;
        }
        chromeSend('dashboardAgentResumeFromStepHeadless', [cbId, body]);
      } catch (e) {
        try {
          delete windowRef[cbId];
        } catch (_) {}
        resolve({ok: false, error: formatError(e)});
      }
    });
  };

  return {
    refreshScripts: windowRef.DashboardScriptsChanged,
  };
}


function createActionParamNormalizer(parseVisualCoords, formatVisualCoords) {
  return function normalizeAgentActionParams(action, params) {
    if (!params || typeof params !== 'object') {
      return params || {};
    }

    const out = {...params};

    if (action === 'click' || action === 'hover') {
      let coords = null;

      const mergeCoords = (parsed) => {
        if (!parsed) return;
        if (!coords) {
          coords = {x: parsed.x, y: parsed.y};
          return;
        }
        if (coords.x === null && parsed.x !== null) {
          coords.x = parsed.x;
        }
        if (coords.y === null && parsed.y !== null) {
          coords.y = parsed.y;
        }
      };

      if (Array.isArray(out.locators)) {
        const remainingLocators = [];
        for (const loc of out.locators) {
          if (loc && typeof loc === 'object') {
            const t = String(loc.type || '').toLowerCase();
            const isCoordType = t === 'coordinates' || t === 'visual_coords' ||
                t === 'visual-coords' || t === 'coords' || t === 'xy';
            if (isCoordType) {
              const parsed = parseVisualCoords(loc.value);
              if (parsed) {
                mergeCoords(parsed);
                continue;
              }
            }
          }
          remainingLocators.push(loc);
        }
        if (remainingLocators.length) {
          out.locators = remainingLocators;
        } else {
          delete out.locators;
        }
      }

      if (!coords && (typeof out.x === 'number' || typeof out.y === 'number')) {
        mergeCoords({
          x: Number.isFinite(out.x) ? Math.round(out.x) : null,
          y: Number.isFinite(out.y) ? Math.round(out.y) : null,
        });
      }
      if (!coords && out.coordinates) {
        mergeCoords(parseVisualCoords(out.coordinates));
      }

      if (coords && (coords.x !== null || coords.y !== null)) {
        const locator = {
          type: 'VISUAL_COORDS',
          value: formatVisualCoords(coords),
        };
        const locs = Array.isArray(out.locators) ? out.locators : [];
        out.locators = [locator, ...locs];
      }

      delete out.coordinates;
      delete out.x;
      delete out.y;

      if (action === 'hover') {
        out.click_count = 0;
        out.clickCount = 0;
      } else if (action === 'click') {
        const rawClickCount = parseInt(
          out.clickCount ?? out.click_count ?? 1,
          10);
        const clickCount = Number.isFinite(rawClickCount)
          ? Math.min(3, Math.max(1, rawClickCount))
          : 1;
        out.click_count = clickCount;
        out.clickCount = clickCount;
      }
    }

    return out;
  };
}

export function createDashboardCommandDispatcher({
  appendMessage,
  historyEl,
  chromeSend,
  parseVisualCoords,
  formatVisualCoords,
  cubbyRead,
  cubbyWrite,
  onResetAgent,
  onRefreshPageMapRequested,
  onCommandTrace = () => {},
  windowRef = window,
}) {
  const normalizeAgentActionParams =
      createActionParamNormalizer(parseVisualCoords, formatVisualCoords);
  const configureListenerMatch = async (payload = {}) => {
    const getScriptJson = resolveWindowApi(windowRef, 'dashboardAgentGetScriptJson');
    const saveScriptJson = resolveWindowApi(windowRef, 'dashboardAgentSaveScriptJson');
    if (!getScriptJson || !saveScriptJson) {
      return {ok: false, error: 'dashboard_api_unavailable'};
    }

    const requestedScriptId = String(payload?.script_id || payload?.id || '').trim();
    let scriptId = requestedScriptId;
    if (!scriptId) {
      try {
        const listScripts = resolveWindowApi(windowRef, 'dashboardAgentListScripts');
        const listRes = listScripts ? listScripts({}) : null;
        const scripts = Array.isArray(listRes?.scripts) ? listRes.scripts : [];
        const currentId = String(listRes?.current_script_id || '').trim();
        const current = scripts.find((s) => String(s?.id || '') === currentId);
        if (String(current?.kind || '').toLowerCase() === 'listener') {
          scriptId = currentId;
        } else {
          const firstListener = scripts.find((s) =>
            String(s?.kind || '').toLowerCase() === 'listener');
          if (firstListener && firstListener.id) {
            scriptId = String(firstListener.id);
          }
        }
      } catch (_) {}
    }
    const readRes = await getScriptJson(scriptId ? {script_id: scriptId} : {});
    if (!readRes || !readRes.ok || !readRes.script_json) {
      return {
        ok: false,
        error: String(readRes?.error || 'script_read_failed'),
      };
    }

    const scriptJson = JSON.parse(JSON.stringify(readRes.script_json));
    const ensuredListen = ensurePrimaryListenAction(scriptJson);
    const listenRefs = ensuredListen.refs;
    if (!listenRefs.length) {
      return {ok: false, error: 'no_listen_action_found'};
    }

    const requestedIndex = Number(payload?.listen_index ?? payload?.action_index);
    const selectedIndex =
      Number.isInteger(requestedIndex) &&
      requestedIndex >= 0 &&
      requestedIndex < listenRefs.length ?
        requestedIndex :
        0;
    const selected = listenRefs[selectedIndex];
    const params = (selected.action.params && typeof selected.action.params === 'object') ?
      selected.action.params :
      {};
    selected.action.params = params;

    const matchSpec = resolveListenerMatchSpec(payload);
    if (!matchSpec.ok) {
      return {ok: false, error: matchSpec.error};
    }
    const matchValue = matchSpec.value;
    const useRegex = matchSpec.useRegex;

    const rawTarget = String(payload?.target || payload?.field || 'auto').toLowerCase();
    const currentCondition = String(params.condition || '').toLowerCase();
    const target = rawTarget === 'text' || rawTarget === 'attribute' ?
      rawTarget :
      (currentCondition.startsWith('attribute_') ? 'attribute' : 'text');
    const requestedCondition = String(payload?.condition || '').trim();

    if (target === 'attribute') {
      params.condition = requestedCondition || 'attribute_contains';
      params.attribute = String(payload?.attribute || params.attribute || 'href');
      params.text = matchValue;
      params.attributeUseRegex = !!useRegex;
    } else {
      params.condition = requestedCondition || 'text_contains';
      params.text = matchValue;
      params.textUseRegex = !!useRegex;
    }

    const captureVariable = String(payload?.capture_variable || payload?.captureVariable || '').trim();
    if (captureVariable) {
      params.captureVariable = captureVariable;
    }

    const writeRes = await saveScriptJson({
      script_id: String(readRes.script_id || scriptJson.id || scriptId || ''),
      script_json: scriptJson,
    });
    if (!writeRes || !writeRes.ok) {
      return {
        ok: false,
        error: String(writeRes?.error || 'script_save_failed'),
      };
    }

    return {
      ok: true,
      script_id: String(writeRes.script_id || readRes.script_id || ''),
      listen_index: selectedIndex,
      listen_path: selected.path,
      created_listen_action: !!ensuredListen.created,
      match_value: matchValue,
      use_regex: !!useRegex,
      target,
      condition: params.condition,
      capture_variable: String(params.captureVariable || ''),
    };
  };
  const commandQueue = [];
  let commandInFlight = false;

  const sendInspectCommand = (command) => {
    return new Promise((resolve) => {
      const payload = (command && command.payload) ? command.payload : {};
      const cbId = 'dashInspect_' + Date.now() + Math.random().toString(36).slice(2);
      windowRef[cbId] = (res) => {
        try {
          delete windowRef[cbId];
        } catch (_) {}
        const normalized = (res && typeof res === 'object')
          ? res
          : {ok: false, error: 'inspect_empty_response'};
        try {
          if (normalized && normalized.report) {
            windowRef.__dashLastElementReport = normalized.report;
          }
          if (normalized && normalized.ok) {
            const idx =
                (typeof normalized.index === 'number') ? (` #${normalized.index}`) : '';
            appendMessage('assistant', `[Agent] Element inspection${idx} complete.`);
          } else if (normalized && normalized.error) {
            appendMessage('assistant', `[Agent] Inspect failed: ${normalized.error}`);
          }
        } catch (e) {
          console.warn('[AI] inspect_element callback error', e);
        }
        resolve(normalized);
      };
      try {
        console.log('[AI] Sending command: dashboardAgentInspectElement', payload);
        chromeSend('dashboardAgentInspectElement', [cbId, payload]);
      } catch (e) {
        console.error('[AI] Failed to send inspect_element:', e);
        appendMessage('assistant', `[Agent] Error sending inspect_element: ${e.message}`);
        try {
          delete windowRef[cbId];
        } catch (_) {}
        resolve({ok: false, error: String(e && e.message ? e.message : e)});
      }
    });
  };

  const dispatchNext = (entry) => {
    if (!entry || !entry.command) return;
    commandInFlight = true;
    const command = normalizeCommandForDispatch(entry.command);
    const resolveEntry = (result) => {
      try {
        if (entry && typeof entry.resolve === 'function') {
          entry.resolve(result);
        }
      } catch (_) {}
    };
    const finish = () => {
      commandInFlight = false;
      const next = commandQueue.shift();
      if (next) {
        dispatchNext(next);
      }
    };

    if (!command || typeof command !== 'object' || !command.command) {
      console.warn('AI tried to send invalid command:', command);
      resolveEntry({ok: false, error: 'invalid_command'});
      finish();
      return;
    }

    const cbToken = 'dashCmd_' + Date.now() + Math.random().toString(36).slice(2);
    const commandStartedAt = Date.now();
    try {
      onCommandTrace({
        phase: 'start',
        command: String(command.command || ''),
        payload: command.payload || {},
        started_at: commandStartedAt,
      });
    } catch (_) {}
    let settled = false;
    const cb = (res) => {
      if (settled) return;
      settled = true;
      const normalized = normalizeCommandResult(command, res, commandStartedAt);
      const ok = !!normalized.ok;
      const errorText = normalized.error ? ` error=${normalized.error}` : '';
      console.log(`[Agent] Command '${command.command}' executed:`, normalized);
      appendMessage(
          'assistant',
          `[Agent] ${command.command}: ${ok ? 'ok' : 'failed'}${errorText}`);
      try {
        onCommandTrace({
          phase: 'finish',
          command: String(command.command || ''),
          payload: command.payload || {},
          result: normalized,
          started_at: commandStartedAt,
          finished_at: Date.now(),
        });
      } catch (_) {}
      try {
        if (normalized && normalized.image && historyEl) {
          const img = document.createElement('img');
          img.src = normalized.image;
          img.style.maxWidth = '100%';
          img.style.borderRadius = '6px';
          const box = document.createElement('div');
          box.style.margin = '6px 0';
          box.appendChild(img);
          historyEl.appendChild(box);
          historyEl.scrollTop = historyEl.scrollHeight;
        }
      } catch (_) {}
      try {
        delete windowRef[cbToken];
      } catch (_) {}
      resolveEntry(normalized);
      finish();
    };
    windowRef[cbToken] = cb;

    try {
      switch (command.command) {
        case 'store_memento': {
          const payload = (command.payload && typeof command.payload === 'object') ?
            command.payload :
            {};
          const text = payload.text || payload.content || '';
          const ok = !!cubbyWrite(text, {
            global: payload.global === true || payload.scope === 'global',
            ttl: payload.ttl,
            taskId: payload.taskId || payload.task_id,
          });
          cb({ok});
          return;
        }
        case 'read_memento': {
          const payload = (command.payload && typeof command.payload === 'object') ?
            command.payload :
            {};
          const text = typeof cubbyRead === 'function' ?
            cubbyRead(payload.taskId || payload.task_id) :
            '';
          cb({ok: true, text: text || ''});
          return;
        }
        case 'reset_agent': {
          onResetAgent();
          cb({ok: true});
          return;
        }
        case 'dashboard_list_scripts': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentListScripts');
          cb(fn ? fn(command.payload || {}) : {ok: false, error: 'dashboard_api_unavailable'});
          return;
        }
        case 'dashboard_select_script': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentSelectScript');
          const scriptId = command.payload?.script_id || command.payload?.id || '';
          cb(fn ? fn(scriptId) : {ok: false, error: 'dashboard_api_unavailable'});
          return;
        }
        case 'dashboard_set_view': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentSetView');
          const viewName = command.payload?.view || command.payload?.name || '';
          cb(fn ? fn(viewName) : {ok: false, error: 'dashboard_api_unavailable'});
          return;
        }
        case 'dashboard_run_script': {
          if (shouldUseHeadlessScriptRuns(windowRef)) {
            const fn = resolveWindowApi(
                windowRef, 'dashboardAgentRunScriptHeadless');
            if (!fn) {
              cb({ok: false, error: 'dashboard_api_unavailable'});
              return;
            }
            Promise.resolve(fn(command.payload || {}))
                .then((res) => {
                  rememberInvestigateTarget(windowRef, res);
                  cb(res || {ok: true});
                })
                .catch((e) => cb({
                  ok: false,
                  error: String(e && e.message ? e.message : e),
                }));
            return;
          }
          const fn = resolveWindowApi(windowRef, 'dashboardAgentRunScript');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_resume_script_from_step': {
          if (shouldUseHeadlessScriptRuns(windowRef)) {
            const fn = resolveWindowApi(
                windowRef, 'dashboardAgentResumeFromStepHeadless');
            if (!fn) {
              cb({ok: false, error: 'dashboard_api_unavailable'});
              return;
            }
            Promise.resolve(fn(command.payload || {}))
                .then((res) => {
                  rememberInvestigateTarget(windowRef, res);
                  cb(res || {ok: true});
                })
                .catch((e) => cb({
                  ok: false,
                  error: String(e && e.message ? e.message : e),
                }));
            return;
          }
          const fn = resolveWindowApi(windowRef, 'dashboardAgentResumeScriptFromStep');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_set_global_setting': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentSetGlobalSetting');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          const key = command.payload?.key || '';
          const value = command.payload?.value;
          Promise.resolve(fn(key, value))
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_get_script_json': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentGetScriptJson');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => {
                const raw = res || {ok: true};
                const script = raw.script || raw.script_json || raw.scriptJson ||
                    raw;
                const actions = script && Array.isArray(script.actions) ?
                  script.actions :
                  null;
                const wantFull = !!(command.payload &&
                    (command.payload.full === true ||
                     command.payload.include_actions === true));
                if (wantFull) {
                  cb(raw);
                  return;
                }
                // Default: summary only — full scripts burn tokens in heal loops.
                cb({
                  ok: raw.ok !== false,
                  error: raw.error,
                  script_id: raw.script_id || raw.scriptId ||
                      (script && (script.id || script.script_id)),
                  name: script && script.name,
                  action_count: actions ? actions.length : undefined,
                  actions_preview: actions ?
                    actions.slice(0, 8).map((step, i) => ({
                      i,
                      action: step && (step.action || step.type),
                    })) :
                    undefined,
                });
              })
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_get_script_step': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentGetScriptStep');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_update_script_step': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentUpdateScriptStep');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_delete_script_step': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentDeleteScriptStep');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_list_collections': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentListCollections');
          cb(fn ? fn(command.payload || {}) : {ok: false, error: 'dashboard_api_unavailable'});
          return;
        }
        case 'dashboard_save_script_json': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentSaveScriptJson');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => {
                const raw = res || {ok: true};
                const script = raw.script || raw.script_json ||
                    command.payload?.script_json || command.payload?.script;
                const actions = script && Array.isArray(script.actions) ?
                  script.actions :
                  null;
                cb({
                  ok: raw.ok !== false,
                  error: raw.error,
                  script_id: raw.script_id || raw.scriptId ||
                      (script && (script.id || script.script_id)),
                  name: (script && script.name) || raw.name,
                  action_count: actions ? actions.length :
                    (raw.action_count ?? raw.actionCount),
                });
              })
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_investigate_page': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentInvestigatePage');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => {
                const raw = res || {ok: false, error: 'investigate_failed'};
                rememberInvestigateTarget(windowRef, raw);
                cb(applyInvestigateContextDiff(raw) ||
                   slimInvestigateResult(raw) ||
                   {ok: false, error: 'investigate_failed'});
              })
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_configure_listener_match':
        case 'dashboard_configure_listener_regex': {
          Promise.resolve(configureListenerMatch(command.payload || {}))
              .then((res) => cb(res || {ok: false, error: 'configure_listener_match_failed'}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_record_agent_action': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentRecordAction');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_list_timeline_failures': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentListTimelineFailures');
          cb(fn ? fn(command.payload || {}) : {ok: false, error: 'dashboard_api_unavailable'});
          return;
        }
        case 'dashboard_queue_auto_heal': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentQueueAutoHeal');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn(command.payload || {}))
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_process_auto_heal_queue': {
          const fn = resolveWindowApi(windowRef, 'dashboardAgentProcessAutoHealQueue');
          if (!fn) {
            cb({ok: false, error: 'dashboard_api_unavailable'});
            return;
          }
          Promise.resolve(fn())
              .then((res) => cb(res || {ok: true}))
              .catch((e) => cb({ok: false, error: String(e && e.message ? e.message : e)}));
          return;
        }
        case 'dashboard_handoff_to_assistant': {
          // Meta-command: runner consumes findings; no chrome.send.
          const payload =
              (command.payload && typeof command.payload === 'object') ?
                command.payload :
                {};
          cb({
            ok: true,
            handoff: true,
            findings: payload,
            status: String(payload.status || 'handoff'),
            summary: String(
                payload.user_summary || payload.diagnosis || payload.why ||
                ''),
          });
          return;
        }
        case 'run_timeline': {
          if (isInvestigatorLoopProfile(windowRef)) {
            const fn = resolveWindowApi(
                windowRef, 'dashboardAgentExecuteActionsHeadless');
            if (!fn) {
              cb({ok: false, error: 'dashboard_api_unavailable'});
              return;
            }
            let actions = [];
            if (Array.isArray(command.payload)) {
              actions = command.payload;
            } else if (command.payload &&
                       Array.isArray(command.payload.timeline)) {
              actions = command.payload.timeline;
            } else if (command.payload &&
                       Array.isArray(command.payload.actions)) {
              actions = command.payload.actions;
            }
            const body = {
              actions,
              target_id: currentInvestigateTargetId(windowRef, command.payload),
              return_context: true,
            };
            Promise.resolve(fn(body))
                .then((res) => {
                  rememberInvestigateTarget(windowRef, res);
                  cb(res || {ok: true});
                })
                .catch((e) => cb({
                  ok: false,
                  error: String(e && e.message ? e.message : e),
                }));
            return;
          }
          let timelinePayload;
          if (Array.isArray(command.payload)) {
            timelinePayload = {
              timeline: command.payload,
              is_agent_mode: true,
            };
          } else if (command.payload && typeof command.payload === 'object') {
            timelinePayload = {...command.payload, is_agent_mode: true};
          } else {
            timelinePayload = {is_agent_mode: true};
          }
          console.log('[AI] Sending command: dashboardAgentRunTimeline', timelinePayload);
          chromeSend('dashboardAgentRunTimeline', [cbToken, timelinePayload]);
          break;
        }
        case 'call_action': {
          const payload = (command.payload && typeof command.payload === 'object')
            ? command.payload
            : {};
          const action = payload.action;
          if (!action) {
            cb({ok: false, error: 'missing_action'});
            return;
          }
          const rawParams = payload.params || {};
          const params = normalizeAgentActionParams(action, rawParams);
          if (isInvestigatorLoopProfile(windowRef)) {
            const fn = resolveWindowApi(
                windowRef, 'dashboardAgentExecuteActionsHeadless');
            if (!fn) {
              cb({ok: false, error: 'dashboard_api_unavailable'});
              return;
            }
            const step = {action, params};
            const body = {
              actions: [step],
              target_id: currentInvestigateTargetId(windowRef, params),
              return_context: true,
            };
            Promise.resolve(fn(body))
                .then((res) => {
                  rememberInvestigateTarget(windowRef, res);
                  cb(res || {ok: true});
                })
                .catch((e) => cb({
                  ok: false,
                  error: String(e && e.message ? e.message : e),
                }));
            return;
          }
          console.log('[AI] Sending command: dashboardAgentCallAction', action, params);
          chromeSend('dashboardAgentCallAction', [cbToken, action, params]);
          break;
        }
        case 'eval_js':
          cb({ok: false, error: 'unsupported_command'});
          return;
        case 'refresh_pagemap':
        case 'request_more_context':
        case 'request_more_controls':
        case 'request_primary_options':
          if (isInvestigatorLoopProfile(windowRef)) {
            const fn = resolveWindowApi(windowRef, 'dashboardAgentInvestigatePage');
            if (!fn) {
              cb({ok: false, error: 'dashboard_api_unavailable'});
              return;
            }
            const lookPayload = {
              target_id: currentInvestigateTargetId(windowRef, command.payload),
              closeTarget: false,
            };
            Promise.resolve(fn(lookPayload))
                .then((res) => {
                  const slim = applyInvestigateContextDiff(res || {});
                  rememberInvestigateTarget(windowRef, slim);
                  cb(slim);
                })
                .catch((e) => cb({
                  ok: false,
                  error: String(e && e.message ? e.message : e),
                }));
            return;
          }
          console.log('[AI] Sending command: dashboardAgentRefreshPageMap');
          onRefreshPageMapRequested();
          chromeSend('dashboardAgentRefreshPageMap', [cbToken, command.payload || {}]);
          break;
        case 'reload_page':
          if (isInvestigatorLoopProfile(windowRef)) {
            const fn = resolveWindowApi(windowRef, 'dashboardAgentInvestigatePage');
            if (!fn) {
              cb({ok: false, error: 'dashboard_api_unavailable'});
              return;
            }
            const url = String(command.payload?.url || '').trim();
            Promise.resolve(fn({
              ...(url ? {url} : {}),
              target_id: currentInvestigateTargetId(windowRef, command.payload),
              closeTarget: false,
            })).then((res) => {
              rememberInvestigateTarget(windowRef, res);
              cb(applyInvestigateContextDiff(res || {}));
            }).catch((e) => cb({
              ok: false,
              error: String(e && e.message ? e.message : e),
            }));
            return;
          }
          console.log('[AI] Sending command: dashboardAgentReloadPage', command.payload || {});
          chromeSend('dashboardAgentReloadPage', [cbToken, command.payload || {}]);
          break;
        case 'screenshot':
        case 'request_screenshot':
          if (isInvestigatorLoopProfile(windowRef)) {
            const fn = resolveWindowApi(windowRef, 'dashboardAgentInvestigatePage');
            if (!fn) {
              cb({ok: false, error: 'dashboard_api_unavailable'});
              return;
            }
            Promise.resolve(fn({
              target_id: currentInvestigateTargetId(windowRef, command.payload),
              include_screenshot: true,
              includeScreenshot: true,
              closeTarget: false,
            })).then((res) => {
              rememberInvestigateTarget(windowRef, res);
              cb(applyInvestigateContextDiff(res || {}));
            }).catch((e) => cb({
              ok: false,
              error: String(e && e.message ? e.message : e),
            }));
            return;
          }
          console.log('[AI] Sending command: dashboardAgentScreenshot');
          chromeSend('dashboardAgentScreenshot', [cbToken, {}]);
          break;
        case 'agent_finish':
          cb({
            ok: true,
            done: true,
            status: String(command.payload?.status || 'done'),
            summary: String(command.payload?.summary || ''),
          });
          return;
        default:
          console.warn('AI sent an unknown command:', command.command);
          cb({ok: false, error: `unknown_command:${command.command}`});
          return;
      }
    } catch (e) {
      console.error('Failed to send command to agent:', e);
      cb({
        ok: false,
        error: String(e && e.message ? e.message : e),
        failure_stage: 'dispatch',
        error_class: 'command_send_failed',
      });
    }
  };

  const sendCommandToAgent = (command) => {
    return new Promise((resolve) => {
      const normalized = normalizeCommandForDispatch(command);
      if (!normalized || !normalized.command) {
        resolve({ok: false, error: 'missing_command'});
        return;
      }
      if (normalized.command === 'inspect_element') {
        sendInspectCommand(normalized).then(resolve).catch((e) => {
          resolve({ok: false, error: String(e && e.message ? e.message : e)});
        });
        return;
      }
      const entry = {command: normalized, resolve};
      if (commandInFlight) {
        appendMessage(
            'assistant',
            `[Agent] Busy; will run '${normalized?.command || 'command'}' after the current task.`);
        commandQueue.push(entry);
        return;
      }
      dispatchNext(entry);
    });
  };

  return {
    sendCommandToAgent,
  };
}

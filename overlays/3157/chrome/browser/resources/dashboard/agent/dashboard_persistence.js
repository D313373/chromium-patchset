export function createDashboardPersistenceManager({
  appState,
  dashboardHandler,
  normalizeScriptForSave,
  normalizeScriptsDict,
  renderAll,
  getNestedActionList,
  getLoopStepsList,
  getKeyDownNestedList,
  validateAction = null,
  alertFn = (msg) => alert(msg),
}) {
  const AUTOSAVE_DEBOUNCE_MS = 450;
  const autosaveState = {
    timers: new Map(),
    inFlight: new Map(),
    pending: new Set(),
    inFlightWaiters: new Map(),
  };

  function resolveInFlightWaiters(scriptId) {
    const waiters = autosaveState.inFlightWaiters.get(scriptId);
    if (!waiters || !waiters.size) return;
    autosaveState.inFlightWaiters.delete(scriptId);
    for (const resolve of waiters) {
      try { resolve(true); } catch (_) {}
    }
  }

  function waitForInFlightToFinish(scriptId, timeoutMs = 5000) {
    if (!autosaveState.inFlight.get(scriptId)) {
      return Promise.resolve(true);
    }
    return new Promise((resolve) => {
      const waiters =
          autosaveState.inFlightWaiters.get(scriptId) || new Set();
      autosaveState.inFlightWaiters.set(scriptId, waiters);
      let settled = false;
      const done = (ok) => {
        if (settled) return;
        settled = true;
        clearTimeout(timerId);
        waiters.delete(done);
        if (!waiters.size) {
          autosaveState.inFlightWaiters.delete(scriptId);
        }
        resolve(ok);
      };
      waiters.add(done);
      const timerId = setTimeout(() => done(false), timeoutMs);
    });
  }

  function countPersistableActionsInScript(script) {
    const countList = (list) => {
      if (!Array.isArray(list)) return 0;
      let n = 0;
      for (const a of list) {
        if (!a || typeof a !== 'object') continue;
        if (a.__draft) continue;
        n++;
        const t = String(a?.action || '').toLowerCase();
        if (t === 'if_condition') {
          n += countList(getNestedActionList(a, 'then', false));
          n += countList(getNestedActionList(a, 'else', false));
        } else if (t === 'loop') {
          n += countList(getLoopStepsList(a, false));
        } else if (t === 'key_down' || t === 'key') {
          n += countList(getKeyDownNestedList(a, false));
        } else if (t === 'authenticate') {
          const methods = a.params && Array.isArray(a.params.methods) ?
            a.params.methods : [];
          for (const method of methods) {
            if (method && method.custom === true) n += countList(method.actions);
          }
        }
      }
      return n;
    };
    if (!script || typeof script !== 'object') return 0;
    if (Array.isArray(script.actions) && script.actions.length) {
      return countList(script.actions);
    }
    let n = 0;
    if (Array.isArray(script.url_contexts)) {
      for (const ctx of script.url_contexts) {
        n += countList(ctx?.actions);
      }
    }
    return n;
  }

  function scriptHasDraftActions(script) {
    const hasDraftInList = (list) => {
      if (!Array.isArray(list)) return false;
      for (const a of list) {
        if (a && typeof a === 'object' && a.__draft) return true;
        const t = String(a?.action || '').toLowerCase();
        if (t === 'if_condition') {
          if (hasDraftInList(getNestedActionList(a, 'then', false)) ||
              hasDraftInList(getNestedActionList(a, 'else', false))) return true;
        } else if (t === 'loop') {
          if (hasDraftInList(getLoopStepsList(a, false))) return true;
        } else if (t === 'key_down' || t === 'key') {
          if (hasDraftInList(getKeyDownNestedList(a, false))) return true;
        } else if (t === 'authenticate') {
          const methods = a.params && Array.isArray(a.params.methods) ?
            a.params.methods : [];
          for (const method of methods) {
            if (method && method.custom === true &&
                hasDraftInList(method.actions)) return true;
          }
        }
      }
      return false;
    };
    if (!script || typeof script !== 'object') return false;
    if (Array.isArray(script.actions) && script.actions.length) {
      return hasDraftInList(script.actions);
    }
    if (Array.isArray(script.url_contexts)) {
      for (const ctx of script.url_contexts) {
        if (hasDraftInList(ctx?.actions)) return true;
      }
    }
    return false;
  }

  function collectPersistableActions(script) {
    const out = [];
    const visitList = (list, pathPrefix = '') => {
      if (!Array.isArray(list)) return;
      let visibleIndex = 0;
      for (const action of list) {
        if (!action || typeof action !== 'object') continue;
        if (action.__draft) continue;
        visibleIndex += 1;
        const path = pathPrefix ? `${pathPrefix}.${visibleIndex}` : `${visibleIndex}`;
        out.push({action, path});
        const type = String(action?.action || '').toLowerCase();
        if (type === 'if_condition') {
          visitList(getNestedActionList(action, 'then', false), `${path}.then`);
          visitList(getNestedActionList(action, 'else', false), `${path}.else`);
        } else if (type === 'loop') {
          visitList(getLoopStepsList(action, false), `${path}.steps`);
        } else if (type === 'key_down' || type === 'key') {
          visitList(getKeyDownNestedList(action, false), `${path}.nested`);
        } else if (type === 'authenticate') {
          const methods = action.params && Array.isArray(action.params.methods) ?
            action.params.methods : [];
          for (const method of methods) {
            if (method && method.custom === true) {
              visitList(method.actions, `${path}.${method.id}`);
            }
          }
        }
      }
    };

    if (!script || typeof script !== 'object') return out;
    if (Array.isArray(script.actions) && script.actions.length) {
      visitList(script.actions);
      return out;
    }
    if (Array.isArray(script.url_contexts)) {
      for (const ctx of script.url_contexts) {
        visitList(ctx?.actions);
      }
    }
    return out;
  }

  async function collectInvalidActionReports(script, limit = 12) {
    if (typeof validateAction !== 'function') return [];
    const reports = [];
    for (const {action, path} of collectPersistableActions(script)) {
      try {
        const result = await validateAction(action);
        if (result?.valid === false) {
          reports.push({
            path,
            action: String(action?.action || 'unknown'),
            error: String(result?.error || 'invalid'),
          });
        }
      } catch (e) {
        reports.push({
          path,
          action: String(action?.action || 'unknown'),
          error: `validate crashed (${String(e?.message || e)})`,
        });
      }
      if (reports.length >= limit) break;
    }
    return reports;
  }

  function cancelAutoSaveTimer(scriptId) {
    if (!scriptId) return;
    const prev = autosaveState.timers.get(scriptId);
    if (prev) clearTimeout(prev);
    autosaveState.timers.delete(scriptId);
  }

  async function saveScriptById(
      scriptId,
      {silent = true, refreshAfter = false, returnStatus = false} = {}) {
    if (!scriptId) return returnStatus ? {ok: false, reason: 'missing_script'} : false;
    const currentScript = appState.scripts[scriptId];
    if (!currentScript) {
      return returnStatus ? {ok: false, reason: 'missing_script'} : false;
    }

    const origCount = countPersistableActionsInScript(currentScript);
    const invalidReports = await collectInvalidActionReports(currentScript, 12);
    if (invalidReports.length) {
      console.warn('[DashboardUI] save continuing with invalid action(s)', {
        scriptId,
        invalidReports,
      });
    }

    let normalized = null;
    try {
      normalized = normalizeScriptForSave(currentScript);
    } catch (e) {
      console.error('[DashboardUI] normalizeScriptForSave failed:', e);
      if (!silent) {
        try { alert(`Save failed: ${e?.message || e}`); } catch (_) {}
      }
      return returnStatus ? {
        ok: false,
        reason: 'normalize_failed',
        error: String(e?.message || e),
      } : false;
    }

    const normCount = countPersistableActionsInScript(normalized);
    let savePayload = normalized;
    if (normCount < origCount) {
      console.warn('[DashboardUI] save bypassing normalization drop guard', {
        origCount,
        normCount,
        scriptId,
      });
      if (!silent) {
        try {
          alertFn(
              `This script still has ${origCount - normCount} invalid or legacy action(s).\n\n` +
              'The script will still be saved, but the malformed actions are being ' +
              'preserved as-is. Open the flagged actions and repair them when convenient.');
        } catch (_) {}
      }
      savePayload = JSON.parse(JSON.stringify(currentScript));
    }

    if (!Array.isArray(savePayload.url_contexts)) {
      savePayload.url_contexts = [];
    }

    delete savePayload.__localUnsavedChanges;

    appState.scripts[scriptId] = savePayload;

    await dashboardHandler.saveScript(scriptId, savePayload);

    if (refreshAfter) {
      try {
        const list = await dashboardHandler.getScripts();
        if (list && list.scripts) {
          const prevName = appState.scripts[scriptId]?.name || '';
          appState.scripts = normalizeScriptsDict(list.scripts || {});
          if (scriptId && appState.scripts[scriptId]) {
            appState.currentScriptId = scriptId;
            if (prevName && !appState.scripts[scriptId].name) {
              appState.scripts[scriptId].name = prevName;
            }
          }
          renderAll();
        }
      } catch (e) {
        console.warn('[DashboardUI] getScripts after save failed', e);
      }
    }

    return returnStatus ? {
      ok: true,
      invalidReports,
    } : true;
  }

  async function performAutoSave(scriptId, reason) {
    if (!scriptId) return;
    const s = appState.scripts?.[scriptId];
    if (scriptHasDraftActions(s)) return;
    if (autosaveState.inFlight.get(scriptId)) {
      autosaveState.pending.add(scriptId);
      return;
    }
    autosaveState.inFlight.set(scriptId, true);
    autosaveState.pending.delete(scriptId);
    try {
      await saveScriptById(scriptId, {silent: true, refreshAfter: false});
    } catch (e) {
      console.error('[DashboardUI] autosave failed', {scriptId, reason, error: e});
    } finally {
      autosaveState.inFlight.set(scriptId, false);
      resolveInFlightWaiters(scriptId);
      if (autosaveState.pending.has(scriptId)) {
        scheduleAutoSave(scriptId, 'pending', AUTOSAVE_DEBOUNCE_MS);
      }
    }
  }

  function scheduleAutoSave(
      scriptId, reason = 'edit', delayMs = AUTOSAVE_DEBOUNCE_MS) {
    if (!scriptId) return;
    cancelAutoSaveTimer(scriptId);
    const s = appState.scripts?.[scriptId];
    if (scriptHasDraftActions(s)) return;
    const t = setTimeout(() => {
      autosaveState.timers.delete(scriptId);
      performAutoSave(scriptId, reason);
    }, Math.max(0, delayMs));
    autosaveState.timers.set(scriptId, t);
  }

  async function flushPendingSavesAndSaveNow(scriptId) {
    if (!scriptId) return false;
    cancelAutoSaveTimer(scriptId);
    if (autosaveState.inFlight.get(scriptId)) {
      await waitForInFlightToFinish(scriptId, 5000);
    }
    return await saveScriptById(scriptId, {silent: true, refreshAfter: false});
  }

  return {
    AUTOSAVE_DEBOUNCE_MS,
    cancelAutoSaveTimer,
    flushPendingSavesAndSaveNow,
    performAutoSave,
    saveScriptById,
    scheduleAutoSave,
    scriptHasDraftActions,
  };
}

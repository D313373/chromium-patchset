export function markDraftAction(action) {
  if (!action || typeof action !== 'object') return action;
  action.__draft = true;
  if (!action.__draftCreatedAt) action.__draftCreatedAt = Date.now();
  return action;
}

export function getNestedActionList(action, key, create = false) {
  if (!action || typeof action !== 'object') return null;
  const keyText = String(key || '');
  if (keyText.startsWith('method:')) {
    const methodId = keyText.slice('method:'.length);
    const methods = action.params && Array.isArray(action.params.methods) ?
      action.params.methods : null;
    if (!methods) return null;
    const method = methods.find((item) => item && item.id === methodId);
    if (!method || method.custom !== true) return null;
    if (!Array.isArray(method.actions)) {
      if (!create) return null;
      method.actions = [];
    }
    return method.actions;
  }
  if (Array.isArray(action[key])) return action[key];
  const fromParams =
    action.params && typeof action.params === 'object' &&
    Array.isArray(action.params[key]) ? action.params[key] : null;
  if (fromParams) {
    action[key] = fromParams;
    return action[key];
  }
  if (create) {
    action[key] = [];
    return action[key];
  }
  return null;
}

export function getLoopStepsList(action, create = false) {
  const direct = getNestedActionList(action, 'steps', false);
  if (Array.isArray(direct)) return direct;
  if (create) {
    action.steps = [];
    return action.steps;
  }
  return null;
}

export function getKeyDownNestedList(action, create = false) {
  const direct = getNestedActionList(action, 'nested', false);
  if (Array.isArray(direct)) return direct;
  if (create) {
    action.nested = [];
    return action.nested;
  }
  return null;
}

export function materializeControlFlowChildren(action) {
  if (!action || typeof action !== 'object') return action;
  repairLegacyActionInPlace(action);
  const t = String(action.action || '').toLowerCase();
  if (t === 'if_condition') {
    const thenList = getNestedActionList(action, 'then', true) || [];
    const elseList = getNestedActionList(action, 'else', true) || [];
    for (const child of thenList) materializeControlFlowChildren(child);
    for (const child of elseList) materializeControlFlowChildren(child);
  } else if (t === 'loop') {
    const steps = getLoopStepsList(action, true) || [];
    for (const child of steps) materializeControlFlowChildren(child);
  } else if (t === 'key_down') {
    const nested = getKeyDownNestedList(action, true) || [];
    for (const child of nested) materializeControlFlowChildren(child);
  } else if (t === 'key') {
    const nested = getKeyDownNestedList(action, true) || [];
    for (const child of nested) materializeControlFlowChildren(child);
  } else if (t === 'authenticate') {
    const methods = action.params && Array.isArray(action.params.methods) ?
      action.params.methods : [];
    for (const method of methods) {
      if (!method || method.custom !== true) continue;
      if (!Array.isArray(method.actions)) method.actions = [];
      for (const child of method.actions) materializeControlFlowChildren(child);
    }
  }
  return action;
}

function inferLegacyActionKind(action) {
  if (!action || typeof action !== 'object') {
    return '';
  }
  const type = String(action.action || action.type || '').trim().toLowerCase();
  const params =
    action.params && typeof action.params === 'object' ? action.params : {};
  if (type === 'cdp_drag') return 'drag';
  if (type === 'cdp_type') return 'type';
  if (type === 'cdp_key_down') return 'key_down';
  if (type && type !== 'unknown') return type;

  const hasLocators = Array.isArray(params.locators) && params.locators.length > 0;
  const hasSingleLocator = !!(params.locator && typeof params.locator === 'object');
  if (typeof params.url === 'string' && String(params.url).trim()) return 'navigate';
  if (typeof params.key === 'string' && String(params.key).trim()) return 'key_press';
  if ((Array.isArray(params.fromLocators) && params.fromLocators.length) ||
      (Array.isArray(params.toLocators) && params.toLocators.length) ||
      (params.fromCoordinates && typeof params.fromCoordinates === 'object') ||
      (params.toCoordinates && typeof params.toCoordinates === 'object') ||
      (params.fromCoords && typeof params.fromCoords === 'object') ||
      (params.toCoords && typeof params.toCoords === 'object')) {
    return 'drag';
  }
  if (params.conditionType || Array.isArray(action.then) ||
      Array.isArray(action.else) || Array.isArray(params.then) ||
      Array.isArray(params.else)) {
    return 'if_condition';
  }
  if (params.loopMode || Array.isArray(action.steps) || Array.isArray(params.steps)) {
    return 'loop';
  }
  if (typeof params.condition === 'string' ||
      typeof params.onMatchScriptId === 'string' ||
      typeof params.captureVariable === 'string') {
    return 'listen';
  }
  if (params.waitType || params.time != null || params.timeout != null) {
    return 'wait';
  }
  if (params.distance != null || params.percentX != null || params.percentY != null) {
    return 'scroll';
  }
  if ((Array.isArray(params.fields) && params.fields.length) ||
      (Array.isArray(params.rowLocators) && params.rowLocators.length)) {
    return 'scrape_table';
  }
  if (typeof params.variableName === 'string' &&
      (hasLocators || hasSingleLocator)) {
    return 'scrape_element';
  }
  if (typeof params.text === 'string' && String(params.text).length) {
    return 'type';
  }
  if (hasLocators || hasSingleLocator) {
    const clickCount = Number(
      params.clickCount ?? params.click_count ?? (params.hoverOnly ? 0 : 1));
    if (params.hoverOnly || clickCount === 0) return 'hover';
    return 'click';
  }
  return type || 'unknown';
}

function repairLegacyActionInPlace(action) {
  if (!action || typeof action !== 'object') {
    return action;
  }
  if (!action.params || typeof action.params !== 'object') {
    action.params = {};
  }
  const repaired = inferLegacyActionKind(action);
  if (repaired) {
    action.action = repaired;
  }
  return action;
}

export function flattenActionsFromUrlContexts(urlContexts) {
  const out = [];
  if (!Array.isArray(urlContexts)) {
    return out;
  }
  for (const ctx of urlContexts) {
    if (!ctx || typeof ctx !== 'object') {
      continue;
    }
    const acts = Array.isArray(ctx.actions) ? ctx.actions : [];
    for (const a of acts) {
      if (a && typeof a === 'object') {
        out.push(a);
      }
    }
  }
  return out;
}

function readActionContextUrl(action) {
  return String(action?.contextUrl || action?.context_url || '').trim();
}

function readNavigateActionUrl(action) {
  if (String(action?.action || '').toLowerCase() !== 'navigate') {
    return '';
  }
  return String(action?.params?.url || '').trim();
}

export function buildUrlContextsFromActions(actions, startUrl = '') {
  const out = [];
  if (!Array.isArray(actions) || !actions.length) {
    return out;
  }
  let currentUrl = String(startUrl || '').trim();
  for (const action of actions) {
    if (!action || typeof action !== 'object') {
      continue;
    }
    const explicitContextUrl = readActionContextUrl(action);
    const navigateUrl = readNavigateActionUrl(action);
    const contextUrl = explicitContextUrl || navigateUrl || currentUrl;
    const last = out[out.length - 1];
    if (!last || String(last.url || '') !== contextUrl) {
      out.push({
        url: contextUrl,
        actions: [],
      });
    }
    out[out.length - 1].actions.push(action);
    if (navigateUrl) {
      currentUrl = navigateUrl;
    } else if (explicitContextUrl) {
      currentUrl = explicitContextUrl;
    }
  }
  return out;
}

export function normalizeScriptsDict(raw) {
  const out = {};
  if (!raw || typeof raw !== 'object') return out;
  for (const [id, script] of Object.entries(raw)) {
    if (!script || typeof script !== 'object') continue;
    const name = String(script.name || `Script ${id}`);
    const rawUrlContexts = Array.isArray(script.url_contexts)
      ? script.url_contexts.map(ctx => ({
          url: String(ctx?.url || ''),
          name: ctx?.name || undefined,
          note: ctx?.note || undefined,
          actions: Array.isArray(ctx?.actions) ? ctx.actions : []
        }))
      : [];
    const primaryActions =
      Array.isArray(script.actions) && script.actions.length
        ? script.actions
        : flattenActionsFromUrlContexts(rawUrlContexts);
    const normalizedActions = Array.isArray(primaryActions)
      ? primaryActions
          .map(a => (a && typeof a === 'object')
            ? materializeControlFlowChildren(repairLegacyActionInPlace(a))
            : null)
          .filter(Boolean)
      : [];
    const url_contexts = buildUrlContextsFromActions(
      normalizedActions, script.startUrl);
    out[id] = {
      name,
      kind: script.kind || undefined,
      startUrl: (typeof script.startUrl === 'string') ? script.startUrl : undefined,
      keyScriptId: script.keyScriptId || undefined,
      url_contexts: url_contexts.length ? url_contexts : rawUrlContexts,
      actions: normalizedActions,
      settings: script.settings || {},
      settingsOverrides: script.settingsOverrides || {}
    };
  }
  return out;
}

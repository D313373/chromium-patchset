export function createDashboardScriptNormalizer({
  parseVisualCoords,
  formatVisualCoords,
  flattenActionsFromUrlContexts,
  buildUrlContextsFromActions,
} = {}) {
  const safeJsonClone = (value, fallback = null) => {
    try {
      return JSON.parse(JSON.stringify(value ?? fallback));
    } catch (_) {
      return fallback;
    }
  };
  const parseCoords =
    (typeof parseVisualCoords === 'function') ? parseVisualCoords : () => null;
  const formatCoords =
    (typeof formatVisualCoords === 'function') ?
      formatVisualCoords :
      ((coords) => JSON.stringify(coords || {x: 0, y: 0}));
  const flattenActions =
    (typeof flattenActionsFromUrlContexts === 'function') ?
      flattenActionsFromUrlContexts :
      (() => []);
  const buildUrlContexts =
    (typeof buildUrlContextsFromActions === 'function') ?
      buildUrlContextsFromActions :
      (() => []);

  function inferLegacyActionKind(rawType, params, action = null) {
    const type = String(rawType || '').trim().toLowerCase();
    const p = (params && typeof params === 'object') ? params : {};
    if (type === 'cdp_drag') return 'drag';
    if (type === 'cdp_type') return 'type';
    if (type === 'cdp_key_down') return 'key_down';
    if (type && type !== 'unknown') return type;

    const hasLocators = Array.isArray(p.locators) && p.locators.length > 0;
    const hasSingleLocator = !!(p.locator && typeof p.locator === 'object');
    if (typeof p.url === 'string' && String(p.url).trim()) return 'navigate';
    if (typeof p.key === 'string' && String(p.key).trim()) return 'key_press';
    if ((Array.isArray(p.fromLocators) && p.fromLocators.length) ||
        (Array.isArray(p.toLocators) && p.toLocators.length) ||
        (p.fromCoordinates && typeof p.fromCoordinates === 'object') ||
        (p.toCoordinates && typeof p.toCoordinates === 'object') ||
        (p.fromCoords && typeof p.fromCoords === 'object') ||
        (p.toCoords && typeof p.toCoords === 'object')) {
      return 'drag';
    }
    if (p.conditionType || Array.isArray(action?.then) || Array.isArray(action?.else) ||
        Array.isArray(p.then) || Array.isArray(p.else)) {
      return 'if_condition';
    }
    if (p.loopMode || Array.isArray(action?.steps) || Array.isArray(p.steps)) {
      return 'loop';
    }
    if (typeof p.condition === 'string' || typeof p.onMatchScriptId === 'string' ||
        typeof p.captureVariable === 'string') {
      return 'listen';
    }
    if (p.waitType || p.time != null || p.timeout != null) {
      return 'wait';
    }
    if (p.distance != null || p.percentX != null || p.percentY != null) {
      return 'scroll';
    }
    if ((Array.isArray(p.fields) && p.fields.length) ||
        (Array.isArray(p.rowLocators) && p.rowLocators.length)) {
      return 'scrape_table';
    }
    if (typeof p.variableName === 'string' && (hasLocators || hasSingleLocator)) {
      return 'scrape_element';
    }
    if (typeof p.text === 'string' && String(p.text).length) {
      return 'type';
    }
    if (hasLocators || hasSingleLocator) {
      const clickCount = Number(
          p.clickCount ?? p.click_count ?? (p.hoverOnly ? 0 : 1));
      if (p.hoverOnly || clickCount === 0) return 'hover';
      return 'click';
    }
    return type || 'unknown';
  }

  function canonicalizeLocatorType(t) {
    if (!t) return 'CSS_SELECTOR';
    const u = String(t).toUpperCase();
    switch (u) {
      case 'ID': case 'CSS_SELECTOR': case 'NAME': case 'ACCESSIBILITY_NAME':
      case 'CLASS_NAME': case 'JS_EVAL': case 'SHADOW_DOM': case 'LINK_TEXT':
      case 'PARTIAL_LINK_TEXT': case 'TAG_NAME': case 'XPATH': case 'TEXT_CONTENT':
      case 'VISUAL_COORDS': case 'VISUAL_AI': case 'FRAME_ID': case 'RESOURCE_TREE':
        return u;
    }
    if (u === 'CSS' || u === 'SELECTOR' || u === 'CSS-SELECTOR') return 'CSS_SELECTOR';
    if (u === 'CLASS' || u === 'CLASS-NAME') return 'CLASS_NAME';
    if (u === 'A11Y' || u === 'ARIA' || u === 'ACCESSIBILITY') return 'ACCESSIBILITY_NAME';
    if (u === 'LINK' || u === 'LINKTEXT' || u === 'LINK-TEXT') return 'LINK_TEXT';
    if (u === 'TAG') return 'TAG_NAME';
    if (u === 'TEXT' || u === 'INNER_TEXT' || u === 'INNERTEXT') return 'TEXT_CONTENT';
    if (u === 'JS' || u === 'JAVASCRIPT') return 'JS_EVAL';
    if (u === 'SHADOW' || u === 'SHADOW-DOM') return 'SHADOW_DOM';
    if (u === 'COORDS' || u === 'COORDINATES') return 'VISUAL_COORDS';
    if (u === 'AI' || u === 'VISUAL-AI') return 'VISUAL_AI';
    if (u === 'FRAME' || u === 'FRAME_ID') return 'FRAME_ID';
    if (u === 'RESOURCE' || u === 'RESOURCE_TREE') return 'RESOURCE_TREE';
    return 'CSS_SELECTOR';
  }

  function normalizeLayerParams(params) {
    if (!params || typeof params !== 'object') return params;
    if (!params.frame_id && params.frameId) params.frame_id = params.frameId;
    if (!params.shadow_chain && params.shadowChain) params.shadow_chain = params.shadowChain;
    if (!params.shadow_chain && params.shadowChainSelector) {
      params.shadow_chain = params.shadowChainSelector;
    }
    if (!params.resource_path && params.resourcePath) {
      params.resource_path = params.resourcePath;
    }
    if (!params.ax_role && params.axRole) params.ax_role = params.axRole;
    if (!params.ax_name && params.axName) params.ax_name = params.axName;
    if (!params.interactive_ancestor_role &&
        params.interactiveAncestorRole) {
      params.interactive_ancestor_role = params.interactiveAncestorRole;
    }
    return params;
  }

  function extractLayerHintsFromLocators(locators) {
    const out = [];
    let frame_id = '';
    let shadow_chain = '';
    if (!Array.isArray(locators)) return {locators: [], frame_id, shadow_chain};
    for (const l of locators) {
      const t = canonicalizeLocatorType(l?.type);
      const v = String(l?.value || '');
      if (t === 'FRAME_ID' && v && !frame_id) {
        frame_id = v;
        continue;
      }
      if (t === 'SHADOW_DOM' && v && !shadow_chain) {
        shadow_chain = v;
        continue;
      }
      out.push(l);
    }
    return {locators: out, frame_id, shadow_chain};
  }

  function normalizeLocators(locs) {
    if (!Array.isArray(locs)) return [];
    return locs
      .map((l) => {
        const type = canonicalizeLocatorType(l?.type);
        if (type === 'VISUAL_COORDS') {
          const coords = parseCoords(l?.value);
          const value = coords ? formatCoords(coords) : '';
          return {type, value};
        }
        return {type, value: String(l?.value || '').trim()};
      })
      .filter((l) => !!l.value)
      .map((l) => {
        if (l.type === 'ID' && l.value.startsWith('#')) l.value = l.value.slice(1);
        return l;
      });
  }

  function normalizeLoopProfile(rawProfile) {
    const value = String(rawProfile || '').trim().toLowerCase();
    if (!value) return '';
    if (value === 'task' || value === 'task_completer' ||
        value === 'task-completer' || value === 'glic' ||
        value === 'glic-task') {
      return 'glic_task';
    }
    if (value === 'assistant' || value === 'dashboard' ||
        value === 'dashboard-assistant') {
      return 'dashboard_assistant';
    }
    if (value === 'investigate' || value === 'error_investigator' ||
        value === 'error-investigator') {
      return 'investigator';
    }
    if (value === 'glic_task' || value === 'dashboard_assistant' ||
        value === 'investigator') {
      return value;
    }
    return '';
  }

  function normalizeActionList(list, options = {}) {
    return Array.isArray(list)
      ? list.map((action) => normalizeAction(action, options)).filter(Boolean)
      : [];
  }

  function normalizeAction(a, options = {}) {
    if (!a || typeof a !== 'object') return null;
    const preserveDrafts = !!options.preserveDrafts;
    if (preserveDrafts && a.__draft) {
      return safeJsonClone(a, null);
    }
    const params =
      a.params && typeof a.params === 'object' ? {...a.params} : {};
    const type = inferLegacyActionKind(a.action, params, a);
    if (!type) return null;
    const contextUrl = String(a.contextUrl || a.context_url || '').trim();
    const out = {
      action: type,
      params,
      nonCritical: !!a.nonCritical,
      comment: a.comment || '',
    };
    if (contextUrl) {
      out.contextUrl = contextUrl;
    }
    switch (type) {
      case 'navigate': {
        params.url = String(params.url || '').trim();
        if (!params.url) return null;
        break;
      }
      case 'click':
      case 'hover': {
        params.locators = normalizeLocators(params.locators);
        if (!params.locators.length) {
          const coords = parseCoords(params.coordinates);
          const fromXY = parseCoords({x: params.x, y: params.y});
          const merged = coords || fromXY;
          if (merged) {
            params.locators = [{
              type: 'VISUAL_COORDS',
              value: formatCoords(merged),
            }];
          }
        }
        delete params.coordinates;
        delete params.x;
        delete params.y;
        if (!params.locators.length) return null;
        params.waitTimeout =
          Number.isFinite(params.waitTimeout) ? params.waitTimeout : 15000;
        params.humanOffset = !!params.humanOffset;
        const defaultClickCount = (type === 'hover') ? 0 : 1;
        const minClickCount = (type === 'hover') ? 0 : 1;
        const parsedClickCount = parseInt(
          params.clickCount ?? params.click_count ?? defaultClickCount,
          10);
        params.clickCount = Number.isFinite(parsedClickCount)
          ? Math.min(3, Math.max(minClickCount, parsedClickCount))
          : defaultClickCount;
        delete params.click_count;
        if (type === 'hover') {
          params.clickCount = 0;
        }
        break;
      }
      case 'drag': {
        params.fromLocators = normalizeLocators(
          Array.isArray(params.fromLocators) ? params.fromLocators : params.locators
        );
        params.toLocators = normalizeLocators(params.toLocators);
        delete params.locators;

        const fromCoords =
          parseCoords(params.fromCoordinates) ||
          parseCoords(params.fromCoords) ||
          parseCoords({x: params.fromX, y: params.fromY});
        const toCoords =
          parseCoords(params.toCoordinates) ||
          parseCoords(params.toCoords) ||
          parseCoords({x: params.toX, y: params.toY});

        if (fromCoords) {
          params.fromCoordinates = {x: fromCoords.x, y: fromCoords.y};
        } else {
          delete params.fromCoordinates;
        }
        if (toCoords) {
          params.toCoordinates = {x: toCoords.x, y: toCoords.y};
        } else {
          delete params.toCoordinates;
        }
        delete params.fromCoords;
        delete params.toCoords;
        delete params.fromX;
        delete params.fromY;
        delete params.toX;
        delete params.toY;

        if (!params.fromLocators.length && !fromCoords) return null;
        if (!params.toLocators.length && !toCoords) return null;

        const parsedSteps = parseInt(params.steps ?? 12, 10);
        params.steps = Number.isFinite(parsedSteps)
          ? Math.min(80, Math.max(2, parsedSteps))
          : 12;
        params.waitTimeout =
          Number.isFinite(params.waitTimeout) ? params.waitTimeout : 15000;
        params.humanOffset =
          (params.humanOffset === undefined) ? true : !!params.humanOffset;
        break;
      }
      case 'type': {
        params.locators = normalizeLocators(params.locators);
        params.text = String(params.text || '');
        params.waitTimeout =
          Number.isFinite(params.waitTimeout) ? params.waitTimeout : 15000;
        if (!params.text) return null;
        break;
      }
      case 'key_press': {
        params.key = String(params.key || '').trim();
        if (!params.key) return null;
        const parsedCount = parseInt(params.count ?? 1, 10);
        params.count = Number.isFinite(parsedCount)
          ? Math.max(1, parsedCount)
          : 1;
        break;
      }
      case 'wait': {
        let waitType = params.waitType || params.wait_type || '';
        if (!waitType) {
          waitType =
            (Array.isArray(params.locators) && params.locators.length) ||
              params.timeout != null ?
              'element' :
              'time';
        }
        waitType = String(waitType).toLowerCase();
        if (waitType === 'time') {
          params.waitType = 'time';
          params.time = Number.isFinite(params.time) ? params.time : 1000;
          if (params.time <= 0) return null;
        } else if (waitType === 'element') {
          params.waitType = 'element';
          params.locators = normalizeLocators(params.locators);
          params.timeout = Number.isFinite(params.timeout) ? params.timeout : 15000;
          if (!params.locators.length) return null;
        } else {
          return null;
        }
        delete params.wait_type;
        break;
      }
      case 'if_condition': {
        let conditionType =
          params.conditionType || params.condition_type || '';
        if (!conditionType) {
          conditionType = String(params.text || '').trim() ?
            'text_contains' :
            'element_exists';
        }
        conditionType = String(conditionType).toLowerCase();
        if (conditionType !== 'element_exists' &&
            conditionType !== 'text_contains') {
          conditionType = String(params.text || '').trim() ?
            'text_contains' :
            'element_exists';
        }
        params.conditionType = conditionType;
        delete params.condition_type;
        if (conditionType === 'element_exists') {
          params.locators = normalizeLocators(params.locators);
        } else {
          params.text = String(params.text || '').trim();
        }
        const thenRaw = Array.isArray(a.then)
          ? a.then
          : (Array.isArray(params.then) ? params.then : []);
        const elseRaw = Array.isArray(a.else)
          ? a.else
          : (Array.isArray(params.else) ? params.else : []);
        out.then = normalizeActionList(thenRaw, options);
        out.else = normalizeActionList(elseRaw, options);
        delete params.then;
        delete params.else;
        break;
      }
      case 'loop': {
        const stepsRaw = Array.isArray(a.steps)
          ? a.steps
          : (Array.isArray(params.steps) ? params.steps : []);
        out.steps = normalizeActionList(stepsRaw, options);
        const rawMode = String(
            params.loopMode ?? params.conditionType ?? '').toLowerCase();
        const mode = (rawMode === 'while_element_exists') ?
          'while_element_exists' :
          ((rawMode === 'for_each') ? 'for_each' : 'count');
        params.loopMode = mode;
        if (mode === 'count') {
          const n = parseInt(
              params.iterations ?? params.count ?? params.maxIterations ?? 1,
              10);
          params.iterations = Number.isFinite(n) ? Math.max(1, n) : 1;
          if (!Number.isFinite(Number(params.maxIterations))) {
            params.maxIterations = params.iterations;
          } else {
            params.maxIterations = Math.max(
                params.iterations, parseInt(params.maxIterations, 10));
          }
          delete params.locators;
        } else {
          params.locators = normalizeLocators(params.locators);
          const maxN = parseInt(
              params.maxIterations ?? (mode === 'for_each' ? 200 : 50), 10);
          params.maxIterations = Number.isFinite(maxN) ?
            Math.max(1, maxN) :
            (mode === 'for_each' ? 200 : 50);
          if (mode === 'for_each') {
            const strategy = String(
                params.forEachStrategy || '').toLowerCase();
            params.forEachStrategy =
              (strategy === 'requery_each_iteration') ?
              'requery_each_iteration' :
              'snapshot_once';
          } else {
            delete params.forEachStrategy;
          }
          delete params.iterations;
        }
        delete params.steps;
        break;
      }
      case 'key_down':
      case 'key': {
        const nestedRaw = Array.isArray(a.nested)
          ? a.nested
          : (Array.isArray(params.nested) ? params.nested : []);
        out.nested = normalizeActionList(nestedRaw, options);
        delete params.nested;
        break;
      }
      case 'scrape_element': {
        params.locators = normalizeLocators(params.locators);
        if (!params.locators.length) return null;
        params.variableName = String(params.variableName || 'result');
        break;
      }
      case 'scrape_table': {
        params.rowLocators = normalizeLocators(
          Array.isArray(params.rowLocators) ? params.rowLocators : params.locators
        );
        delete params.locators;

        const variableName = String(params.variableName || '').trim();
        if (variableName) {
          params.variableName = variableName;
        } else {
          delete params.variableName;
        }
        const rawFields = Array.isArray(params.fields) ? params.fields : [];
        params.fields = rawFields
          .map((field) => {
            const next = (field && typeof field === 'object') ? {...field} : {};
            next.name = String(next.name || '').trim();
            next.locators = normalizeLocators(next.locators);
            return next;
          })
          .filter((field) =>
              Array.isArray(field.locators) && field.locators.length);
        if (!params.rowLocators.length || !params.fields.length) {
          return null;
        }

        const mode = String(params.rowSelectionMode || '').toLowerCase();
        if (mode === 'index') {
          params.rowSelectionMode = 'index';
          const spec = String(params.rowIndex ?? '1').trim();
          params.rowIndex = spec || '1';
        } else {
          params.rowSelectionMode = 'all';
          delete params.rowIndex;
        }

        params.saveCsv = params.saveCsv !== false;
        params.appendCsv = params.saveCsv ? !!params.appendCsv : false;
        params.csvFileName = params.saveCsv
          ? String(params.csvFileName || '').trim()
          : '';
        break;
      }
      case 'glic_prompt': {
        const rawFocusAreas = Array.isArray(params.focusAreas) ?
          params.focusAreas :
          (Array.isArray(params.locators) && params.locators.length ?
            [{label: 'Primary focus', locators: params.locators}] : []);
        params.focusAreas = rawFocusAreas
          .map((area) => {
            const next = area && typeof area === 'object' ? {...area} : {};
            next.label = String(next.label || '').trim();
            next.locators = normalizeLocators(next.locators);
            normalizeLayerParams(next);
            ['frame_id', 'shadow_chain', 'resource_path', 'ax_role', 'ax_name',
              'interactive_ancestor_role'].forEach((key) => {
              if (typeof next[key] === 'string') {
                next[key] = next[key].trim();
                if (!next[key]) delete next[key];
              }
            });
            return next;
          })
          .filter((area) => {
            return area.label ||
              (Array.isArray(area.locators) && area.locators.length) ||
              ['frame_id', 'shadow_chain', 'resource_path', 'ax_role', 'ax_name',
                'interactive_ancestor_role']
                .some((key) => !!area[key]);
          });
        delete params.locators;
        params.focusOnlyContext = !!params.focusOnlyContext;
        params.model = String(params.model || params.modelOverride || '').trim();
        if (!params.model) {
          delete params.model;
        }
        delete params.modelOverride;
        params.loopProfile = normalizeLoopProfile(
          params.loopProfile || params.profile || '');
        if (!params.loopProfile) {
          delete params.loopProfile;
        }
        delete params.profile;
        params.prompt = String(params.prompt || '').trim();
        if (!params.prompt) return null;
        break;
      }
      case 'listen': {
        params.locators = normalizeLocators(params.locators);
        if (!params.locators.length) return null;
        params.condition = String(params.condition || '').trim().toLowerCase();
        if (!params.condition) return null;
        params.text = String(params.text || '').trim();
        if (!params.text) delete params.text;
        params.textUseRegex = !!params.textUseRegex;
        if (!params.textUseRegex) delete params.textUseRegex;
        params.attribute = String(params.attribute || '').trim();
        if (!params.attribute) delete params.attribute;
        params.attributeUseRegex = !!params.attributeUseRegex;
        if (!params.attributeUseRegex) delete params.attributeUseRegex;

        const captureVariable = String(params.captureVariable || '').trim();
        if (captureVariable) {
          params.captureVariable = captureVariable;
        } else {
          delete params.captureVariable;
        }
        const rawFanOut =
          (params.fanOutPerMatchedUrl == null)
            ? params.fan_out_per_matched_url
            : params.fanOutPerMatchedUrl;
        params.fanOutPerMatchedUrl =
          (rawFanOut == null) ? true : !!rawFanOut;
        delete params.fan_out_per_matched_url;

        const onMatchScriptId = String(params.onMatchScriptId || '').trim();
        if (onMatchScriptId) {
          params.onMatchScriptId = onMatchScriptId;
        } else {
          delete params.onMatchScriptId;
        }

        const timeoutMs = parseInt(
          params.timeout_ms ?? params.timeoutMs ?? 60000,
          10);
        params.timeout_ms = Number.isFinite(timeoutMs)
          ? Math.max(1, timeoutMs)
          : 60000;
        delete params.timeoutMs;

        const executionFrequencyMs = parseInt(params.executionFrequencyMs, 10);
        if (Number.isFinite(executionFrequencyMs) && executionFrequencyMs > 0) {
          params.executionFrequencyMs = executionFrequencyMs;
        } else {
          delete params.executionFrequencyMs;
        }

        const sampleIntervalMs = parseInt(
          params.sampleIntervalMs ?? params.sample_interval_ms,
          10);
        if (Number.isFinite(sampleIntervalMs) && sampleIntervalMs > 0) {
          params.sampleIntervalMs = sampleIntervalMs;
        } else {
          delete params.sampleIntervalMs;
        }
        delete params.sample_interval_ms;

        const triggerThrottleMs = parseInt(
          params.triggerThrottleMs ?? params.trigger_throttle_ms,
          10);
        if (Number.isFinite(triggerThrottleMs) && triggerThrottleMs > 0) {
          params.triggerThrottleMs = triggerThrottleMs;
        } else {
          delete params.triggerThrottleMs;
        }
        delete params.trigger_throttle_ms;

        params.autoRefresh = !!params.autoRefresh;
        if (!params.autoRefresh) delete params.autoRefresh;
        params.refreshJitter = !!params.refreshJitter;
        if (!params.refreshJitter) delete params.refreshJitter;
        params.extremeEvasion = !!params.extremeEvasion;
        if (!params.extremeEvasion) delete params.extremeEvasion;

        [
          'initialScrollPercentX',
          'initialScrollPercentY',
          'refreshCycleScrollPercentX',
          'refreshCycleScrollPercentY',
        ].forEach((key) => {
          const raw = Number(params[key]);
          if (!Number.isFinite(raw)) {
            delete params[key];
            return;
          }
          const normalized = Math.max(-100, Math.min(100, Math.trunc(raw)));
          if (normalized === 0) {
            delete params[key];
            return;
          }
          params[key] = normalized;
        });
        break;
      }
      case 'authenticate': {
        const methods = Array.isArray(params.methods) ? params.methods : [];
        const keepRef = (value) => {
          const text = String(value || '').trim();
          if (!text || text === 'third_party') return text;
          return /^\{[^{}]+\}$/.test(text) ? text : '';
        };
        params.methods = methods.filter((method) => method && typeof method === 'object')
            .map((method) => {
              const next = {...method};
              next.enabled = next.enabled !== false;
              const captcha = next.captcha && typeof next.captcha === 'object' ?
                {...next.captcha} :
                {enabled: false, brand: 'recaptcha', challenge: 'checkbox'};
              captcha.enabled = !!captcha.enabled;
              captcha.brand = String(captcha.brand || 'recaptcha');
              captcha.challenge = String(captcha.challenge || 'checkbox');
              next.captcha = captcha;
              next.steps = (Array.isArray(next.steps) ? next.steps : [])
                  .filter((item) => item && typeof item === 'object')
                  .map((item) => {
                    const step = {...item};
                    step.enabled = step.enabled !== false;
                    if (step.kind === 'username' || step.kind === 'password') {
                      step.token = keepRef(step.token);
                    }
                    if (step.kind === 'two_factor') {
                      step.emailToken = keepRef(step.emailToken);
                    }
                    return step;
                  });
              if (next.custom === true) {
                next.custom = true;
                next.actions = normalizeActionList(next.actions, options);
              }
              return next;
            });
        if (!params.methods.length) return null;
        break;
      }
      case 'scroll': {
        if (Array.isArray(params.locators)) {
          params.locators = normalizeLocators(params.locators);
        }
        if (Number.isFinite(params.distance)) {
          params.distance = parseInt(params.distance, 10);
        } else {
          delete params.distance;
        }
        params.percentY = Math.max(0, Math.min(100, parseInt(params.percentY || 0, 10)));
        params.percentX = Math.max(0, Math.min(100, parseInt(params.percentX || 0, 10)));
        break;
      }
      default:
        break;
    }
    return out;
  }

  function normalizeScriptForSave(script) {
    const src = (script && typeof script === 'object') ? script : {};
    const normalizeList = (list) =>
      (Array.isArray(list)
        ? normalizeActionList(list, {preserveDrafts: true})
        : []);
    const out = {
      name: String(src.name || 'Untitled'),
      kind: (typeof src.kind === 'string') ? src.kind : undefined,
      settings: safeJsonClone(src.settings, {}),
      settingsOverrides: safeJsonClone(src.settingsOverrides, undefined),
      actions: normalizeList(src.actions),
    };
    if (src.startUrl != null) {
      const su = String(src.startUrl || '').trim();
      if (su) {
        out.startUrl = su;
      }
    }

    if (!out.actions.length && Array.isArray(src.url_contexts)) {
      out.actions = normalizeList(flattenActions(src.url_contexts));
    }
    out.url_contexts = buildUrlContexts(out.actions, out.startUrl || '');

    if (src.keyScriptId != null) {
      const ks = String(src.keyScriptId || '').trim();
      out.keyScriptId = ks || undefined;
    }
    return out;
  }

  return {
    canonicalizeLocatorType,
    extractLayerHintsFromLocators,
    normalizeAction,
    normalizeLayerParams,
    normalizeScriptForSave,
  };
}

import {appendAuthenticateBranches} from '../actions/authenticate_timeline.js';

export function createDashboardTimelineRenderer({
  actionMenuEl,
  timelineEl,
  appState,
  actionMetadata,
  flattenActionsFromUrlContexts,
  getNestedActionList,
  getLoopStepsList,
  getKeyDownNestedList,
  markDraftAction,
  createNewActionFromType,
  cancelAutoSaveTimer,
  scheduleAutoSave,
  renderAll,
  formatActionTitle,
  alertFn = (msg) => alert(msg),
}) {
  function getActionKind(action) {
    return String(action?.action || action?.type || '').toLowerCase();
  }

  function getActionParams(action) {
    return action && typeof action.params === 'object' ? action.params : {};
  }

  function getActionContextUrl(action) {
    const raw = String(action?.contextUrl || action?.context_url || '').trim();
    if (!raw) {
      return '';
    }
    try {
      const url = new URL(raw);
      return url.href;
    } catch (_) {
      return raw;
    }
  }

  function formatContextChipLabel(rawUrl) {
    const raw = String(rawUrl || '').trim();
    if (!raw) {
      return '';
    }
    try {
      const url = new URL(raw);
      const host = url.host || url.hostname || '';
      let path = String(url.pathname || '');
      if (path === '/') path = '';
      if (url.search) path += '?...';
      const compact = `${host}${path}`;
      return compact.length > 34 ? `${compact.slice(0, 34)}...` : compact;
    } catch (_) {
      return raw.length > 34 ? `${raw.slice(0, 34)}...` : raw;
    }
  }

  function getLocatorSummary(locators) {
    if (!Array.isArray(locators) || !locators.length) {
      return 'locator pending';
    }
    const preferred = locators.find((loc) => {
      const type = String(loc?.type || '').toUpperCase();
      return type === 'ACCESSIBILITY_NAME' || type === 'LINK_TEXT' ||
          type === 'TEXT_CONTENT' || type === 'CSS_SELECTOR' ||
          type === 'XPATH';
    }) || locators[0];
    const type = String(preferred?.type || 'LOCATOR').toUpperCase();
    const value = String(preferred?.value || '').trim();
    if (!value) {
      return type;
    }
    const shown = value.length > 88 ? `${value.slice(0, 88)}...` : value;
    return `${type}: ${shown}`;
  }

  function isRootScopeLocator(locator) {
    const type = String(locator?.type || '').toUpperCase();
    const value = String(locator?.value || '').trim().toLowerCase();
    if (!type || !value) {
      return false;
    }
    if (type === 'CSS_SELECTOR') {
      return value === 'body' || value === 'html';
    }
    if (type === 'XPATH') {
      return value === '//body' || value === '/html/body' ||
        value === '//html' || value === '/html';
    }
    return false;
  }

  function isRootScopeLocatorList(locators) {
    return Array.isArray(locators) && locators.length > 0 &&
      locators.every((locator) => isRootScopeLocator(locator));
  }

  function getLoopStepList(action) {
    if (Array.isArray(action?.steps)) {
      return action.steps;
    }
    const params = getActionParams(action);
    return Array.isArray(params.steps) ? params.steps : [];
  }

  function getGuidedMeta(action) {
    const params = getActionParams(action);
    const meta = action && typeof action.meta === 'object' ? action.meta : {};
    if (meta.guided && typeof meta.guided === 'object') {
      return meta.guided;
    }
    if (params.guidedMeta && typeof params.guidedMeta === 'object') {
      return params.guidedMeta;
    }
    return {};
  }

  function getLocatorPreviewValue(locators) {
    if (!Array.isArray(locators) || !locators.length) {
      return {
        text: 'locator pending',
        title: 'locator pending',
      };
    }
    const preferredTypes = [
      'TEXT_CONTENT',
      'ACCESSIBILITY_NAME',
      'LINK_TEXT',
      'PARTIAL_LINK_TEXT',
      'VALUE',
      'PLACEHOLDER',
      'NAME',
      'ID',
      'CSS_SELECTOR',
      'XPATH',
      'TAG_NAME',
    ];
    const preferred = preferredTypes.map((type) => {
      return locators.find((locator) => String(locator?.type || '').toUpperCase() === type);
    }).find(Boolean) || locators[0];
    const type = String(preferred?.type || 'LOCATOR').toUpperCase();
    const value = String(preferred?.value || '').trim();
    if (!value) {
      return {text: type, title: type};
    }
    const isPlainValue = type === 'TEXT_CONTENT' || type === 'ACCESSIBILITY_NAME' ||
      type === 'LINK_TEXT' || type === 'PARTIAL_LINK_TEXT' || type === 'VALUE';
    const fullText = isPlainValue ? value : `${type}: ${value}`;
    return {
      text: fullText,
      title: fullText,
    };
  }

  function getCanonicalTimelineActions(currentScript) {
    if (!currentScript || typeof currentScript !== 'object') {
      return [];
    }
    const primary = Array.isArray(currentScript.actions)
      ? currentScript.actions
      : [];
    if (primary.length) {
      return primary;
    }
    return flattenActionsFromUrlContexts(currentScript.url_contexts);
  }

  function openActionEditor(target, extra = {}) {
    const {
      action,
      index,
      parentType,
      parentIndex,
      scopeType,
      scopeIndex,
      listPath,
    } = target;
    if (!action) {
      return;
    }
    const safeClone = (v) => {
      try {
        return JSON.parse(JSON.stringify(v ?? null));
      } catch (_) {
        return null;
      }
    };
    const original = safeClone(action);
    const editorAddedDraftFlag =
      !(action && typeof action === 'object' && action.__draft);
    if (editorAddedDraftFlag) {
      markDraftAction(action);
    }
    appState.isEditingAction = true;
    appState.editingActionData = {
      action,
      index,
      parentType,
      parentIndex,
      scopeType,
      scopeIndex,
      listPath: Array.isArray(listPath) ? [...listPath] : null,
      isNewAction: false,
      original,
      editorAddedDraftFlag,
      ...extra,
    };
    renderAll();
  }

  function makePreviewButton(text, title, onClick) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'timeline-preview-link';
    button.textContent = text;
    button.title = title || text;
    button.addEventListener('click', (e) => {
      e.stopPropagation();
      onClick();
    });
    return button;
  }

  function buildLoopTargetPreview(action, target) {
    if (getActionKind(action) !== 'loop') {
      return null;
    }
    const params = getActionParams(action);
    if (String(params.loopMode || '').toLowerCase() !== 'for_each') {
      return null;
    }
    const hasScrapeTableStep = getLoopStepList(action)
      .some((step) => getActionKind(step) === 'scrape_table');
    if (!hasScrapeTableStep) {
      return null;
    }
    const locators = Array.isArray(params.locators) ? params.locators : [];
    if (!locators.length) {
      return null;
    }
    const preview = document.createElement('div');
    preview.className = 'timeline-action-preview timeline-loop-target-preview';

    const label = document.createElement('span');
    label.className = 'timeline-action-preview-label';
    label.textContent = 'Loop target';
    preview.appendChild(label);

    const summary = getLocatorSummary(locators);
    preview.appendChild(makePreviewButton(
        summary,
        `Edit loop target locators\n${summary}`,
        () => openActionEditor(target, {focusParamKey: 'locators'})));

    const guidedMeta = getGuidedMeta(
        getLoopStepList(action).find((step) => getActionKind(step) === 'scrape_table'));
    const targetCount = Number(guidedMeta.loopTargetCount || params.maxIterations || 0);
    if (Number.isFinite(targetCount) && targetCount > 0) {
      const countChip = document.createElement('span');
      countChip.className = 'timeline-preview-meta';
      countChip.textContent = `${targetCount} targets`;
      preview.appendChild(countChip);
    }
    return preview;
  }

  function buildScrapeTablePreview(action, target) {
    if (getActionKind(action) !== 'scrape_table') {
      return null;
    }
    const params = getActionParams(action);
    const fields = Array.isArray(params.fields) ? params.fields : [];
    if (!fields.length) {
      return null;
    }
    const preview = document.createElement('div');
    preview.className = 'timeline-action-preview timeline-scrape-preview';

    const rowLocators = Array.isArray(params.rowLocators) ? params.rowLocators :
      (Array.isArray(params.locators) ? params.locators : []);
    if (rowLocators.length) {
      const scopeRow = document.createElement('div');
      scopeRow.className = 'timeline-scrape-preview-scope';
      const scopeLabel = document.createElement('span');
      scopeLabel.className = 'timeline-action-preview-label';
      scopeLabel.textContent = isRootScopeLocatorList(rowLocators) ?
        'Detail page scope' :
        'Record scope';
      scopeRow.appendChild(scopeLabel);
      const scopeSummary = isRootScopeLocatorList(rowLocators) ?
        'Current detail page body' :
        getLocatorSummary(rowLocators);
      scopeRow.appendChild(makePreviewButton(
          scopeSummary,
          `Edit scrape-table scope locators\n${scopeSummary}`,
          () => openActionEditor(target, {focusParamKey: 'rowLocators'})));
      preview.appendChild(scopeRow);
    }

    const tableWrap = document.createElement('div');
    tableWrap.className = 'timeline-scrape-preview-wrap';
    const table = document.createElement('table');
    table.className = 'timeline-scrape-preview-table';
    const thead = document.createElement('thead');
    const headRow = document.createElement('tr');
    const bodyRow = document.createElement('tr');

    fields.forEach((field, fieldIndex) => {
      const th = document.createElement('th');
      th.textContent = String(field?.name || `field_${fieldIndex + 1}`).trim() ||
        `field_${fieldIndex + 1}`;
      headRow.appendChild(th);

      const td = document.createElement('td');
      const previewValue = getLocatorPreviewValue(
          Array.isArray(field?.locators) ? field.locators : []);
      const button = makePreviewButton(
          previewValue.text,
          `${th.textContent}\n${previewValue.title}`,
          () => openActionEditor(target, {focusFieldIndex: fieldIndex}));
      button.classList.add('timeline-preview-cell-button');
      td.appendChild(button);
      bodyRow.appendChild(td);
    });

    thead.appendChild(headRow);
    table.appendChild(thead);
    const tbody = document.createElement('tbody');
    tbody.appendChild(bodyRow);
    table.appendChild(tbody);
    tableWrap.appendChild(table);
    preview.appendChild(tableWrap);
    return preview;
  }

  function parseListPath(value) {
    if (Array.isArray(value)) return value;
    if (typeof value !== 'string' || !value.trim()) return [];
    try {
      const parsed = JSON.parse(value);
      return Array.isArray(parsed) ? parsed : [];
    } catch (_) {
      return [];
    }
  }

  function nestedRuleViolationMessage(actionType, listPathValue) {
    const t = String(actionType || '').toLowerCase();
    const listPath = parseListPath(listPathValue);
    if (!listPath.length) return '';
    const insideLoop = listPath.some((seg) => String(seg || '').startsWith('steps:'));
    const insideIf = listPath.some((seg) => {
      const s = String(seg || '');
      return s.startsWith('then:') || s.startsWith('else:');
    });
    if (t === 'loop' && insideLoop) {
      return 'Nesting a loop inside another loop is not supported.';
    }
    if (t === 'if_condition' && insideIf) {
      return 'Nesting an if_condition inside another if_condition is not supported.';
    }
    const insideMethod = listPath.some((seg) => String(seg || '').startsWith('method:'));
    if (t === 'authenticate' && insideMethod) {
      return 'Nesting Authenticate inside a login method is not supported.';
    }
    return '';
  }

  function resolveTargetListByPath(currentScript, listPath, create = true) {
    if (!currentScript || !Array.isArray(listPath) || !listPath.length) return null;
    let list = null;
    const first = String(listPath[0] || '');
    if (first === 'main') {
      if (!Array.isArray(currentScript.actions) && create) currentScript.actions = [];
      list = Array.isArray(currentScript.actions) ? currentScript.actions : null;
    } else if (first.startsWith('urlctx:')) {
      const idx = parseInt(first.split(':')[1], 10);
      if (!Array.isArray(currentScript.url_contexts) || !Number.isFinite(idx) ||
          idx < 0 || idx >= currentScript.url_contexts.length) return null;
      const ctx = currentScript.url_contexts[idx];
      if (!Array.isArray(ctx.actions) && create) ctx.actions = [];
      list = Array.isArray(ctx.actions) ? ctx.actions : null;
    } else {
      return null;
    }
    if (!Array.isArray(list)) return null;
    for (let i = 1; i < listPath.length; i += 1) {
      const seg = String(listPath[i] || '');
      const colon = seg.lastIndexOf(':');
      if (colon <= 0) return null;
      const branch = seg.slice(0, colon);
      const actionIndex = parseInt(seg.slice(colon + 1), 10);
      if (!Number.isFinite(actionIndex) || actionIndex < 0 ||
          actionIndex >= list.length) return null;
      const parentAction = list[actionIndex];
      if (!parentAction || typeof parentAction !== 'object') return null;
      const nested = getNestedActionList(parentAction, branch, create);
      if (!Array.isArray(nested)) return null;
      list = nested;
    }
    return list;
  }

  function resolveTargetList(
      parentType, parentIndex, scopeType, scopeIndex, listPath = null) {
    const currentScript = appState.scripts[appState.currentScriptId];
    if (!currentScript) return null;
    let path = null;
    if (Array.isArray(listPath)) path = listPath;
    else if (typeof listPath === 'string' && listPath.trim()) {
      try {
        const parsed = JSON.parse(listPath);
        if (Array.isArray(parsed)) path = parsed;
      } catch (_) {}
    }
    if (path && path.length) {
      const byPath = resolveTargetListByPath(currentScript, path, true);
      if (Array.isArray(byPath)) return byPath;
    }
    if (parentType === 'main') return currentScript.actions;
    if (parentType === 'urlctx') {
      const ctx = (currentScript.url_contexts || [])[scopeIndex];
      return ctx ? (ctx.actions || (ctx.actions = [])) : null;
    }
    let parentAction = null;
    if (scopeType === 'urlctx') {
      const ctx = (currentScript.url_contexts || [])[scopeIndex];
      if (ctx) parentAction = (ctx.actions || [])[parentIndex];
    } else {
      parentAction = (currentScript.actions || [])[parentIndex];
    }
    if (parentAction) {
      const nested = getNestedActionList(parentAction, parentType, true);
      if (Array.isArray(nested)) return nested;
    }
    return null;
  }

  function createPlusButton(
      index, parentType, parentIndex, scopeType = 'main',
      scopeIndex = -1, listPath = null) {
    const btn = document.createElement('button');
    btn.className = 'plus-button';
    btn.textContent = '+';
    btn.dataset.index = index;
    btn.dataset.parentType = parentType;
    btn.dataset.parentIndex = parentIndex;
    btn.dataset.scopeType = scopeType;
    btn.dataset.scopeIndex = scopeIndex;
    if (Array.isArray(listPath)) {
      btn.dataset.listPath = JSON.stringify(listPath);
    }
    const lastPath = appState.lastPlusTarget?.listPath;
    const currentPath = Array.isArray(listPath) ? JSON.stringify(listPath) : '';
    if (appState.lastPlusTarget &&
        appState.lastPlusTarget.parentType === parentType &&
        appState.lastPlusTarget.parentIndex === parentIndex &&
        appState.lastPlusTarget.index == index &&
        appState.lastPlusTarget.scopeType === scopeType &&
        appState.lastPlusTarget.scopeIndex == scopeIndex &&
        lastPath === currentPath) {
      btn.style.background = 'rgba(34,197,94,0.35)';
      btn.style.border = '2px solid #22c55e';
    }
    btn.addEventListener('click', () => {
      appState.lastPlusTarget = {
        parentType,
        parentIndex,
        index,
        scopeType,
        scopeIndex,
        listPath: currentPath,
      };
      appState.isEditingAction = false;
      renderAll();
    });
    return btn;
  }

  function createActionCard(
      action, index, parentType, parentIndex, scopeType, scopeIndex,
      listPath = null, context = {}, runGroup = null) {
    const actionTarget = {
      action,
      index,
      parentType,
      parentIndex,
      scopeType,
      scopeIndex,
      listPath,
    };
    const actionCard = document.createElement('div');
    actionCard.className = 'timeline-action-card';
    actionCard.draggable = true;
    actionCard.dataset.listPath =
      Array.isArray(listPath) ? JSON.stringify(listPath) : '';
    actionCard.dataset.actionIndex = String(index);
    actionCard.addEventListener('dragstart', (e) => {
      const payload = {
        kind: 'existing_action',
        actionType: String(action?.action || '').toLowerCase(),
        listPath: Array.isArray(listPath) ? [...listPath] : null,
        index,
      };
      try {
        e.dataTransfer.setData(
            'application/x-dashboard-action', JSON.stringify(payload));
        e.dataTransfer.setData('application/json', JSON.stringify(payload));
        e.dataTransfer.setData('text/plain', String(payload.actionType || ''));
      } catch (_) {}
    });
    if (action && typeof action === 'object' && action.__draft) {
      actionCard.classList.add('draft');
    }
    actionCard.addEventListener('click', (e) => {
      if (e.target.closest('.action-controls button')) return;
      openActionEditor(actionTarget);
    });

    const contextUrl = getActionContextUrl(action);
    if (contextUrl) {
      actionCard.classList.add('has-context-chip');
      const contextChip = document.createElement('span');
      contextChip.className = 'timeline-context-chip';
      contextChip.textContent = formatContextChipLabel(contextUrl);
      contextChip.title = `Context URL: ${contextUrl}`;
      actionCard.appendChild(contextChip);
    }

    const info = document.createElement('div');
    info.className = 'action-info';
    if (runGroup && runGroup.runCount >= 2) {
      const toggle = document.createElement('button');
      toggle.className = 'timeline-run-toggle';
      toggle.type = 'button';
      toggle.title = runGroup.collapsed ?
        'Expand grouped actions' : 'Collapse grouped actions';
      toggle.setAttribute('aria-expanded', runGroup.collapsed ? 'false' : 'true');
      const chevron = document.createElement('span');
      chevron.className = `timeline-run-toggle-chevron${runGroup.collapsed ? '' : ' expanded'}`;
      chevron.textContent = '▶';
      const count = document.createElement('span');
      count.className = 'timeline-run-toggle-count';
      count.textContent = `x${runGroup.runCount}`;
      toggle.appendChild(chevron);
      toggle.appendChild(count);
      toggle.addEventListener('click', (e) => {
        e.stopPropagation();
        runGroup.toggle();
      });
      info.appendChild(toggle);
    }
    const strong = document.createElement('strong');
    strong.textContent = formatActionTitle(
        action, (scriptId) => appState?.scripts?.[scriptId]?.name || '');
    info.appendChild(strong);
    if (action && typeof action === 'object' && action.__draft) {
      const pill = document.createElement('span');
      pill.className = 'draft-pill';
      pill.textContent = 'DRAFT';
      info.appendChild(pill);
    }

    const controls = document.createElement('div');
    controls.className = 'action-controls';
    const delButton = document.createElement('button');
    delButton.className = 'delete-action-btn';
    delButton.title = 'Delete';
    delButton.textContent = 'X';
    controls.appendChild(delButton);

    const topRow = document.createElement('div');
    topRow.className = 'timeline-action-card-top';
    topRow.appendChild(info);
    topRow.appendChild(controls);

    actionCard.appendChild(topRow);
    const loopPreview = buildLoopTargetPreview(action, actionTarget);
    if (loopPreview) {
      actionCard.appendChild(loopPreview);
    }
    const scrapePreview = buildScrapeTablePreview(action, actionTarget);
    if (scrapePreview) {
      actionCard.appendChild(scrapePreview);
    }

    delButton.addEventListener('click', (e) => {
      e.stopPropagation();
      const currentScript = appState.scripts[appState.currentScriptId];
      if (!currentScript) return;
      const targetList = resolveTargetList(
          parentType, parentIndex, scopeType, scopeIndex, listPath);
      if (targetList) {
        targetList.splice(index, 1);
        scheduleAutoSave(appState.currentScriptId, 'delete-action');
        renderAll();
      }
    });
    return actionCard;
  }

  function actionRunSignature(action) {
    const type = String(action?.action || '').toLowerCase();
    if (!type) return '';
    if (type === 'if_condition' || type === 'loop' || type === 'authenticate') {
      return '';
    }
    return type;
  }

  function runGroupStateStore() {
    if (!appState.collapsedRunGroups ||
        typeof appState.collapsedRunGroups !== 'object') {
      appState.collapsedRunGroups = {};
    }
    return appState.collapsedRunGroups;
  }

  function runGroupKey(listPath, startIndex, runSig, runCount) {
    const path = Array.isArray(listPath) ? JSON.stringify(listPath) : '';
    return `${path}|${startIndex}|${runSig}|${runCount}`;
  }

  function renderActionList(
      actions, container, parentType, parentIndex = -1, scopeType = 'main',
      scopeIndex = -1, listPath = null) {
    if (!Array.isArray(listPath) || !listPath.length) {
      if (parentType === 'main') listPath = ['main'];
      else if (parentType === 'urlctx') listPath = [`urlctx:${scopeIndex}`];
      else {
        const root = scopeType === 'urlctx' ? [`urlctx:${scopeIndex}`] : ['main'];
        listPath = [...root, `${parentType}:${parentIndex}`];
      }
    }
    container.dataset.scopeType = scopeType;
    container.dataset.scopeIndex = scopeIndex;
    container.dataset.listPath = JSON.stringify(listPath);
    container.appendChild(createPlusButton(
        0, parentType, parentIndex, scopeType, scopeIndex, listPath));
    let index = 0;
    while (index < actions.length) {
      const runSig = actionRunSignature(actions[index]);
      let runCount = 1;
      if (runSig) {
        for (let j = index + 1; j < actions.length; j += 1) {
          if (actionRunSignature(actions[j]) !== runSig) break;
          runCount += 1;
        }
      }
      const groupedRun = runCount >= 2;
      let runCollapsed = false;
      let runKey = '';
      if (groupedRun) {
        const store = runGroupStateStore();
        runKey = runGroupKey(listPath, index, runSig, runCount);
        if (!(runKey in store)) {
          store[runKey] = true;
        }
        runCollapsed = !!store[runKey];
      }
      const offsets = runCollapsed ? [0] : Array.from({length: runCount}, (_, i) => i);

      for (const offset of offsets) {
        const actionIndex = index + offset;
        const action = actions[actionIndex];
        container.appendChild(createActionCard(
            action, actionIndex, parentType, parentIndex, scopeType, scopeIndex,
            listPath,
            {actions},
            groupedRun && offset === 0 ? {
              runCount,
              collapsed: runCollapsed,
              toggle: () => {
                const store = runGroupStateStore();
                store[runKey] = !store[runKey];
                renderAll();
              },
            } : null));
        if (action.action === 'if_condition') {
        const thenList = getNestedActionList(action, 'then', true) || [];
        const elseList = getNestedActionList(action, 'else', true) || [];
        const branchesContainer = document.createElement('div');
        branchesContainer.className = 'if-branches-container';
        const thenBranch = document.createElement('div');
        thenBranch.className = 'timeline-branch';
        thenBranch.dataset.parentType = 'then';
        thenBranch.dataset.parentIndex = actionIndex;
        thenBranch.dataset.scopeType = scopeType;
        thenBranch.dataset.scopeIndex = scopeIndex;
        thenBranch.dataset.listPath = JSON.stringify([...listPath, `then:${actionIndex}`]);
        thenBranch.innerHTML = '<div class="branch-header">THEN</div>';
        const elseBranch = document.createElement('div');
        elseBranch.className = 'timeline-branch';
        elseBranch.dataset.parentType = 'else';
        elseBranch.dataset.parentIndex = actionIndex;
        elseBranch.dataset.scopeType = scopeType;
        elseBranch.dataset.scopeIndex = scopeIndex;
        elseBranch.dataset.listPath = JSON.stringify([...listPath, `else:${actionIndex}`]);
        elseBranch.innerHTML = '<div class="branch-header">ELSE</div>';
        renderActionList(
            thenList, thenBranch, 'then', actionIndex, scopeType, scopeIndex,
            [...listPath, `then:${actionIndex}`]);
        renderActionList(
            elseList, elseBranch, 'else', actionIndex, scopeType, scopeIndex,
            [...listPath, `else:${actionIndex}`]);
        branchesContainer.appendChild(thenBranch);
        branchesContainer.appendChild(elseBranch);
        container.appendChild(branchesContainer);
        }
        if (action.action === 'loop') {
        const stepList = getLoopStepsList(action, true) || [];
        const loopContainer = document.createElement('div');
        loopContainer.className = 'loop-container';
        loopContainer.dataset.parentType = 'steps';
        loopContainer.dataset.parentIndex = actionIndex;
        loopContainer.dataset.scopeType = scopeType;
        loopContainer.dataset.scopeIndex = scopeIndex;
        loopContainer.dataset.listPath = JSON.stringify([...listPath, `steps:${actionIndex}`]);
        renderActionList(
            stepList, loopContainer, 'steps', actionIndex, scopeType, scopeIndex,
            [...listPath, `steps:${actionIndex}`]);
        container.appendChild(loopContainer);
        }
        if (action.action === 'key_down' || action.action === 'key') {
        const nestedList = getKeyDownNestedList(action, true) || [];
        const nestedContainer = document.createElement('div');
        nestedContainer.className = 'loop-container';
        nestedContainer.dataset.parentType = 'nested';
        nestedContainer.dataset.parentIndex = actionIndex;
        nestedContainer.dataset.scopeType = scopeType;
        nestedContainer.dataset.scopeIndex = scopeIndex;
        nestedContainer.dataset.listPath = JSON.stringify([...listPath, `nested:${actionIndex}`]);
        renderActionList(
            nestedList, nestedContainer, 'nested', actionIndex, scopeType, scopeIndex,
            [...listPath, `nested:${actionIndex}`]);
        container.appendChild(nestedContainer);
        }
        if (action.action === 'authenticate') {
          appendAuthenticateBranches(action, container, {
            scheduleAutoSave: () => scheduleAutoSave(
                appState.currentScriptId, 'authenticate'),
            renderAll,
            renderMethodTimeline: (method, host) => {
              if (!Array.isArray(method.actions)) method.actions = [];
              const methodPath = [...listPath, `method:${method.id}:${actionIndex}`];
              host.classList.add('timeline-branch');
              host.dataset.parentType = `method:${method.id}`;
              host.dataset.parentIndex = String(actionIndex);
              host.dataset.scopeType = scopeType;
              host.dataset.scopeIndex = String(scopeIndex);
              host.dataset.listPath = JSON.stringify(methodPath);
              renderActionList(
                  method.actions, host, `method:${method.id}`, actionIndex,
                  scopeType, scopeIndex, methodPath);
            },
          });
        }
        if (!runCollapsed) {
          container.appendChild(createPlusButton(
              actionIndex + 1, parentType, parentIndex, scopeType, scopeIndex,
              listPath));
        }
      }
      if (runCollapsed) {
        container.appendChild(createPlusButton(
            index + runCount, parentType, parentIndex, scopeType, scopeIndex,
            listPath));
      }

      index += runCount;
    }
  }

  function renderTimeline() {
    timelineEl.innerHTML = '';
    const currentScript = appState.scripts[appState.currentScriptId];
    if (!currentScript) {
      console.warn('[DashboardUI] renderTimeline: no currentScript');
      return;
    }
    const actions = getCanonicalTimelineActions(currentScript);
    renderActionList(actions, timelineEl, 'main', -1, 'main', -1, ['main']);
  }

  function createUrlContextSeparator(upperIndex, lowerIndex) {
    const sep = document.createElement('div');
    sep.className = 'url-context-separator';
    sep.dataset.upperIndex = upperIndex;
    sep.dataset.lowerIndex = lowerIndex;
    sep.addEventListener('dragover', (e) => {
      e.preventDefault();
      const r = sep.getBoundingClientRect();
      const topHalf = (e.clientY - r.top) < (r.height / 2);
      sep.classList.toggle('top-hover', topHalf);
      sep.classList.toggle('bottom-hover', !topHalf);
    });
    sep.addEventListener('dragleave', () => {
      sep.classList.remove('top-hover');
      sep.classList.remove('bottom-hover');
    });
    sep.addEventListener('drop', async (e) => {
      e.preventDefault();
      const currentScript = appState.scripts[appState.currentScriptId];
      if (!currentScript || !Array.isArray(currentScript.url_contexts)) return;
      const r = sep.getBoundingClientRect();
      const topHalf = (e.clientY - r.top) < (r.height / 2);
      const upper = parseInt(sep.dataset.upperIndex, 10);
      const lower = parseInt(sep.dataset.lowerIndex, 10);
      const targetGroupIndex = topHalf ? upper : lower;
      if (isNaN(targetGroupIndex) || targetGroupIndex < 0 ||
          targetGroupIndex > currentScript.url_contexts.length - (topHalf ? 0 : 1)) {
        return;
      }
      const actionType = e.dataTransfer && e.dataTransfer.getData('text/plain');
      if (!actionType) return;
      const newAction = markDraftAction(await createNewActionFromType(actionType));
      let ctxIndex = 0;
      let actionIndex = 0;
      if (topHalf && upper >= 0) {
        const list = currentScript.url_contexts[upper].actions ||
            (currentScript.url_contexts[upper].actions = []);
        list.push(newAction);
        ctxIndex = upper;
        actionIndex = list.length - 1;
      } else if (!topHalf && lower >= 0 && lower < currentScript.url_contexts.length) {
        const list = currentScript.url_contexts[lower].actions ||
            (currentScript.url_contexts[lower].actions = []);
        list.unshift(newAction);
        ctxIndex = lower;
        actionIndex = 0;
      } else if (upper === -1 && lower === 0) {
        const list = currentScript.url_contexts[0].actions ||
            (currentScript.url_contexts[0].actions = []);
        list.unshift(newAction);
        ctxIndex = 0;
        actionIndex = 0;
      }
      cancelAutoSaveTimer(appState.currentScriptId);
      appState.isEditingAction = true;
      appState.editingActionData = {
        action: newAction,
        index: actionIndex,
        parentType: 'urlctx',
        parentIndex: ctxIndex,
        scopeType: 'urlctx',
        scopeIndex: ctxIndex,
        listPath: [`urlctx:${ctxIndex}`],
        isNewAction: true,
        original: null,
        editorAddedDraftFlag: false,
      };
      renderAll();
    });
    return sep;
  }

  function renderUrlContextTimeline(urlContexts) {
    timelineEl.innerHTML = '';
    const formatUrlLabel = (u) => {
      try {
        const url = new URL(u);
        return (url.host + url.pathname).slice(0, 60) +
            (url.pathname.length > 60 ? '...' : '');
      } catch {
        return (u || '').slice(0, 60);
      }
    };
    timelineEl.appendChild(createUrlContextSeparator(-1, 0));
    urlContexts.forEach((ctx, i) => {
      const header = document.createElement('div');
      header.className = 'url-context-header';
      header.title = ctx.url || '';
      header.textContent = ctx.url ? formatUrlLabel(ctx.url) : '(initial)';
      timelineEl.appendChild(header);

      const groupBox = document.createElement('div');
      groupBox.className = 'url-context-group';
      groupBox.style.background = (i % 2 === 0) ?
        'rgba(0,0,0,0.02)' : 'rgba(0,0,0,0.05)';
      timelineEl.appendChild(groupBox);
      renderActionList(
          ctx.actions || [], groupBox, 'urlctx', i, 'urlctx', i, [`urlctx:${i}`]);
      timelineEl.appendChild(createUrlContextSeparator(i, i + 1));
    });
  }

  function renderActionMenu() {
    actionMenuEl.innerHTML = '<div class="panel-header">Action Menu</div>';
    for (const actionType in actionMetadata) {
      const meta = actionMetadata[actionType] || {};
      if (meta.display_in_gui === false) continue;
      const item = document.createElement('div');
      item.className = 'action-menu-item';
      item.draggable = true;
      item.dataset.actionType = actionType;
      item.innerHTML =
        `<span>${(meta.name || actionType).toUpperCase()}</span>` +
        `<div class="tooltip-container">?<span class="tooltip-text">` +
        `${meta.description || ''}</span></div>`;
      actionMenuEl.appendChild(item);

      item.addEventListener('click', async () => {
        const t = appState.lastPlusTarget;
        if (!t) {
          alertFn('Click a green + in the timeline to choose where to insert.');
          return;
        }
        const currentScript = appState.scripts[appState.currentScriptId];
        if (!currentScript) return;
        const list = resolveTargetList(
            t.parentType, t.parentIndex, t.scopeType, t.scopeIndex, t.listPath);
        if (!list) return;
        const violation = nestedRuleViolationMessage(actionType, t.listPath || '');
        if (violation) {
          alertFn(violation);
          return;
        }
        const newAction = markDraftAction(await createNewActionFromType(actionType));
        list.splice(t.index, 0, newAction);
        cancelAutoSaveTimer(appState.currentScriptId);
        appState.isEditingAction = true;
        appState.editingActionData = {
          action: newAction,
          index: t.index,
          parentType: t.parentType,
          parentIndex: t.parentIndex,
          scopeType: t.scopeType || 'main',
          scopeIndex: Number.isFinite(t.scopeIndex) ? t.scopeIndex : -1,
          listPath: t.listPath || null,
          isNewAction: true,
          original: null,
          editorAddedDraftFlag: false,
        };
        appState.lastPlusTarget = null;
        renderAll();
      });
    }
  }

  return {
    parseListPath,
    nestedRuleViolationMessage,
    resolveTargetListByPath,
    resolveTargetList,
    renderActionMenu,
    renderTimeline,
    renderUrlContextTimeline,
  };
}

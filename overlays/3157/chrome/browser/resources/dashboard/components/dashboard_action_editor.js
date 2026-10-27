export function createDashboardActionEditorController({
  appState,
  actionEditorEl,
  actionMetadata,
  renderLayerEditor,
  renderLocatorEditor,
  scheduleAutoSave,
  saveScriptById,
  dashboardHandler,
  refreshSmartInputCatalog,
  resolveTargetList,
  renderAll,
  normalizeLayerParams,
  extractLayerHintsFromLocators,
  canonicalizeLocatorType,
  normalizeAction,
  validateAction,
  onScriptsChanged,
  alertFn = (msg) => alert(msg),
  documentRef = document,
}) {
  const byId = (id) => documentRef.getElementById(id);
  const CREATE_NEW_TRIGGER_SCRIPT_VALUE = '__create_new_trigger_script__';

  function highlightEditorTarget(el, focusInput = null) {
    if (!el) {
      return;
    }
    el.style.borderColor = 'var(--main-blue)';
    el.style.boxShadow = '0 0 0 1px color-mix(in srgb, var(--main-blue) 55%, transparent)';
    el.style.background =
      'color-mix(in srgb, var(--panel-bg) 82%, var(--btn-hover-bg) 18%)';
    requestAnimationFrame(() => {
      try {
        el.scrollIntoView({block: 'nearest', behavior: 'smooth'});
      } catch (_) {}
      try {
        focusInput?.focus?.();
      } catch (_) {}
    });
  }

  function readFieldsFromDOM(listContainer) {
    const fields = [];
    listContainer.querySelectorAll('.field-item').forEach((item) => {
      const name = item.querySelector('.field-name-input')?.value || '';
      let locators = [];
      if (item.__locatorEditor) {
        locators = item.__locatorEditor.getValue();
      }
      fields.push({name, locators});
    });
    return fields;
  }

  function renderLocatorListEditor(container, locators, onChange, captureKey = '') {
    container.innerHTML = '';
    const listContainer = documentRef.createElement('div');
    listContainer.className = 'locator-list';
    container.appendChild(listContainer);

    const editor = renderLocatorEditor(
      listContainer,
      locators || [],
      (value) => {
        try {
          if (typeof onChange === 'function') onChange(value);
        } catch (_) {}
      },
      {
        captureKey,
        onDataVariablesChanged: refreshSmartInputCatalog,
      },
    );
    container.__locatorEditor = editor;
  }

  function renderFieldListEditor(container, fields, onChange = null) {
    const notifyChange = (listContainer) => {
      if (typeof onChange !== 'function') {
        return;
      }
      try {
        onChange(readFieldsFromDOM(listContainer));
      } catch (_) {}
    };

    const listContainer = documentRef.createElement('div');
    listContainer.className = 'field-list';

    (fields || []).forEach((field, index) => {
      const itemEl = documentRef.createElement('div');
      itemEl.className = 'field-item';
      itemEl.dataset.fieldIndex = String(index);

      const nameInput = documentRef.createElement('smart-input');
      nameInput.className = 'input-field field-name-input mb-2';
      nameInput.placeholder = 'Field Name (e.g., product_name)';
      nameInput.value = field.name || '';

      const locatorsLabel = documentRef.createElement('label');
      locatorsLabel.textContent = 'Locators for this field:';
      locatorsLabel.className = 'text-xs text-gray-400 mb-1';

      const locatorsContainer = documentRef.createElement('div');
      locatorsContainer.dataset.fieldName = field.name || '';

      const editorHost = documentRef.createElement('div');
      locatorsContainer.appendChild(editorHost);
      const editor = renderLocatorEditor(
        editorHost,
        field.locators || [],
        () => notifyChange(listContainer),
        {
          captureKey: `field-list:${index}:locators`,
          onDataVariablesChanged: refreshSmartInputCatalog,
        },
      );
      itemEl.__locatorEditor = editor;
      nameInput.addEventListener('input-change', () => notifyChange(listContainer));

      const removeBtn = documentRef.createElement('button');
      removeBtn.className = 'btn btn-danger w-full mt-2';
      removeBtn.textContent = 'Remove Field';
      removeBtn.onclick = () => {
        const currentFields = readFieldsFromDOM(listContainer);
        currentFields.splice(index, 1);
        renderFieldListEditor(container, currentFields, onChange);
        if (typeof onChange === 'function') {
          try {
            onChange(currentFields);
          } catch (_) {}
        }
        scheduleAutoSave(appState.currentScriptId, 'delete-field');
      };

      itemEl.appendChild(nameInput);
      itemEl.appendChild(locatorsLabel);
      itemEl.appendChild(locatorsContainer);
      itemEl.appendChild(removeBtn);
      if (Number(appState.editingActionData?.focusFieldIndex) === index) {
        highlightEditorTarget(itemEl, nameInput);
      }
      listContainer.appendChild(itemEl);
    });

    const addBtn = documentRef.createElement('button');
    addBtn.className = 'btn mt-2';
    addBtn.textContent = '+ Add Field';
    addBtn.onclick = () => {
      const currentFields = readFieldsFromDOM(listContainer);
      currentFields.push({name: '', locators: []});
      renderFieldListEditor(container, currentFields, onChange);
      if (typeof onChange === 'function') {
        try {
          onChange(currentFields);
        } catch (_) {}
      }
      scheduleAutoSave(appState.currentScriptId, 'add-field');
    };

    container.innerHTML = '';
    container.appendChild(listContainer);
    container.appendChild(addBtn);
    notifyChange(listContainer);
  }

  function sanitizeGlicPromptFocusLocators(locators) {
    if (!Array.isArray(locators)) return [];
    return locators.filter((locator) => {
      const type = canonicalizeLocatorType(locator?.type || '');
      return type !== 'VISUAL_COORDS';
    });
  }

  function readFocusAreasFromDOM(listContainer) {
    const areas = [];
    const isGlicPrompt =
      String(appState.editingActionData?.action?.action || '') === 'glic_prompt';
    listContainer.querySelectorAll('.focus-area-item').forEach((item) => {
      const label = String(
          item.querySelector('.focus-area-label-input')?.value || '').trim();
      let locators = item.__locatorEditor?.getValue?.() || [];
      if (isGlicPrompt) {
        locators = sanitizeGlicPromptFocusLocators(locators);
      }
      const hints = item.__layerHints || {};
      const area = {label, locators};
      const hintKeys = [
        'frame_id',
        'shadow_chain',
        'resource_path',
        'ax_role',
        'ax_name',
        'interactive_ancestor_role',
      ];
      hintKeys.forEach((key) => {
        const value = String(hints?.[key] || '').trim();
        if (value) {
          area[key] = value;
        }
      });
      if (!label && !locators.length &&
          !hintKeys.some((key) => String(area[key] || '').trim())) {
        return;
      }
      areas.push(area);
    });
    return areas;
  }

  function renderFocusAreaListEditor(container, focusAreas, onChange,
                                     onSelectionChange = null) {
    const notifyChange = (listContainer) => {
      try {
        if (typeof onChange === 'function') {
          onChange(readFocusAreasFromDOM(listContainer));
        }
      } catch (_) {}
    };

    const applySelectionState = (listContainer) => {
      listContainer.querySelectorAll('.focus-area-item').forEach((itemEl) => {
        const index = parseInt(itemEl.dataset.focusAreaIndex || '-1', 10);
        const isSelected =
          Number(appState.editingActionData?.focusAreaIndex ?? 0) === index;
        itemEl.style.background = isSelected ?
          'color-mix(in srgb, var(--panel-bg) 76%, var(--main-blue) 24%)' :
          'color-mix(in srgb, var(--panel-bg) 88%, var(--btn-hover-bg) 12%)';
        itemEl.style.boxShadow = isSelected ?
          '0 0 0 1px color-mix(in srgb, var(--main-blue) 55%, transparent)' :
          'none';
      });
    };

    const renderAllAreas = (areas) => {
      container.innerHTML = '';
      const listContainer = documentRef.createElement('div');
      listContainer.className = 'focus-area-list';
      listContainer.style.display = 'flex';
      listContainer.style.flexDirection = 'column';
      listContainer.style.gap = '12px';

      (areas || []).forEach((area, index) => {
        const itemEl = documentRef.createElement('div');
        itemEl.className = 'focus-area-item';
        itemEl.dataset.focusAreaIndex = String(index);
        itemEl.style.padding = '12px';
        itemEl.style.border = '1px solid var(--border-color)';
        itemEl.style.borderRadius = '12px';
        itemEl.style.cursor = 'pointer';
        itemEl.__layerHints = {
          frame_id: String(area?.frame_id || ''),
          shadow_chain: String(area?.shadow_chain || ''),
          resource_path: String(area?.resource_path || ''),
          ax_role: String(area?.ax_role || ''),
          ax_name: String(area?.ax_name || ''),
          interactive_ancestor_role: String(
              area?.interactive_ancestor_role || ''),
        };
        itemEl.addEventListener('click', () => {
          appState.editingActionData.focusAreaIndex = index;
          applySelectionState(listContainer);
          if (typeof onSelectionChange === 'function') {
            onSelectionChange(index, readFocusAreasFromDOM(listContainer));
          }
        });

        const headerRow = documentRef.createElement('div');
        headerRow.style.display = 'flex';
        headerRow.style.justifyContent = 'space-between';
        headerRow.style.alignItems = 'center';
        headerRow.style.gap = '12px';
        headerRow.style.marginBottom = '10px';

        const labelWrap = documentRef.createElement('div');
        labelWrap.style.flex = '1';

        const labelTitle = documentRef.createElement('label');
        labelTitle.textContent = `Element ${index + 1}`;
        labelTitle.style.display = 'block';
        labelTitle.style.fontWeight = '700';
        labelTitle.style.marginBottom = '6px';

        const labelInput = documentRef.createElement('smart-input');
        labelInput.className = 'input-field focus-area-label-input';
        labelInput.setAttribute(
            'placeholder',
            'Optional area label (e.g. Reply composer, Post actions)');
        labelInput.value = area?.label || '';
        labelInput.addEventListener('input-change',
                                    () => notifyChange(listContainer));
        labelWrap.append(labelTitle, labelInput);

        const removeBtn = documentRef.createElement('button');
        removeBtn.className = 'btn btn-danger';
        removeBtn.type = 'button';
        removeBtn.textContent = 'Remove Element';
        removeBtn.addEventListener('click', () => {
          appState.editingActionData.focusAreaIndex = Math.max(0, index - 1);
          const currentAreas = readFocusAreasFromDOM(listContainer);
          currentAreas.splice(index, 1);
          renderAllAreas(currentAreas);
          scheduleAutoSave(appState.currentScriptId, 'delete-focus-area');
        });

        headerRow.append(labelWrap, removeBtn);

        const helper = documentRef.createElement('div');
        helper.className = 'form-help';
        helper.textContent =
          'Group locators that describe the same element/region. The agent sees each group as a separate area of interest.';
        helper.style.marginBottom = '8px';

        const editorHost = documentRef.createElement('div');
        const initialLocators =
          String(appState.editingActionData?.action?.action || '') ===
              'glic_prompt' ?
            sanitizeGlicPromptFocusLocators(area?.locators || []) :
            (area?.locators || []);
        itemEl.__locatorEditor = renderLocatorEditor(
            editorHost,
            initialLocators,
            () => notifyChange(listContainer),
            {captureKey: `focus-area:${index}:locators`});
        editorHost.addEventListener('click', () => {
          appState.editingActionData.focusAreaIndex = index;
          applySelectionState(listContainer);
          if (typeof onSelectionChange === 'function') {
            onSelectionChange(index, readFocusAreasFromDOM(listContainer));
          }
        });

        itemEl.append(headerRow, helper, editorHost);
        listContainer.appendChild(itemEl);
      });

      const addBtn = documentRef.createElement('button');
      addBtn.className = 'btn mt-2';
      addBtn.type = 'button';
      addBtn.textContent = '+ Add Element';
      addBtn.addEventListener('click', () => {
        const currentAreas = readFocusAreasFromDOM(listContainer);
        currentAreas.push({label: '', locators: []});
        appState.editingActionData.focusAreaIndex = currentAreas.length - 1;
        renderAllAreas(currentAreas);
        scheduleAutoSave(appState.currentScriptId, 'add-focus-area');
      });

      container.appendChild(listContainer);
      container.appendChild(addBtn);
      container.__focusAreaEditor = {
        getValue() {
          return readFocusAreasFromDOM(listContainer);
        },
        applySelection() {
          applySelectionState(listContainer);
        },
      };
      applySelectionState(listContainer);
      notifyChange(listContainer);
    };

    renderAllAreas(focusAreas || []);
  }

  function readActionParamsFromDOM() {
    const form = byId('current-action-form');
    if (!form) return {};

    const newParams = {};
    form.querySelectorAll('.form-group').forEach((group) => {
      if (group.style.display === 'none') return;

      const paramKey = group.dataset.paramKey;
      if (!paramKey) return;
      if (group.dataset.disabled === 'true') return;

      const fieldList = group.querySelector('.field-list');
      const focusAreaList = group.querySelector('.focus-area-list');
      const keyCapture = group.querySelector('.key-capture-input');
      const smartInput = group.querySelector('smart-input');
      const checkboxInput = group.querySelector('input[type="checkbox"]');
      const numberInput = group.querySelector('input[type="number"]');
      const selectInput = group.querySelector('select');
      const textInput = (!numberInput && !selectInput) ?
          group.querySelector('input[type="text"]') : null;
      const paramType = String(group.dataset.paramType || '');
      const regexParamKey = String(group.dataset.regexParamKey || '').trim();

      if (group.__locatorEditor) {
        newParams[paramKey] = group.__locatorEditor.getValue();
      } else if (fieldList) {
        newParams[paramKey] = readFieldsFromDOM(fieldList);
      } else if (focusAreaList) {
        newParams[paramKey] = readFocusAreasFromDOM(focusAreaList);
      } else if (keyCapture) {
        newParams[paramKey] = keyCapture.value;
      } else if (smartInput) {
        if (paramType === 'number') {
          const raw = String(smartInput.value ?? '').trim();
          if (!raw) return;
          const n = Number(raw);
          if (!Number.isFinite(n)) return;
          newParams[paramKey] = n;
        } else {
          newParams[paramKey] = smartInput.value;
        }
        if (regexParamKey) {
          newParams[regexParamKey] = !!smartInput.regexEnabled;
        }
      } else if (checkboxInput) {
        newParams[paramKey] = !!checkboxInput.checked;
      } else if (numberInput) {
        const v = numberInput.valueAsNumber;
        newParams[paramKey] = Number.isFinite(v) ? v : 0;
      } else if (selectInput) {
        newParams[paramKey] = selectInput.value;
      } else if (textInput) {
        newParams[paramKey] = textInput.value;
      }
    });

    if (appState.editingActionData?.action?.action === 'click') {
      const rawClickCount = Number(newParams.clickCount ?? newParams.click_count);
      newParams.clickCount = Number.isFinite(rawClickCount) ?
        Math.min(3, Math.max(1, rawClickCount)) :
        1;
      delete newParams.click_count;
    } else if (appState.editingActionData?.action?.action === 'hover') {
      newParams.clickCount = 0;
      newParams.click_count = 0;
    } else if (appState.editingActionData?.action?.action === 'key_press') {
      const rawCount = Number(newParams.count);
      newParams.count = Number.isFinite(rawCount) ?
        Math.max(1, Math.trunc(rawCount)) :
        1;
    } else if (appState.editingActionData?.action?.action === 'listen') {
      const rawFanOut =
        (newParams.fanOutPerMatchedUrl == null) ?
          newParams.fan_out_per_matched_url :
          newParams.fanOutPerMatchedUrl;
      newParams.fanOutPerMatchedUrl =
        (rawFanOut == null) ? true : !!rawFanOut;
      delete newParams.fan_out_per_matched_url;
      [
        'initialScrollPercentX',
        'initialScrollPercentY',
        'refreshCycleScrollPercentX',
        'refreshCycleScrollPercentY',
      ].forEach((key) => {
        const raw = Number(newParams[key]);
        if (!Number.isFinite(raw)) {
          delete newParams[key];
          return;
        }
        const normalized = Math.max(-100, Math.min(100, Math.trunc(raw)));
        if (normalized === 0) {
          delete newParams[key];
          return;
        }
        newParams[key] = normalized;
      });
    }

    if (Array.isArray(newParams.focusAreas) &&
        appState.editingActionData?.action?.action === 'glic_prompt') {
      if (Array.isArray(form.__focusAreaLayerEditors)) {
        form.__focusAreaLayerEditors.forEach((editor, index) => {
          if (editor && newParams.focusAreas[index]) {
            const hints = editor.getValue();
            Object.assign(newParams.focusAreas[index], hints);
          }
        });
      } else if (form.__focusAreaLayerEditor) {
        const selectedIndex = Number.isInteger(appState.editingActionData.focusAreaIndex) ?
          appState.editingActionData.focusAreaIndex : 0;
        if (newParams.focusAreas[selectedIndex]) {
          const hints = form.__focusAreaLayerEditor.getValue();
          Object.assign(newParams.focusAreas[selectedIndex], hints);
        }
      }
    }

    normalizeLayerParams(newParams);
    if (Array.isArray(newParams.locators)) {
      newParams.locators = newParams.locators.filter((l) => {
        const t = canonicalizeLocatorType(l?.type);
        return t !== 'FRAME_ID' && t !== 'SHADOW_DOM';
      });
    }
    if (typeof newParams.frame_id === 'string' &&
        !newParams.frame_id.trim()) delete newParams.frame_id;
    if (typeof newParams.shadow_chain === 'string' &&
        !newParams.shadow_chain.trim()) delete newParams.shadow_chain;
    if (typeof newParams.resource_path === 'string' &&
        !newParams.resource_path.trim()) delete newParams.resource_path;
    if (typeof newParams.ax_role === 'string' &&
        !newParams.ax_role.trim()) delete newParams.ax_role;
    if (typeof newParams.ax_name === 'string' &&
        !newParams.ax_name.trim()) delete newParams.ax_name;
    if (typeof newParams.interactive_ancestor_role === 'string' &&
        !newParams.interactive_ancestor_role.trim()) {
      delete newParams.interactive_ancestor_role;
    }
    if (Array.isArray(newParams.focusAreas)) {
      newParams.focusAreas = newParams.focusAreas.map((area) => {
        const next = area && typeof area === 'object' ? {...area} : {};
        ['frame_id', 'shadow_chain', 'resource_path', 'ax_role', 'ax_name',
          'interactive_ancestor_role'].forEach((key) => {
          if (typeof next[key] === 'string' && !next[key].trim()) {
            delete next[key];
          }
        });
        return next;
      }).filter((area) => {
        return String(area.label || '').trim() ||
          (Array.isArray(area.locators) && area.locators.length) ||
          ['frame_id', 'shadow_chain', 'resource_path', 'ax_role', 'ax_name',
            'interactive_ancestor_role']
            .some((key) => String(area[key] || '').trim());
      });
    }

    return newParams;
  }

  function readActionContextUrlFromDOM() {
    const input = byId('action-context-url-input');
    if (!input) return '';
    return String(input.value || '').trim();
  }

  function applyActionEditorChanges() {
    const data = appState.editingActionData;
    if (!data || !data.action) return null;
    if (String(data.action.action || '') === 'authenticate') {
      return data.action;
    }
    data.action.params = readActionParamsFromDOM();
    const contextUrl = readActionContextUrlFromDOM();
    if (contextUrl) {
      data.action.contextUrl = contextUrl;
    } else {
      delete data.action.contextUrl;
    }
    delete data.action.context_url;
    return data.action;
  }

  function buildUniqueScriptName(baseName) {
    const trimmedBase = String(baseName || '').trim() || 'Listener Trigger';
    const existingNames = new Set(
      Object.values(appState.scripts || {})
        .map((script) => String(script?.name || '').trim().toLowerCase())
        .filter(Boolean),
    );
    if (!existingNames.has(trimmedBase.toLowerCase())) {
      return trimmedBase;
    }
    for (let i = 2; i < 10000; i += 1) {
      const candidate = `${trimmedBase} (${i})`;
      if (!existingNames.has(candidate.toLowerCase())) {
        return candidate;
      }
    }
    return `${trimmedBase} (${Date.now()})`;
  }

  async function resolveCreateTriggerScriptSelection(updatedAction) {
    if (!updatedAction || updatedAction.action !== 'listen') {
      return true;
    }
    const params = updatedAction.params && typeof updatedAction.params === 'object' ?
      updatedAction.params :
      null;
    if (!params) {
      return true;
    }
    const rawTarget = String(params.onMatchScriptId || '').trim();
    if (rawTarget !== CREATE_NEW_TRIGGER_SCRIPT_VALUE) {
      return true;
    }

    const listenerScript = appState.scripts?.[appState.currentScriptId] || null;
    const listenerName = String(listenerScript?.name || '').trim() || 'Listener';
    const newScriptName = buildUniqueScriptName(`${listenerName} Trigger`);
    if (!dashboardHandler || typeof dashboardHandler.createNewScript !== 'function') {
      alertFn('Script creation is unavailable right now.');
      return false;
    }

    try {
      const response = await dashboardHandler.createNewScript({name: newScriptName});
      const newScriptId = String(response?.scriptId || '').trim();
      if (!newScriptId) {
        alertFn('Failed to create trigger script.');
        return false;
      }
      if (!appState.scripts?.[newScriptId]) {
        appState.scripts[newScriptId] = {
          name: newScriptName,
          actions: [],
          settings: {},
          kind: 'main',
        };
      }
      params.onMatchScriptId = newScriptId;
      return true;
    } catch (error) {
      console.error('Failed to create trigger script from Listen action:', error);
      alertFn('Could not create trigger script. Please try again.');
      return false;
    }
  }

  function applyLocatorCaptureMetadata(metadata = {}) {
    const data = appState.editingActionData;
    if (!data || !data.action) return {ok: false, error: 'no_open_action_editor'};

    if (byId('current-action-form')) {
      applyActionEditorChanges();
    }

    const nextParams = normalizeLayerParams({
      ...(data.action.params && typeof data.action.params === 'object'
        ? data.action.params
        : {}),
    });
    const selectedFocusAreaIndex = Number.isInteger(data.focusAreaIndex) ?
      data.focusAreaIndex : 0;
    const focusAreas = Array.isArray(nextParams.focusAreas) ?
      nextParams.focusAreas.map((area) => ({...(area || {})})) :
      [];
    const usingFocusAreaHints =
      data.action.action === 'glic_prompt' && focusAreas[selectedFocusAreaIndex];
    const targetParams = usingFocusAreaHints ?
      focusAreas[selectedFocusAreaIndex] : nextParams;
    const mergeIfEmpty = (paramKey, sourceKey = paramKey) => {
      const value = String(metadata?.[sourceKey] || '').trim();
      if (!value || String(targetParams[paramKey] || '').trim()) {
        return;
      }
      targetParams[paramKey] = value;
    };

    mergeIfEmpty('frame_id');
    mergeIfEmpty('shadow_chain');
    mergeIfEmpty('resource_path');
    mergeIfEmpty('ax_role');
    mergeIfEmpty('ax_name');
    mergeIfEmpty('interactive_ancestor_role');
    if (usingFocusAreaHints) {
      nextParams.focusAreas = focusAreas;
    }
    data.action.params = nextParams;

    const contextUrl = String(
        metadata?.context_url || metadata?.page_url || '').trim();
    if (contextUrl &&
        !String(data.action.contextUrl || data.action.context_url || '').trim()) {
      data.action.contextUrl = contextUrl;
      delete data.action.context_url;
    }

    data.openAdvancedDiagnostics = true;

    renderActionEditor();
    return {ok: true};
  }

  function discardActionEditorChanges() {
    const data = appState.editingActionData;
    if (!data || !data.action) return;

    const targetList = resolveTargetList(
      data.parentType, data.parentIndex, data.scopeType, data.scopeIndex,
      data.listPath || null);

    if (data.isNewAction && targetList && Number.isFinite(data.index)) {
      if (targetList[data.index] === data.action) {
        targetList.splice(data.index, 1);
      } else {
        const i = targetList.indexOf(data.action);
        if (i >= 0) targetList.splice(i, 1);
      }
      return;
    }

    if (data.original && typeof data.original === 'object') {
      const a = data.action;
      for (const k of Object.keys(a)) delete a[k];
      Object.assign(a, data.original);
      return;
    }

    if (data.editorAddedDraftFlag) {
      try {
        delete data.action.__draft;
        delete data.action.__draftCreatedAt;
      } catch (_) {}
    }
  }

  function closeActionEditor() {
    appState.isEditingAction = false;
    appState.editingActionData = null;
    renderAll();
  }

  function renderActionEditor() {
    const {action} = appState.editingActionData;
    if (!action.params || typeof action.params !== 'object') {
      action.params = {};
    }
    if (typeof normalizeAction === 'function') {
      const repaired = normalizeAction(action);
      if (repaired && typeof repaired === 'object' && repaired.action) {
        const draftFlags = {
          __draft: action.__draft,
          __draftCreatedAt: action.__draftCreatedAt,
        };
        for (const key of Object.keys(action)) {
          delete action[key];
        }
        Object.assign(action, repaired);
        if (draftFlags.__draft) {
          action.__draft = true;
        }
        if (draftFlags.__draftCreatedAt) {
          action.__draftCreatedAt = draftFlags.__draftCreatedAt;
        }
      }
      if (!action.params || typeof action.params !== 'object') {
        action.params = {};
      }
    }
    const meta = actionMetadata[action.action];
    if (action.action === 'authenticate') {
      actionEditorEl.innerHTML =
        '<div class="panel-header">Edit: AUTHENTICATE</div>' +
        '<div class="action-editor-form"><div class="help-text">' +
        'Login methods are edited on the timeline, inside a while loop. ' +
        'The far left method is tried first. Drag a method to change that order. ' +
        'When a method fails, the loop returns to the login menu and tries the ' +
        'next enabled method. It stops when a method succeeds. Add a login method ' +
        'to build that method as its own timeline. Save &amp; Close keeps this action.' +
        '</div></div>';
      return;
    }
    if (!meta || !meta.params) {
      actionEditorEl.innerHTML =
        `<div class="panel-header">Edit: ${String(action.action || 'unknown').toUpperCase()}</div>`;
      const unsupported = documentRef.createElement('div');
      unsupported.className = 'action-editor-form';
      const message = documentRef.createElement('div');
      message.className = 'form-group';
      message.innerHTML = `
        <label>Unsupported or Corrupted Action</label>
        <div class="help-text">
          This action could not be mapped to a known editor schema.
          Check the saved script JSON for a corrupted or legacy action kind,
          then reload the dashboard.
        </div>
      `;
      const raw = documentRef.createElement('pre');
      raw.className = 'help-text';
      raw.style.whiteSpace = 'pre-wrap';
      raw.textContent = JSON.stringify(action, null, 2);
      message.appendChild(raw);
      unsupported.appendChild(message);
      actionEditorEl.appendChild(unsupported);
      return;
    }
    if (action.action === 'click') {
      const rawClickCount = Number(
          action?.params?.clickCount ?? action?.params?.click_count);
      const clickCount = Number.isFinite(rawClickCount) ?
        Math.min(3, Math.max(1, rawClickCount)) :
        1;
      action.params = {
        ...(action.params && typeof action.params === 'object' ?
          action.params :
          {}),
        clickCount,
      };
      delete action.params.click_count;
    } else if (action.action === 'hover') {
      action.params = {
        ...(action.params && typeof action.params === 'object' ?
          action.params :
          {}),
        clickCount: 0,
        click_count: 0,
      };
    } else if (action.action === 'key_press') {
      const rawCount = Number(action?.params?.count);
      const count = Number.isFinite(rawCount) ?
        Math.max(1, Math.trunc(rawCount)) :
        1;
      action.params = {
        ...(action.params && typeof action.params === 'object' ?
          action.params :
          {}),
        count,
      };
    } else if (action.action === 'listen') {
      const rawFanOut = (action?.params?.fanOutPerMatchedUrl == null) ?
        action?.params?.fan_out_per_matched_url :
        action?.params?.fanOutPerMatchedUrl;
      const fanOutPerMatchedUrl = (rawFanOut == null) ? true : !!rawFanOut;
      action.params = {
        ...(action.params && typeof action.params === 'object' ?
          action.params :
          {}),
        fanOutPerMatchedUrl,
      };
      delete action.params.fan_out_per_matched_url;
    }

    actionEditorEl.innerHTML =
      `<div class="panel-header">Edit: ${action.action.toUpperCase()}</div>`;
    const form = documentRef.createElement('div');
    form.className = 'action-editor-form';
    form.id = 'current-action-form';

    const paramContainer = documentRef.createElement('div');
    paramContainer.className = 'action-editor-params';
    form.appendChild(paramContainer);

    const evalShowIf = (expr, params) => {
      if (!expr || typeof expr !== 'string') return true;
      try {
        const m = expr.match(/\s*([a-zA-Z0-9_]+)\s*===\s*['\"]?([^'\"]+)['\"]?\s*/);
        if (!m) return true;
        const key = m[1];
        const val = m[2];
        return String(params?.[key]) === val;
      } catch (_) {
        return true;
      }
    };

    const makeSmartInput = (paramMeta, value, typeHint = 'string') => {
      const input = documentRef.createElement('smart-input');
      input.className = 'input-field';
      const smartType = typeHint === 'number' ? 'number' : 'text';
      input.setAttribute('data-type', smartType);
      if (paramMeta?.multiline) {
        input.setAttribute('multiline', '');
        if (paramMeta?.rows) {
          input.setAttribute('rows', String(paramMeta.rows));
        }
      }
      if (paramMeta?.placeholder) {
        input.setAttribute('placeholder', String(paramMeta.placeholder));
      }
      input.value = value ?? '';
      return input;
    };

    const renderParams = (currentParams) => {
      paramContainer.innerHTML = '';
      for (const paramKey in meta.params) {
        if (action.action === 'listen' &&
            (paramKey === 'textUseRegex' || paramKey === 'attributeUseRegex')) {
          // Rendered inline beside text/attribute fields instead of standalone rows.
          continue;
        }
        const paramMeta = meta.params[paramKey];
        const group = documentRef.createElement('div');
        group.className = 'form-group';
        group.dataset.paramKey = paramKey;
        const disableForAutoRefresh =
          action.action === 'listen' &&
          paramKey === 'sampleIntervalMs' &&
          !!currentParams.autoRefresh;
        group.dataset.disabled = disableForAutoRefresh ? 'true' : 'false';
        if (disableForAutoRefresh) {
          group.style.opacity = '0.7';
        }

        const label = documentRef.createElement('label');
        label.textContent = paramMeta.label;
        group.appendChild(label);

        const widget = paramMeta.widget_type || paramMeta.type;
        group.dataset.paramType = (widget === 'number' || paramMeta.type === 'number')
          ? 'number'
          : String(paramMeta.type || widget || 'string');

        const isKeyAction =
          (action.action === 'key_press' || action.action === 'key_down' ||
           action.action === 'key') && (paramKey === 'key');
        if (isKeyAction) {
          const NON_CHAR_KEYS = [
            'Enter', 'Tab', 'Escape', 'Backspace', 'Delete', 'Space',
            'Shift', 'Control', 'Alt', 'Meta', 'CapsLock',
            'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
            'PageUp', 'PageDown', 'Home', 'End',
          ];
          const input = documentRef.createElement('input');
          input.type = 'text';
          input.className = 'input-field key-capture-input';
          input.value = currentParams[paramKey] || '';
          const normalizeKey = (k) => {
            if (!k) return '';
            if (NON_CHAR_KEYS.includes(k)) return k;
            if (k.length === 1) return k;
            return k;
          };
          input.addEventListener('keydown', (e) => {
            const k = normalizeKey(e.key);
            if (NON_CHAR_KEYS.includes(k)) {
              e.preventDefault();
              input.value = k;
              currentParams[paramKey] = k;
            }
          });
          input.addEventListener('input',
              (e) => currentParams[paramKey] = e.target.value);
          group.appendChild(input);

          const select = documentRef.createElement('select');
          select.className = 'input-field mt-1';
          const ph = documentRef.createElement('option');
          ph.value = '';
          ph.textContent = '- Common keys -';
          ph.disabled = true;
          ph.selected = !currentParams[paramKey];
          select.appendChild(ph);
          NON_CHAR_KEYS.forEach((k) => {
            const opt = documentRef.createElement('option');
            opt.value = k;
            opt.textContent = k;
            if (currentParams[paramKey] === k) opt.selected = true;
            select.appendChild(opt);
          });
          select.addEventListener('change', (e) => {
            input.value = e.target.value;
            currentParams[paramKey] = e.target.value;
          });
          group.appendChild(select);
        } else if (widget === 'script-select') {
          const select = documentRef.createElement('select');
          select.className = 'input-field';
          const isRequired = !!paramMeta.required;
          const placeholder = documentRef.createElement('option');
          placeholder.value = '';
          placeholder.textContent = isRequired ? '(choose a script)' : '(none)';
          select.appendChild(placeholder);
          const isCallScriptTarget =
            action.action === 'call_script' && paramKey === 'scriptId';
          const isListenTriggerTarget =
            action.action === 'listen' && paramKey === 'onMatchScriptId';
          const scripts = Object.entries(appState.scripts || {})
            .map(([id, s]) => {
              const slash = id.lastIndexOf('/');
              const collection = slash > 0 ? id.slice(0, slash) :
                String(s?.collection || '').trim();
              return {
                id,
                name: String(s?.name || id),
                kind: String(s?.kind || 'main').toLowerCase(),
                collection,
              };
            })
            .filter((s) => {
              if ((isCallScriptTarget || isListenTriggerTarget) &&
                  s.kind === 'listener') return false;
              if (isCallScriptTarget && s.id === appState.currentScriptId) {
                return false;
              }
              return true;
            })
            .sort((a, b) => {
              const ca = a.collection || '';
              const cb = b.collection || '';
              if (ca !== cb) return ca.localeCompare(cb);
              return a.name.localeCompare(b.name);
            });
          if (isListenTriggerTarget) {
            const collections = [...new Set(
              scripts.map((s) => s.collection).filter(Boolean),
            )].sort((a, b) => a.localeCompare(b));
            for (const collection of collections) {
              const opt = documentRef.createElement('option');
              opt.value = `collection:${collection}`;
              opt.textContent = `Collection: ${collection}`;
              if ((currentParams[paramKey] || '') === opt.value) {
                opt.selected = true;
              }
              select.appendChild(opt);
            }
          }
          for (const s of scripts) {
            const opt = documentRef.createElement('option');
            opt.value = s.id;
            const shortId = s.id.includes('/') ?
              s.id.slice(s.id.lastIndexOf('/') + 1, s.id.lastIndexOf('/') + 9) :
              s.id.slice(0, 8);
            const prefix = s.collection ? `[${s.collection}] ` : '';
            opt.textContent = `${prefix}${s.name} (${shortId})`;
            if ((currentParams[paramKey] || '') === s.id) opt.selected = true;
            select.appendChild(opt);
          }
          if (isListenTriggerTarget) {
            const opt = documentRef.createElement('option');
            opt.value = CREATE_NEW_TRIGGER_SCRIPT_VALUE;
            opt.textContent = 'Create New Trigger Script...';
            if ((currentParams[paramKey] || '') === CREATE_NEW_TRIGGER_SCRIPT_VALUE) {
              opt.selected = true;
            }
            select.appendChild(opt);
          }
          select.value = currentParams[paramKey] || '';
          select.addEventListener('change',
              (e) => currentParams[paramKey] = e.target.value);
          group.appendChild(select);
        } else if (widget === 'entry' || widget === 'string') {
          const input = makeSmartInput(
              paramMeta, currentParams[paramKey] || '', 'string');
          if (paramMeta?.regexToggleParam) {
            group.dataset.regexParamKey = String(paramMeta.regexToggleParam);
            input.setAttribute('allow-regex', 'true');
            if (currentParams[paramMeta.regexToggleParam]) {
              input.setAttribute('regex-enabled', 'true');
            } else {
              input.removeAttribute('regex-enabled');
            }
            input.addEventListener('regex-change', (e) => {
              currentParams[paramMeta.regexToggleParam] = !!e.detail;
            });
          } else {
            delete group.dataset.regexParamKey;
          }
          if (disableForAutoRefresh) {
            input.setAttribute('disabled', '');
          } else {
            input.removeAttribute('disabled');
          }
          input.addEventListener('input-change',
              (e) => currentParams[paramKey] = e.detail);
          group.appendChild(input);
        } else if (widget === 'boolean') {
          const checkbox = documentRef.createElement('input');
          checkbox.type = 'checkbox';
          checkbox.className = 'input-checkbox';
          checkbox.checked = !!currentParams[paramKey];
          checkbox.addEventListener('change', (e) => {
            currentParams[paramKey] = !!e.target.checked;
            renderParams(readActionParamsFromDOM());
          });
          group.appendChild(checkbox);
        } else if (widget === 'number') {
          const initialValue =
            paramKey === 'clickCount' &&
              currentParams[paramKey] == null &&
              currentParams.click_count != null ?
              currentParams.click_count :
              (currentParams[paramKey] ?? paramMeta.default ?? '');
          const input = makeSmartInput(
              paramMeta, initialValue, 'number');
          if (disableForAutoRefresh) {
            input.setAttribute('disabled', '');
          } else {
            input.removeAttribute('disabled');
          }
          input.addEventListener('input-change', (e) => {
            const raw = String(e.detail ?? '').trim();
            if (!raw) {
              delete currentParams[paramKey];
              return;
            }
            const n = Number(raw);
            if (!Number.isFinite(n)) {
              delete currentParams[paramKey];
              return;
            }
            currentParams[paramKey] = n;
          });
          group.appendChild(input);
        } else if (widget === 'dropdown' || widget === 'select') {
          const select = documentRef.createElement('select');
          select.className = 'input-field';
          const options = Array.isArray(paramMeta.options) ? paramMeta.options : [];
          if ((currentParams[paramKey] == null ||
               String(currentParams[paramKey]).trim() === '') &&
              options.length) {
            currentParams[paramKey] = String(options[0]);
          }
          options.forEach((opt) => {
            const option = documentRef.createElement('option');
            option.value = opt;
            option.textContent = opt;
            if ((currentParams[paramKey] || '') === opt) option.selected = true;
            select.appendChild(option);
          });
          select.addEventListener('change', (e) => {
            currentParams[paramKey] = e.target.value;
            if (action.action === 'if_condition' &&
                (paramKey === 'condition_type' || paramKey === 'conditionType')) {
              appState.editingActionData.action.params = readActionParamsFromDOM();
              renderActionEditor();
              return;
            }
            renderParams(readActionParamsFromDOM());
          });
          group.appendChild(select);
        } else if (widget === 'dynamic_locator_list' ||
                   widget === 'locator-list') {
          const raw = currentParams[paramKey] || [];
          const extracted = extractLayerHintsFromLocators(raw);
          currentParams[paramKey] = extracted.locators;
          if (!currentParams.frame_id && extracted.frame_id) {
            currentParams.frame_id = extracted.frame_id;
          }
          if (!currentParams.shadow_chain && extracted.shadow_chain) {
            currentParams.shadow_chain = extracted.shadow_chain;
          }
          renderLocatorListEditor(
            group,
            extracted.locators,
            (newLocs) => {
              currentParams[paramKey] = newLocs;
            },
            `action-param:${paramKey}:locators`,
          );
        } else if (widget === 'focus-area-list') {
          const focusAreas = Array.isArray(currentParams[paramKey])
            ? currentParams[paramKey]
            : [];
          const legacyLocators = Array.isArray(currentParams.locators)
            ? currentParams.locators
            : [];
          const isGlicPromptAction =
            String(appState.editingActionData?.action?.action || '') ===
            'glic_prompt';
          const initialAreas = focusAreas.length ? focusAreas :
            (legacyLocators.length
              ? [{label: 'Primary focus', locators: legacyLocators}]
              : []);
          if (isGlicPromptAction) {
            currentParams.locators = sanitizeGlicPromptFocusLocators(legacyLocators);
          }
          const sanitizedInitialAreas = isGlicPromptAction ?
            initialAreas.map((area) => ({
              ...(area || {}),
              locators: sanitizeGlicPromptFocusLocators(area?.locators || []),
            })) :
            initialAreas;
          currentParams[paramKey] = sanitizedInitialAreas;
          renderFocusAreaListEditor(group, sanitizedInitialAreas, (newAreas) => {
            currentParams[paramKey] = newAreas;
          }, () => {
            appState.editingActionData.action.params = readActionParamsFromDOM();
            if (typeof form.__applyFocusAreaSelection === 'function') {
              form.__applyFocusAreaSelection();
            }
            if (typeof form.__showFocusAreaDiagnostics === 'function') {
              form.__showFocusAreaDiagnostics(
                  Number.isInteger(appState.editingActionData?.focusAreaIndex) ?
                    appState.editingActionData.focusAreaIndex : 0);
            }
          });
          if (group.__focusAreaEditor &&
              typeof group.__focusAreaEditor.applySelection === 'function') {
            form.__applyFocusAreaSelection =
              group.__focusAreaEditor.applySelection;
          }
        } else if (widget === 'dynamic_field_list' || widget === 'field-list') {
          renderFieldListEditor(group, currentParams[paramKey] || [], (newFields) => {
            currentParams[paramKey] = newFields;
          });
        } else {
          const input = makeSmartInput(
              paramMeta, currentParams[paramKey] || '', 'string');
          if (disableForAutoRefresh) {
            input.setAttribute('disabled', '');
          } else {
            input.removeAttribute('disabled');
          }
          input.addEventListener('input-change',
              (e) => currentParams[paramKey] = e.detail);
          group.appendChild(input);
        }

        if (disableForAutoRefresh) {
          const note = documentRef.createElement('div');
          note.className = 'form-help';
          note.textContent =
            'Disabled when Auto Refresh Page is enabled. The listener checks only after refresh.';
          note.style.opacity = '0.85';
          note.style.fontSize = '0.9em';
          note.style.marginTop = '6px';
          group.appendChild(note);
        }

        if (paramMeta.showIf) {
          group.style.display = evalShowIf(paramMeta.showIf, currentParams) ? '' : 'none';
        }
        if (String(appState.editingActionData?.focusParamKey || '') === paramKey) {
          highlightEditorTarget(group);
        }
        paramContainer.appendChild(group);
      }
    };

    normalizeLayerParams(action.params);
    renderParams(action.params);

    const commentGroup = documentRef.createElement('div');
    commentGroup.className = 'form-group mt-2';
    const commentLabel = documentRef.createElement('label');
    commentLabel.textContent = 'Comment (optional)';
    const commentInput = documentRef.createElement('textarea');
    commentInput.className = 'input-field';
    commentInput.rows = 3;
    commentInput.placeholder =
      'Explain the purpose, tips, and gotchas for this action...';
    commentInput.value = action.comment || '';
    commentInput.addEventListener('input', (e) => action.comment = e.target.value);
    commentGroup.appendChild(commentLabel);
    commentGroup.appendChild(commentInput);
    form.appendChild(commentGroup);

    const adv = documentRef.createElement('details');
    if (appState.editingActionData?.openAdvancedDiagnostics) {
      adv.open = true;
    }
    const sum = documentRef.createElement('summary');
    sum.textContent = 'Advanced / Diagnostics';
    adv.appendChild(sum);
    const advBody = documentRef.createElement('div');
    advBody.className = 'mt-2';
    if (action.action === 'glic_prompt' &&
        Array.isArray(action.params?.focusAreas) &&
        action.params.focusAreas.length) {
      form.__focusAreaLayerEditor = null;
      const selectedIndex = Math.max(
          0,
          Math.min(
              Number.isInteger(appState.editingActionData?.focusAreaIndex) ?
                appState.editingActionData.focusAreaIndex : 0,
              action.params.focusAreas.length - 1));
      appState.editingActionData.focusAreaIndex = selectedIndex;
      const selectedArea = action.params.focusAreas[selectedIndex] || {};

      const pickerGroup = documentRef.createElement('div');
      pickerGroup.className = 'form-group mt-2';
      const pickerLabel = documentRef.createElement('label');
      pickerLabel.textContent = 'Diagnostics target';
      const picker = documentRef.createElement('select');
      picker.className = 'input-field';
      action.params.focusAreas.forEach((area, index) => {
        const option = documentRef.createElement('option');
        option.value = String(index);
        const label = String(area?.label || '').trim();
        option.textContent = label ?
          `Element ${index + 1}: ${label}` :
          `Element ${index + 1}`;
        option.selected = index === selectedIndex;
        picker.appendChild(option);
      });
      picker.addEventListener('change', (e) => {
        appState.editingActionData.action.params = readActionParamsFromDOM();
        appState.editingActionData.focusAreaIndex =
          parseInt(e.target.value, 10) || 0;
        if (typeof form.__applyFocusAreaSelection === 'function') {
          form.__applyFocusAreaSelection();
        }
        if (typeof form.__showFocusAreaDiagnostics === 'function') {
          form.__showFocusAreaDiagnostics(appState.editingActionData.focusAreaIndex);
        }
      });
      pickerGroup.append(pickerLabel, picker);
      advBody.appendChild(pickerGroup);

      const layerHelp = documentRef.createElement('div');
      layerHelp.className = 'help-text';
      layerHelp.textContent =
        'Editing diagnostics for the currently selected focus element only.';
      advBody.appendChild(layerHelp);
      const panelsHost = documentRef.createElement('div');
      panelsHost.className = 'focus-area-diagnostics-panels';
      form.__focusAreaLayerEditors = [];
      const showDiagnosticsForIndex = (visibleIndex) => {
        panelsHost.querySelectorAll('[data-focus-area-panel-index]').forEach((panel) => {
          const panelIndex = parseInt(panel.dataset.focusAreaPanelIndex || '-1', 10);
          panel.style.display = panelIndex === visibleIndex ? '' : 'none';
        });
        picker.value = String(visibleIndex);
      };
      action.params.focusAreas.forEach((area, index) => {
        const panel = documentRef.createElement('div');
        panel.dataset.focusAreaPanelIndex = String(index);
        panel.style.display = index === selectedIndex ? '' : 'none';
        form.__focusAreaLayerEditors[index] = renderLayerEditor(
            panel, normalizeLayerParams({...area}), {trackInForm: false});
        panelsHost.appendChild(panel);
      });
      form.__showFocusAreaDiagnostics = showDiagnosticsForIndex;
      showDiagnosticsForIndex(selectedIndex);
      advBody.appendChild(panelsHost);
    } else {
      form.__focusAreaLayerEditors = null;
      form.__showFocusAreaDiagnostics = null;
      form.__focusAreaLayerEditor = null;
      renderLayerEditor(advBody, normalizeLayerParams(action.params));
    }
    const contextGroup = documentRef.createElement('div');
    contextGroup.className = 'form-group mt-2';
    const contextLabel = documentRef.createElement('label');
    contextLabel.textContent = 'Context URL (optional)';
    const contextInput = documentRef.createElement('input');
    contextInput.type = 'text';
    contextInput.id = 'action-context-url-input';
    contextInput.className = 'input-field';
    contextInput.placeholder = 'Auto-filled from recording/playback';
    contextInput.value = String(
        action.contextUrl ||
        action.context_url ||
        '',
    ).trim();
    contextInput.addEventListener('input', (e) => {
      action.contextUrl = String(e.target.value || '');
      delete action.context_url;
    });
    contextGroup.appendChild(contextLabel);
    contextGroup.appendChild(contextInput);
    advBody.appendChild(contextGroup);
    adv.appendChild(advBody);
    form.appendChild(adv);

    const actionsRow = documentRef.createElement('div');
    actionsRow.className = 'action-editor-actions mt-auto';
    const closeBtn = documentRef.createElement('button');
    closeBtn.className = 'btn';
    closeBtn.textContent = 'Close';
    closeBtn.onclick = () => {
      discardActionEditorChanges();
      closeActionEditor();
    };
    const commitBtn = documentRef.createElement('button');
    commitBtn.className = 'btn btn-primary';
    commitBtn.textContent = 'Save & Close';
    commitBtn.onclick = async () => {
      const updated = applyActionEditorChanges();
      if (!updated) return;
      const triggerResolved = await resolveCreateTriggerScriptSelection(updated);
      if (!triggerResolved) return;
      if (updated && updated.__draft) {
        try {
          const result = await validateAction(updated);
          if (!result?.valid) {
            alertFn(result?.error || 'Action is incomplete or invalid.');
            return;
          }
        } catch (e) {
          console.warn('[DashboardUI] validateAction failed; refusing to commit draft', e);
          alertFn('Could not validate action. Try again.');
          return;
        }
        delete updated.__draft;
        delete updated.__draftCreatedAt;
      }
      const result = await saveScriptById(
          appState.currentScriptId,
          {silent: true, refreshAfter: false, returnStatus: true});
      if (!result?.ok) {
        return;
      }
      closeActionEditor();
      try {
        onScriptsChanged();
      } catch (_) {}
    };
    actionsRow.appendChild(closeBtn);
    actionsRow.appendChild(commitBtn);
    form.appendChild(actionsRow);
    actionEditorEl.appendChild(form);
  }

  return {
    applyActionEditorChanges,
    applyLocatorCaptureMetadata,
    closeActionEditor,
    discardActionEditorChanges,
    readActionParamsFromDOM,
    renderActionEditor,
  };
}

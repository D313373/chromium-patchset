export function renderDashboardScriptList({
  scriptListEl,
  appState,
  onRunSelected,
  onStopAllScripts,
  onClearSelected,
  onToggleSelectScript,
  onSelectScript,
}) {
  scriptListEl.innerHTML = '';
  appState.selectedScripts = (Array.isArray(appState.selectedScripts) ?
    appState.selectedScripts :
    []).filter((scriptId) => {
    const script = appState.scripts?.[scriptId];
    return !!script && String(script.kind || 'main') === 'main';
  });

  const controls = document.createElement('div');
  controls.style.cssText =
    'display:flex;align-items:center;justify-content:space-between;margin-bottom:8px;gap:8px;';
  const left = document.createElement('div');
  left.style.cssText = 'display:flex;align-items:center;gap:8px;flex-wrap:wrap;';
  const selCount = appState.selectedScripts.length;
  const runBtn = document.createElement('button');
  runBtn.className = 'btn btn-primary';
  runBtn.textContent = selCount ? `Run Selected (${selCount})` : 'Run Selected';
  runBtn.disabled = selCount === 0 || appState.isQueueRunning;
  runBtn.onclick = () => onRunSelected();
  left.appendChild(runBtn);

  const stopAllBtn = document.createElement('button');
  stopAllBtn.className = 'btn btn-secondary';
  stopAllBtn.textContent = 'Stop All';
  stopAllBtn.title =
    'Stop every running script and clear the Run Selected queue, including chained listen/onMatch runs.';
  stopAllBtn.onclick = () => {
    if (typeof onStopAllScripts === 'function') {
      onStopAllScripts();
    }
  };
  left.appendChild(stopAllBtn);
  controls.appendChild(left);

  const clearBtn = document.createElement('button');
  clearBtn.className = 'btn btn-secondary';
  clearBtn.textContent = 'Clear';
  clearBtn.disabled = selCount === 0 || appState.isQueueRunning;
  clearBtn.onclick = () => onClearSelected();
  controls.appendChild(clearBtn);
  scriptListEl.appendChild(controls);

  const collectionOf = (scriptId, script) => {
    const slash = String(scriptId || '').lastIndexOf('/');
    if (slash > 0) {
      return String(scriptId).slice(0, slash);
    }
    return String(script?.collection || '').trim();
  };

  const entries = Object.entries(appState.scripts || {}).sort((a, b) => {
    const ca = collectionOf(a[0], a[1]);
    const cb = collectionOf(b[0], b[1]);
    if (ca !== cb) {
      if (!ca) return 1;
      if (!cb) return -1;
      return ca.localeCompare(cb);
    }
    return String(a[1]?.name || a[0]).localeCompare(String(b[1]?.name || b[0]));
  });

  let lastCollection = null;
  for (const [scriptId, script] of entries) {
    const collection = collectionOf(scriptId, script);
    if (collection !== lastCollection) {
      lastCollection = collection;
      const header = document.createElement('div');
      header.style.cssText =
        'margin:10px 0 4px;font-size:11px;font-weight:700;letter-spacing:0.04em;' +
        'text-transform:uppercase;color:#6b7280;';
      header.textContent = collection ? `Collection · ${collection}` : 'Uncategorized';
      scriptListEl.appendChild(header);
    }
    const kind = String(script.kind || 'main');
    const isMain = kind === 'main';
    const container = document.createElement('div');
    container.style.cssText =
      'display:flex;align-items:center;gap:8px;margin-bottom:6px;';

    const badge = document.createElement('div');
    badge.style.cssText =
      'min-width:22px;min-height:22px;border-radius:12px;display:flex;' +
      'align-items:center;justify-content:center;font-size:12px;' +
      'font-weight:700;cursor:pointer;user-select:none;';
    const idx = appState.selectedScripts.indexOf(scriptId);
    if (idx >= 0) {
      badge.textContent = (idx + 1).toString();
      badge.style.background = '#2563eb';
      badge.style.color = 'white';
      badge.title = 'Click to remove from queue';
    } else {
      badge.style.border = '1px solid #9ca3af';
      badge.style.color = '#9ca3af';
      if (isMain) {
        badge.textContent = '.';
        badge.style.background = 'transparent';
        badge.title = 'Click to add to queue';
      } else if (kind === 'login') {
        badge.textContent = '🔑';
        badge.style.background = 'rgba(245, 158, 11, 0.10)';
        badge.title =
          'Key script: runs automatically for authentication recovery when an assigned script hits a login gate.';
      } else if (kind === 'listener') {
        badge.textContent = '🔔';
        badge.style.background = 'rgba(16, 185, 129, 0.10)';
        badge.title =
          'Listener script: start and monitor it from the Debugging tab. It is not queued from Dashboard.';
      } else {
        badge.textContent = '·';
        badge.style.background = 'transparent';
        badge.title = 'Only main scripts can be queued';
      }
    }
    if (isMain && !appState.isQueueRunning) {
      badge.addEventListener('click', () => onToggleSelectScript(scriptId));
    } else {
      badge.style.opacity = '0.5';
      badge.style.cursor = 'not-allowed';
    }
    container.appendChild(badge);

    const button = document.createElement('button');
    button.className = 'btn text-left w-full';
    button.style.cssText = 'flex:1;';
    button.textContent =
      script.name + (appState.currentlyExecutingId === scriptId ? '  >' : '');
    button.onclick = () => onSelectScript(scriptId);
    if (scriptId === appState.currentScriptId) button.classList.add('active');
    container.appendChild(button);
    scriptListEl.appendChild(container);
  }
}

export function toggleSelectedScript(appState, scriptId) {
  const i = appState.selectedScripts.indexOf(scriptId);
  if (i >= 0) {
    appState.selectedScripts.splice(i, 1);
  } else {
    appState.selectedScripts.push(scriptId);
  }
}

export async function runSelectedScriptsQueue({
  appState,
  playScript,
  onRender,
  waitMs = 0,
}) {
  appState.selectedScripts = (Array.isArray(appState.selectedScripts) ?
    appState.selectedScripts :
    []).filter((scriptId) => {
    const script = appState.scripts?.[scriptId];
    return !!script && String(script.kind || 'main') === 'main';
  });
  if (appState.isQueueRunning || appState.selectedScripts.length === 0) return;

  const yieldToUI = () => new Promise((resolve) => {
    if (typeof window !== 'undefined' &&
        typeof window.requestAnimationFrame === 'function') {
      window.requestAnimationFrame(() => resolve());
      return;
    }
    setTimeout(resolve, 0);
  });

  appState.queueStopRequested = false;
  appState.isQueueRunning = true;
  onRender();
  try {
    for (let i = 0; i < appState.selectedScripts.length; i++) {
      if (appState.queueStopRequested) {
        break;
      }
      const scriptId = appState.selectedScripts[i];
      if (!appState.scripts?.[scriptId]) {
        continue;
      }
      appState.currentlyExecutingId = scriptId;
      onRender();
      try {
        await playScript(scriptId);
      } catch (e) {
        console.error('playScript failed', e);
      }
      if (appState.queueStopRequested) {
        break;
      }
      const interScriptDelay = Math.max(0, Number(waitMs) || 0);
      if (interScriptDelay > 0) {
        await new Promise((r) => setTimeout(r, interScriptDelay));
      } else {
        await yieldToUI();
      }
    }
  } finally {
    appState.isQueueRunning = false;
    appState.queueStopRequested = false;
    appState.currentlyExecutingId = null;
    appState.selectedScripts = [];
    onRender();
  }
}

export function createDashboardScriptCrudController({
  appState,
  dashboardHandler,
  renderAll,
  inputModalTitleEl,
  inputModal,
  scriptNameInput,
  confirmInputBtn,
  cancelInputBtn,
  confirmationModal,
  confirmationText,
  confirmActionBtn,
  cancelConfirmBtn,
}) {
  const showInputModal = (title, callback) => {
    if (!inputModalTitleEl || !inputModal || !scriptNameInput ||
        !confirmInputBtn || !cancelInputBtn) {
      return;
    }
    inputModalTitleEl.textContent = title;
    scriptNameInput.value = '';
    inputModal.classList.remove('hidden');

    const confirmHandler = () => {
      const maybePromise = callback(scriptNameInput.value);
      Promise.resolve(maybePromise).finally(cleanup);
    };
    const cancelHandler = () => {
      const maybePromise = callback(null);
      Promise.resolve(maybePromise).finally(cleanup);
    };
    const cleanup = () => {
      inputModal.classList.add('hidden');
      confirmInputBtn.removeEventListener('click', confirmHandler);
      cancelInputBtn.removeEventListener('click', cancelHandler);
    };

    confirmInputBtn.addEventListener('click', confirmHandler);
    cancelInputBtn.addEventListener('click', cancelHandler);
  };

  const showConfirmationModal = (message, callback) => {
    if (!confirmationModal || !confirmationText ||
        !confirmActionBtn || !cancelConfirmBtn) {
      return;
    }
    confirmationText.textContent = message;
    confirmationModal.classList.remove('hidden');

    const confirmHandler = () => {
      const maybePromise = callback(true);
      Promise.resolve(maybePromise).finally(cleanup);
    };
    const cancelHandler = () => {
      const maybePromise = callback(false);
      Promise.resolve(maybePromise).finally(cleanup);
    };
    const cleanup = () => {
      confirmationModal.classList.add('hidden');
      confirmActionBtn.removeEventListener('click', confirmHandler);
      cancelConfirmBtn.removeEventListener('click', cancelHandler);
    };

    confirmActionBtn.addEventListener('click', confirmHandler);
    cancelConfirmBtn.addEventListener('click', cancelHandler);
  };

  const handleNewScript = async () => {
    showInputModal(
        'New script name (optional: Name @ Collection)',
        async (raw) => {
          if (!raw) {
            return;
          }
          const text = String(raw).trim();
          let name = text;
          let collection = '';
          const at = text.lastIndexOf('@');
          if (at > 0) {
            name = text.slice(0, at).trim();
            collection = text.slice(at + 1).trim();
          }
          if (!name) {
            return;
          }
          if (collection && !/^[A-Za-z0-9._-]+$/.test(collection)) {
            console.error(
                '[DashboardUI] invalid collection name:', collection);
            return;
          }
          try {
            console.log(
                '[DashboardUI] handleNewScript name=', name,
                'collection=', collection || '(none)');
            const payload = {name};
            if (collection) {
              payload.collection = collection;
            }
            const response = await dashboardHandler.createNewScript(payload);
            console.log(
                '[DashboardUI] createNewScript response:', response);
            const newScriptId = response.scriptId;
            appState.scripts[newScriptId] = {
              name,
              actions: [],
              settings: {},
              kind: 'main',
              ...(collection ? {collection} : {}),
            };
            appState.currentScriptId = newScriptId;
            appState.isEditingAction = false;
            console.log(
                '[DashboardUI] after create, scripts keys:',
                Object.keys(appState.scripts));
            renderAll();
          } catch (error) {
            console.error('Failed to create new script:', error);
          }
        });
  };

  const handleDeleteScript = () => {
    const selected = (Array.isArray(appState.selectedScripts) ?
      appState.selectedScripts :
      []).filter((scriptId) => !!appState.scripts?.[scriptId]);
    const deleteIds = selected.length ?
      selected.slice() :
      (appState.currentScriptId && appState.scripts[appState.currentScriptId] ?
        [appState.currentScriptId] :
        []);
    if (!deleteIds.length) {
      return;
    }
    const names = deleteIds.map((scriptId) =>
      String(appState.scripts[scriptId]?.name || scriptId));
    const message = deleteIds.length === 1 ?
      `Are you sure you want to delete "${names[0]}"?` :
      `Are you sure you want to delete ${deleteIds.length} scripts?\n\n` +
        names.slice(0, 8).map((name) => `- ${name}`).join('\n') +
        (names.length > 8 ? `\n- ...and ${names.length - 8} more` : '');
    showConfirmationModal(
      message,
      (confirmed) => {
        if (!confirmed) {
          return;
        }
        for (const scriptId of deleteIds) {
          delete appState.scripts[scriptId];
          console.log('[DashboardUI] deleteScript id=', scriptId);
          dashboardHandler.deleteScript(scriptId);
        }
        const deletedIds = new Set(deleteIds);
        appState.selectedScripts = (Array.isArray(appState.selectedScripts) ?
          appState.selectedScripts :
          []).filter((scriptId) => !deletedIds.has(scriptId));
        if (deletedIds.has(appState.currentScriptId)) {
          appState.currentScriptId = Object.keys(appState.scripts)[0] || null;
        }
        if (deletedIds.has(appState.currentlyExecutingId)) {
          appState.currentlyExecutingId = null;
        }
        appState.isEditingAction = false;
        renderAll();
      });
  };

  return {
    handleDeleteScript,
    handleNewScript,
    showConfirmationModal,
    showInputModal,
  };
}

export function createDashboardScriptSettingsController({
  appState,
  renderAll,
  scheduleAutoSave,
  flushPendingSavesAndSaveNow,
  documentRef = document,
  modalId = 'script-settings-modal',
}) {
  const byId = (id) => documentRef.getElementById(id);

  const ensureListenerContextStartUrl = (script, startUrl) => {
    if (!script || (script.kind || 'main') !== 'listener') {
      return;
    }
    script.url_contexts = script.url_contexts || [{url: '', actions: []}];
    if (!script.url_contexts[0]) {
      script.url_contexts[0] = {url: '', actions: []};
    }
    script.url_contexts[0].url = startUrl;
  };

  const closeModal = (modal) => {
    if (!modal) return;
    modal.innerHTML = '';
    modal.classList.add('hidden');
  };

  const persistModalState = async ({
    scriptId,
    currentScript,
    startUrlInput,
    applyExecutionFrequencyFromControls,
  }) => {
    try {
      applyExecutionFrequencyFromControls();
      if (startUrlInput) {
        const nextStartUrl = String(startUrlInput.value || '');
        currentScript.startUrl = nextStartUrl;
        ensureListenerContextStartUrl(currentScript, nextStartUrl);
      }
      // Serialize with pending autosaves so older in-flight saves cannot
      // overwrite the latest values when the modal closes quickly.
      await flushPendingSavesAndSaveNow(scriptId);
    } catch (_) {
    }
  };

  function openScriptSettingsModal() {
    const modal = byId(modalId);
    if (!modal) return;
    const scriptId = appState.currentScriptId;
    const currentScript = appState.scripts[scriptId];
    if (!currentScript) return;

    let startUrlInput = null;
    let applyExecutionFrequencyFromControls = () => {};
    modal.classList.remove('hidden');

    const content = documentRef.createElement('div');
    content.className = 'modal-content';
    content.innerHTML = '<h3 class="modal-header">Script Settings (Overrides)</h3>';
    const body = documentRef.createElement('div');
    body.id = 'script-settings-body';

    const kindRow = documentRef.createElement('div');
    kindRow.style.cssText =
      'margin-bottom:12px; display:flex; align-items:center; gap:8px;';
    const kindLabel = documentRef.createElement('label');
    kindLabel.textContent = 'Script Type:';
    const kindSelect = documentRef.createElement('select');
    [
      {value: 'normal', label: 'normal'},
      {value: 'login', label: 'key'},
      {value: 'listener', label: 'listener'},
    ].forEach(({value, label}) => {
      const opt = documentRef.createElement('option');
      opt.value = value;
      opt.textContent = label;
      kindSelect.appendChild(opt);
    });
    const initialKind =
      (currentScript.kind === 'main') ? 'normal' : (currentScript.kind || 'normal');
    kindSelect.value = initialKind;
    kindRow.appendChild(kindLabel);
    kindRow.appendChild(kindSelect);
    body.appendChild(kindRow);

    const keyRow = documentRef.createElement('div');
    keyRow.style.cssText =
      'margin-bottom:12px; display:flex; align-items:center; gap:8px; flex-wrap:wrap;';
    const keyLabel = documentRef.createElement('label');
    keyLabel.textContent = 'Key Script:';
    const keySelect = documentRef.createElement('select');
    const noneOpt = documentRef.createElement('option');
    noneOpt.value = '';
    noneOpt.textContent = '(none)';
    keySelect.appendChild(noneOpt);

    const scripts = appState.scripts || {};
    const desiredKeyId = String(currentScript.keyScriptId || '').trim();
    let hasDesired = false;
    Object.entries(scripts).forEach(([sid, script]) => {
      if (!script || sid === scriptId) return;
      const kind = script.kind || 'main';
      if (kind === 'listener') return;
      if (sid === desiredKeyId) hasDesired = true;
      const opt = documentRef.createElement('option');
      opt.value = sid;
      const kindTag = kind && kind !== 'main' ? ` [${kind}]` : '';
      opt.textContent = script.name ? `${script.name}${kindTag}` : `${sid}${kindTag}`;
      keySelect.appendChild(opt);
    });

    if (desiredKeyId && !hasDesired) {
      const missingOpt = documentRef.createElement('option');
      missingOpt.value = desiredKeyId;
      missingOpt.textContent = `(missing) ${desiredKeyId}`;
      keySelect.insertBefore(missingOpt, keySelect.children[1] || null);
    }

    keySelect.value = desiredKeyId;
    keySelect.onchange = () => {
      const value = String(keySelect.value || '').trim();
      currentScript.keyScriptId = value || undefined;
      scheduleAutoSave(scriptId, 'key-script', 0);
    };
    keyRow.appendChild(keyLabel);
    keyRow.appendChild(keySelect);
    body.appendChild(keyRow);

    const kindSpecificRows = documentRef.createElement('div');
    body.appendChild(kindSpecificRows);

    const renderKindSpecificRows = () => {
      kindSpecificRows.innerHTML = '';
      startUrlInput = null;
      applyExecutionFrequencyFromControls = () => {};

      const currentKind = currentScript.kind || 'main';
      const isListenerKind = currentKind === 'listener';
      const isKeyKind = currentKind === 'login';

      if (isKeyKind) {
        currentScript.keyScriptId = undefined;
      }
      keyRow.style.display = isKeyKind ? 'none' : 'flex';

      const urlRow = documentRef.createElement('div');
      urlRow.style.cssText =
        'margin:12px 0; display:flex; align-items:center; gap:8px; flex-wrap:wrap;';
      const urlLabel = documentRef.createElement('label');
      urlLabel.textContent = isListenerKind ?
        'Listener Start URL:' :
        'Start URL (optional):';
      const urlInput = documentRef.createElement('input');
      urlInput.type = 'text';
      urlInput.className = 'input-field';
      urlInput.style.minWidth = '360px';
      startUrlInput = urlInput;
      const legacyContextUrl =
        (Array.isArray(currentScript.url_contexts) && currentScript.url_contexts[0]) ?
          String(currentScript.url_contexts[0].url || '') :
          '';
      urlInput.value = String(currentScript.startUrl || legacyContextUrl || '');
      urlInput.placeholder = 'https://… (leave empty to rely on first navigate action)';
      urlInput.addEventListener('input', () => {
        const nextStartUrl = String(urlInput.value || '');
        currentScript.startUrl = nextStartUrl;
        ensureListenerContextStartUrl(currentScript, nextStartUrl);
        scheduleAutoSave(
          scriptId,
          isListenerKind ? 'listener-start-url' : 'script-start-url',
          0);
      });
      urlInput.addEventListener('blur', () => {
        scheduleAutoSave(
          scriptId,
          isListenerKind ? 'listener-start-url' : 'script-start-url');
      });
      urlRow.appendChild(urlLabel);
      urlRow.appendChild(urlInput);
      kindSpecificRows.appendChild(urlRow);
      if (isListenerKind || isKeyKind) {
        return;
      }

      const freqRow = documentRef.createElement('div');
      freqRow.style.cssText =
        'margin:12px 0; display:flex; align-items:center; gap:8px; flex-wrap:wrap;';
      const freqLabel = documentRef.createElement('label');
      freqLabel.textContent = 'Execution Frequency:';
      const prefix = documentRef.createElement('span');
      prefix.textContent = 'Run this script every';
      const freqInput = documentRef.createElement('input');
      freqInput.type = 'number';
      freqInput.min = '1';
      freqInput.style.width = '80px';
      const unitSelect = documentRef.createElement('select');
      ['minutes', 'hours', 'days'].forEach((unit) => {
        const opt = documentRef.createElement('option');
        opt.value = unit;
        opt.textContent = unit;
        unitSelect.appendChild(opt);
      });
      const ef =
        (currentScript.settingsOverrides &&
         currentScript.settingsOverrides.executionFrequency) || null;
      if (ef && typeof ef.value === 'number') {
        freqInput.value = String(ef.value);
      }
      unitSelect.value = (ef && ef.unit) ? ef.unit : 'minutes';
      applyExecutionFrequencyFromControls = () => {
        currentScript.settingsOverrides = currentScript.settingsOverrides || {};
        const value = parseInt(freqInput.value, 10);
        if (!isNaN(value) && value > 0) {
          currentScript.settingsOverrides.executionFrequency = {
            value,
            unit: unitSelect.value,
          };
        } else {
          currentScript.settingsOverrides.executionFrequency = null;
        }
      };
      const onChange = () => {
        applyExecutionFrequencyFromControls();
        scheduleAutoSave(scriptId, 'exec-frequency', 0);
      };
      freqInput.addEventListener('input', onChange);
      unitSelect.addEventListener('change', onChange);
      freqRow.appendChild(freqLabel);
      freqRow.appendChild(prefix);
      freqRow.appendChild(freqInput);
      freqRow.appendChild(unitSelect);
      kindSpecificRows.appendChild(freqRow);
    };

    kindSelect.onchange = () => {
      currentScript.kind =
        (kindSelect.value === 'normal') ? 'main' : kindSelect.value;
      if ((currentScript.kind || 'main') === 'login') {
        currentScript.keyScriptId = undefined;
      }
      renderKindSpecificRows();
      scheduleAutoSave(scriptId, 'script-kind', 0);
      renderAll();
    };

    renderKindSpecificRows();
    content.appendChild(body);

    const actions = documentRef.createElement('div');
    actions.className = 'modal-actions';
    const closeBtn = documentRef.createElement('button');
    closeBtn.className = 'btn';
    closeBtn.textContent = 'Close';
    const persistAndClose = async () => {
      await persistModalState({
        scriptId,
        currentScript,
        startUrlInput,
        applyExecutionFrequencyFromControls,
      });
      closeModal(modal);
    };
    closeBtn.onclick = () => {
      void persistAndClose();
    };
    actions.appendChild(closeBtn);
    content.appendChild(actions);
    modal.innerHTML = '';
    modal.appendChild(content);

    modal.onclick = (e) => {
      if (e.target !== modal) return;
      void persistAndClose();
    };
  }

  return {
    openScriptSettingsModal,
  };
}

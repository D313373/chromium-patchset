import { DashboardHandler } from './mojom-lite/dashboard.mojom-lite.js';
import { ACTION_METADATA, LOCATOR_TYPES } from './actions/index.js';
import { defaultAuthenticateParams } from './actions/authenticate_model.js';
import { applyTheme } from './settings.js';
import { buildSmartInputCatalog } from './data_view.js';
import './components/smartInput.js';  // Registers <smart-input> custom element
import { renderLocatorEditor } from './components/locatorEditor.js';
import { renderLayerEditor } from './components/layerEditor.js';
import { createDashboardTimelineRenderer } from './components/dashboard_timeline_renderer.js';
import { createDashboardActionEditorController } from './components/dashboard_action_editor.js';
import {
  normalizeAiLoopProfile,
} from './agent/loop_profiles.js';
import {
  installDashboardAgentApi,
} from './agent/command_dispatcher.js';
import { createDashboardAutoHealManager } from './agent/dashboard_auto_heal.js';
import { createDashboardAutoHealAgentRunner } from './agent/dashboard_auto_heal_agent_runner.js';
import { watchPlaybackAndAutoHeal } from './agent/dashboard_playback_auto_heal.js';
import {
  registerDashboardRecorderBridge,
  installDashboardLocatorCaptureBridge,
} from './agent/recorder_bridge.js';
import {
  markDraftAction,
  getNestedActionList,
  getLoopStepsList,
  getKeyDownNestedList,
  buildUrlContextsFromActions,
  flattenActionsFromUrlContexts,
  normalizeScriptsDict,
} from './agent/dashboard_script_state.js';
import { formatActionTitle } from './agent/dashboard_timeline.js';
import { createDashboardPersistenceManager } from './agent/dashboard_persistence.js';
import { createDashboardScriptNormalizer } from './agent/dashboard_script_normalization.js';
import { createDashboardScriptQueueController } from './agent/script/dashboard_script_queue_controller.js';
import { createDashboardViewRouter } from './agent/ui/dashboard_view_router.js';
import { installDashboardTimelineDragDrop } from './agent/timeline/dashboard_timeline_dragdrop.js';
import { installDashboardSchema } from './agent/dashboard_schema.js';
import { installDashboardUtilityBridges } from './agent/ui/dashboard_bridge_helpers.js';
import {
  initializeDashboardShell,
  loadScriptsIntoState,
} from './agent/ui/dashboard_bootstrap.js';
import { createDashboardAutoHealViewCallbacks } from './agent/ui/dashboard_auto_heal_callbacks.js';
import {
  createDashboardScriptCrudController,
  createDashboardScriptSettingsController,
} from './agent/dashboard_script_list.js';
import {promptScriptExport} from './agent/script_export.js';

// Minimal helper to replace chrome://resources util.js '$'
const $ = (id) => document.getElementById(id);

// Shared helpers used across the dashboard UI (autosave normalization, agent
// command normalization, etc). These must live at module scope.
function parseVisualCoords(value) {
  const ensureNumber = (n) => (Number.isFinite(n) ? Math.round(n) : null);
  if (value == null) return null;

  if (typeof value === 'object') {
    const x = ensureNumber(Number(value.x));
    const y = ensureNumber(Number(value.y));
    if (x === null && y === null)
      return null;
    return {x, y};
  }

  const s = String(value).trim();
  if (!s)
    return null;

  try {
    const parsed = JSON.parse(s);
    if (parsed && typeof parsed === 'object') {
      const x = ensureNumber(Number(parsed.x));
      const y = ensureNumber(Number(parsed.y));
      if (x === null && y === null)
        return null;
      return {x, y};
    }
  } catch (e) {
    // Fall through to "120,360" or "120 360" parsing.
  }

  const parts = s.split(/[, ]+/).filter(Boolean);
  if (parts.length < 2)
    return null;
  const x = ensureNumber(Number(parts[0]));
  const y = ensureNumber(Number(parts[1]));
  if (x === null && y === null)
    return null;
  return {x, y};
}

function formatVisualCoords(coords) {
  if (!coords) return '';
  const x = Number.isFinite(coords.x) ? coords.x : 0;
  const y = Number.isFinite(coords.y) ? coords.y : 0;
  return JSON.stringify({x, y});
}

const scriptNormalizer = createDashboardScriptNormalizer({
  parseVisualCoords,
  formatVisualCoords,
  flattenActionsFromUrlContexts,
  buildUrlContextsFromActions,
});
const {
  canonicalizeLocatorType,
  normalizeLayerParams,
  extractLayerHintsFromLocators,
  normalizeAction,
  normalizeScriptForSave,
} = scriptNormalizer;

// 3. GET THE REAL MOJO REMOTE HANDLER
const dashboardHandler = DashboardHandler.getRemote();
// Expose for debugging from DevTools console.
window.dashboardHandler = dashboardHandler;
installDashboardUtilityBridges({
  windowRef: window,
  documentRef: document,
  installLocatorCaptureBridge: installDashboardLocatorCaptureBridge,
  chromeSend: (name, payload) => chrome.send(name, payload),
  alertFn: (msg) => alert(msg),
});
installDashboardSchema({
  windowRef: window,
  handler: dashboardHandler,
  actionMetadata: ACTION_METADATA,
});

function applySmartInputCatalog(catalog, appStateRef = null) {
  if (!catalog || typeof catalog !== 'object') {
    return;
  }
  window.__dashboardSmartInputCatalog = catalog;
  window.globalTags = catalog.tags || {};
  window.dispatchEvent(new CustomEvent('dashboard-smartinput-data-changed',
                                        {detail: catalog}));
  if (appStateRef && typeof appStateRef === 'object') {
    appStateRef.dataReferenceCatalog = catalog;
  }
}

async function refreshSmartInputCatalogFromBackend(handler, appStateRef = null) {
  if (!handler) {
    return;
  }
  try {
    const [varsRes, secretsRes] = await Promise.all([
      handler.getDataVariables?.() ?? {variables: []},
      handler.getDataSecrets?.() ?? {secrets: []},
    ]);
    const vars = Array.isArray(varsRes?.variables) ? varsRes.variables : [];
    const secrets = Array.isArray(secretsRes?.secrets) ? secretsRes.secrets : [];
    const catalog = buildSmartInputCatalog(
        vars, secrets, appStateRef?.tables || {}, appStateRef?.scripts || {});
    applySmartInputCatalog(catalog, appStateRef);
  } catch (e) {
    console.warn('[DashboardUI] refreshSmartInputCatalogFromBackend failed', e);
  }
}

window.appendToTimelineFromAgent = function(action) {
  if (typeof window.insertAgentActionIntoTimeline === 'function') {
    window.insertAgentActionIntoTimeline(action);
  } else if (window.appendRecordedActionToTimeline) {
    window.appendRecordedActionToTimeline(action);
  } else {
    const card = createActionCardFromSchema(action); // fallback
    document.getElementById('timeline-tab')?.appendChild(card);
  }
};

// =============================================================================
// MAIN APPLICATION LOGIC
// =============================================================================
let appState = {
    scripts: {},
    currentScriptId: null,
    currentView: 'dashboard',
    isEditingAction: false,
    editingActionData: null,
    tables: {},  // For table-based actions
    selectedScripts: [], // queue of scriptIds in execution order
    isQueueRunning: false,
    queueStopRequested: false,
    currentlyExecutingId: null
};

const autoHealState = {
  autoQueueEnabled: false,
  preferHeadlessInvestigation: true,
  queue: [],
  inFlight: false,
  operationToken: 0,
  lastFailures: [],
  knownFailureKeys: new Set(),
  currentTask: null,
  progress: null,
  suppressAutoQueueUntil: 0,
};

window.__dashAutoHealState = autoHealState;

// Expose appState globally for action editors that need it
window.appState = appState;

function collectActionStrings(value, out) {
  if (typeof value === 'string') {
    out.push(value);
    return;
  }
  if (Array.isArray(value)) {
    for (const item of value) collectActionStrings(item, out);
    return;
  }
  if (value && typeof value === 'object') {
    for (const nested of Object.values(value)) collectActionStrings(nested, out);
  }
}

function extractTokenRefsFromText(text) {
  const refs = [];
  const s = String(text || '');
  const re = /\{([^{}]+)\}/g;
  let m = re.exec(s);
  while (m) {
    const raw = String(m[1] || '').trim();
    if (!/^[A-Za-z0-9_.:#-]+$/.test(raw)) {
      m = re.exec(s);
      continue;
    }
    const normalized = raw.toLowerCase();
    if (normalized) refs.push({normalized, display: raw});
    m = re.exec(s);
  }
  return refs;
}

function normalizeRuntimeTokenName(raw) {
  return String(raw || '').trim().toLowerCase();
}

function collectDeclaredRuntimeTokens(action, tokenSet,
                                      {includeFocusLabels = false} = {}) {
  if (!action || typeof action !== 'object' || !(tokenSet instanceof Set)) {
    return;
  }

  const params =
      (action.params && typeof action.params === 'object') ? action.params : {};
  const actionName = String(action.action || '').trim().toLowerCase();

  const register = (value) => {
    const normalized = normalizeRuntimeTokenName(value);
    if (normalized) {
      tokenSet.add(normalized);
    }
  };

  register(params.captureVariable || action.captureVariable || '');
  register(params.variableName || action.variableName || '');

  if (includeFocusLabels && actionName === 'glic_prompt' &&
      Array.isArray(params.focusAreas)) {
    for (const area of params.focusAreas) {
      register(area?.label || '');
    }
  }
}

// Provide a safe noop for native recording stopped notifications.
if (!window.DashboardRecordingStopped) {
  window.DashboardRecordingStopped = function() {
    return;
  };
}

document.addEventListener('DOMContentLoaded', async () => {
    void initializeDashboardShell({
      dashboardHandler,
      applyTheme,
    });
    const mainViews = {
        dashboard: $('dashboard-main-view'),
        debugging: $('dashboard-debugging-view'),
        data: $('dashboard-data-view'),
        settings: $('dashboard-settings-view'),
    };
    const navButtons = {
        dashboard: $('nav-dashboard-btn'),
        debugging: $('nav-debugging-btn'),
        data: $('nav-data-btn'),
        settings: $('nav-settings-btn'),
    };
    const scriptListEl = $('script-list');
    const timelineEl = $('timeline');
    const timelineHeader = $('timeline-header');
    const actionEditorEl = $('action-editor');
    const actionMenuEl = $('action-menu');
    const actionEditorShellEl = $('global-agent-console-shell');
    const timelineSidebarEl = $('timeline-sidebar');
    const createNewScriptBtn = $('create-new-script-btn');
    const exportScriptBtn = $('export-script-btn');
    const deleteScriptBtn = $('delete-script-btn-dash');
    const inputModal = $('input-modal');
    const scriptNameInput = $('script-name-input');
    const confirmInputBtn = $('confirm-input');
    const cancelInputBtn = $('cancel-input');
    const confirmationModal = $('confirmation-modal');
    const confirmationText = $('confirmation-text');
    const confirmActionBtn = $('confirm-action');
    const cancelConfirmBtn = $('cancel-confirm');
    let renderAll = async () => {};
    let scheduleRender = () => {};
    let switchView = () => {};
    let scriptQueueController = null;

    if (actionEditorShellEl) {
      actionEditorShellEl.hidden = true;
    }
    document.body.classList.remove('editor-shell-open');
    let actionEditorShellHideTimer = 0;

    function syncActionEditorShell({deferHide = true} = {}) {
      if (!actionEditorShellEl) {
        document.body.classList.toggle('editor-shell-open', !!appState.isEditingAction);
        return;
      }

      if (actionEditorShellHideTimer) {
        clearTimeout(actionEditorShellHideTimer);
        actionEditorShellHideTimer = 0;
      }

      if (appState.isEditingAction) {
        actionEditorShellEl.hidden = false;
        actionEditorEl.classList.remove('hidden');
        document.body.classList.add('editor-shell-open');
        if (typeof renderActionEditor === 'function') {
          renderActionEditor();
        }
        return;
      }

      document.body.classList.remove('editor-shell-open');
      if (!deferHide) {
        actionEditorShellEl.hidden = true;
        actionEditorEl.classList.add('hidden');
        return;
      }

      actionEditorShellHideTimer = window.setTimeout(() => {
        actionEditorShellHideTimer = 0;
        if (!appState.isEditingAction) {
          actionEditorShellEl.hidden = true;
          actionEditorEl.classList.add('hidden');
        }
      }, 180);
    }

    const autoHealAgentRunner = createDashboardAutoHealAgentRunner({
      windowRef: window,
      dashboardHandler,
      chromeSend: (name, args) => chrome.send(name, args),
    });

    // Investigator → assistant handoff for heal + general tech-support tasks.
    window.dashboardAgentRunSupportHandoff = async (prompt, options = {}) => {
      if (!autoHealAgentRunner?.runSupportHandoffPrompt) {
        return {ok: false, error: 'support_handoff_unavailable'};
      }
      return autoHealAgentRunner.runSupportHandoffPrompt(prompt, options || {});
    };
    window.dashboardAgentRunInvestigatorAssistantHandoff = async (prompt, options = {}) => {
      if (!autoHealAgentRunner?.runInvestigatorAssistantHandoff) {
        return {ok: false, error: 'handoff_runner_unavailable'};
      }
      return autoHealAgentRunner.runInvestigatorAssistantHandoff(
          prompt, options || {});
    };

    const autoHealManager = createDashboardAutoHealManager({
      windowRef: window,
      dashboardHandler,
      autoHealState,
      sendPrompt: async (prompt, context = {}) => {
        if (!autoHealAgentRunner) {
          return {ok: false, error: 'auto_heal_agent_runner_unavailable'};
        }
        try {
          const res = await autoHealAgentRunner.runAutoHealPrompt(prompt, {
            onProgress: typeof context?.onProgress === 'function' ?
              context.onProgress :
              null,
            taskId: context?.taskId || '',
          });
          if (!res?.ok) {
            console.warn('[AutoHeal] agent run finished with issues', res);
          }
          return {ok: res?.ok !== false, result: res};
        } catch (e) {
          console.warn('[AutoHeal] agent run failed', e);
          return {ok: false, error: String(e && e.message ? e.message : e)};
        }
      },
    });

    const autoHealUnavailable = () => ({ok: false, error: 'auto_heal_manager_unavailable'});
    const enqueueAutoHealIncident = (...args) =>
      autoHealManager ? autoHealManager.enqueueAutoHealIncident(...args) : autoHealUnavailable();
    const processAutoHealQueue = (...args) =>
      autoHealManager ? autoHealManager.processAutoHealQueue(...args) : autoHealUnavailable();
    const cancelCurrentAutoHeal = (...args) =>
      autoHealManager ? autoHealManager.cancelCurrentAutoHeal(...args) : autoHealUnavailable();
    const removeQueueItemByKey = (...args) =>
      autoHealManager ? autoHealManager.removeQueueItemByKey(...args) : autoHealUnavailable();
    const moveQueueItemByKey = (...args) =>
      autoHealManager ? autoHealManager.moveQueueItemByKey(...args) : autoHealUnavailable();
    const prioritizeQueueItem = (...args) =>
      autoHealManager ? autoHealManager.prioritizeQueueItem(...args) : autoHealUnavailable();
    const autoQueueFromFailureSnapshot = (...args) =>
      autoHealManager ? autoHealManager.autoQueueFromFailureSnapshot(...args) : undefined;
    const queueStateSnapshot = (...args) =>
      autoHealManager ? autoHealManager.queueStateSnapshot(...args) : {
        enabled: false,
        in_flight: false,
        current_key: '',
        queue: [],
      };
    const updateAutoHealSettings = (...args) =>
      autoHealManager ? autoHealManager.updateAutoHealSettings(...args) : undefined;
    const clearAutoHealQueue = (...args) =>
      autoHealManager ? autoHealManager.clearQueue(...args) : autoHealUnavailable();
    const autoHealViewCallbacks = createDashboardAutoHealViewCallbacks({
      autoHealState,
      autoQueueFromFailureSnapshot,
      queueStateSnapshot,
      enqueueAutoHealIncident,
      processAutoHealQueue,
      cancelCurrentAutoHeal,
      clearAutoHealQueue,
      removeQueueItemByKey,
      moveQueueItemByKey,
      prioritizeQueueItem,
      alertFn: (msg) => alert(msg),
    });

    const askDashboardModelFromDebugging = (prompt, context = {}) => {
      const text = String(prompt || '').trim();
      if (!text) {
        return {ok: false, error: 'empty_prompt'};
      }
      const contextJson = (() => {
        try {
          return JSON.stringify(context || {}, null, 2);
        } catch (_) {
          return '{}';
        }
      })();
      console.warn('[DashboardUI] Legacy dashboard agent console removed', {
        prompt: text,
        contextJson,
      });
      alert('Dashboard agent console was removed. Use the Glic UI for interactive agent work.');
      return {ok: false, error: 'dashboard_agent_console_removed'};
    };

    if (autoHealManager) {
      autoHealManager.installAgentApi();
      void autoHealManager.syncAutoHealSettingsFromBackend();
    } else {
      console.warn('[AutoHeal] manager unavailable; auto-heal features disabled');
    }

    window.createFreshAgentGoalScript = async function(goal) {
      const trimmedGoal = String(goal || '').trim();
      const title = trimmedGoal ? trimmedGoal.slice(0, 48) : 'Untitled Goal';
      const name = `Agent Goal: ${title}`;

      try {
        const created = await dashboardHandler.createNewScript({name});
        const scriptId = created?.scriptId || `agent_${Date.now()}`;
        appState.scripts[scriptId] = appState.scripts[scriptId] || {
          name,
          actions: [],
          settings: {},
          kind: 'main'
        };
        appState.currentScriptId = scriptId;
        appState.scripts[scriptId].__agent_next_insert_index = 0;
        scheduleRender('agent-create-goal');
        return {ok: true, scriptId, name};
      } catch (e) {
        console.warn('[DashboardUI] createFreshAgentGoalScript failed', e);
        return {ok: false, error: String(e)};
      }
    };

    window.insertAgentActionIntoCurrentScriptTimeline = function(action) {
      const scriptId = appState.currentScriptId;
      const script = scriptId ? appState.scripts?.[scriptId] : null;
      if (!script) {
        return {ok: false, error: 'No active script'};
      }

      let cloned = null;
      if (action && typeof action === 'object') {
        try {
          cloned = JSON.parse(JSON.stringify(action));
        } catch (_) {
          cloned = null;
        }
      }
      if (!cloned) {
        return {ok: false, error: 'Invalid action'};
      }

      const normalized = normalizeAction(cloned) || cloned;
      const targetList = script.actions || (script.actions = []);
      const captureGuidedLoopMetadata = (candidateAction, actions, actionIndex) => {
        if (!candidateAction || String(candidateAction.action || '').toLowerCase() !== 'scrape_table') {
          return;
        }
        const params = candidateAction.params && typeof candidateAction.params === 'object' ?
          candidateAction.params :
          (candidateAction.params = {});
        const guidedMeta = params.guidedMeta && typeof params.guidedMeta === 'object' ?
          {...params.guidedMeta} :
          {};

        for (let i = actionIndex - 1; i >= 0; i -= 1) {
          if (actionIndex - i > 4) {
            break;
          }
          const previous = actions[i];
          if (String(previous?.action || '').toLowerCase() !== 'loop') {
            continue;
          }
          const previousParams =
            previous?.params && typeof previous.params === 'object' ?
              previous.params :
              {};
          if (String(previousParams.loopMode || '').toLowerCase() !== 'for_each') {
            continue;
          }
          if (!Array.isArray(guidedMeta.loopTargetLocators) ||
              guidedMeta.loopTargetLocators.length === 0) {
            const locators = Array.isArray(previousParams.locators) ?
              previousParams.locators :
              [];
            if (locators.length) {
              guidedMeta.loopTargetLocators = JSON.parse(JSON.stringify(locators));
            }
          }
          if (!guidedMeta.originUrl) {
            const contextUrl = String(previous.contextUrl || previous.context_url || '').trim();
            if (contextUrl) {
              guidedMeta.originUrl = contextUrl;
            }
          }
          if (!Number.isFinite(Number(guidedMeta.loopTargetCount))) {
            const count = Number(previousParams.maxIterations);
            if (Number.isFinite(count) && count > 0) {
              guidedMeta.loopTargetCount = Math.floor(count);
            }
          }
          break;
        }

        if (Object.keys(guidedMeta).length) {
          params.guidedMeta = guidedMeta;
        }
      };

      const preferred = Number.isInteger(script.__agent_next_insert_index)
        ? script.__agent_next_insert_index
        : targetList.length;
      const insertIndex = Math.max(0, Math.min(preferred, targetList.length));
      captureGuidedLoopMetadata(normalized, targetList, insertIndex);
      targetList.splice(insertIndex, 0, normalized);
      script.__agent_next_insert_index = insertIndex + 1;

      scheduleAutoSave(scriptId, 'agent-success');
      scheduleRender('agent-insert-action');
      return {ok: true, scriptId, index: insertIndex};
    };

    registerDashboardRecorderBridge({
      windowRef: window,
      insertAction: (action) =>
          window.insertAgentActionIntoCurrentScriptTimeline(action),
      saveScript: (scriptId, opts) => saveScriptById(scriptId, opts),
      getCurrentScriptId: () => appState.currentScriptId || '',
      getLoopProfile: () => {
        const getter = window.dashboardGetAiLoopProfile;
        return typeof getter === 'function' ? getter() : window.__dashAiLoopProfile;
      },
      normalizeLoopProfile: normalizeAiLoopProfile,
    });

    Object.keys(navButtons).forEach(viewName => {
        navButtons[viewName].addEventListener('click', () => switchView(viewName));
    });

    const scriptCrudController = createDashboardScriptCrudController({
      appState,
      dashboardHandler,
      renderAll: (...args) => scheduleRender(...args),
      inputModalTitleEl: $('input-modal-title'),
      inputModal,
      scriptNameInput,
      confirmInputBtn,
      cancelInputBtn,
      confirmationModal,
      confirmationText,
      confirmActionBtn,
      cancelConfirmBtn,
    });
    createNewScriptBtn.addEventListener(
      'click', () => void scriptCrudController.handleNewScript());
    deleteScriptBtn.addEventListener(
      'click', () => scriptCrudController.handleDeleteScript());

    const captureLocatorsBtn = $('capture-locators-btn');
    if (captureLocatorsBtn) {
      captureLocatorsBtn.addEventListener('click', () => {
        void window.dashboardStartLocatorCapture('dashboard_button');
      });
    }

    // Script-specific settings (overrides) modal
    const scriptSettingsController = createDashboardScriptSettingsController({
      appState,
      renderAll: (...args) => scheduleRender(...args),
      scheduleAutoSave: (...args) => scheduleAutoSave(...args),
      flushPendingSavesAndSaveNow: (...args) =>
          flushPendingSavesAndSaveNow(...args),
      documentRef: document,
      modalId: 'script-settings-modal',
    });
    const scriptSettingsBtn = $('script-settings-btn');
    const scriptSettingsModal = $('script-settings-modal');
    if (scriptSettingsBtn && scriptSettingsModal) {
      scriptSettingsBtn.addEventListener('click', () => {
        scriptSettingsController.openScriptSettingsModal();
      });
    }

    installDashboardTimelineDragDrop({
      actionMenuEl,
      timelineEl,
      appState,
      nestedRuleViolationMessage: (...args) =>
          nestedRuleViolationMessage(...args),
      resolveTargetList: (...args) => resolveTargetList(...args),
      resolveTargetListByPath: (...args) =>
          resolveTargetListByPath(...args),
      createNewActionFromType,
      markDraftAction,
      cancelAutoSaveTimer: (...args) => cancelAutoSaveTimer(...args),
      scheduleAutoSave: (...args) => scheduleAutoSave(...args),
      renderAll: (...args) => scheduleRender(...args),
      alertFn: (msg) => alert(msg),
    });

    function renderDashboardView() {
        const currentScript = appState.scripts[appState.currentScriptId];
        timelineHeader.textContent = currentScript ? currentScript.name : 'No Script Selected';
        if (scriptQueueController) {
          scriptQueueController.renderScriptList();
        }
        renderTimeline();
        if (appState.isEditingAction) {
            actionMenuEl.classList.add('hidden');
            if (timelineSidebarEl) {
              timelineSidebarEl.hidden = true;
            }
            syncActionEditorShell({deferHide: false});
        } else {
            actionMenuEl.classList.remove('hidden');
            if (timelineSidebarEl) {
              timelineSidebarEl.hidden = false;
            }
            syncActionEditorShell({deferHide: true});
            renderActionMenu();
        }
    }

    const viewRouter = createDashboardViewRouter({
      appState,
      mainViews,
      navButtons,
      dashboardHandler,
      renderDashboardView,
      syncActionEditorShell: (...args) => syncActionEditorShell(...args),
      onApplySmartInputCatalog: (catalog) =>
          applySmartInputCatalog(catalog, appState),
      ...autoHealViewCallbacks,
      onAskDashboardModel: askDashboardModelFromDebugging,
      updateAutoHealSettings,
      processAutoHealQueue,
      autoHealState,
      alertFn: (msg) => alert(msg),
    });
    switchView = (...args) => viewRouter.switchView(...args);
    renderAll = (...args) => viewRouter.renderAll(...args);
    scheduleRender = (...args) => viewRouter.scheduleRender(...args);

    async function createNewActionFromType(actionType) {
      // Prefer backend schema templates so ScriptSchema remains the source of truth.
      let action = await window.DashboardSchema.getActionTemplate(actionType);
      if (!action || typeof action !== 'object') action = { action: actionType, params: {} };
      if (!action.params || typeof action.params !== 'object') action.params = {};

      // Control-flow actions store their branches at the top-level.
      if (actionType === 'if_condition') {
        action.then = Array.isArray(action.then) ? action.then : [];
        action.else = Array.isArray(action.else) ? action.else : [];
      }
      if (actionType === 'authenticate') {
        const seeded = defaultAuthenticateParams();
        if (!Array.isArray(action.params.methods) || !action.params.methods.length) {
          action.params.methods = seeded.methods;
        }
        if (!action.params.loopMode) {
          action.params.loopMode = seeded.loopMode;
        }
      }
      if (actionType === 'loop') {
        action.steps = Array.isArray(action.steps) ? action.steps : [];
      }
      if (actionType === 'key_down' || actionType === 'key') {
        action.nested = Array.isArray(action.nested) ? action.nested : [];
      }

      // UI-friendly defaults for waits.
      if (actionType === 'wait') {
        if (!action.params.waitType) action.params.waitType = 'time';
        if (action.params.time == null) action.params.time = 1000;
      }

      return action;
    }

    let dashboardActionEditorController = null;

    const timelineRenderer = createDashboardTimelineRenderer({
      actionMenuEl,
      timelineEl,
      appState,
      actionMetadata: ACTION_METADATA,
      flattenActionsFromUrlContexts,
      getNestedActionList,
      getLoopStepsList,
      getKeyDownNestedList,
      markDraftAction,
      createNewActionFromType,
      cancelAutoSaveTimer: (...args) => cancelAutoSaveTimer(...args),
      scheduleAutoSave: (...args) => scheduleAutoSave(...args),
      renderAll: (...args) => scheduleRender(...args),
      formatActionTitle,
      alertFn: (msg) => alert(msg),
    });
    const nestedRuleViolationMessage =
        timelineRenderer.nestedRuleViolationMessage;
    const resolveTargetListByPath = timelineRenderer.resolveTargetListByPath;
    const renderActionMenu = timelineRenderer.renderActionMenu;
    const renderTimeline = timelineRenderer.renderTimeline;
    const resolveTargetList = timelineRenderer.resolveTargetList;

    const collectScriptActionList = (script) => {
      if (!script || typeof script !== 'object') return [];
      const primary = Array.isArray(script.actions) ? script.actions : [];
      if (primary.length) {
        return primary;
      }
      return flattenActionsFromUrlContexts(script.url_contexts);
    };

    const collectDeepActionList = (actions, out = []) => {
      if (!Array.isArray(actions)) return out;
      for (const action of actions) {
        if (!action || typeof action !== 'object') continue;
        out.push(action);
        collectDeepActionList(getNestedActionList(action, 'then', false), out);
        collectDeepActionList(getNestedActionList(action, 'else', false), out);
        collectDeepActionList(getLoopStepsList(action, false), out);
        collectDeepActionList(getKeyDownNestedList(action, false), out);
        const methods = action.params && Array.isArray(action.params.methods) ?
          action.params.methods : [];
        for (const method of methods) {
          if (method && method.custom === true) {
            collectDeepActionList(method.actions, out);
          }
        }
      }
      return out;
    };

    const hasDraftActionsInScript = (script) => {
      const actions = collectScriptActionList(script);
      const deep = collectDeepActionList(actions, []);
      return deep.some((action) => !!(action && typeof action === 'object' &&
          action.__draft));
    };

    const runPreflightForScript = async (scriptId) => {
      const id = String(scriptId || '').trim();
      const script = id ? appState.scripts?.[id] : null;
      const scriptName = script?.name || id || '(unknown script)';
      const report = {
        scriptId: id,
        scriptName,
        errors: [],
        warnings: [],
      };
      if (!script || typeof script !== 'object') {
        report.errors.push('Script not found.');
        return report;
      }

      const actions = collectScriptActionList(script);
      if (!Array.isArray(actions) || actions.length === 0) {
        report.errors.push('Script has no actions.');
      }
      if (hasDraftActionsInScript(script)) {
        report.errors.push('Script has unsaved draft actions.');
      }

      const allActions = collectDeepActionList(actions, []);
      const knownTokens = new Set(
          (appState.dataReferenceCatalog?.tokens || [])
              .map((t) => String(t || '').trim().toLowerCase())
              .filter(Boolean));
      const unresolved = new Map();
      const schemaApi = window.DashboardSchema;

      for (let i = 0; i < allActions.length; i += 1) {
        const action = allActions[i];
        if (schemaApi && typeof schemaApi.validateAction === 'function') {
          try {
            const res = await schemaApi.validateAction(action);
            if (res && res.valid === false) {
              report.errors.push(
                  `Action #${i + 1} (${String(action.action || 'unknown')}): ${String(res.error || 'invalid')}`);
              if (report.errors.length >= 10) break;
            }
          } catch (e) {
            report.errors.push(
                `Action #${i + 1} (${String(action.action || 'unknown')}): validate crashed (${String(e && e.message ? e.message : e)})`);
            if (report.errors.length >= 10) break;
          }
        }

        const visibleTokens = new Set(knownTokens);
        collectDeclaredRuntimeTokens(
            action, visibleTokens, {includeFocusLabels: true});
        const strings = [];
        collectActionStrings(action, strings);
        for (const s of strings) {
          for (const ref of extractTokenRefsFromText(s)) {
            if (!visibleTokens.has(ref.normalized) &&
                !unresolved.has(ref.normalized)) {
              unresolved.set(ref.normalized, ref.display);
            }
          }
        }

        collectDeclaredRuntimeTokens(action, knownTokens);
      }

      if (script.keyScriptId && !appState.scripts?.[script.keyScriptId]) {
        report.warnings.push(`Key script "${script.keyScriptId}" is missing.`);
      }
      if (unresolved.size) {
        const sample = Array.from(unresolved.values()).slice(0, 6);
        report.warnings.push(
            `Unresolved data tokens: ${sample.join(', ')}${unresolved.size > sample.length ? ' ...' : ''}`);
      }
      return report;
    };

    scriptQueueController = createDashboardScriptQueueController({
      appState,
      dashboardHandler,
      scriptListEl,
      renderAll: (...args) => scheduleRender(...args),
      runPreflight: (scriptId) => runPreflightForScript(scriptId),
      playScriptWithAutoHeal: async (scriptId) => {
        await watchPlaybackAndAutoHeal({
          dashboardHandler,
          appState,
          autoHealState,
          enqueueAutoHealIncident,
          processAutoHealQueue,
          scriptId,
          options: {waitForCompletion: true},
        });
      },
      onClearAutoHealQueue: () => {
        cancelCurrentAutoHeal('stop_all_scripts');
        clearAutoHealQueue();
      },
      alertFn: (msg) => alert(msg),
      confirmFn: (msg) => confirm(msg),
    });
    window.__debugRunSelected = () => (
      scriptQueueController ? scriptQueueController.handleRunSelected() : Promise.resolve());
    window.__debugStopAllScripts = () => (
      scriptQueueController ? scriptQueueController.handleStopAllScripts() : Promise.resolve());

    const persistenceManager = createDashboardPersistenceManager({
      appState,
      dashboardHandler,
      normalizeScriptForSave,
      normalizeScriptsDict,
      renderAll: (...args) => scheduleRender(...args),
      getNestedActionList,
      getLoopStepsList,
      getKeyDownNestedList,
      validateAction: (action) => window.DashboardSchema.validateAction(action),
      alertFn: (msg) => alert(msg),
    });
    const flushPendingSavesAndSaveNow =
        persistenceManager.flushPendingSavesAndSaveNow;
    const cancelAutoSaveTimer = persistenceManager.cancelAutoSaveTimer;
    const saveScriptById = persistenceManager.saveScriptById;
    const performAutoSave = persistenceManager.performAutoSave;
    const scheduleAutoSave = persistenceManager.scheduleAutoSave;
    const scriptHasDraftActions = persistenceManager.scriptHasDraftActions;

    exportScriptBtn?.addEventListener('click', async () => {
      const scriptId = appState.currentScriptId;
      const script = scriptId ? appState.scripts?.[scriptId] : null;
      if (!script) {
        alert('Select a script to export.');
        return;
      }
      try {
        await flushPendingSavesAndSaveNow(scriptId);
      } catch (_) {
      }
      await promptScriptExport({
        script,
        scriptId,
        dashboardHandler,
        onScriptUpdated: async (updatedScript) => {
          appState.scripts[scriptId] = updatedScript;
          await saveScriptById(scriptId, {silent: true, refreshAfter: true});
        },
      });
    });

    dashboardActionEditorController = createDashboardActionEditorController({
      appState,
      actionEditorEl,
      actionMetadata: ACTION_METADATA,
      renderLayerEditor,
      renderLocatorEditor,
      scheduleAutoSave,
      saveScriptById,
      dashboardHandler,
      refreshSmartInputCatalog: () =>
          refreshSmartInputCatalogFromBackend(dashboardHandler, appState),
      resolveTargetList,
      renderAll: (...args) => scheduleRender(...args),
      normalizeLayerParams,
      extractLayerHintsFromLocators,
      canonicalizeLocatorType,
      normalizeAction,
      validateAction: (action) => window.DashboardSchema.validateAction(action),
      onScriptsChanged: () => {
        if (typeof window.DashboardScriptsChanged === 'function') {
          window.DashboardScriptsChanged();
        }
      },
      alertFn: (msg) => alert(msg),
      promptFn: (msg, defaultValue = '') => prompt(msg, defaultValue),
      documentRef: document,
    });
    window.dashboardApplyCapturedLocatorMetadata = (metadata = {}) =>
      dashboardActionEditorController.applyLocatorCaptureMetadata(metadata);

    // Initial render must happen after timeline/action-editor controllers are
    // initialized, otherwise dashboard view can reference TDZ-bound bindings.
    switchView('dashboard');
    // Defensive layout recovery: if modularized render/state wiring leaves the
    // shell hidden, force key containers visible so users don't see a blank UI.
    try {
      const ensureVisible = (el, displayValue, label) => {
        if (!el) return;
        const hidden = getComputedStyle(el).display === 'none';
        if (hidden) {
          el.style.display = displayValue;
          console.warn(`[DashboardUI] Forced visible: ${label}`);
        }
      };
      ensureVisible(document.querySelector('.dashboard-nav'), 'block', 'dashboard-nav');
      ensureVisible(document.querySelector('.dashboard-content'), 'flex', 'dashboard-content');
      const activeView = mainViews[appState.currentView] || mainViews.dashboard;
      if (activeView && getComputedStyle(activeView).display === 'none') {
        activeView.classList.add('active');
        console.warn('[DashboardUI] Forced active class on current view');
      }
    } catch (e) {
      console.warn('[DashboardUI] layout recovery failed', e);
    }
    // Expose a manual save hook for debugging.
    window.__dashboardAutoSaveNow = () =>
      performAutoSave(appState.currentScriptId, 'manual');

    // The recorder can call this after state changes; refresh scripts when it
    // does so new saves become visible.
    if (!window.DashboardRecordingStopped) {
      window.DashboardRecordingStopped = function() {
        if (typeof window.DashboardScriptsChanged === 'function') {
          window.DashboardScriptsChanged();
        }
      }
    }

    installDashboardAgentApi({
      windowRef: window,
      documentRef: document,
      appState,
      dashboardHandler,
      chromeSend: (name, args) => chrome.send(name, args),
      normalizeScriptsDict,
      renderAll: (...args) => scheduleRender(...args),
      switchView,
      scriptHasDraftActions,
      updateAutoHealSettings,
      processAutoHealQueue,
      getAutoHealState: () => autoHealState,
    });

    const bootstrapInitialDashboardState = async () => {
      await loadScriptsIntoState({
        dashboardHandler,
        normalizeScriptsDict,
        appState,
      });
      await refreshSmartInputCatalogFromBackend(dashboardHandler, appState);
      scheduleRender('initial-dashboard-state');
    };

    void bootstrapInitialDashboardState();


function renderActionEditor() {
  if (dashboardActionEditorController) {
    dashboardActionEditorController.renderActionEditor();
  }
}
});

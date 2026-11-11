import { renderDebuggingView } from '../../debugging_view.js';
import { renderDataView } from '../../data_view.js';
import { renderSettingsTab } from '../../settings.js';

export function createDashboardViewRouter({
  appState,
  mainViews,
  navButtons,
  dashboardHandler,
  renderDashboardView,
  syncActionEditorShell = () => {},
  onApplySmartInputCatalog,
  onDebugDataSnapshot,
  getAutoHealQueueState,
  onQueueAutoHeal,
  onAutoHealProcessNext,
  onAutoHealCancelCurrent,
  onAutoHealClearQueue,
  onAutoHealRemoveQueueItem,
  onAutoHealMoveQueueItem,
  onAutoHealPrioritizeQueueItem,
  onAskDashboardModel,
  updateAutoHealSettings,
  processAutoHealQueue,
  autoHealState,
  alertFn = (msg) => alert(msg),
}) {
  if (!appState || !mainViews || !navButtons || !dashboardHandler) {
    throw new Error('createDashboardViewRouter requires app/ui dependencies');
  }

  let renderScheduled = false;
  let renderInFlight = false;
  let renderQueued = false;
  let renderRequestToken = 0;

  const escapeHtml = (value) => String(value || '').replace(
      /[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));

  const scheduleOnNextFrame = (fn) => {
    if (typeof globalThis.requestAnimationFrame === 'function') {
      globalThis.requestAnimationFrame(() => fn());
      return;
    }
    setTimeout(fn, 0);
  };

  const runRenderCycle = async () => {
    if (renderInFlight) {
      renderQueued = true;
      return;
    }
    renderInFlight = true;
    try {
      await renderAll();
    } finally {
      renderInFlight = false;
      if (renderQueued) {
        renderQueued = false;
        scheduleRender('queued');
      }
    }
  };

  function scheduleRender(_reason = '') {
    renderRequestToken += 1;
    if (renderScheduled) return renderRequestToken;
    renderScheduled = true;
    scheduleOnNextFrame(() => {
      renderScheduled = false;
      void runRenderCycle();
    });
    return renderRequestToken;
  }

  function switchView(viewName) {
    appState.currentView = viewName;
    Object.keys(mainViews).forEach((key) => {
      const viewEl = mainViews[key];
      if (viewEl) {
        viewEl.classList.toggle('active', key === viewName);
      }
      const navBtn = navButtons[key];
      if (navBtn) {
        navBtn.classList.toggle('active', key === viewName);
      }
    });
    scheduleRender('switch-view');
  }

  async function renderAll() {
    try {
      syncActionEditorShell();
    } catch (e) {
      console.error('syncActionEditorShell failed', e);
    }

    if (appState.currentView === 'dashboard') {
      const view = mainViews.dashboard;
      try {
        renderDashboardView();
      } catch (e) {
        console.error('renderDashboardView failed', e);
        if (view) {
          const msg = (e && e.message) ? e.message : String(e || 'Unknown error');
          view.innerHTML = `
            <div class="panel" style="max-width:960px;">
              <div class="panel-header">Dashboard View Failed To Render</div>
              <div class="dbg-incident-error">${escapeHtml(msg)}</div>
            </div>`;
        }
      }
      return;
    }

    if (appState.currentView === 'debugging') {
      const view = mainViews.debugging;
      if (!view) return;
      try {
        await renderDebuggingView(view, dashboardHandler, appState, {
          openScript: (scriptId) => {
            if (!scriptId) return;
            appState.currentScriptId = scriptId;
            switchView('dashboard');
          },
          onDebugDataSnapshot,
          getAutoHealQueueState: () => getAutoHealQueueState(),
          onQueueAutoHeal: (incident) => onQueueAutoHeal(incident, view),
          onAutoHealProcessNext: () => onAutoHealProcessNext(view),
          onAutoHealCancelCurrent: () => onAutoHealCancelCurrent(view),
          onAutoHealClearQueue: () => onAutoHealClearQueue(view),
          onAutoHealRemoveQueueItem: (key) => onAutoHealRemoveQueueItem(key, view),
          onAutoHealMoveQueueItem: (key, delta) =>
              onAutoHealMoveQueueItem(key, delta, view),
          onAutoHealPrioritizeQueueItem: (key) =>
              onAutoHealPrioritizeQueueItem(key, view),
          onAskDashboardModel: (prompt, context) => {
            if (typeof onAskDashboardModel === 'function') {
              onAskDashboardModel(prompt, context);
            }
          },
          isActive: () => appState.currentView === 'debugging',
        });
      } catch (e) {
        console.error('renderDebuggingView failed', e);
        const msg = (e && e.message) ? e.message : String(e || 'Unknown error');
        view.innerHTML = `
          <div class="dbg-root">
            <div class="dbg-header">
              <div class="dbg-header-left">
                <h1 class="dbg-title">Debugging</h1>
              </div>
            </div>
            <div class="dbg-panel" style="max-width:960px;">
              <div class="dbg-panel-title">Debugging view failed to render</div>
              <div class="dbg-incident-error">${msg}</div>
            </div>
          </div>`;
      }
      return;
    }

    if (appState.currentView === 'data') {
      const view = mainViews.data;
      if (!view) return;
      try {
        await renderDataView(view, dashboardHandler, appState, {
          onSmartInputDataChanged: (catalog) => onApplySmartInputCatalog(catalog),
        });
      } catch (e) {
        console.error('renderDataView failed', e);
        view.innerHTML = (
          '<div class="p-4"><h1 class="text-2xl mb-2">Data</h1>' +
          '<p>Failed to load data.</p></div>'
        );
      }
      return;
    }

    if (appState.currentView === 'settings') {
      const view = mainViews.settings;
      if (!view) return;
      try {
        const {settings} = await dashboardHandler.getEffectiveSettings('');
        const saveSettings = async (newSettings) => {
          try {
            const res = await dashboardHandler.setGlobalSettings(newSettings);
            const ok = !!(res &&
              (res.ok === true ||
               res.success === true ||
               res.result === true ||
               res === true));
            if (ok) {
              const quiet = !!(newSettings && newSettings.__quietAgentSave);
              if (quiet) {
                try { delete newSettings.__quietAgentSave; } catch (_) {}
              }
              const restartRequired = !!(res && res.restartRequired === true);
              const restartSwitches = Array.isArray(res?.restartSwitches) ?
                res.restartSwitches.map((v) => String(v || '').trim()).filter(Boolean) :
                [];
              try {
                // Update heal flags only — never start queue work from Settings
                // save (theme/toggles were kicking pending auto-heal runs).
                updateAutoHealSettings(newSettings || {});
              } catch (_) {}
              if (restartRequired && restartSwitches.length) {
                alertFn(
                    'Settings saved. Restart required for some options.\n\n' +
                    'Pending launch switches:\n' + restartSwitches.join('\n'));
              } else if (restartRequired) {
                alertFn('Settings saved. Restart required for some options.');
              } else if (!quiet) {
                alertFn('Settings saved.');
              }
              try {
                const {settings} = await dashboardHandler.getEffectiveSettings('');
                try {
                  updateAutoHealSettings(settings || {});
                } catch (_) {}
                if (!quiet) {
                  renderSettingsTab(view, settings || {}, saveSettings);
                }
              } catch (_) {
                console.error('Failed to refresh settings after save');
              }
            } else {
              console.warn('[Settings] save returned non-ok response:', res);
              alertFn('Failed to save settings.');
            }
          } catch (e) {
            console.error('setGlobalSettings failed', e);
            alertFn('Error while saving settings.');
          }
        };
        renderSettingsTab(view, settings || {}, saveSettings);
      } catch (e) {
        console.error('getEffectiveSettings failed', e);
        view.innerHTML = (
          '<div class="p-4"><h1 class="text-2xl mb-2">Settings</h1>' +
          '<p>Failed to load settings.</p></div>'
        );
      }
      return;
    }

    const view = mainViews[appState.currentView];
    if (view) {
      view.innerHTML = (
        '<div class="p-4"><h1 class="text-2xl">' +
        appState.currentView.charAt(0).toUpperCase() +
        appState.currentView.slice(1) +
        ' View Content</h1><p>UI for this section goes here.</p></div>'
      );
    }
  }

  return {
    renderAll,
    scheduleRender,
    switchView,
  };
}

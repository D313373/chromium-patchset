// Lightweight bridge for Dashboard WebUI -> C++ using chrome.send()
// Provides a Promise-based API similar to Mojo Remote used by dashboard_app.js

let seq = 0;
function sendWithPromise(message, ...args) {
  const timeoutMs = 15000;
  return new Promise((resolve, reject) => {
    const cb = `__dash_cb_${Date.now()}_${++seq}`;
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      try { delete window[cb]; } catch (_) {}
      reject(new Error(`chrome.send timeout: ${message}`));
    }, timeoutMs);
    try {
      // Install a one-shot callback on window
      window[cb] = (result) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        try {
          // Some WebUI call sites deliver a single-arg payload wrapped in an array
          if (Array.isArray(result) && result.length === 1) {
            result = result[0];
          }
        } catch (_) {}
        try { delete window[cb]; } catch (_) {}
        resolve(result || null);
      };
      if (window.chrome && typeof window.chrome.send === 'function') {
        window.chrome.send(message, [cb, ...args]);
      } else {
        // Fallback when chrome.send is unavailable
        settled = true;
        clearTimeout(timer);
        delete window[cb];
        reject(new Error('chrome.send is not available'));
      }
    } catch (e) {
      settled = true;
      clearTimeout(timer);
      try { delete window[cb]; } catch (_) {}
      reject(e);
    }
  });
}

// Helper to map scripts into the UI-friendly shape expected by dashboard_app.js.
function normalizeScriptsDict(raw) {
  // raw is expected to be an object mapping id -> script object (canonical schema).
  const out = {};
  if (!raw || typeof raw !== 'object') return out;
  for (const [id, script] of Object.entries(raw)) {
    if (!script || typeof script !== 'object')
      continue;
    out[id] = {
      name: script?.name || `Script ${id}`,
      actions: Array.isArray(script?.actions) ? script.actions : [],
      settings: script?.settings || {},
    };
  }
  return out;
}

export class DashboardHandler {
  static getRemote() {
    if (!this._remote) {
      this._remote = {
        // Ensure storage is set up and a default script exists
        initialize: async () => {
          await sendWithPromise('dashboardInitialize');
          return { ok: true };
        },
        // Returns { scripts: { id: { ...raw script... }, ... } } (no normalization here)
        getScripts: async () => {
          let raw = await sendWithPromise('dashboardGetScripts');
          try {
            if (Array.isArray(raw) && raw.length === 1) raw = raw[0];
          } catch (_) {}
          const scripts = raw && raw.scripts;
          return { scripts };
        },
        // Expects payload { name }
        createNewScript: async (payload) => {
          let res = await sendWithPromise('dashboardCreateNewScript', payload || {});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return { scriptId: res && res.scriptId };
        },
        // Expects id, and data map with stringified actions/settings per dashboard_app.js
        saveScript: async (id, data) => {
          await sendWithPromise('dashboardSaveScript', id, data || {});
          return { ok: true };
        },
        deleteScript: async (id) => {
          await sendWithPromise('dashboardDeleteScript', id);
          return { ok: true };
        },
        // Start playback in the browser. Options may include waitForCompletion.
        playScript: async (id, options = {}) => {
          let res = await sendWithPromise('dashboardPlayScript', id || '', options || {});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            ok: !!(res && res.ok),
            success: res && Object.prototype.hasOwnProperty.call(res, 'success')
              ? !!res.success
              : undefined,
            completed: !!(res && res.completed),
            waited: !!(res && res.completed),
            runId: (res && (res.runId || res.run_id)) || '',
            failed_step: Number.isFinite(res?.failed_step) ? Number(res.failed_step) : -1,
            message: (res && (res.message || res.error)) || '',
            error: (res && res.error) || '',
          };
        },
        // Stop every in-flight Play / onMatch / trigger run and drop queued triggers.
        stopAllScripts: async () => {
          let res = await sendWithPromise('dashboardStopAllScripts');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            ok: !!(res && res.ok),
            stopped: Number.isFinite(res?.stopped) ? Number(res.stopped) : 0,
          };
        },
        // Debug logging controls
        setDebugLogging: async (enabled) => {
          const res = await sendWithPromise('dashboardSetDebugLogging', !!enabled);
          return { ok: !!(res && res.ok), enabled: !!(res && res.enabled) };
        },
        getDebugLogging: async () => {
          const res = await sendWithPromise('dashboardGetDebugLogging');
          return { enabled: !!(res && res.enabled) };
        },
        // Run history / debugging
        getRunHistory: async (scriptIdFilter, fromIso, toIso, limit) => {
          let res = await sendWithPromise(
              'dashboardGetRunHistory',
              scriptIdFilter || '',
              fromIso || '',
              toIso || '',
              (typeof limit === 'number') ? limit : 2000);
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          const runs = (res && res.runs) ? res.runs : [];
          return { runs };
        },
        getRunDetails: async (runId) => {
          let res = await sendWithPromise('dashboardGetRunDetails', runId || '');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          const run = (res && res.run) ? res.run : (res || null);
          return { run };
        },
        getListenerEvents: async (fromIso, toIso, limit) => {
          let res = await sendWithPromise(
              'dashboardGetListenerEvents',
              fromIso || '',
              toIso || '',
              (typeof limit === 'number') ? limit : 2000);
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          const events = (res && res.events) ? res.events : [];
          return { events };
        },
        // Optional APIs used by the Debugging tab (may be implemented later).
        getScheduledScripts: async () => {
          return { scripts: [] };
        },
        getActiveListeners: async () => {
          let res = await sendWithPromise('dashboardGetActiveListeners');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          const listeners = (res && res.listeners) ? res.listeners : [];
          const headlessTabs =
            (res && (res.headless_tabs || res.headlessTabs)) ?
              (res.headless_tabs || res.headlessTabs) :
              [];
          const headlessWarning = String(
            (res && (res.headless_warning || res.headlessWarning)) || ''
          ).trim();
          return {
            listeners,
            headless_tabs: headlessTabs,
            headlessTabs,
            headless_warning: headlessWarning,
            headlessWarning,
          };
        },
        startListener: async (id) => {
          let res = await sendWithPromise('dashboardStartListener', id || '');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            success: !!(res && res.success),
            error: (res && res.error) || '',
            warning: (res && res.warning) || '',
            mode: (res && res.mode) || ''
          };
        },
        stopListener: async (id) => {
          let res = await sendWithPromise('dashboardStopListener', id || '');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            success: !!(res && res.success),
            error: (res && res.error) || '',
            mode: (res && res.mode) || ''
          };
        },
        stopHeadlessTask: async (taskId) => {
          let res = await sendWithPromise('dashboardStopHeadlessTask', taskId || '');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            success: !!(res && res.success),
            error: (res && res.error) || '',
            task_id: (res && (res.task_id || res.taskId)) || '',
          };
        },
        refreshHeadlessTab: async (ownerKind, ownerId) => {
          let res = await sendWithPromise(
              'dashboardRefreshHeadlessTab',
              ownerKind || '',
              ownerId || '');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            success: !!(res && res.success),
            error: (res && res.error) || '',
            owner_id: (res && (res.owner_id || res.ownerId)) || '',
          };
        },
        requestHeadlessScreenshot: async (scriptId) => {
          let res = await sendWithPromise(
              'dashboardRequestHeadlessScreenshot',
              scriptId || '');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            success: !!(res && res.success),
            error: (res && res.error) || '',
            script_id: (res && (res.script_id || res.scriptId)) || '',
            ts_ms: (res && (res.ts_ms || res.tsMs)) || 0,
            path: (res && res.path) || '',
            image_data_url: (res && (res.image_data_url || res.imageDataUrl)) || '',
            image_base64: (res && (res.image_base64 || res.imageBase64)) || '',
            current_url: (res && (res.current_url || res.currentUrl)) || '',
          };
        },
        resumeScriptFromStep: async (scriptId, stepIndex, runId = '') => {
          let res = await sendWithPromise(
              'dashboardResumeScriptFromStep',
              scriptId || '',
              Number.isFinite(stepIndex) ? Number(stepIndex) : -1,
              runId || '');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            success: !!(res && res.success),
            error: (res && res.error) || '',
            runId: (res && (res.runId || res.run_id)) || '',
          };
        },
        agentOllamaChat: async (payload = {}) => {
          let res = await sendWithPromise('dashboardAgentOllamaChat', payload || {});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            ok: !!(res && res.ok),
            http_status: Number(res?.http_status || res?.httpStatus || 0),
            body: String((res && res.body) || ''),
            error: String((res && res.error) || ''),
          };
        },
        // Settings API
        getEffectiveSettings: async (scriptId) => {
          let res = await sendWithPromise('dashboardGetEffectiveSettings', scriptId || '');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          const settings = (res && res.settings) || {};
          return { settings };
        },
        setGlobalSettings: async (settings) => {
          let res = await sendWithPromise('dashboardSetGlobalSettings', settings || {});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          const okField = res && (res.ok ?? res.success ?? res.result);
          const ok = !!(
              res === true ||
              okField === true ||
              okField === 'true' ||
              okField === 1);
          const restartRequired = !!(res && res.restartRequired === true);
          const restartSwitches =
            Array.isArray(res?.restartSwitches) ? res.restartSwitches : [];
          return { ok, restartRequired, restartSwitches, raw: res };
        },
        // Data tab: variables + secrets (stored locally).
        getDataVariables: async () => {
          let res = await sendWithPromise('dashboardGetDataVariables');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          const variables = (res && res.variables) ? res.variables : [];
          return { variables };
        },
        saveDataVariables: async (dict) => {
          let res = await sendWithPromise('dashboardSaveDataVariables', dict || {});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return { ok: !!(res && res.ok) };
        },
        getDataSecrets: async () => {
          let res = await sendWithPromise('dashboardGetDataSecrets');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          const secrets = (res && res.secrets) ? res.secrets : [];
          return { secrets };
        },
        saveDataSecrets: async (payload) => {
          let res = await sendWithPromise('dashboardSaveDataSecrets', payload || {});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return { ok: !!(res && res.ok) };
        },
        getScrapeParentDir: async () => {
          let res = await sendWithPromise('dashboardGetScrapeParentDir');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            path: String(res?.path || ''),
            defaultPath: String(res?.defaultPath || ''),
          };
        },
        setScrapeParentDir: async (path) => {
          let res = await sendWithPromise(
              'dashboardSetScrapeParentDir', {path: String(path || '')});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return { ok: !!(res && res.ok), path: String(res?.path || '') };
        },
        listScrapes: async () => {
          let res = await sendWithPromise('dashboardListScrapes');
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            files: Array.isArray(res?.files) ? res.files : [],
            parentDir: String(res?.parentDir || ''),
          };
        },
        readScrapeCsv: async (relativePath) => {
          let res = await sendWithPromise(
              'dashboardReadScrapeCsv', {relativePath: String(relativePath || '')});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            ok: !!(res && res.ok),
            text: String(res?.text || ''),
            error: String(res?.error || ''),
            relativePath: String(res?.relativePath || relativePath || ''),
          };
        },
        deleteScrapeCsv: async (relativePath) => {
          let res = await sendWithPromise(
              'dashboardDeleteScrapeCsv', {relativePath: String(relativePath || '')});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            ok: !!(res && res.ok),
            error: String(res?.error || ''),
            relativePath: String(res?.relativePath || relativePath || ''),
          };
        },
        getTrafficEntries: async (limit = 100) => {
          let res = await sendWithPromise(
              'dashboardGetTrafficEntries', {limit: Number(limit) || 100});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            ok: !!(res && res.ok !== false),
            entries: Array.isArray(res?.entries) ? res.entries : [],
          };
        },
        getTrafficEntry: async (requestId) => {
          let res = await sendWithPromise(
              'dashboardGetTrafficEntry', {requestId: String(requestId || '')});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            ok: !!(res && res.ok),
            entry: res?.entry || null,
            error: String(res?.error || ''),
          };
        },
        promoteTrafficEntry: async (requestId) => {
          let res = await sendWithPromise(
              'dashboardPromoteTrafficEntry',
              {requestId: String(requestId || '')});
          try { if (Array.isArray(res) && res.length === 1) res = res[0]; } catch (_) {}
          return {
            ok: !!(res && res.ok),
            action: res?.action || null,
            error: String(res?.error || ''),
          };
        },
      };
    }
    return this._remote;
  }
}

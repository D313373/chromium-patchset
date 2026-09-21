export async function initializeDashboardShell({
  dashboardHandler,
  applyTheme,
} = {}) {
  if (!dashboardHandler) {
    return {ok: false, error: 'missing_handler'};
  }

  try {
    await dashboardHandler.initialize();
  } catch (e) {
    console.error(
        'initialize() failed; continuing UI setup so buttons still bind:', e);
  }

  try {
    const res = await dashboardHandler.getEffectiveSettings('');
    const settings = (res && res.settings) ? res.settings : {};
    const configuredModelRaw =
      String(settings?.agent?.model || '').trim() || 'qwen3-coder-next';
    const configuredProvider =
      String(settings?.agent?.provider || '').trim().toLowerCase() || 'ollama';
    const anthropicAliases = {
      'claude-haiku-4-5-20251001': 'claude-haiku-4-5',
      'claude-3-5-haiku-latest': 'claude-haiku-4-5',
      'claude-3-5-sonnet-latest': 'claude-sonnet-4-5',
      'claude-sonnet-4-20250514': 'claude-sonnet-4-5',
    };
    const configuredModel = (configuredProvider === 'anthropic' &&
                             anthropicAliases[configuredModelRaw]) ?
      anthropicAliases[configuredModelRaw] :
      configuredModelRaw;
    const configuredEndpoint =
      String(settings?.agent?.ollamaEndpoint || '').trim() ||
      'http://127.0.0.1:11434';
    const configuredLoopProfile =
      String(settings?.agent?.loopProfile || '').trim() || 'glic_task';
    const configuredKeepWarmHeadless =
      settings?.agent?.keepWarmHeadlessInvestigationTab !== false;
    window.__DASH_MODEL = configuredModel;
    window.__DASH_PROVIDER = configuredProvider;
    window.__DASH_AGENT_PROVIDER = configuredProvider;
    window.__DASH_OLLAMA_ENDPOINT = configuredEndpoint;
    window.__DASH_LOOP_PROFILE = configuredLoopProfile;
    window.__DASH_KEEP_WARM_HEADLESS = configuredKeepWarmHeadless;
    try {
      localStorage.setItem('glic.loop_profile', configuredLoopProfile);
      localStorage.setItem('glic.provider', configuredProvider);
      localStorage.setItem('glic.model', configuredModel);
      localStorage.setItem(
          'glic.keep_warm_headless_investigation_tab',
          configuredKeepWarmHeadless ? '1' : '0');
    } catch (_) {}
    const theme =
      (String(settings.theme || '').toLowerCase() === 'light') ? 'light' : 'dark';
    if (typeof applyTheme === 'function') {
      applyTheme(theme);
    }
  } catch (e) {
    console.warn('getEffectiveSettings() for theme failed; defaulting to Matrix Dark', e);
    window.__DASH_MODEL = window.__DASH_MODEL || 'qwen3-coder-next';
    window.__DASH_PROVIDER = window.__DASH_PROVIDER || 'ollama';
    window.__DASH_OLLAMA_ENDPOINT =
      window.__DASH_OLLAMA_ENDPOINT || 'http://127.0.0.1:11434';
    window.__DASH_LOOP_PROFILE = window.__DASH_LOOP_PROFILE || 'glic_task';
    window.__DASH_KEEP_WARM_HEADLESS =
      window.__DASH_KEEP_WARM_HEADLESS !== false;
    if (typeof applyTheme === 'function') {
      applyTheme('dark');
    }
  }

  return {ok: true};
}

export async function loadScriptsIntoState({
  dashboardHandler,
  normalizeScriptsDict,
  appState,
} = {}) {
  if (!dashboardHandler || !appState || typeof normalizeScriptsDict !== 'function') {
    return {ok: false, error: 'invalid_bootstrap_dependencies'};
  }

  try {
    const response = await dashboardHandler.getScripts();

    let scriptsRaw = response && response.scripts;
    if (scriptsRaw &&
        typeof scriptsRaw === 'object' &&
        typeof scriptsRaw.forEach === 'function' &&
        typeof scriptsRaw.size === 'number') {
      const obj = {};
      scriptsRaw.forEach((v, k) => {
        obj[k] = v;
      });
      scriptsRaw = obj;
    }
    appState.scripts = normalizeScriptsDict(scriptsRaw || {});
  } catch (error) {
    console.error('Failed to get scripts:', error);
    appState.scripts = {};
  }

  appState.currentScriptId = Object.keys(appState.scripts || {})[0] || null;
  return {ok: true, currentScriptId: appState.currentScriptId};
}

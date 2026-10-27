// dashboard/settings.js
// Render Settings tab. Anti-detection is limited to browser-level user-agent
// and Accept-Language overrides. Page-script spoofs are not offered.
// onSave: function(newSettingsDict)

const DASHBOARD_AGENT_PROVIDERS = [
  'anthropic',
  'openai',
  'grok',
  'gemini',
  'cerebras',
  'local',
];
const DASHBOARD_AGENT_LOOP_PROFILES = [
  'glic_task',
  'manager',
  'dashboard_assistant',
  'investigator',
];

const DEFAULT_MODELS_BY_PROVIDER = {
  local: 'local-model',
  cerebras: 'qwen-3-235b-a22b-instruct-2507',
  gemini: 'gemini-2.5-pro',
  openai: 'gpt-4.1',
  grok: 'grok-4',
  anthropic: 'claude-haiku-4-5',
};

const DEFAULT_ENDPOINTS_BY_PROVIDER = {
  local: 'http://127.0.0.1:11434/v1/chat/completions',
  cerebras: 'https://api.cerebras.ai/v1/chat/completions',
  gemini: 'https://generativelanguage.googleapis.com/v1beta',
  openai: 'https://api.openai.com/v1/chat/completions',
  grok: 'https://api.x.ai/v1/chat/completions',
  anthropic: 'https://api.anthropic.com/v1/messages',
};


const DEFAULT_LOCAL_MODEL = DEFAULT_MODELS_BY_PROVIDER.local;
const DEFAULT_LOCAL_ENDPOINT = DEFAULT_ENDPOINTS_BY_PROVIDER.local;
// Back-compat aliases for older settings / scripts.
const DEFAULT_OLLAMA_MODEL = DEFAULT_LOCAL_MODEL;
const DEFAULT_OLLAMA_ENDPOINT = DEFAULT_LOCAL_ENDPOINT;

// Presets only. Local model ids are free text and must keep ':' (Ollama
// tags are `name:tag`, e.g. qwen2.5vl:7b). Do not attach these to a
// <datalist>: Chrome commits a datalist value at the first colon, so
// `qwen2.5-vl:7b` is saved as `qwen2.5-vl7b`.
const LOCAL_MODEL_OPTIONS = [
  {value: 'qwen2.5vl:7b', label: 'qwen2.5vl:7b'},
  {value: 'qwen2.5vl:3b', label: 'qwen2.5vl:3b'},
  {value: 'qwen3-vl:2b', label: 'qwen3-vl:2b'},
  {value: 'qwen3.5:9b', label: 'qwen3.5:9b'},
  {value: 'ahmadwaqar/smolvlm2-agentic-gui:latest',
   label: 'ahmadwaqar/smolvlm2-agentic-gui:latest'},
  {value: 'llama3.2', label: 'llama3.2'},
  {value: 'qwen2.5-coder', label: 'qwen2.5-coder'},
  {value: 'mistral', label: 'mistral'},
];
const LOCAL_ENDPOINT_PRESETS = [
  {label: 'Ollama (/v1)', value: 'http://127.0.0.1:11434/v1/chat/completions'},
  {label: 'LM Studio', value: 'http://127.0.0.1:1234/v1/chat/completions'},
  {label: 'llama.cpp server', value: 'http://127.0.0.1:8080/v1/chat/completions'},
];

const CEREBRAS_MODEL_OPTIONS = [
  {value: 'qwen-3-235b-a22b-instruct-2507'},
  {value: 'llama3.1-8b'},
];

const GEMINI_MODEL_OPTIONS = [
  {value: 'gemini-flash-latest'},
  {value: 'gemini-2.5-pro'},
  {value: 'gemini-2.5-flash'},
];

const OPENAI_MODEL_OPTIONS = [
  {value: 'gpt-4.1'},
  {value: 'gpt-4.1-mini'},
  {value: 'o4-mini'},
];

const GROK_MODEL_OPTIONS = [
  {value: 'grok-4'},
  {value: 'grok-3'},
];

const ANTHROPIC_MODEL_OPTIONS = [
  {value: 'claude-haiku-4-5', label: 'claude-haiku-4-5 (cheap, default)'},
  {value: 'claude-sonnet-4-5', label: 'claude-sonnet-4-5 (higher quality)'},
  {value: 'claude-sonnet-5', label: 'claude-sonnet-5 (higher quality)'},
];

// Older saves / GLIC picks that Anthropic rejects or that we retired from the UI.
const ANTHROPIC_MODEL_ALIASES = {
  'claude-haiku-4-5-20251001': 'claude-haiku-4-5',
  'claude-3-5-haiku-latest': 'claude-haiku-4-5',
  'claude-3-5-sonnet-latest': 'claude-sonnet-4-5',
  'claude-sonnet-4-20250514': 'claude-sonnet-4-5',
};

const PROVIDER_MODEL_OPTIONS = {
  local: LOCAL_MODEL_OPTIONS,
  cerebras: CEREBRAS_MODEL_OPTIONS,
  gemini: GEMINI_MODEL_OPTIONS,
  openai: OPENAI_MODEL_OPTIONS,
  grok: GROK_MODEL_OPTIONS,
  anthropic: ANTHROPIC_MODEL_OPTIONS,
};

function normalizeDashboardProvider(rawProvider) {
  let provider = String(rawProvider || '').trim().toLowerCase();
  // Migrate legacy Ollama selection to generic Local (OpenAI-compatible).
  if (provider === 'ollama') {
    provider = 'local';
  }
  return DASHBOARD_AGENT_PROVIDERS.includes(provider) ? provider : 'anthropic';
}

function normalizeDashboardLoopProfile(rawProfile) {
  const profile = String(rawProfile || '').trim().toLowerCase();
  if (!profile) return 'glic_task';
  if (profile === 'task' || profile === 'task_completer' ||
      profile === 'task-completer' || profile === 'glic' ||
      profile === 'glic-task') {
    return 'glic_task';
  }
  if (profile === 'assistant' || profile === 'dashboard' ||
      profile === 'dashboard-assistant') {
    return 'dashboard_assistant';
  }
  if (profile === 'investigate' || profile === 'error_investigator' ||
      profile === 'error-investigator') {
    return 'investigator';
  }
  if (profile === 'manager' || profile === 'manager_vlm' ||
      profile === 'embedded_manager') {
    return 'manager';
  }
  return DASHBOARD_AGENT_LOOP_PROFILES.includes(profile) ?
    profile :
    'glic_task';
}

function defaultModelForProvider(provider) {
  const normalized = normalizeDashboardProvider(provider);
  return DEFAULT_MODELS_BY_PROVIDER[normalized] || DEFAULT_OLLAMA_MODEL;
}

function defaultEndpointForProvider(provider) {
  const normalized = normalizeDashboardProvider(provider);
  return DEFAULT_ENDPOINTS_BY_PROVIDER[normalized] ||
    DEFAULT_ENDPOINTS_BY_PROVIDER.ollama;
}

function normalizeHostedEndpoint(value, fallback) {
  let endpoint = String(value || '').trim();
  const fallbackValue = String(fallback || '').trim() || 'https://';
  if (!endpoint) endpoint = fallbackValue;
  while (endpoint.endsWith('/')) endpoint = endpoint.slice(0, -1);
  if (!endpoint.startsWith('http://') && !endpoint.startsWith('https://')) {
    endpoint = `https://${endpoint}`;
  }
  return endpoint || fallbackValue;
}

function providerUsesLocalEndpoint(provider) {
  return normalizeDashboardProvider(provider) === 'local';
}


// Legacy name used below / in older call sites.
function providerUsesOllamaEndpoint(provider) {
  return providerUsesLocalEndpoint(provider);
}

function normalizeActionExecutionPath(rawPath, fallback = 'cdp') {
  const path = String(rawPath || '').trim().toLowerCase();
  if (path === 'native' || path === 'cdp') {
    return path;
  }
  return fallback;
}

function normalizeActionExecutionFallbackPath(rawPath, primaryPath) {
  const path = String(rawPath || '').trim().toLowerCase();
  if (!path || path === 'none' || path === 'disabled') {
    return '';
  }
  if (path !== 'native' && path !== 'cdp') {
    return '';
  }
  return path === primaryPath ? '' : path;
}

function getModelOptionsForProvider(provider) {
  const normalized = normalizeDashboardProvider(provider);
  return PROVIDER_MODEL_OPTIONS[normalized] || LOCAL_MODEL_OPTIONS;
}

function isModelSupportedByProvider(provider, model) {
  const candidate = String(model || '').trim();
  if (!candidate) {
    return false;
  }
  return getModelOptionsForProvider(provider).some((option) =>
    String(option?.value || '').trim() === candidate);
}

function sanitizeModelForProvider(provider, model) {
  const normalizedProvider = normalizeDashboardProvider(provider);
  let candidate = String(model || '').trim();
  if (normalizedProvider === 'anthropic' &&
      ANTHROPIC_MODEL_ALIASES[candidate]) {
    candidate = ANTHROPIC_MODEL_ALIASES[candidate];
  }
  // Local servers use arbitrary model ids. Return the string unchanged,
  // including ':'. Ollama ids are `name:tag`.
  if (normalizedProvider === 'local') {
    return candidate || defaultModelForProvider(normalizedProvider);
  }
  if (isModelSupportedByProvider(normalizedProvider, candidate)) {
    return candidate;
  }
  return defaultModelForProvider(normalizedProvider);
}

function buildModelSelectOptions(provider, selectedModel, selectedEndpoint) {
  const normalizedProvider = normalizeDashboardProvider(provider);
  const usesOllamaEndpoint = providerUsesOllamaEndpoint(normalizedProvider);
  const options = [...getModelOptionsForProvider(normalizedProvider)];
  const normalizedSelected = sanitizeModelForProvider(
      normalizedProvider, selectedModel);
  return options.map(option => {
    const label = option.label ||
      (usesOllamaEndpoint
        ? option.endpoint === 'http://127.0.0.1:11435'
          ? `${option.value} via 11435`
          : option.value
        : option.value);
    const selected =
      option.value === normalizedSelected &&
      (!usesOllamaEndpoint || !selectedEndpoint || option.endpoint === selectedEndpoint);
    const endpointAttr =
      !usesOllamaEndpoint || !option.endpoint
        ? ''
        : ` data-endpoint="${escapeHtml(option.endpoint)}"`;
    return `<option value="${escapeHtml(option.value)}"${endpointAttr}${selected ? ' selected' : ''}>${escapeHtml(label)}</option>`;
  }).join('');
}


function setEndpointRowActive(el, active) {
  if (!el) return;
  const row = el.closest('.setting-item') || el.parentElement;
  if (row) {
    row.style.opacity = active ? '' : '0.45';
    row.style.pointerEvents = active ? '' : 'none';
  }
  el.disabled = !active;
}

function fillLocalModelPreset(selectEl) {
  if (!selectEl) {
    return;
  }
  while (selectEl.options.length > 1) {
    selectEl.remove(1);
  }
  for (const option of LOCAL_MODEL_OPTIONS) {
    const el = document.createElement('option');
    // Assign in script. An HTML value attribute is what the datalist
    // parser splits on ':'.
    el.value = String(option.value || '');
    el.textContent = option.label || el.value;
    selectEl.appendChild(el);
  }
  selectEl.value = '';
}

function syncDashboardModelOptions(providerSelect, modelSelect, endpointInput,
    hostedEndpointInput = null, hostedEndpointState = null,
    hostedEndpointLabel = null, hostedEndpointHelp = null) {
  if (!providerSelect || !modelSelect) {
    return;
  }
  const provider = normalizeDashboardProvider(providerSelect.value);
  const typedLocalInput =
    (typeof document !== 'undefined')
      ? document.getElementById('dashboardModelInput')
      : null;
  const typedLocal = typedLocalInput
    ? String(typedLocalInput.value || '').trim()
    : '';
  // The visible local field is the text input. The hidden <select> only
  // has presets, so reading it would replace a typed `name:tag` id.
  const currentModel = (providerUsesLocalEndpoint(provider) && typedLocal)
    ? typedLocal
    : modelSelect.value.trim();
  const currentEndpoint = endpointInput ? endpointInput.value.trim() : '';
  const fallbackModel = sanitizeModelForProvider(provider, currentModel);
  modelSelect.innerHTML = buildModelSelectOptions(
      provider,
      fallbackModel,
      currentEndpoint);
  modelSelect.value = sanitizeModelForProvider(provider, modelSelect.value);
  if (providerUsesLocalEndpoint(provider)) {
    modelSelect.title = 'Local OpenAI-compatible model id (free text).';
  } else {
    modelSelect.title = 'Hosted provider model list. Use Data > Secrets to set the matching API key.';
  }
  const modelInput =
    (typeof document !== 'undefined')
      ? document.getElementById('dashboardModelInput')
      : null;
  const localPreset =
    (typeof document !== 'undefined')
      ? document.getElementById('dashboardLocalModelPreset')
      : null;
  const usesLocal = providerUsesLocalEndpoint(provider);
  if (modelInput) {
    modelInput.style.display = usesLocal ? '' : 'none';
    modelSelect.style.display = usesLocal ? 'none' : '';
    // Never replace a typed id. The hidden <select> does not hold it, and
    // filling from that select drops ids that contain ':'.
    if (usesLocal && !String(modelInput.value || '').trim()) {
      modelInput.value = fallbackModel;
    }
  }
  if (localPreset) {
    localPreset.style.display = usesLocal ? '' : 'none';
  }
  if (endpointInput) {
    const usesLocal = providerUsesLocalEndpoint(provider);
    setEndpointRowActive(endpointInput, usesLocal);
    if (!usesLocal) {
      endpointInput.title = 'Only used when Provider is Local (OpenAI-compatible).';
    } else {
      endpointInput.title = 'OpenAI-compatible chat completions URL.';
      const selectedOption =
        modelSelect.options[modelSelect.selectedIndex] || null;
      const selectedEndpoint = selectedOption?.dataset?.endpoint || '';
      if (selectedEndpoint) {
        endpointInput.value = selectedEndpoint;
      }
    }
  }
  if (hostedEndpointInput) {
    const usesLocal = providerUsesLocalEndpoint(provider);
    setEndpointRowActive(hostedEndpointInput, !usesLocal);
    if (usesLocal) {
      hostedEndpointInput.title = 'Inactive while Local provider is selected.';
    } else {
      hostedEndpointInput.title = '';
      const fallback = defaultEndpointForProvider(provider);
      const stored = hostedEndpointState &&
              typeof hostedEndpointState === 'object'
        ? hostedEndpointState[provider]
        : '';
      hostedEndpointInput.value = normalizeHostedEndpoint(
          stored || hostedEndpointInput.value,
          fallback);
    }
    if (hostedEndpointLabel) {
      hostedEndpointLabel.textContent = usesLocal
        ? 'Hosted Provider Endpoint (inactive for Local):'
        : `${provider.charAt(0).toUpperCase()}${provider.slice(1)} Endpoint:`;
    }
    if (hostedEndpointHelp) {
      hostedEndpointHelp.innerHTML = usesLocal
        ? 'Used when provider is one of Cerebras, Gemini, OpenAI, or Anthropic.'
        : `Default for ${provider}: <code>${escapeHtml(defaultEndpointForProvider(provider))}</code>. Use Data > Secrets for the API key.`;
    }
  }
}

function apiKeyNameForDashboardProvider(provider) {
  const normalized = normalizeDashboardProvider(provider);
  if (normalized === 'anthropic') return 'ANTHROPIC_API_KEY';
  if (normalized === 'openai') return 'OPENAI_API_KEY';
  if (normalized === 'grok') return 'XAI_API_KEY';
  if (normalized === 'gemini') return 'GEMINI_API_KEY';
  if (normalized === 'cerebras') return 'CEREBRAS_API_KEY';
  if (normalized === 'local') return 'LOCAL_API_KEY';
  return '';
}

function readAgentConnectionForm(container) {
  const q = (sel) => container.querySelector(sel);
  const provider = normalizeDashboardProvider(
      /** @type {HTMLSelectElement} */(q('#dashboardProviderSelect'))?.value ||
      'anthropic');
  const modelRaw = String(
      (provider === 'local'
        ? (/** @type {HTMLInputElement} */(q('#dashboardModelInput')))?.value
        : (/** @type {HTMLSelectElement} */(q('#dashboardModelSelect')))?.value) ||
      '').trim();
  const model = sanitizeModelForProvider(provider, modelRaw) ||
      defaultModelForProvider(provider);
  let endpoint = '';
  if (providerUsesLocalEndpoint(provider)) {
    endpoint = String(
        (/** @type {HTMLInputElement} */(q('#dashboardOllamaEndpointInput')))
            ?.value ||
        '').trim() ||
        defaultEndpointForProvider(provider);
  } else {
    endpoint = normalizeHostedEndpoint(
        (/** @type {HTMLInputElement} */(q('#dashboardHostedEndpointInput')))
            ?.value ||
            defaultEndpointForProvider(provider),
        defaultEndpointForProvider(provider));
  }
  return {
    provider,
    model,
    endpoint,
    apiKeyName: apiKeyNameForDashboardProvider(provider),
  };
}

function summarizeAgentChatBody(bodyText, provider) {
  const raw = String(bodyText || '').trim();
  if (!raw) return '';
  try {
    const parsed = JSON.parse(raw);
    const p = String(provider || '').toLowerCase();
    if (p === 'anthropic') {
      const parts = parsed?.content;
      if (Array.isArray(parts)) {
        const text = parts.map((x) => x?.text || '').filter(Boolean).join(' ');
        if (text) return text.slice(0, 120);
      }
    }
    const choice = parsed?.choices?.[0];
    const msg = choice?.message?.content || choice?.text ||
        parsed?.message?.content || parsed?.response ||
        parsed?.candidates?.[0]?.content?.parts?.[0]?.text;
    if (typeof msg === 'string' && msg.trim()) return msg.trim().slice(0, 120);
  } catch (_) {}
  return raw.slice(0, 120);
}

async function testDashboardAgentConnection(container) {
  const statusEl = container.querySelector('#testAgentConnectionStatus');
  const btn = container.querySelector('#testAgentConnectionBtn');
  const setStatus = (msg, ok) => {
    if (!statusEl) return;
    statusEl.textContent = msg;
    statusEl.style.color = ok === true ?
      'var(--success-text, #3dd68c)' :
      (ok === false ? 'var(--danger-text, #ff6b6b)' : 'var(--secondary-text)');
  };
  const cfg = readAgentConnectionForm(container);
  if (!cfg.model) {
    setStatus('Set an Agent Model first.', false);
    return;
  }
  if (!cfg.endpoint) {
    setStatus('Set an Endpoint first.', false);
    return;
  }
  const handler = (typeof window !== 'undefined') ? window.dashboardHandler : null;
  if (!handler || typeof handler.agentOllamaChat !== 'function') {
    setStatus('Dashboard backend unavailable (agentOllamaChat).', false);
    return;
  }
  if (btn) btn.disabled = true;
  setStatus(
      `Testing ${cfg.provider} · ${cfg.model} @ ${cfg.endpoint} …`, null);
  const started = Date.now();
  try {
    const res = await handler.agentOllamaChat({
      provider: cfg.provider,
      endpoint: cfg.endpoint,
      model: cfg.model,
      api_key_name: cfg.apiKeyName,
      system_prompt:
          'Reply with exactly: pong. No other words.',
      messages: [{role: 'user', content: 'ping'}],
      max_tokens: 32,
    });
    const ms = Date.now() - started;
    const http = Number(res?.http_status || 0);
    if (!res?.ok) {
      const err = String(res?.error || res?.body || 'request_failed').slice(0, 200);
      setStatus(`Failed (${http || 'no-http'}): ${err}`, false);
      return;
    }
    const snippet = summarizeAgentChatBody(res?.body, cfg.provider);
    setStatus(
        `OK ${http || 200} in ${ms}ms — ${cfg.provider}/${cfg.model}` +
            (snippet ? ` · “${snippet}”` : ''),
        true);
  } catch (e) {
    setStatus(`Failed: ${String(e && e.message ? e.message : e).slice(0, 200)}`,
              false);
  } finally {
    if (btn) btn.disabled = false;
  }
}


async function applyAgentFieldsFromSettingsForm(container, onSave, currentSettings) {
  if (typeof onSave !== 'function') return;
  const q = (sel) => container.querySelector(sel);
  const selectedProvider = normalizeDashboardProvider(
      /** @type {HTMLSelectElement} */(q('#dashboardProviderSelect'))?.value ||
      'anthropic');
  const selectedModelRaw = String(
      (selectedProvider === 'local'
        ? (/** @type {HTMLInputElement} */(q('#dashboardModelInput')))?.value
        : (/** @type {HTMLSelectElement} */(q('#dashboardModelSelect')))?.value) ||
      '').trim();
  const selectedModel = sanitizeModelForProvider(
      selectedProvider, selectedModelRaw) ||
      defaultModelForProvider(selectedProvider);
  const localEndpoint = String(
      (/** @type {HTMLInputElement} */(q('#dashboardOllamaEndpointInput')))
          ?.value ||
      '').trim() || DEFAULT_LOCAL_ENDPOINT;
  const hostedEndpointInput =
      /** @type {HTMLInputElement} */(q('#dashboardHostedEndpointInput'));
  const providerEndpoints =
      (currentSettings?.agent?.providerEndpoints &&
       typeof currentSettings.agent.providerEndpoints === 'object')
        ? {...currentSettings.agent.providerEndpoints}
        : {};
  if (!providerUsesLocalEndpoint(selectedProvider) && hostedEndpointInput) {
    providerEndpoints[selectedProvider] = normalizeHostedEndpoint(
        hostedEndpointInput.value,
        defaultEndpointForProvider(selectedProvider));
  }
  const base = (currentSettings && typeof currentSettings === 'object')
      ? JSON.parse(JSON.stringify(currentSettings))
      : {};
  base.agent = Object.assign({}, base.agent || {}, {
    provider: selectedProvider,
    model: selectedModel,
    ollamaEndpoint: localEndpoint,
    localEndpoint: localEndpoint,
    endpoint: providerUsesLocalEndpoint(selectedProvider)
      ? localEndpoint
      : (providerEndpoints[selectedProvider] ||
         defaultEndpointForProvider(selectedProvider)),
    providerEndpoints,
  });
  try {
    window.__DASH_PROVIDER = selectedProvider;
    window.__DASH_MODEL = selectedModel;
    window.__DASH_OLLAMA_ENDPOINT = localEndpoint;
    localStorage.setItem('glic.provider', selectedProvider);
    localStorage.setItem('glic.model', selectedModel);
    if (providerUsesLocalEndpoint(selectedProvider)) {
      localStorage.setItem('glic.ollama_endpoint', localEndpoint);
    }
  } catch (_) {}
  // Quiet save: view router wraps onSave with alerts — pass a flag via symbol
  base.__quietAgentSave = true;
  await onSave(base);
}

export function renderSettingsTab(container, currentSettings = {}, onSave = null) {
  const anti = currentSettings.antiDetection || {};
  const input = currentSettings.inputBehavior || {};
  const performance = currentSettings.performance || {};
  const subsystem = currentSettings.subsystem || {};
  const theme =
    String(currentSettings.theme || '').trim().toLowerCase() === 'light'
      ? 'light'
      : 'dark';
  const ua = anti.userAgent || '';
  const languages = Array.isArray(anti.languages) ? anti.languages.join(',') : '';
  const listenerMode = currentSettings.listenerMode || {};
  const runListenersInHeadlessMode =
    listenerMode.runListenersInHeadlessMode !== false;
  const listenerExecutionMode = runListenersInHeadlessMode ?
    'headless' :
    'background';
  const agent = currentSettings.agent || {};
  const jsContextInjectionFallback = agent.jsContextInjectionFallback === true;
  const dashboardProvider = normalizeDashboardProvider(agent.provider || 'ollama');
  const dashboardLoopProfile = normalizeDashboardLoopProfile(
      agent.loopProfile || 'glic_task');
  const dashboardModelRaw = String(
      agent.model ||
      defaultModelForProvider(dashboardProvider)).trim() ||
    defaultModelForProvider(dashboardProvider);
  const dashboardModel = sanitizeModelForProvider(
      dashboardProvider, dashboardModelRaw);
  const dashboardOllamaEndpoint =
    String(agent.localEndpoint || agent.ollamaEndpoint || DEFAULT_LOCAL_ENDPOINT)
      .trim() || DEFAULT_LOCAL_ENDPOINT;
  const keepWarmHeadlessInvestigationTab =
    agent.keepWarmHeadlessInvestigationTab !== false;
  const rawProviderEndpoints =
    (agent.providerEndpoints && typeof agent.providerEndpoints === 'object')
      ? agent.providerEndpoints
      : {};
  const providerEndpointState = {};
  for (const provider of DASHBOARD_AGENT_PROVIDERS) {
    if (providerUsesOllamaEndpoint(provider)) {
      continue;
    }
    const fallback = defaultEndpointForProvider(provider);
    const configured = String(rawProviderEndpoints[provider] || '').trim();
    const legacy =
      (provider === dashboardProvider)
        ? String(agent.endpoint || '').trim()
        : '';
    providerEndpointState[provider] = normalizeHostedEndpoint(
        configured || legacy || fallback,
        fallback);
  }
  const dashboardHostedEndpoint = providerUsesOllamaEndpoint(dashboardProvider)
    ? providerEndpointState.cerebras
    : (providerEndpointState[dashboardProvider] ||
       defaultEndpointForProvider(dashboardProvider));
  const autoHeal = currentSettings.autoHeal || {};
  const autoQueueHealFailures = autoHeal.autoQueueFailures === true;
  const autoHealInvestigateInHeadless =
    autoHeal.investigateInHeadless !== false;
  const recording = currentSettings.recording || {};
  const recordingStealthMode = recording.stealthMode !== false;
  const recordingAllowPageInjects = recording.allowPageInjects === true;
  const recordingEagerCdpAttach = recording.eagerCdpAttach === true;
  const aggressivePerformance = performance.aggressivePerformance === true;
  const deterministicMode = performance.deterministicMode === true;
  const failureReplayEnabled = performance.failureReplayEnabled === true;
  const pdfViewerEnabled = subsystem.pdfViewerEnabled !== false;
  const extensionsEnabled = subsystem.extensionsEnabled !== false;
  const printPreviewEnabled = subsystem.printPreviewEnabled !== false;
  const printStubEnabled = subsystem.printStubEnabled === true;
  const safeBrowsingEnabled = subsystem.safeBrowsingEnabled !== false;
  const remoteDebuggingEnabled = subsystem.remoteDebuggingEnabled === true;
  const serviceDiscoveryEnabled = subsystem.serviceDiscoveryEnabled !== false;
  const geolocationAllowedByDefault =
    subsystem.geolocationAllowedByDefault !== false;
  const randomDelayEnabled = input.randomDelayEnabled !== false;
  const clickPathJitterPx = Number.isFinite(input.clickPathJitterPx) ? input.clickPathJitterPx : 1.5;
  const clickHoverRadiusPx = Number.isFinite(input.clickHoverRadiusPx) ? input.clickHoverRadiusPx : 2.0;
  const clickHoverMoves = (typeof input.clickHoverMoves === 'number' && Number.isInteger(input.clickHoverMoves)) ? input.clickHoverMoves : (typeof input.clickHoverMoves === 'number' ? Math.round(input.clickHoverMoves) : 3);
  const showPlaybackOverlay = input.showPlaybackOverlay === true;
  const execution = currentSettings.execution || {};
  const executionPrimaryPath = normalizeActionExecutionPath(
      execution.primaryPath || execution.primary || 'cdp',
      'cdp');
  const executionFallbackPath = normalizeActionExecutionFallbackPath(
      execution.fallbackPath || execution.fallback || '',
      executionPrimaryPath);

  container.innerHTML = `
    <div class="settings-scroll-shell">
      <div class="settings-toolbar">
        <div class="settings-toolbar-copy">
          <h2 class="settings-toolbar-title">Settings Actions</h2>
          <p class="settings-toolbar-subtitle">Applies to all sections below.</p>
        </div>
        <button id="saveSettingsBtn" class="btn btn-primary">Save All Settings</button>
      </div>

      <div class="settings-scroll-body">
        <div class="settings-grid">
      <div class="settings-column">
      <section class="settings-card">
        <h2 class="settings-card-title">General Settings</h2>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="randomDelayToggle" checked />
            <span>Enable 1–5s randomized pause between actions</span>
          </label>
        </div>
        <div class="setting-item">
          <label class="label-with-tooltip">
            <span>Action Execution Primary Path:</span>
            <span class="tooltip-container" tabindex="0" aria-label="Execution path note">?
              <span class="tooltip-text">Native execution is most reliable on the active foreground tab and may fail on background tabs. Use native primarily when you want more human-like foreground automation.</span>
            </span>
          </label>
          <select id="actionExecutionPrimarySelect" class="input-field">
            <option value="cdp" ${executionPrimaryPath === 'cdp' ? 'selected' : ''}>CDP</option>
            <option value="native" ${executionPrimaryPath === 'native' ? 'selected' : ''}>Native</option>
          </select>
          <div class="help-text">Default execution path for click, type, drag, scroll, and key actions.</div>
        </div>
        <div class="setting-item">
          <label>Action Execution Fallback Path (optional):</label>
          <select id="actionExecutionFallbackSelect" class="input-field">
            <option value="" ${!executionFallbackPath ? 'selected' : ''}>None</option>
            <option value="cdp" ${executionFallbackPath === 'cdp' ? 'selected' : ''}>CDP</option>
            <option value="native" ${executionFallbackPath === 'native' ? 'selected' : ''}>Native</option>
          </select>
          <div class="help-text">Used only if the primary path fails for a given action.</div>
        </div>
        <div class="setting-item">
          <label>Theme:</label>
          <select id="themeSelect" class="input-field">
            <option value="dark">Matrix Dark</option>
            <option value="light">Light Theme</option>
          </select>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="smartLoginToggle" checked />
            <span>Smart Login (try-first, fallback to login)</span>
          </label>
        </div>
        <div class="setting-item">
          <label>Listener execution mode:</label>
          <select id="listenersModeSelect" class="input-field">
            <option value="headless">Headless listener instance (isolated Chromium on :9222)</option>
            <option value="background">Background tab in this browser</option>
          </select>
          <div class="help-text">Headless launches a separate HeadlessChrome process (shared profile under Simulachrome/Headless). Start failures surface as errors instead of silently opening a headed background tab. Choose Background tab when you want in-process debugging.</div>
        </div>
      </section>

      <section class="settings-card">
        <h2 class="settings-card-title">Dashboard Agent</h2>
        <div class="setting-item">
          <label>Dashboard Agent Provider:</label>
          <select id="dashboardProviderSelect" class="input-field">
            <option value="anthropic" ${dashboardProvider === 'anthropic' ? 'selected' : ''}>Anthropic</option>
            <option value="openai" ${dashboardProvider === 'openai' ? 'selected' : ''}>OpenAI</option>
            <option value="grok" ${dashboardProvider === 'grok' ? 'selected' : ''}>Grok</option>
            <option value="gemini" ${dashboardProvider === 'gemini' ? 'selected' : ''}>Gemini</option>
            <option value="cerebras" ${dashboardProvider === 'cerebras' ? 'selected' : ''}>Cerebras</option>
            <option value="local" ${dashboardProvider === 'local' ? 'selected' : ''}>Local (OpenAI-compatible)</option>
          </select>
          <div class="help-text">Hosted providers read keys from Data > Secrets (<code>ANTHROPIC_API_KEY</code>, <code>OPENAI_API_KEY</code>, <code>XAI_API_KEY</code>, <code>GEMINI_API_KEY</code>, <code>CEREBRAS_API_KEY</code>). Grok uses strict structured outputs. Local uses an OpenAI-compatible base URL you run yourself (Ollama <code>/v1</code>, LM Studio, llama.cpp, etc.). Optional <code>LOCAL_API_KEY</code>.</div>
        </div>
        <div class="setting-item">
          <label>Dashboard Agent Model:</label>
          <input type="text" id="dashboardModelInput" class="input-field"
                 autocomplete="off" autocapitalize="off" spellcheck="false"
                 placeholder="model id, including the colon (qwen2.5vl:7b)"
                 style="${dashboardProvider === 'local' ? '' : 'display:none'}" />
          <select id="dashboardLocalModelPreset" class="input-field"
                  style="${dashboardProvider === 'local' ? '' : 'display:none'}">
            <option value="">Insert a known model id…</option>
          </select>
          <select id="dashboardModelSelect" class="input-field"
                  style="${dashboardProvider === 'local' ? 'display:none' : ''}">
            ${buildModelSelectOptions(dashboardProvider, dashboardModel,
                                      dashboardOllamaEndpoint)}
          </select>
          <div class="help-text">Hosted: pick from the list. Local: type the exact id, colon included (<code>qwen2.5vl:7b</code>). The field saves that string as typed.</div>
        </div>
        <div class="setting-item">
          <label>Dashboard Agent Loop Profile:</label>
          <select id="dashboardLoopProfileSelect" class="input-field">
            <option value="glic_task" ${dashboardLoopProfile === 'glic_task' ? 'selected' : ''}>Task Completer</option>
            <option value="manager" ${dashboardLoopProfile === 'manager' ? 'selected' : ''}>Manager (embedded VLM eyes/hands)</option>
            <option value="dashboard_assistant" ${dashboardLoopProfile === 'dashboard_assistant' ? 'selected' : ''}>Dashboard Assistant</option>
            <option value="investigator" ${dashboardLoopProfile === 'investigator' ? 'selected' : ''}>Investigator</option>
          </select>
          <div class="help-text">Task Completer: GLiC model drives clicks/types via page context. Manager: GLiC model plans; embedded smolvlm2 (utility process) is eyes/hands via Blink. Dashboard Assistant: scripts/settings/data. Investigator: headless diagnosis.</div>
        </div>
        <div class="setting-item">
          <label>Local Endpoint (OpenAI-compatible):</label>
          <input type="text" id="dashboardOllamaEndpointInput" class="input-field"
                 placeholder="e.g. http://127.0.0.1:11434/v1/chat/completions"
                 value="${escapeHtml(dashboardOllamaEndpoint)}" />
          <div class="help-text">Used only for Local. Prefer a <code>/v1/chat/completions</code> URL. Presets: ${LOCAL_ENDPOINT_PRESETS.map((p) => p.label).join(', ')}. Start the server yourself — Simulachrome does not auto-launch local runtimes.</div>
        </div>
        <div class="setting-item">
          <label id="dashboardHostedEndpointLabel">Hosted Provider Endpoint:</label>
          <input type="text" id="dashboardHostedEndpointInput" class="input-field"
                 placeholder="e.g. https://api.openai.com/v1/chat/completions"
                 value="${escapeHtml(dashboardHostedEndpoint)}" />
          <div class="help-text" id="dashboardHostedEndpointHelp">Used when provider is one of Cerebras, Gemini, OpenAI, or Anthropic.</div>
        </div>
        <div class="setting-item">
          <div style="display:flex; gap:10px; align-items:center; flex-wrap:wrap;">
            <button type="button" id="testAgentConnectionBtn" class="btn btn-secondary">Test Agent Connection</button>
            <span class="data-status" id="testAgentConnectionStatus" aria-live="polite"></span>
          </div>
          <div class="help-text">Sends a tiny chat request using the <em>current</em> Provider, Model, and Endpoint fields (API keys from Data &gt; Secrets). Does not save settings.</div>
        </div>
      </section>
      </div>

      <div class="settings-column">
      <section class="settings-card">
        <h2 class="settings-card-title">Agent context and target acquisition</h2>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="jsContextInjectionFallbackToggle" ${jsContextInjectionFallback ? 'checked' : ''} />
            <span>Last resort fallback to JS injection for context</span>
          </label>
          <div class="help-text">Off. Page context comes from Annotated Page Content. The page script runs only when this is checked and that snapshot fails. While off, the script is not sent to the page.</div>
        </div>
      </section>

      <section class="settings-card">
        <h2 class="settings-card-title">Performance Modes</h2>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="aggressivePerformanceToggle" ${aggressivePerformance ? 'checked' : ''} />
            <span>Aggressive Performance (Process Compression)</span>
          </label>
          <div class="help-text">Restart required. Adds: --disable-site-isolation-trials, --process-per-site, --renderer-process-limit=4</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="deterministicModeToggle" ${deterministicMode ? 'checked' : ''} />
            <span>Deterministic Replay Mode</span>
          </label>
          <div class="help-text">Restart required. Disables BFCache/prerender spare-renderer paths for more consistent automation hooks.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="failureReplayToggle" ${failureReplayEnabled ? 'checked' : ''} />
            <span>Failure replay capture</span>
          </label>
          <div class="help-text">When a script fails, save a short low-res (≤480×270) PNG ring buffer from the tab surface. Off by default — CopyFromSurface captures contend with page paint/load on heavy SPAs.</div>
        </div>
      </section>

      <section class="settings-card">
        <h2 class="settings-card-title">Recording (Stealth)</h2>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="recordingStealthModeToggle" ${recordingStealthMode ? 'checked' : ''} />
            <span>Stealth recording (recommended)</span>
          </label>
          <div class="help-text">Default on. Uses browser-side InputObserver + stealthy element resolver. Avoids page-visible __dash_* injects and eager CDP Runtime/DOM enable that trigger bot detection.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="recordingAllowPageInjectsToggle" ${recordingAllowPageInjects ? 'checked' : ''} />
            <span>Allow page injects (badges / click guards / window.open patch)</span>
          </label>
          <div class="help-text">Off by default. Only enable for guided-loop UI overlays; sites can detect these and hard-block.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="recordingEagerCdpAttachToggle" ${recordingEagerCdpAttach ? 'checked' : ''} />
            <span>Eager CDP attach (Runtime.enable + DOM.enable on record start)</span>
          </label>
          <div class="help-text">Off by default. Domains enable lazily only if a CDP fallback (e.g. DOM.getNodeForLocation) is needed.</div>
        </div>
      </section>

      <section class="settings-card">
        <h2 class="settings-card-title">Anti-Detection</h2>
        <div class="help-text">Applied by the browser, not by script in the page. navigator.webdriver is left native: headless launches disable AutomationControlled, and a JavaScript getter on that property is itself a detection signal. Platform, hardware concurrency, and plugins stay real for the same reason.</div>
        <div class="setting-item">
          <label>User-Agent:</label>
          <input type="text" id="uaInput" class="input-field" placeholder="Leave blank to use this browser's UA and client hints" value="${escapeHtml(ua)}" />
          <div class="help-text">A custom value overrides the request header and navigator.userAgent together. Client hints stay this browser's real ones, so leave this blank unless the default string is the problem. Takes effect on the next navigation.</div>
        </div>
        <div class="setting-item">
          <label>Languages (comma-separated):</label>
          <input type="text" id="langsInput" class="input-field" placeholder="Leave blank to use the profile's languages" value="${escapeHtml(languages)}" />
          <div class="help-text">Sets the Accept-Language preference, which drives both the request header and navigator.languages.</div>
        </div>
      </section>
      </div>

      <div class="settings-column">
      <section class="settings-card">
        <h2 class="settings-card-title">Input Behavior</h2>
        <div class="setting-item">
          <label>Click Path Jitter (px per step):</label>
          <input type="number" id="clickPathJitter" class="input-field" step="0.1" min="0" value="${clickPathJitterPx}" />
        </div>
        <div class="setting-item">
          <label>Micro Hover Radius (px):</label>
          <input type="number" id="clickHoverRadius" class="input-field" step="0.1" min="0" value="${clickHoverRadiusPx}" />
        </div>
        <div class="setting-item">
          <label>Micro Hover Moves (count):</label>
          <input type="number" id="clickHoverMoves" class="input-field" min="0" max="10" value="${clickHoverMoves}" />
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="showPlaybackOverlayToggle" ${showPlaybackOverlay ? 'checked' : ''} />
            <span>Show playback/agent cursor overlay (crosshair/trail/click pulse)</span>
          </label>
          <div class="help-text">Debug-only visual aid for manual playback and agent executor runs. Overlay is non-click-blocking.</div>
        </div>
      </section>

      <section class="settings-card">
        <h2 class="settings-card-title">Debugging</h2>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="clickDebugLoggingToggle" />
            <span>Enable click failure diagnostic logging</span>
          </label>
          <div class="help-text">When enabled, playback logs elementFromPoint diagnostics, computed styles, overlay detection, and rects. Off by default.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="autoQueueHealFailuresToggle" ${autoQueueHealFailures ? 'checked' : ''} />
            <span>Auto-queue failures for model auto-heal</span>
          </label>
          <div class="help-text">When enabled, failed runs are added to a serialized auto-heal queue.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="autoHealHeadlessInvestigateToggle" ${autoHealInvestigateInHeadless ? 'checked' : ''} />
            <span>Prefer headless investigation for auto-heal</span>
          </label>
          <div class="help-text">Runs investigate_page on the isolated headless Chromium (:9222). Never opens tabs in the headed main browser.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="keepWarmHeadlessInvestigationTabToggle" ${keepWarmHeadlessInvestigationTab ? 'checked' : ''} />
            <span>Keep warm headless investigation tab</span>
          </label>
          <div class="help-text">Keeps one reusable target alive inside the isolated headless Chromium (:9222) between probes when supported. Does not open tabs in the headed main browser.</div>
        </div>
      </section>

      <section class="settings-card">
        <h2 class="settings-card-title">Subsystem Runtime Cuts</h2>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="pdfViewerEnabledToggle" ${pdfViewerEnabled ? 'checked' : ''} />
            <span>Enable built-in PDF viewer (PDFium)</span>
          </label>
          <div class="help-text">When disabled, PDFs are downloaded externally instead of rendered in-browser.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="geolocationAllowedByDefaultToggle" ${geolocationAllowedByDefault ? 'checked' : ''} />
            <span>Allow website geolocation by default</span>
          </label>
          <div class="help-text">Auto-allows site location prompts inside Chromium (chrome://settings/content/location default). Does not grant macOS Location Services — unsigned Chromium builds must be codesigned and approved in System Settings or getCurrentPosition will TIMEOUT even when this is on.</div>
        </div>
      </section>

      <section class="settings-card">
        <h2 class="settings-card-title">Restart Required Settings</h2>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="extensionsEnabledToggle" ${extensionsEnabled ? 'checked' : ''} />
            <span>Enable extensions runtime</span>
          </label>
          <div class="help-text">When disabled, launch uses --disable-extensions.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="printPreviewEnabledToggle" ${printPreviewEnabled ? 'checked' : ''} />
            <span>Enable print preview UI</span>
          </label>
          <div class="help-text">When disabled, launch uses --disable-print-preview.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="printStubEnabledToggle" ${printStubEnabled ? 'checked' : ''} />
            <span>Enable print pipeline stub</span>
          </label>
          <div class="help-text">Keeps window.print() callable but skips heavy print pipeline (beforeprint/afterprint still fire).</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="remoteDebuggingEnabledToggle" ${remoteDebuggingEnabled ? 'checked' : ''} />
            <span>Enable remote debugging port (DevTools / CDP)</span>
          </label>
          <div class="help-text">Off by default for production. When on, launch may open a DevTools port (default 9223).</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="safeBrowsingEnabledToggle" ${safeBrowsingEnabled ? 'checked' : ''} />
            <span>Enable Safe Browsing protections</span>
          </label>
          <div class="help-text">Disabling reduces overhead but removes protection layers. Requires restart for launch-flag parity.</div>
        </div>
        <div class="setting-item">
          <label class="inline-toggle">
            <input type="checkbox" id="serviceDiscoveryEnabledToggle" ${serviceDiscoveryEnabled ? 'checked' : ''} />
            <span>Enable service discovery</span>
          </label>
          <div class="help-text">Disabling trims local discovery paths used by media routing and related services.</div>
        </div>
        <div class="help-text">These options are saved immediately but only fully apply after restarting Simulachrome.</div>
      </section>
      </div>
        </div>
      </div>
    </div>
  `;

  const q = (sel) => container.querySelector(sel);
  const delayToggle = q('#randomDelayToggle');
  const themeSelect = q('#themeSelect');

  // Initialize current values (local UI state)
  if (delayToggle) delayToggle.checked = randomDelayEnabled;
  if (themeSelect) themeSelect.value = theme;
  const dashboardModelSelect = q('#dashboardModelSelect');
  if (dashboardModelSelect) dashboardModelSelect.value = dashboardModel;
  const dashboardLoopProfileSelect = q('#dashboardLoopProfileSelect');
  if (dashboardLoopProfileSelect) {
    dashboardLoopProfileSelect.value = dashboardLoopProfile;
  }
  const dashboardOllamaEndpointInput = q('#dashboardOllamaEndpointInput');
  if (dashboardOllamaEndpointInput) {
    dashboardOllamaEndpointInput.value = dashboardOllamaEndpoint;
  }
  const listenersModeSelect = q('#listenersModeSelect');
  if (listenersModeSelect) listenersModeSelect.value = listenerExecutionMode;
  applyTheme(theme);

  if (delayToggle) {
    delayToggle.addEventListener('change', () => {
      if (!delayToggle.checked) {
        if (!confirm("Disabling randomized delay may cause bot detection issues. Proceed?")) {
          delayToggle.checked = true;
          return;
        }
      }
    });
  }

  if (themeSelect) {
    themeSelect.addEventListener('change', () => {
      applyTheme(themeSelect.value);
    });
  }

  const saveBtn = q('#saveSettingsBtn');
  const executionPrimarySelect =
    /** @type {HTMLSelectElement} */(q('#actionExecutionPrimarySelect'));
  const executionFallbackSelect =
    /** @type {HTMLSelectElement} */(q('#actionExecutionFallbackSelect'));
  const providerSelect =
    /** @type {HTMLSelectElement} */(q('#dashboardProviderSelect'));
  const modelSelect =
    /** @type {HTMLSelectElement} */(q('#dashboardModelSelect'));
  const endpointInput =
    /** @type {HTMLInputElement} */(q('#dashboardOllamaEndpointInput'));
  const hostedEndpointInput =
    /** @type {HTMLInputElement} */(q('#dashboardHostedEndpointInput'));
  const hostedEndpointLabel = q('#dashboardHostedEndpointLabel');
  const hostedEndpointHelp = q('#dashboardHostedEndpointHelp');
  const modelInputEl =
      /** @type {HTMLInputElement} */(q('#dashboardModelInput'));
  if (modelInputEl) {
    // Set the DOM property, not the value="" attribute. The old datalist
    // committed at the first ':' and saved the id with the colon deleted.
    modelInputEl.value = dashboardModel;
  }
  const localPresetEl =
      /** @type {HTMLSelectElement} */(q('#dashboardLocalModelPreset'));
  fillLocalModelPreset(localPresetEl);
  if (providerSelect && modelSelect) {
    syncDashboardModelOptions(
        providerSelect, modelSelect, endpointInput, hostedEndpointInput,
        providerEndpointState, hostedEndpointLabel, hostedEndpointHelp);
    const onProviderChange = () => {
      syncDashboardModelOptions(
          providerSelect, modelSelect, endpointInput, hostedEndpointInput,
          providerEndpointState, hostedEndpointLabel, hostedEndpointHelp);
    };
    providerSelect.addEventListener('change', () => {
      onProviderChange();
      if (typeof onSave === 'function') {
        applyAgentFieldsFromSettingsForm(container, onSave, currentSettings);
      }
    });
  }
  if (modelInputEl && typeof onSave === 'function') {
    modelInputEl.addEventListener('change', () => {
      applyAgentFieldsFromSettingsForm(container, onSave, currentSettings);
    });
  }
  if (localPresetEl && modelInputEl) {
    localPresetEl.addEventListener('change', () => {
      const id = String(localPresetEl.value || '');
      localPresetEl.value = '';
      if (!id) {
        return;
      }
      modelInputEl.value = id;
      if (typeof onSave === 'function') {
        applyAgentFieldsFromSettingsForm(container, onSave, currentSettings);
      }
    });
  }
  if (providerSelect && hostedEndpointInput) {
    hostedEndpointInput.addEventListener('input', () => {
      const provider = normalizeDashboardProvider(providerSelect.value);
      if (!providerUsesOllamaEndpoint(provider)) {
        providerEndpointState[provider] = hostedEndpointInput.value.trim();
      }
    });
  }
  if (modelSelect && endpointInput) {
    modelSelect.addEventListener('change', () => {
      if (providerUsesOllamaEndpoint(providerSelect?.value || 'ollama')) {
        const selectedOption =
          modelSelect.options[modelSelect.selectedIndex] || null;
        const selectedEndpoint = selectedOption?.dataset?.endpoint || '';
        if (selectedEndpoint) {
          endpointInput.value = selectedEndpoint;
        }
      }
      if (typeof onSave === 'function') {
        applyAgentFieldsFromSettingsForm(container, onSave, currentSettings);
      }
    });
  }
  const testConnectionBtn = q('#testAgentConnectionBtn');
  if (testConnectionBtn) {
    testConnectionBtn.addEventListener('click', () => {
      testDashboardAgentConnection(container);
    });
  }
  if (saveBtn) saveBtn.addEventListener('click', () => {
    const selectedProvider = normalizeDashboardProvider(
        /** @type {HTMLSelectElement} */(q('#dashboardProviderSelect')).value);
    const selectedLoopProfile = normalizeDashboardLoopProfile(
        /** @type {HTMLSelectElement} */(q('#dashboardLoopProfileSelect')).value);
    const selectedModelRaw = String(
        (selectedProvider === 'local'
          ? (/** @type {HTMLInputElement} */(q('#dashboardModelInput')))?.value
          : (/** @type {HTMLSelectElement} */(q('#dashboardModelSelect')))?.value) ||
      defaultModelForProvider(selectedProvider)).trim() ||
      defaultModelForProvider(selectedProvider);
    const selectedModel = sanitizeModelForProvider(
        selectedProvider, selectedModelRaw);
    if (!providerUsesOllamaEndpoint(selectedProvider) && hostedEndpointInput) {
      providerEndpointState[selectedProvider] =
        hostedEndpointInput.value.trim();
    }
    const normalizedProviderEndpoints = {};
    for (const provider of DASHBOARD_AGENT_PROVIDERS) {
      if (providerUsesOllamaEndpoint(provider)) {
        continue;
      }
      const fallback = defaultEndpointForProvider(provider);
      normalizedProviderEndpoints[provider] = normalizeHostedEndpoint(
          providerEndpointState[provider] || fallback,
          fallback);
    }
    const selectedOllamaEndpoint = (providerUsesOllamaEndpoint(selectedProvider)
      ? (((/** @type {HTMLSelectElement} */(q('#dashboardModelSelect'))).selectedOptions?.[0]?.dataset?.endpoint) ||
         (/** @type {HTMLInputElement} */(q('#dashboardOllamaEndpointInput'))).value ||
         DEFAULT_OLLAMA_ENDPOINT)
      : ((/** @type {HTMLInputElement} */(q('#dashboardOllamaEndpointInput'))).value ||
         DEFAULT_OLLAMA_ENDPOINT)).trim() || DEFAULT_OLLAMA_ENDPOINT;
    const selectedHostedEndpoint = providerUsesOllamaEndpoint(selectedProvider)
      ? normalizedProviderEndpoints.cerebras
      : normalizeHostedEndpoint(
          normalizedProviderEndpoints[selectedProvider] ||
            defaultEndpointForProvider(selectedProvider),
          defaultEndpointForProvider(selectedProvider));
    const selectedExecutionPrimaryPath = normalizeActionExecutionPath(
        executionPrimarySelect?.value || 'cdp',
        'cdp');
    const selectedExecutionFallbackPath =
      normalizeActionExecutionFallbackPath(
          executionFallbackSelect?.value || '',
          selectedExecutionPrimaryPath);

    const newSettings = {
      theme: /** @type {HTMLSelectElement} */(q('#themeSelect')).value === 'light'
        ? 'light'
        : 'dark',
      execution: {
        primaryPath: selectedExecutionPrimaryPath,
        fallbackPath: selectedExecutionFallbackPath,
      },
      agent: {
        provider: selectedProvider,
        loopProfile: selectedLoopProfile,
        model: selectedModel,
        localEndpoint: selectedOllamaEndpoint,
        ollamaEndpoint: selectedOllamaEndpoint,
        endpoint: selectedHostedEndpoint,
        providerEndpoints: normalizedProviderEndpoints,
        keepWarmHeadlessInvestigationTab:
          /** @type {HTMLInputElement} */(q('#keepWarmHeadlessInvestigationTabToggle')).checked,
        jsContextInjectionFallback:
          /** @type {HTMLInputElement} */(q('#jsContextInjectionFallbackToggle')).checked === true,
        // Preserve autonomous-task settings not yet exposed in this form.
        responseMode: String(agent.responseMode || 'json'),
        autoStartTaskOnLaunch: agent.autoStartTaskOnLaunch === true,
        autoStartDelayMs: Number.isFinite(Number(agent.autoStartDelayMs))
          ? Number(agent.autoStartDelayMs)
          : 5000,
        autoStartPrompt: String(agent.autoStartPrompt || ''),
        autoSaveScriptOnComplete: agent.autoSaveScriptOnComplete !== false,
        reviewScriptAfterComplete: agent.reviewScriptAfterComplete !== false,
        maxAutonomousSteps: Number.isFinite(Number(agent.maxAutonomousSteps))
          ? Number(agent.maxAutonomousSteps)
          : 40,
        modelStepTimeoutMs: Number.isFinite(Number(agent.modelStepTimeoutMs))
          ? Number(agent.modelStepTimeoutMs)
          : (20 * 60 * 1000),
      },
      smartLogin: /** @type {HTMLInputElement} */(q('#smartLoginToggle')).checked,
      antiDetection: {
        userAgent: /** @type {HTMLInputElement} */(q('#uaInput')).value.trim(),
        languages: /** @type {HTMLInputElement} */(q('#langsInput')).value.split(',').map(s => s.trim()).filter(Boolean),
      },
      inputBehavior: {
        randomDelayEnabled:
          /** @type {HTMLInputElement} */(q('#randomDelayToggle')).checked,
        clickPathJitterPx: Math.max(0, parseFloat(/** @type {HTMLInputElement} */(q('#clickPathJitter')).value || '1.5')),
        clickHoverRadiusPx: Math.max(0, parseFloat(/** @type {HTMLInputElement} */(q('#clickHoverRadius')).value || '2')),
        clickHoverMoves: Math.max(0, parseInt(/** @type {HTMLInputElement} */(q('#clickHoverMoves')).value || '3', 10)),
        showPlaybackOverlay: /** @type {HTMLInputElement} */(q('#showPlaybackOverlayToggle')).checked
      },
      listenerMode: {
        runListenersInHeadlessMode:
          /** @type {HTMLSelectElement} */(q('#listenersModeSelect')).value !== 'background'
      },
      autoHeal: {
        autoQueueFailures:
          /** @type {HTMLInputElement} */(q('#autoQueueHealFailuresToggle')).checked,
        investigateInHeadless:
          /** @type {HTMLInputElement} */(q('#autoHealHeadlessInvestigateToggle')).checked,
      },
      recording: {
        stealthMode:
          /** @type {HTMLInputElement} */(q('#recordingStealthModeToggle')).checked,
        allowPageInjects:
          /** @type {HTMLInputElement} */(q('#recordingAllowPageInjectsToggle')).checked,
        eagerCdpAttach:
          /** @type {HTMLInputElement} */(q('#recordingEagerCdpAttachToggle')).checked,
      },
      performance: {
        aggressivePerformance:
          /** @type {HTMLInputElement} */(q('#aggressivePerformanceToggle')).checked,
        deterministicMode:
          /** @type {HTMLInputElement} */(q('#deterministicModeToggle')).checked,
        failureReplayEnabled:
          /** @type {HTMLInputElement} */(q('#failureReplayToggle')).checked,
      },
      subsystem: {
        pdfViewerEnabled:
          /** @type {HTMLInputElement} */(q('#pdfViewerEnabledToggle')).checked,
        geolocationAllowedByDefault:
          /** @type {HTMLInputElement} */(q('#geolocationAllowedByDefaultToggle')).checked,
        extensionsEnabled:
          /** @type {HTMLInputElement} */(q('#extensionsEnabledToggle')).checked,
        printPreviewEnabled:
          /** @type {HTMLInputElement} */(q('#printPreviewEnabledToggle')).checked,
        printStubEnabled:
          /** @type {HTMLInputElement} */(q('#printStubEnabledToggle')).checked,
        remoteDebuggingEnabled:
          /** @type {HTMLInputElement} */(q('#remoteDebuggingEnabledToggle')).checked,
        safeBrowsingEnabled:
          /** @type {HTMLInputElement} */(q('#safeBrowsingEnabledToggle')).checked,
        serviceDiscoveryEnabled:
          /** @type {HTMLInputElement} */(q('#serviceDiscoveryEnabledToggle')).checked,
      }
    };
    try {
      window.__DASH_MODEL = newSettings.agent.model;
      window.__DASH_PROVIDER = newSettings.agent.provider;
      window.__DASH_AGENT_PROVIDER = newSettings.agent.provider;
      window.__DASH_OLLAMA_ENDPOINT = newSettings.agent.ollamaEndpoint;
      window.__DASH_LOOP_PROFILE = newSettings.agent.loopProfile;
      window.__DASH_KEEP_WARM_HEADLESS =
        newSettings.agent.keepWarmHeadlessInvestigationTab !== false;
      localStorage.setItem(
          'glic.loop_profile',
          normalizeDashboardLoopProfile(newSettings.agent.loopProfile));
      localStorage.setItem(
          'glic.provider',
          normalizeDashboardProvider(newSettings.agent.provider));
      localStorage.setItem('glic.model', String(newSettings.agent.model || ''));
      localStorage.setItem(
          'glic.keep_warm_headless_investigation_tab',
          newSettings.agent.keepWarmHeadlessInvestigationTab !== false ?
            '1' :
            '0');
    } catch (_) {}
    if (typeof onSave === 'function') onSave(newSettings);
  });

  // REFACTOR: Removed API key management listeners

  // Helper function for sending messages to the backend
  const call = (name, args=[])=> new Promise((resolve)=>{ const cb='__settings_cb_'+Date.now()+Math.random().toString(36).slice(2); window[cb]=(res)=>{ console.log('[Settings] Callback '+cb+' received:', res); try{ delete window[cb]; }catch(_){ } resolve(res); }; console.log('[Settings] Calling '+name+' with callback '+cb); chrome.send(name, [cb, ...args]); });

  // Initialize Debug Logging toggle state and handlers
  const debugToggle = q('#clickDebugLoggingToggle');
  (async()=>{
    try {
      const res = await call('dashboardGetDebugLogging');
      if (debugToggle) debugToggle.checked = !!(res && (res.enabled === true || res.enabled === 'true'));
    } catch(e) { console.warn('[Settings] Unable to get debug logging state', e); }
  })();
  if (debugToggle) debugToggle.addEventListener('change', async()=>{
    try { await call('dashboardSetDebugLogging', [debugToggle.checked]); }
    catch(e) { console.error('[Settings] Failed to set debug logging', e); }
  });

  // REFACTOR: Removed the async IIFE that called dashboardGetApiKey
  // REFACTOR: Removed apiSave click listener
  // REFACTOR: Removed apiClear click listener
}

// Apply theme CSS
export function applyTheme(theme) {
  if (theme === 'light') {
    document.documentElement.classList.add('light-theme');
    document.documentElement.classList.remove('dark-theme');
  } else {
    document.documentElement.classList.add('dark-theme');
    document.documentElement.classList.remove('light-theme');
  }
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
}

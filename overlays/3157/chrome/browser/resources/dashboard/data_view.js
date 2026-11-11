function normalizeKey(raw) {
  let s = String(raw || '').trim();
  if (!s) return '';
  if (s.startsWith('{') && s.endsWith('}') && s.length >= 3) {
    s = s.slice(1, -1).trim();
  }
  return s;
}

function splitTaggedKey(key) {
  const clean = normalizeKey(key);
  if (!clean) {
    return {raw: '', base: '', tags: []};
  }
  const parts = clean.split('#').map(p => p.trim()).filter(Boolean);
  if (!parts.length) {
    return {raw: clean, base: clean, tags: []};
  }
  return {raw: clean, base: parts[0], tags: parts.slice(1)};
}

function parseTagRef(rawTag) {
  const raw = String(rawTag || '').trim();
  const ref = {
    rawTag: raw,
    groupedTag: raw,
    order: null,
  };
  if (!raw) return ref;
  const match = raw.match(/^(.*?)(\d+)$/);
  if (!match || !match[1]) return ref;
  ref.groupedTag = match[1];
  ref.order = Number.parseInt(match[2], 10);
  return ref;
}

function makeTagTokenValue(rawValue, order, sequence) {
  let value = String(rawValue ?? '');
  let noDelimiter = false;
  if (value.length >= 2) {
    const first = value[0];
    const last = value[value.length - 1];
    if ((first === '"' && last === '"') || (first === '\'' && last === '\'')) {
      noDelimiter = true;
      value = value.slice(1, -1);
    }
  }
  return {value, order, sequence, noDelimiter};
}

function appendUniqueTagValue(tagMap, tag, rawValue, order, sequence) {
  const key = String(tag || '').trim();
  if (!key) return;
  if (!tagMap[key]) tagMap[key] = [];
  const candidate = makeTagTokenValue(rawValue, order, sequence);
  if (tagMap[key].some((existing) => existing.value === candidate.value)) {
    return;
  }
  tagMap[key].push(candidate);
}

function sortTagValues(values) {
  return [...(values || [])].sort((a, b) => {
    const aHasOrder = Number.isInteger(a?.order);
    const bHasOrder = Number.isInteger(b?.order);
    if (aHasOrder !== bHasOrder) return aHasOrder ? -1 : 1;
    if (aHasOrder && bHasOrder && a.order !== b.order) {
      return a.order - b.order;
    }
    return (a?.sequence || 0) - (b?.sequence || 0);
  });
}

function joinTagScalarValue(values) {
  const sorted = sortTagValues(values);
  if (!sorted.length) return '';
  const useConcat = sorted.some((item) => !!item?.noDelimiter);
  return sorted.map((item) => item.value).join(useConcat ? '' : ',');
}

const BUILTIN_LISTENER_TOKENS = [
  'listener.match',
  'listener.match.value',
  'listener.match.text',
  'listener.match.attr',
  'listener.match.url',
  'listener.match.page_url',
];

const RECOMMENDED_PROVIDER_SECRET_KEYS = [
  'GEMINI_API_KEY',
  'CEREBRAS_API_KEY',
  'OPENAI_API_KEY',
  'ANTHROPIC_API_KEY',
];

function collectListenerCaptureVariablesFromAction(action, tokenSet) {
  if (!action || typeof action !== 'object') return;
  const params = (action.params && typeof action.params === 'object') ? action.params : {};
  const captureVariable = normalizeKey(params.captureVariable || '');
  if (captureVariable) {
    tokenSet.add(captureVariable);
  }

  const visitActionList = (list) => {
    if (!Array.isArray(list)) return;
    for (const child of list) {
      collectListenerCaptureVariablesFromAction(child, tokenSet);
    }
  };

  visitActionList(action.steps);
  visitActionList(action.then);
  visitActionList(action.else);
  visitActionList(action.nested);
}

function collectRuntimeTokensFromAction(action, tokenSet) {
  if (!action || typeof action !== 'object') return;
  const params = (action.params && typeof action.params === 'object') ? action.params : {};
  const actionName = String(action.action || '').trim().toLowerCase();

  const captureVariable = normalizeKey(
      params.captureVariable || action.captureVariable || '');
  if (captureVariable) {
    tokenSet.add(captureVariable);
  }

  const variableName = normalizeKey(
      params.variableName || action.variableName || '');
  if (variableName) {
    tokenSet.add(variableName);
  }

  if (actionName === 'glic_prompt' && Array.isArray(params.focusAreas)) {
    for (const area of params.focusAreas) {
      const label = normalizeKey(area?.label || '');
      if (label) {
        tokenSet.add(label);
      }
    }
  }

  const visitActionList = (list) => {
    if (!Array.isArray(list)) return;
    for (const child of list) {
      collectRuntimeTokensFromAction(child, tokenSet);
    }
  };

  visitActionList(action.steps);
  visitActionList(action.then);
  visitActionList(action.else);
  visitActionList(action.nested);
}

function collectListenerTokensFromScripts(scripts, tokenSet) {
  if (!scripts || typeof scripts !== 'object') return;
  for (const [scriptId, script] of Object.entries(scripts)) {
    const normalizedScriptId = normalizeKey(scriptId);
    if (normalizedScriptId) {
      for (const suffix of BUILTIN_LISTENER_TOKENS) {
        if (!suffix.startsWith('listener.')) continue;
        const subpath = suffix.slice('listener.'.length);
        tokenSet.add(`listener.${normalizedScriptId}.${subpath}`);
      }
    }
    const actions = Array.isArray(script?.actions) ? script.actions : [];
    for (const action of actions) {
      collectListenerCaptureVariablesFromAction(action, tokenSet);
      collectRuntimeTokensFromAction(action, tokenSet);
    }
  }
}

function parseIniKeyValue(text) {
  const out = {};
  const lines = String(text || '').split(/\r?\n/);
  for (const raw of lines) {
    const line = String(raw || '').replace(/\r$/, '');
    const trimmed = line.trim();
    if (!trimmed) continue;
    if (trimmed.startsWith('#') || trimmed.startsWith(';')) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = normalizeKey(line.slice(0, eq));
    const value = String(line.slice(eq + 1));
    if (!key) continue;
    out[key] = value;
  }
  return out;
}

function dictToEntries(dict) {
  const out = [];
  for (const [key, value] of Object.entries(dict || {})) {
    const clean = normalizeKey(key);
    if (!clean) continue;
    out.push({key: clean, value: String(value ?? '')});
  }
  out.sort((a, b) => a.key.localeCompare(b.key));
  return out;
}

function mergeRecommendedSecrets(entries) {
  const byKey = new Map();
  for (const entry of entries || []) {
    const key = normalizeKey(entry?.key || '');
    if (!key) continue;
    byKey.set(key, {key, hasValue: !!entry?.hasValue});
  }
  for (const key of RECOMMENDED_PROVIDER_SECRET_KEYS) {
    if (!byKey.has(key)) {
      byKey.set(key, {key, hasValue: false});
    }
  }
  return Array.from(byKey.values())
    .sort((a, b) => String(a.key || '').localeCompare(String(b.key || '')));
}

function escapeHtml(s) {
  return String(s || '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#039;');
}

function rowsToDict(container) {
  const out = {};
  const rows = container.querySelectorAll('.data-row');
  for (const row of rows) {
    const k = normalizeKey(row.querySelector('.data-key')?.value || '');
    if (!k) continue;
    out[k] = String(row.querySelector('.data-val')?.value || '');
  }
  return out;
}

function secretsUiToPayload(container) {
  const keys = [];
  const updates = [];
  const rows = container.querySelectorAll('.data-row');
  for (const row of rows) {
    const k = normalizeKey(row.querySelector('.data-key')?.value || '');
    if (!k) continue;
    keys.push(k);
    const v = String(row.querySelector('.data-secret')?.value || '');
    if (v.trim()) updates.push({key: k, value: v});
  }
  return {keys, updates};
}

function collectSecretEntries(container) {
  const out = [];
  const rows = container.querySelectorAll('.data-row');
  for (const row of rows) {
    const k = normalizeKey(row.querySelector('.data-key')?.value || '');
    if (!k) continue;
    out.push({key: k});
  }
  return out;
}

function renderRows(container, entries, kind) {
  const rows = [];
  for (const e of entries) {
    const key = normalizeKey(e?.key || '');
    if (!key) continue;
    if (kind === 'secrets') {
      const hasValue = !!e?.hasValue;
      rows.push(`
        <div class="data-row">
          <input class="input-field data-key" value="${escapeHtml(key)}" placeholder="Key (e.g. ApiKey)"/>
          <input class="input-field data-secret" type="password" value="" placeholder="${hasValue ? 'Set new value (stored; never shown)' : 'Set value'}"/>
          <button class="btn btn-danger data-remove" title="Remove">✕</button>
        </div>
      `);
    } else {
      const value = String(e?.value || '');
      rows.push(`
        <div class="data-row">
          <input class="input-field data-key" value="${escapeHtml(key)}" placeholder="Key (e.g. FirstName)"/>
          <input class="input-field data-val" value="${escapeHtml(value)}" placeholder="Value"/>
          <button class="btn btn-danger data-remove" title="Remove">✕</button>
        </div>
      `);
    }
  }
  container.innerHTML = rows.join('') || `<div class="data-empty">No entries yet.</div>`;
}

function wireRemoveButtons(container, onChanged = null) {
  for (const btn of container.querySelectorAll('.data-remove')) {
    btn.addEventListener('click', (e) => {
      e.preventDefault();
      const row = btn.closest('.data-row');
      if (row) row.remove();
      if (!container.querySelector('.data-row')) {
        container.innerHTML = `<div class="data-empty">No entries yet.</div>`;
      }
      if (typeof onChanged === 'function') onChanged();
    });
  }
}

function addEmptyRow(container, kind, onChanged = null) {
  if (container.querySelector('.data-empty')) {
    container.textContent = '';
  }
  const row = document.createElement('div');
  row.className = 'data-row';
  if (kind === 'secrets') {
    row.innerHTML = `
      <input class="input-field data-key" value="" placeholder="Key (e.g. ApiKey)"/>
      <input class="input-field data-secret" type="password" value="" placeholder="Set value"/>
      <button class="btn btn-danger data-remove" title="Remove">✕</button>
    `;
  } else {
    row.innerHTML = `
      <input class="input-field data-key" value="" placeholder="Key (e.g. FirstName)"/>
      <input class="input-field data-val" value="" placeholder="Value"/>
      <button class="btn btn-danger data-remove" title="Remove">✕</button>
    `;
  }
  container.appendChild(row);
  wireRemoveButtons(container, onChanged);
  row.querySelector('input')?.focus();
  if (typeof onChanged === 'function') onChanged();
}

function setRowsFromDict(container, kind, dict, includeSecretValues = false) {
  const entries = dictToEntries(dict);
  if (kind === 'secrets') {
    const rendered = entries.map(e => ({key: e.key, hasValue: !!e.value}));
    renderRows(container, rendered, 'secrets');
    if (includeSecretValues) {
      const secretInputs = container.querySelectorAll('.data-secret');
      for (let i = 0; i < secretInputs.length; i += 1) {
        secretInputs[i].value = entries[i]?.value || '';
      }
    }
    return;
  }
  renderRows(container, entries, 'vars');
}

export function buildSmartInputCatalog(variableEntries, secretEntries, tables = {},
                                       scripts = {}) {
  const variables = {};
  const tags = {};
  const tokenSet = new Set();
  const variableKeys = [];
  const secretKeys = [];
  let tagSequence = 0;

  const registerToken = (token) => {
    const t = String(token || '').trim();
    if (!t) return;
    tokenSet.add(t);
  };

  for (const entry of variableEntries || []) {
    const {raw, base, tags: keyTags} = splitTaggedKey(entry?.key);
    const value = String(entry?.value ?? '');
    if (!base) continue;
    variableKeys.push(raw);
    variables[base] = value;
    variables[raw] = value;
    registerToken(base);
    registerToken(raw);
    for (const rawTag of keyTags) {
      const ref = parseTagRef(rawTag);
      registerToken(ref.rawTag);
      appendUniqueTagValue(tags, ref.rawTag, value, null, tagSequence++);
      if (ref.groupedTag && ref.groupedTag !== ref.rawTag) {
        registerToken(ref.groupedTag);
        appendUniqueTagValue(tags, ref.groupedTag, value, ref.order, tagSequence++);
      }
    }
  }

  for (const token of BUILTIN_LISTENER_TOKENS) {
    registerToken(token);
    if (!(token in variables)) variables[token] = '';
  }

  collectListenerTokensFromScripts(scripts, tokenSet);
  for (const token of tokenSet) {
    if (!(token in variables)) variables[token] = '';
  }

  for (const [tag, values] of Object.entries(tags)) {
    variables[tag] = joinTagScalarValue(values);
  }

  // Secrets are suggestion-only references; values stay local/hidden.
  for (const entry of secretEntries || []) {
    const {raw, base, tags: keyTags} = splitTaggedKey(entry?.key);
    if (!base) continue;
    secretKeys.push(raw);
    registerToken(base);
    registerToken(raw);
    if (!(base in variables)) variables[base] = '';
    if (!(raw in variables)) variables[raw] = '';
    for (const rawTag of keyTags) {
      const ref = parseTagRef(rawTag);
      registerToken(ref.rawTag);
      if (!tags[ref.rawTag]) tags[ref.rawTag] = [];
      if (!(ref.rawTag in variables)) variables[ref.rawTag] = '';
      if (ref.groupedTag && ref.groupedTag !== ref.rawTag) {
        registerToken(ref.groupedTag);
        if (!tags[ref.groupedTag]) tags[ref.groupedTag] = [];
        if (!(ref.groupedTag in variables)) variables[ref.groupedTag] = '';
      }
    }
  }

  const catalog = {
    variables,
    tables: (tables && typeof tables === 'object') ? tables : {},
    tags: Object.fromEntries(
      Object.entries(tags).map(([tag, values]) => [tag, sortTagValues(values).map((v) => v.value)])),
    tokens: Array.from(tokenSet).sort((a, b) => a.localeCompare(b)),
    variableKeys,
    secretKeys,
  };
  return catalog;
}

function publishSmartInputCatalog(variableEntries, secretEntries, appState) {
  const tables = appState?.tables || {};
  const scripts = appState?.scripts || {};
  const catalog =
    buildSmartInputCatalog(variableEntries, secretEntries, tables, scripts);
  window.__dashboardSmartInputCatalog = catalog;
  window.globalTags = catalog.tags || {};
  window.dispatchEvent(new CustomEvent('dashboard-smartinput-data-changed',
                                        {detail: catalog}));
  if (appState && typeof appState === 'object') {
    appState.dataReferenceCatalog = catalog;
  }
  return catalog;
}

async function readFileText(file) {
  if (!file) return '';
  if (typeof file.text === 'function') {
    return file.text();
  }
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result || ''));
    reader.onerror = reject;
    reader.readAsText(file);
  });
}

export async function renderDataView(container, dashboardHandler, appState, opts = {}) {
  container.innerHTML = `
    <div class="data-root">
      <div class="data-header">
        <div class="data-title-row">
          <h1 class="data-title">Data</h1>
          <div class="data-help">
            <button type="button" class="data-help-button" aria-expanded="false" aria-controls="data-rules-panel">?</button>
            <div class="data-help-panel" id="data-rules-panel" role="region" aria-label="Data and token rules">
              <p>Values saved here are filled in when a script runs. In any action field, write <code>{Key}</code>. <code>{{Key}}</code> is the same thing. A name that is not saved is left as written.</p>
              <h2>Variables and secrets</h2>
              <ul>
                <li>Variables are stored as plain text. Use them for names, emails, and other values that are not secrets.</li>
                <li>Secrets are encrypted on this machine. After you save, the value is hidden. Use them for passwords and API keys.</li>
                <li><code>{Key}</code> reads either table. If the same key exists in both, the variable wins.</li>
                <li>Import and export are INI lines, <code>key=value</code>. A line that starts with <code>#</code> or <code>;</code> is a comment.</li>
                <li>Braces in the key field are ignored, so <code>{FirstName}</code> and <code>FirstName</code> are the same key.</li>
                <li>Secrets start with <code>GEMINI_API_KEY</code>, <code>CEREBRAS_API_KEY</code>, <code>OPENAI_API_KEY</code>, and <code>ANTHROPIC_API_KEY</code>.</li>
              </ul>
              <h2>Tags</h2>
              <ul>
                <li>Put tags on the key, after <code>#</code>: <code>yahooUsername#username#yahoo#email</code>.</li>
                <li><code>{yahooUsername}</code> is that one value. <code>{yahooUsername#username#yahoo#email}</code> is the same value.</li>
                <li><code>{username}</code> is every value whose key includes <code>#username</code>. The same works for <code>{email}</code>, <code>{yahoo}</code>, and any other tag.</li>
                <li>Several keys with the same tag are joined with commas: <code>a@x.com,b@y.com</code>. A repeated value is kept once.</li>
              </ul>
              <h2>Order</h2>
              <ul>
                <li>A number at the end of a tag is the order, not part of the name. <code>left#pin1</code> and <code>right#pin2</code> both belong to <code>{pin}</code>, and <code>pin1</code> comes first.</li>
                <li>Numbered tags come before tags with no number. Tags with no number follow the order they were saved.</li>
              </ul>
              <h2>Join with nothing between the pieces</h2>
              <ul>
                <li>Wrap a value in quotes: <code>left#pin1="94"</code> and <code>right#pin2="03"</code>.</li>
                <li><code>{pin}</code> becomes <code>9403</code>. The quotes are removed, and no comma is inserted.</li>
                <li>One quoted value switches that whole tag.</li>
                <li><code>{left}</code> still uses the stored text, including the quote marks. Use the tag, <code>{pin}</code>, for the joined result.</li>
              </ul>
              <h2>In a script</h2>
              <ul>
                <li>Fields that suggest values offer saved keys and tags as you type.</li>
                <li>A locator whose entire value is <code>{tag}</code> becomes one locator per value in that tag, up to 256, instead of one comma-joined string.</li>
                <li>Authenticate username and email lists are variables tagged <code>#username</code> or <code>#email</code>.</li>
                <li>The password list is secrets tagged <code>#password</code> or <code>#pw</code>.</li>
                <li>The 2FA inbox list is variables tagged <code>#email</code>.</li>
                <li>A listener can store its match in a capture variable. Scripts can also read <code>{listener.match.value}</code>, <code>{listener.match.text}</code>, <code>{listener.match.attr}</code>, <code>{listener.match.url}</code>, and <code>{listener.match.page_url}</code>.</li>
              </ul>
            </div>
          </div>
        </div>
        <div class="data-sub">
          Variables and secrets used by scripts. Hover or click <strong>?</strong> for keys, tags, and <code>{Token}</code> expansion.
        </div>
      </div>

      <div class="data-grid">
        <div class="panel data-panel">
          <div class="panel-header">Variables</div>
          <div class="data-subtle">Import/export format: INI (<code>key=value</code>), tags in key names.</div>
          <div class="data-table" id="data-vars"></div>
          <div class="data-actions">
            <button class="btn" id="data-add-var">Add variable</button>
            <button class="btn" id="data-import-vars">Import .ini</button>
            <input id="data-import-vars-file" type="file" accept=".ini,text/plain" style="display:none"/>
            <button class="btn btn-primary" id="data-save-vars">Save</button>
            <span class="data-status" id="data-vars-status"></span>
          </div>
        </div>

        <div class="panel data-panel">
          <div class="panel-header">Secrets</div>
          <div class="data-subtle">Secret values are stored locally and never shown here. Provider keys like <code>GEMINI_API_KEY</code> and <code>CEREBRAS_API_KEY</code> are pre-populated. Import supports INI too.</div>
          <div class="data-table" id="data-secrets"></div>
          <div class="data-actions">
            <button class="btn" id="data-add-secret">Add secret</button>
            <button class="btn" id="data-import-secrets">Import .ini</button>
            <input id="data-import-secrets-file" type="file" accept=".ini,text/plain" style="display:none"/>
            <button class="btn btn-primary" id="data-save-secrets">Save</button>
            <span class="data-status" id="data-secrets-status"></span>
          </div>
        </div>
      </div>
    </div>
  `;

  const helpButton = container.querySelector('.data-help-button');
  const helpRoot = container.querySelector('.data-help');
  helpButton?.addEventListener('click', (event) => {
    event.preventDefault();
    event.stopPropagation();
    const open = helpRoot?.classList.toggle('is-open');
    helpButton.setAttribute('aria-expanded', open ? 'true' : 'false');
  });

  const varsEl = container.querySelector('#data-vars');
  const secretsEl = container.querySelector('#data-secrets');
  const varsStatusEl = container.querySelector('#data-vars-status');
  const secretsStatusEl = container.querySelector('#data-secrets-status');
  const varsImportBtn = container.querySelector('#data-import-vars');
  const varsImportFile = container.querySelector('#data-import-vars-file');
  const secretsImportBtn = container.querySelector('#data-import-secrets');
  const secretsImportFile = container.querySelector('#data-import-secrets-file');

  if (!dashboardHandler) {
    varsEl.textContent = 'Dashboard backend unavailable.';
    secretsEl.textContent = 'Dashboard backend unavailable.';
    return;
  }

  const [varsRes, secretsRes] = await Promise.all([
    dashboardHandler.getDataVariables?.() ?? {variables: []},
    dashboardHandler.getDataSecrets?.() ?? {secrets: []},
  ]);

  const vars = Array.isArray(varsRes?.variables) ? varsRes.variables : [];
  const secrets = mergeRecommendedSecrets(
      Array.isArray(secretsRes?.secrets) ? secretsRes.secrets : []);

  let refreshTimer = null;
  const publishSmartInputCatalogNow = () => {
    if (refreshTimer) {
      clearTimeout(refreshTimer);
      refreshTimer = null;
    }
    const varsDict = rowsToDict(varsEl);
    const varEntries = Object.entries(varsDict).map(([key, value]) => ({key, value}));
    const secretEntries = collectSecretEntries(secretsEl);
    const catalog = publishSmartInputCatalog(varEntries, secretEntries, appState);
    if (typeof opts?.onSmartInputDataChanged === 'function') {
      try { opts.onSmartInputDataChanged(catalog); } catch (_) {}
    }
  };
  const refreshSmartInputCatalog = () => {
    if (refreshTimer) clearTimeout(refreshTimer);
    refreshTimer = setTimeout(() => {
      refreshTimer = null;
      publishSmartInputCatalogNow();
    }, 120);
  };

  renderRows(varsEl, vars, 'vars');
  renderRows(secretsEl, secrets, 'secrets');
  wireRemoveButtons(varsEl, publishSmartInputCatalogNow);
  wireRemoveButtons(secretsEl, publishSmartInputCatalogNow);
  publishSmartInputCatalogNow();

  container.querySelector('#data-add-var')?.addEventListener('click', (e) => {
    e.preventDefault();
    addEmptyRow(varsEl, 'vars', publishSmartInputCatalogNow);
  });

  container.querySelector('#data-add-secret')?.addEventListener('click', (e) => {
    e.preventDefault();
    addEmptyRow(secretsEl, 'secrets', publishSmartInputCatalogNow);
  });

  varsEl.addEventListener('input', refreshSmartInputCatalog);
  secretsEl.addEventListener('input', refreshSmartInputCatalog);

  varsImportBtn?.addEventListener('click', () => varsImportFile?.click());
  varsImportFile?.addEventListener('change', async () => {
    const file = varsImportFile.files?.[0];
    varsStatusEl.textContent = '';
    if (!file) return;
    try {
      const text = await readFileText(file);
      const dict = parseIniKeyValue(text);
      setRowsFromDict(varsEl, 'vars', dict);
      wireRemoveButtons(varsEl, publishSmartInputCatalogNow);
      publishSmartInputCatalogNow();
      varsStatusEl.textContent = `Imported ${Object.keys(dict).length} variable(s). Save to persist.`;
    } catch (_) {
      varsStatusEl.textContent = 'Import failed.';
    } finally {
      varsImportFile.value = '';
    }
  });

  secretsImportBtn?.addEventListener('click', () => secretsImportFile?.click());
  secretsImportFile?.addEventListener('change', async () => {
    const file = secretsImportFile.files?.[0];
    secretsStatusEl.textContent = '';
    if (!file) return;
    try {
      const text = await readFileText(file);
      const dict = parseIniKeyValue(text);
      setRowsFromDict(secretsEl, 'secrets', dict, true);
      wireRemoveButtons(secretsEl, publishSmartInputCatalogNow);
      publishSmartInputCatalogNow();
      secretsStatusEl.textContent = `Imported ${Object.keys(dict).length} secret key(s). Save to persist.`;
    } catch (_) {
      secretsStatusEl.textContent = 'Import failed.';
    } finally {
      secretsImportFile.value = '';
    }
  });

  container.querySelector('#data-save-vars')?.addEventListener('click', async (e) => {
    e.preventDefault();
    varsStatusEl.textContent = '';
    const dict = rowsToDict(varsEl);
    try {
      const res = await dashboardHandler.saveDataVariables?.(dict);
      if (res && res.ok) {
        varsStatusEl.textContent = 'Saved.';
        publishSmartInputCatalogNow();
      } else {
        varsStatusEl.textContent = 'Save failed.';
      }
    } catch (err) {
      varsStatusEl.textContent = 'Save failed.';
    }
  });

  container.querySelector('#data-save-secrets')?.addEventListener('click', async (e) => {
    e.preventDefault();
    secretsStatusEl.textContent = '';
    const payload = secretsUiToPayload(secretsEl);
    try {
      const res = await dashboardHandler.saveDataSecrets?.(payload);
      if (res && res.ok) {
        secretsStatusEl.textContent = 'Saved.';
        // Clear any typed secret values so they aren't left visible.
        for (const inp of secretsEl.querySelectorAll('.data-secret')) {
          inp.value = '';
        }
        publishSmartInputCatalogNow();
      } else {
        secretsStatusEl.textContent = 'Save failed.';
      }
    } catch (err) {
      secretsStatusEl.textContent = 'Save failed.';
    }
  });
}

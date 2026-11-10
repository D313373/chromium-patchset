// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

let _pollTimer = null;
let _insertAction = null;
let _dashboardHandler = null;
let _entries = [];

function $(id) {
  return document.getElementById(id);
}

function shortUrl(url) {
  try {
    const u = new URL(url);
    const path = (u.pathname || '/') + (u.search || '');
    return `${u.host}${path}`.slice(0, 72);
  } catch (_) {
    return String(url || '').slice(0, 72);
  }
}

function renderList() {
  const list = $('traffic-list');
  if (!list) return;
  list.innerHTML = '';
  if (!_entries.length) {
    const empty = document.createElement('div');
    empty.className = 'traffic-empty';
    empty.textContent = 'No captured requests yet. Start recording and browse.';
    list.appendChild(empty);
    return;
  }
  for (const entry of _entries) {
    const row = document.createElement('div');
    row.className = 'traffic-row';
    const status = Number(entry.status || 0);
    const method = String(entry.method || 'GET');
    const meta = document.createElement('div');
    meta.className = 'traffic-row-meta';
    meta.innerHTML =
        `<span class="traffic-method">${method}</span>` +
        `<span class="traffic-status">${status || '…'}</span>`;
    const url = document.createElement('div');
    url.className = 'traffic-url';
    url.title = String(entry.url || '');
    url.textContent = shortUrl(entry.url);
    const btn = document.createElement('button');
    btn.className = 'btn btn-small traffic-add-btn';
    btn.textContent = 'Add as request';
    btn.addEventListener('click', async () => {
      btn.disabled = true;
      try {
        await promoteEntry(String(entry.requestId || ''));
      } finally {
        btn.disabled = false;
      }
    });
    row.appendChild(meta);
    row.appendChild(url);
    row.appendChild(btn);
    list.appendChild(row);
  }
}

async function refreshTraffic() {
  if (!_dashboardHandler?.getTrafficEntries) return;
  try {
    const res = await _dashboardHandler.getTrafficEntries(80);
    _entries = Array.isArray(res?.entries) ? res.entries : [];
    renderList();
  } catch (err) {
    console.warn('[TrafficPanel] refresh failed', err);
  }
}

async function promoteEntry(requestId) {
  if (!requestId || !_dashboardHandler?.promoteTrafficEntry) return;
  const res = await _dashboardHandler.promoteTrafficEntry(requestId);
  if (!res?.ok || !res.action) {
    alert(res?.error || 'Failed to promote request');
    return;
  }
  if (typeof _insertAction === 'function') {
    _insertAction(res.action);
  }
}

export function installTrafficPanel({dashboardHandler, insertAction} = {}) {
  _dashboardHandler = dashboardHandler || null;
  _insertAction = insertAction || null;
  const refreshBtn = $('traffic-refresh-btn');
  refreshBtn?.addEventListener('click', () => refreshTraffic());
  if (_pollTimer) {
    clearInterval(_pollTimer);
  }
  _pollTimer = setInterval(() => {
    const panel = $('traffic-panel');
    if (!panel || panel.hidden) return;
    refreshTraffic();
  }, 2500);
  refreshTraffic();
}

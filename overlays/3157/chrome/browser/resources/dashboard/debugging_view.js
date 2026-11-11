let _lastContainer = null;
let _resizeObserver = null;
let _refreshTimer = null;
let _replayTimer = null;
const DEBUG_VIEW_INTERACTION_QUIET_MS = 20000;
const ENABLE_EVENT_STREAM = false;
const RUN_HISTORY_FETCH_LIMIT = 1200;
const LISTENER_EVENTS_FETCH_LIMIT = 1200;
const TIMELINE_LISTENER_ACTIONS = new Set(['trigger', 'error', 'refresh']);
const TIMELINE_EVENT_BUCKET_PX = 6;
const TIMELINE_MAX_MARKERS = 360;
const TIMELINE_MIN_SPAN_24H_MS = 5 * 60 * 1000;
const TIMELINE_MIN_SPAN_7D_MS = 30 * 60 * 1000;
let _state = {
  selectedRunId: '',
  selectedContextItem: null,
  selectedScriptId: '',
  runDetails: null,
  selectedExecutionStep: -1,
  rangeMode: '24h', // '24h' | '7d'
  eventFilterScript: '__all__',
  pendingEventFocusKey: '',
  replay: {
    open: false,
    runId: '',
    loading: false,
    error: '',
    manifest: null,
    frameIndex: 0,
    playing: false,
  },
  headlessScreenshot: {
    scriptId: '',
    loading: false,
    error: '',
    dataUrl: '',
    path: '',
    capturedAtMs: 0,
  },
  listenerTogglePending: {},
};

function isMapLike(v) {
  return !!v && typeof v === 'object' && typeof v.forEach === 'function' && typeof v.size === 'number';
}

function unwrapMojoValue(v) {
  if (!v) return v;
  // mojo_base.mojom.DictionaryValue / ListValue often appear as {value: ...}.
  if (typeof v === 'object' && v.value !== undefined) return v.value;
  return v;
}

function toPlain(v) {
  v = unwrapMojoValue(v);
  if (!v || typeof v !== 'object') return v;
  if (Array.isArray(v)) return v.map(toPlain);
  if (isMapLike(v)) {
    const obj = {};
    v.forEach((val, key) => { obj[String(key)] = toPlain(val); });
    return obj;
  }
  try {
    return JSON.parse(JSON.stringify(v));
  } catch (_) {
    return v;
  }
}

function asArray(v) {
  v = toPlain(v);
  if (Array.isArray(v)) return v;
  if (!v || typeof v !== 'object') return [];
  const keys = Object.keys(v);
  if (!keys.length) return [];
  if (keys.every((k) => /^\d+$/.test(k))) {
    return keys.sort((a, b) => Number(a) - Number(b)).map((k) => toPlain(v[k]));
  }
  return [];
}

function isListenerScript(scriptId, appState) {
  const id = String(scriptId || '').trim();
  if (!id) return false;
  const kind = String(appState?.scripts?.[id]?.kind || '').toLowerCase();
  return kind === 'listener';
}

function escapeHtml(v) {
  return String(v ?? '').replace(/[&<>"']/g, (ch) => (
    ch === '&' ? '&amp;' :
      ch === '<' ? '&lt;' :
        ch === '>' ? '&gt;' :
          ch === '"' ? '&quot;' : '&#39;'
  ));
}

function msNow() {
  return Date.now();
}

function stopReplayTimer() {
  if (_replayTimer) {
    clearInterval(_replayTimer);
    _replayTimer = null;
  }
}

function markDebugViewInteraction(container) {
  if (!container) return;
  container.__dbgLastInteractionMs = msNow();
}

function isDebugViewInteractionActive(container) {
  if (!container) return false;
  const active = document.activeElement;
  if (active && container.contains(active)) return true;
  const last = Number(container.__dbgLastInteractionMs || 0);
  return last > 0 && (msNow() - last) < DEBUG_VIEW_INTERACTION_QUIET_MS;
}

function captureDebugViewScrollState(container) {
  return {
    windowX: window.scrollX || 0,
    windowY: window.scrollY || 0,
    containerScrollTop: container?.scrollTop || 0,
    containerScrollLeft: container?.scrollLeft || 0,
  };
}

function restoreDebugViewScrollState(container, state) {
  if (!container || !state) return;
  container.scrollLeft = Number(state.containerScrollLeft || 0);
  container.scrollTop = Number(state.containerScrollTop || 0);
  window.scrollTo(Number(state.windowX || 0), Number(state.windowY || 0));
}

function installDebugViewInteractionTracking(container) {
  if (!container || container.__dbgInteractionTrackingInstalled) return;
  const mark = () => markDebugViewInteraction(container);
  container.addEventListener('pointerdown', mark, {capture: true});
  container.addEventListener('wheel', mark, {capture: true});
  container.addEventListener('keydown', mark, {capture: true});
  container.addEventListener('focusin', mark, {capture: true});
  container.addEventListener('mouseenter', mark, {capture: true});
  container.addEventListener('scroll', mark, {capture: true});
  container.__dbgInteractionTrackingInstalled = true;
}

function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(hi, v));
}

function pad2(n) {
  return n < 10 ? `0${n}` : `${n}`;
}

function formatLocal(dt) {
  return `${dt.getFullYear()}-${pad2(dt.getMonth() + 1)}-${pad2(dt.getDate())} ${pad2(dt.getHours())}:${pad2(dt.getMinutes())}`;
}

function formatLocalWithSeconds(dt) {
  return `${dt.getFullYear()}-${pad2(dt.getMonth() + 1)}-${pad2(dt.getDate())} ${pad2(dt.getHours())}:${pad2(dt.getMinutes())}:${pad2(dt.getSeconds())}`;
}

function formatDuration(ms) {
  if (!Number.isFinite(ms) || ms < 0) return '';
  const s = Math.round(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const ss = s % 60;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${ss}s`;
  return `${ss}s`;
}

async function requestHeadlessScreenshot(dashboardHandler, scriptId) {
  const id = String(scriptId || '').trim();
  if (!id) {
    return {success: false, error: 'Missing listener script id.'};
  }
  if (!dashboardHandler ||
      typeof dashboardHandler.requestHeadlessScreenshot !== 'function') {
    return {
      success: false,
      error: 'requestHeadlessScreenshot is unavailable in dashboard handler.',
    };
  }
  try {
    const res = await dashboardHandler.requestHeadlessScreenshot(id);
    return (res && typeof res === 'object') ? res : {};
  } catch (err) {
    return {
      success: false,
      error: String(err && err.message ? err.message : err),
    };
  }
}

function toIsoForHandler(ms) {
  return new Date(ms).toISOString();
}

function parseMs(val) {
  if (typeof val === 'number') return val;
  if (typeof val === 'string') {
    const n = Number(val);
    if (Number.isFinite(n)) return n;
    const t = Date.parse(val);
    if (Number.isFinite(t)) return t;
  }
  return null;
}

function readExecutionStepIndex(stepLike) {
  const idx = Number(stepLike?.stepIndex ?? stepLike?.step_index ?? stepLike?.step ?? -1);
  return Number.isFinite(idx) ? idx : -1;
}

function actionIndexLabelFromStep(stepLike) {
  const explicit = String(stepLike?.action_index ?? stepLike?.actionIndex ?? '').trim();
  if (explicit) return explicit;
  const idx = readExecutionStepIndex(stepLike);
  if (idx >= 0) return String(idx + 1);
  return '?';
}

function lookupActionIndexLabelForFailedStep(failedStep, runDetails) {
  const failed = Number(failedStep);
  const steps = Array.isArray(runDetails?.steps) ? runDetails.steps : [];
  if (!Number.isFinite(failed) || failed < 0 || !steps.length) return '';
  for (const step of steps) {
    if (readExecutionStepIndex(step) !== failed) continue;
    return actionIndexLabelFromStep(step);
  }
  return '';
}

function intervalToMs(schedule) {
  const days = Number(schedule?.interval_days ?? schedule?.intervalDays ?? 0);
  const hours = Number(schedule?.interval_hours ?? schedule?.intervalHours ?? 0);
  const minutes = Number(schedule?.interval_minutes ?? schedule?.intervalMinutes ?? 0);
  return ((days * 24 + hours) * 60 + minutes) * 60 * 1000;
}

function assignLanes(items) {
  // items: {startMs,endMs,...}
  const sorted = [...items].sort((a, b) => a.startMs - b.startMs);
  const laneEnds = [];
  for (const it of sorted) {
    let lane = 0;
    for (; lane < laneEnds.length; lane++) {
      if (laneEnds[lane] <= it.startMs) break;
    }
    if (lane === laneEnds.length) laneEnds.push(0);
    it.lane = lane;
    laneEnds[lane] = it.endMs;
  }
  return { items: sorted, laneCount: laneEnds.length };
}

function assignPointLanes(points, laneCount, minGapPx = 16) {
  const sorted = [...points].sort((a, b) => (a.left - b.left) || (a.ts - b.ts));
  const laneLastX = Array(Math.max(1, laneCount)).fill(-1e9);
  for (const p of sorted) {
    let lane = -1;
    for (let i = 0; i < laneLastX.length; i++) {
      if (p.left - laneLastX[i] >= minGapPx) {
        lane = i;
        break;
      }
    }
    if (lane < 0) {
      // All lanes are occupied near this x; place on the least recently used lane.
      lane = 0;
      for (let i = 1; i < laneLastX.length; i++) {
        if (laneLastX[i] < laneLastX[lane]) lane = i;
      }
    }
    p.lane = lane;
    laneLastX[lane] = p.left;
  }
  return sorted;
}

function statusToColor(status) {
  switch (status) {
    case 'success': return 'dbg-run-success';
    case 'error': return 'dbg-run-error';
    case 'running': return 'dbg-run-running';
    case 'scheduled': return 'dbg-run-scheduled';
    case 'warning': return 'dbg-run-warning';
    default: return 'dbg-run-neutral';
  }
}

function buildRunBar(it, windowStartMs, windowEndMs, pxPerMs) {
  const start = clamp(it.startMs, windowStartMs, windowEndMs);
  const end = clamp(it.endMs, windowStartMs, windowEndMs);
  const left = Math.round((start - windowStartMs) * pxPerMs);
  const width = Math.max(3, Math.round((end - start) * pxPerMs));
  return { left, width };
}

function timelineActionPriority(action) {
  switch (String(action || '').toLowerCase()) {
    case 'error':
    case 'run_error':
      return 4;
    case 'trigger':
      return 3;
    case 'refresh':
      return 2;
    case 'jitter':
    case 'long_pause':
      return 1;
    default:
      return 0;
  }
}

function thinPointsByPixelBucket(points, bucketPx, maxPoints) {
  if (!Array.isArray(points) || !points.length) return [];
  const bucketed = new Map();
  for (const p of points) {
    const action = String(p.action || 'trigger').toLowerCase();
    const bucket = Math.floor((Number(p.left) || 0) / Math.max(1, bucketPx));
    const key = `${bucket}`;
    const existing = bucketed.get(key);
    const priority = timelineActionPriority(action);
    if (!existing) {
      bucketed.set(key, {
        ...p,
        count: 1,
        actions: [action],
        _priority: priority,
      });
      continue;
    }
    existing.count += 1;
    if (!existing.actions.includes(action)) {
      existing.actions.push(action);
    }
    if (priority > existing._priority ||
        (priority === existing._priority &&
         (Number(p.ts) || 0) >= (Number(existing.ts) || 0))) {
      existing.ev = p.ev;
      existing.run = p.run;
      existing.kind = p.kind;
      existing.action = p.action;
      existing.ts = p.ts;
      existing.t = p.t;
      existing.left = p.left;
      existing.selectItem = p.selectItem;
      existing.eventKey = p.eventKey;
      existing._priority = priority;
    }
  }

  for (const point of bucketed.values()) {
    delete point._priority;
  }
  const out = Array.from(bucketed.values()).sort((a, b) => (a.ts - b.ts) || (a.left - b.left));
  if (out.length <= maxPoints) return out;
  const stride = Math.ceil(out.length / maxPoints);
  const sampled = [];
  for (let i = 0; i < out.length; i += stride) sampled.push(out[i]);
  return sampled;
}

function collectActivityTimestamps(data, windowStartMs, windowEndMs, appState) {
  const out = [];
  const pushTs = (value) => {
    const ts = parseMs(value);
    if (!ts) return;
    if (ts < windowStartMs || ts > windowEndMs) return;
    out.push(ts);
  };

  for (const run of (data?.runs || [])) {
    if (isListenerScript(run.script_id || run.scriptId, appState)) continue;
    pushTs(run.started_ms ?? run.startedMs ?? run.started);
    pushTs(run.ended_ms ?? run.endedMs ?? run.ended);
  }

  for (const ev of (data?.listenerEvents || [])) {
    const action = String(ev?.action || 'trigger').toLowerCase();
    if (!TIMELINE_LISTENER_ACTIONS.has(action))
      continue;
    pushTs(ev.ts_ms ?? ev.tsMs ?? ev.ts ?? ev.time_ms ?? ev.timeMs ?? ev.time);
  }

  return out;
}

function computeActivityWindow(rangeMode, windowStartMs, windowEndMs, data, appState) {
  const activity = collectActivityTimestamps(data, windowStartMs, windowEndMs, appState);
  if (!activity.length) {
    return {
      startMs: windowStartMs,
      endMs: windowEndMs,
      zoomed: false,
      activityCount: 0,
    };
  }

  let minTs = activity[0];
  let maxTs = activity[0];
  for (let i = 1; i < activity.length; i++) {
    minTs = Math.min(minTs, activity[i]);
    maxTs = Math.max(maxTs, activity[i]);
  }

  const rawSpan = Math.max(1000, maxTs - minTs);
  const minSpan = (rangeMode === '7d') ? TIMELINE_MIN_SPAN_7D_MS : TIMELINE_MIN_SPAN_24H_MS;
  const pad = Math.max(60 * 1000, Math.round(rawSpan * 0.15));
  let startMs = minTs - pad;
  let endMs = maxTs + pad;

  if ((endMs - startMs) < minSpan) {
    const center = Math.round((minTs + maxTs) / 2);
    const half = Math.round(minSpan / 2);
    startMs = center - half;
    endMs = center + half;
  }

  if (startMs < windowStartMs) {
    const shift = windowStartMs - startMs;
    startMs += shift;
    endMs += shift;
  }
  if (endMs > windowEndMs) {
    const shift = endMs - windowEndMs;
    startMs -= shift;
    endMs -= shift;
  }

  startMs = clamp(startMs, windowStartMs, windowEndMs);
  endMs = clamp(endMs, windowStartMs, windowEndMs);
  if (endMs <= startMs) {
    startMs = windowStartMs;
    endMs = windowEndMs;
  }

  const isZoomed = (startMs - windowStartMs) > 1000 || (windowEndMs - endMs) > 1000;
  return {
    startMs,
    endMs,
    zoomed: isZoomed,
    activityCount: activity.length,
  };
}

function eventKeyForRun(runId) {
  return `run:${String(runId || '')}`;
}

function eventKeyForScheduled(scriptId, ts) {
  return `sched:${String(scriptId || '')}:${Number(ts || 0)}`;
}

function eventKeyForListenerStart(scriptId, startedMs) {
  return `listener_start:${String(scriptId || '')}:${Number(startedMs || 0)}`;
}

function eventKeyForListenerEvent(ev, index) {
  const ts = parseMs(ev?.ts_ms ?? ev?.tsMs ?? ev?.ts ?? ev?.time_ms ?? ev?.timeMs ?? ev?.time);
  const action = String(ev?.action || 'trigger').toLowerCase();
  const listenerId = String(
    ev?.listener_script_id || ev?.listenerScriptId || ev?.listener_id || ev?.listenerId || ''
  );
  return `listener_evt:${listenerId}:${action}:${Number(ts || 0)}:${Number(index || 0)}`;
}

function scriptFilterKey(scriptId, scriptName) {
  const id = String(scriptId || '').trim();
  if (id) return `id:${id}`;
  const name = String(scriptName || '').trim();
  return `name:${name}`;
}

function focusEventRowByKey(eventLogContainer, eventKey, smooth = true) {
  if (!eventLogContainer || !eventKey) return false;
  const list = eventLogContainer.querySelector('.dbg-eventlog-list');
  if (!list) return false;

  let target = null;
  for (const row of list.querySelectorAll('.dbg-event-item[data-event-key]')) {
    if ((row.dataset.eventKey || '') === eventKey) {
      target = row;
      break;
    }
  }
  if (!target) return false;

  for (const row of list.querySelectorAll('.dbg-event-item.dbg-event-item-highlight')) {
    row.classList.remove('dbg-event-item-highlight');
  }
  target.classList.add('dbg-event-item-highlight');
  target.scrollIntoView({ block: 'nearest', behavior: smooth ? 'smooth' : 'auto' });
  window.setTimeout(() => {
    target.classList.remove('dbg-event-item-highlight');
  }, 1600);
  return true;
}

function listenerActionMeta(actionRaw) {
  const action = String(actionRaw || 'trigger').toLowerCase();
  if (action === 'refresh') return { key: 'refresh', label: 'REFRESH', badgeClass: 'dbg-event-badge-refresh' };
  if (action === 'jitter') return { key: 'jitter', label: 'JITTER', badgeClass: 'dbg-event-badge-jitter' };
  if (action === 'long_pause') return { key: 'long_pause', label: 'LONG PAUSE', badgeClass: 'dbg-event-badge-sleep' };
  if (action === 'error') return { key: 'error', label: 'ERROR', badgeClass: 'dbg-event-badge-error' };
  return { key: 'trigger', label: 'TRIGGER', badgeClass: 'dbg-event-badge-trigger' };
}

function runStatusMeta(statusRaw) {
  const status = String(statusRaw || '').toLowerCase();
  if (status === 'success') return { label: 'SUCCESS', badgeClass: 'dbg-event-badge-success' };
  if (status === 'error') return { label: 'ERROR', badgeClass: 'dbg-event-badge-error' };
  if (status === 'running') return { label: 'RUNNING', badgeClass: 'dbg-event-badge-running' };
  if (status === 'scheduled') return { label: 'SCHEDULED', badgeClass: 'dbg-event-badge-scheduled' };
  if (status === 'warning') return { label: 'WARNING', badgeClass: 'dbg-event-badge-warning' };
  return { label: 'RUN', badgeClass: 'dbg-event-badge-neutral' };
}

function buildTimelineEventFeed(data, appState, windowStartMs, windowEndMs) {
  const feed = [];

  for (const r of data.runs || []) {
    if (isListenerScript(r.script_id || r.scriptId, appState)) continue;
    const startedMs = parseMs(r.started_ms ?? r.startedMs ?? r.started);
    const endedMs = parseMs(r.ended_ms ?? r.endedMs ?? r.ended);
    if (!startedMs || !endedMs) continue;
    if (startedMs < windowStartMs || startedMs > windowEndMs) continue;
    const scriptId = r.script_id || '';
    const scriptName = r.script_name || (appState.scripts?.[scriptId]?.name ?? scriptId ?? 'script');
    const status = String(r.status || 'success').toLowerCase();
    const meta = runStatusMeta(status);
    const durationMs = Math.max(0, endedMs - startedMs);
    const durationText = formatDuration(durationMs);
    const totalSteps = Number(r.total_steps ?? 0);
    const failedStep = Number(r.failed_step ?? -1);
    const failedActionIndex = String(r.failed_action_index || '').trim();
    const warningFailures = Number(r.warning_failures ?? 0);
    const detailBits = [];
    if (durationText) detailBits.push(`duration ${durationText}`);
    if (totalSteps > 0) detailBits.push(`${totalSteps} step${totalSteps === 1 ? '' : 's'}`);
    if (failedStep >= 0) detailBits.push(`failed step #${failedStep}`);
    if (failedActionIndex) detailBits.push(`action #${failedActionIndex}`);
    if (warningFailures > 0) detailBits.push(`${warningFailures} warning failure${warningFailures === 1 ? '' : 's'}`);
    const detail = detailBits.join(' • ') || `status ${status}`;
    const runId = r.run_id || '';
    feed.push({
      ts: startedMs,
      eventKey: eventKeyForRun(runId),
      scriptName,
      scriptId,
      scriptKey: scriptFilterKey(scriptId, scriptName),
      badgeText: meta.label,
      badgeClass: meta.badgeClass,
      detail,
      selectItem: {
        kind: 'run',
        id: runId,
        scriptId,
        scriptName,
        status,
        error: r.error || '',
        failedStep: r.failed_step ?? -1,
        failedActionIndex: r.failed_action_index || '',
        startMs: startedMs,
        endMs: Math.max(endedMs, startedMs + 1000),
      },
    });
  }

  for (const s of data.scheduledScripts || []) {
    const schedule = s.schedule;
    if (!schedule?.enabled || s.isPaused) continue;
    if (isListenerScript(s.script_id || s.scriptId, appState)) continue;
    const intervalMs = intervalToMs(schedule);
    const nextRun = Date.parse(schedule.next_run_iso || '');
    if (!Number.isFinite(nextRun)) continue;
    const scriptId = s.script_id || '';
    const scriptName = s.script_name || (appState.scripts?.[scriptId]?.name ?? scriptId ?? 'script');
    let t = nextRun;
    let n = 0;
    const cap = 512;
    while (t <= windowEndMs && n < cap) {
      if (t >= windowStartMs) {
        const eventKey = eventKeyForScheduled(scriptId, t);
        feed.push({
          ts: t,
          eventKey,
          scriptName,
          scriptId,
          scriptKey: scriptFilterKey(scriptId, scriptName),
          badgeText: 'SCHEDULED',
          badgeClass: 'dbg-event-badge-scheduled',
          detail: 'scheduled start window',
          selectItem: {
            kind: 'scheduled',
            id: eventKey,
            scriptId,
            scriptName,
            status: 'scheduled',
            startMs: t,
            endMs: t + (2 * 60 * 1000),
          },
        });
      }
      if (!intervalMs) break;
      t += intervalMs;
      n++;
    }
  }

  (data.listenerEvents || []).forEach((ev, index) => {
    const ts = parseMs(ev.ts_ms ?? ev.tsMs ?? ev.ts ?? ev.time_ms ?? ev.timeMs ?? ev.time);
    if (!ts || ts < windowStartMs || ts > windowEndMs) return;
    const actionMeta = listenerActionMeta(ev.action);
    const listenerId = ev.listener_script_id || ev.listenerScriptId || ev.listener_id || ev.listenerId || '';
    const listenerName = ev.listener_script_name || ev.listenerScriptName || ev.listener_name || ev.listener_script_id || 'Listener';
    const triggerId = ev.trigger_script_id || ev.triggerScriptId || '';
    const triggerName = ev.trigger_script_name || ev.triggerScriptName || ev.trigger_name || '';
    const condition = ev.condition ? `condition: ${String(ev.condition)}` : '';
    const detailMsg = ev.detail ? String(ev.detail).trim() : '';
    const detailParts = [];
    if (actionMeta.key === 'trigger') {
      detailParts.push(triggerName ? `triggered ${triggerName}` : 'trigger condition matched');
    } else {
      detailParts.push(actionMeta.key.replace('_', ' '));
    }
    if (condition) detailParts.push(condition);
    if (detailMsg) detailParts.push(detailMsg);
    const runId = String(ev.run_id || ev.runId || '').trim();
    let selectItem = null;
    if (runId) {
      const runScriptId = triggerId || listenerId;
      const runScriptName = triggerName || listenerName;
      selectItem = {
        kind: 'run',
        id: runId,
        scriptId: runScriptId,
        scriptName: runScriptName,
        status: actionMeta.key === 'error' ? 'error' : 'running',
        error: detailMsg,
        failedStep: -1,
        startMs: ts,
        endMs: ts + 1000,
      };
    } else if (actionMeta.key === 'error') {
      selectItem = {
        kind: 'listener_event',
        id: '',
        scriptId: listenerId,
        scriptName: listenerName,
        status: 'error',
        error: detailMsg || detailParts.join(' • '),
        failedStep: -1,
        startMs: ts,
        endMs: ts + 1000,
      };
    }
    feed.push({
      ts,
      eventKey: eventKeyForListenerEvent(ev, index),
      scriptName: listenerName,
      scriptId: listenerId,
      scriptKey: scriptFilterKey(listenerId, listenerName),
      badgeText: actionMeta.label,
      badgeClass: actionMeta.badgeClass,
      detail: detailParts.join(' • '),
      selectItem,
    });
  });

  feed.sort((a, b) => b.ts - a.ts);
  return feed;
}

function renderEventFeed(container, data, appState, windowStartMs, windowEndMs) {
  const maxRows = 600;
  const feed = buildTimelineEventFeed(data, appState, windowStartMs, windowEndMs);
  const scriptsByKey = new Map();
  for (const ev of feed) {
    const key = String(ev.scriptKey || '').trim();
    if (!key || scriptsByKey.has(key)) continue;
    scriptsByKey.set(key, String(ev.scriptName || 'script'));
  }
  const filterOptions = [{ key: '__all__', name: 'All scripts' }];
  for (const [key, name] of scriptsByKey.entries()) {
    filterOptions.push({ key, name });
  }

  if (_state.eventFilterScript !== '__all__' && !scriptsByKey.has(_state.eventFilterScript)) {
    _state.eventFilterScript = '__all__';
  }
  const activeFilter = _state.eventFilterScript || '__all__';
  const filteredFeed = (activeFilter === '__all__')
    ? feed
    : feed.filter((ev) => ev.scriptKey === activeFilter);
  const shown = filteredFeed.slice(0, maxRows);
  const truncated = filteredFeed.length > shown.length;

  container.innerHTML = `
    <div class="dbg-eventlog-header">
      <div class="dbg-eventlog-title-wrap">
        <div class="dbg-eventlog-title">Event stream</div>
        <div class="dbg-eventlog-meta">${filteredFeed.length} event${filteredFeed.length === 1 ? '' : 's'}</div>
      </div>
      <label class="dbg-eventlog-filter">
        <span>Script</span>
        <select id="dbg-event-filter"></select>
      </label>
    </div>
    <div class="dbg-eventlog-list" id="dbg-eventlog-list"></div>
  `;
  const filterSelect = container.querySelector('#dbg-event-filter');
  if (filterSelect) {
    for (const optInfo of filterOptions) {
      const opt = document.createElement('option');
      opt.value = optInfo.key;
      opt.textContent = optInfo.name;
      if (optInfo.key === activeFilter) opt.selected = true;
      filterSelect.appendChild(opt);
    }
    filterSelect.addEventListener('change', (e) => {
      _state.eventFilterScript = String(e.target.value || '__all__');
      container.dispatchEvent(new CustomEvent('dbg-refresh', { bubbles: true }));
    });
  }

  const list = container.querySelector('#dbg-eventlog-list');
  if (!list) return;

  if (!shown.length) {
    const empty = document.createElement('div');
    empty.className = 'dbg-empty';
    empty.textContent = 'No timeline events in this time window.';
    list.appendChild(empty);
    return;
  }

  if (truncated) {
    const note = document.createElement('div');
    note.className = 'dbg-eventlog-note';
    note.textContent = `Showing latest ${shown.length} events for current filter.`;
    list.appendChild(note);
  }

  for (const ev of shown) {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'dbg-event-item';
    row.dataset.eventKey = String(ev.eventKey || '');
    row.dataset.scriptKey = String(ev.scriptKey || '');
    if (!ev.selectItem) row.classList.add('dbg-event-item-passive');
    if (ev.selectItem) {
      row.title = ev.selectItem.kind === 'run'
        ? 'Click to open this run in Incident panel.'
        : 'Click to open this event in Incident panel.';
      row.addEventListener('click', () => {
        container.dispatchEvent(new CustomEvent('dbg-select', { bubbles: true, detail: { item: ev.selectItem } }));
      });
    }

    const time = document.createElement('div');
    time.className = 'dbg-event-time';
    time.textContent = formatLocalWithSeconds(new Date(ev.ts));

    const main = document.createElement('div');
    main.className = 'dbg-event-main';

    const head = document.createElement('div');
    head.className = 'dbg-event-head';

    const badge = document.createElement('span');
    badge.className = `dbg-event-badge ${ev.badgeClass || 'dbg-event-badge-neutral'}`;
    badge.textContent = ev.badgeText || 'EVENT';

    const script = document.createElement('span');
    script.className = 'dbg-event-script';
    script.textContent = ev.scriptName || 'script';

    const detail = document.createElement('div');
    detail.className = 'dbg-event-detail';
    detail.textContent = ev.detail || '';

    head.appendChild(badge);
    head.appendChild(script);
    main.appendChild(head);
    main.appendChild(detail);
    row.appendChild(time);
    row.appendChild(main);
    list.appendChild(row);
  }

  if (_state.pendingEventFocusKey) {
    const found = focusEventRowByKey(container, _state.pendingEventFocusKey, false);
    if (found) _state.pendingEventFocusKey = '';
  }
}

function replayManifestUrl(runId) {
  // Prefer relative URLs for chrome://dashboard fetch compatibility.
  return `replay/${encodeURIComponent(runId)}/manifest.json`;
}

function replayFrameUrl(runId, file) {
  // Prefer relative URLs for chrome://dashboard resource loads.
  return `replay/${encodeURIComponent(runId)}/${file}`;
}

async function fetchReplayManifest(runId) {
  const res = await fetch(replayManifestUrl(runId), { cache: 'no-store' });
  if (!res.ok) throw new Error(`Replay manifest not found (${res.status})`);
  return await res.json();
}

async function fetchDebugData(dashboardHandler, windowStartMs, windowEndMs) {
  const [dbg, runsRes, schedRes, activeListenersRes, listenerEventsRes, settingsRes] = await Promise.all([
    dashboardHandler.getDebugLogging(),
    dashboardHandler.getRunHistory(
      '',
      toIsoForHandler(windowStartMs),
      toIsoForHandler(windowEndMs),
      RUN_HISTORY_FETCH_LIMIT
    ),
    dashboardHandler.getScheduledScripts(),
    dashboardHandler.getActiveListeners(),
    (typeof dashboardHandler.getListenerEvents === 'function')
      ? dashboardHandler.getListenerEvents(
          toIsoForHandler(windowStartMs),
          toIsoForHandler(windowEndMs),
          LISTENER_EVENTS_FETCH_LIMIT
        )
      : Promise.resolve({events: []}),
    (typeof dashboardHandler.getEffectiveSettings === 'function')
      ? dashboardHandler.getEffectiveSettings('')
      : Promise.resolve({settings: {}}),
  ]);
  const dbgObj = toPlain(dbg?.result ?? dbg);
  const runs = asArray(runsRes?.runs ?? runsRes);
  const scheduledScripts = asArray(schedRes?.scripts ?? schedRes);
  const activeListeners = asArray(activeListenersRes?.listeners ?? activeListenersRes);
  const headlessTabsRaw = asArray(
    activeListenersRes?.headless_tabs ?? activeListenersRes?.headlessTabs
  );
  const listenerEvents = asArray(listenerEventsRes?.events ?? listenerEventsRes);
  const effectiveSettings = toPlain(settingsRes?.settings ?? settingsRes) || {};
  const runListenersInHeadlessMode =
    effectiveSettings?.listenerMode?.runListenersInHeadlessMode !== false;
  const listenerModeLabel = runListenersInHeadlessMode ?
    'Headless listener host' :
    'Background tabs';
  const headlessWarning =
    String(
      activeListenersRes?.headless_warning ||
      activeListenersRes?.headlessWarning ||
      listenerEventsRes?.headless_warning ||
      listenerEventsRes?.headlessWarning ||
      ''
    ).trim();
  if (headlessWarning) {
    console.warn('[Listeners] headless warning:', headlessWarning);
  }
  return {
    debugLoggingEnabled: !!dbgObj?.enabled,
    runs: runs,
    scheduledScripts: scheduledScripts,
    activeListeners: activeListeners,
    headlessTabs: headlessTabsRaw,
    listenerEvents: listenerEvents,
    listenerModeLabel,
    runListenersInHeadlessMode,
    headlessWarning,
  };
}

function mergeActiveListenersIntoData(existingData, activeListenersRes) {
  const prev = (existingData && typeof existingData === 'object') ? existingData : {};
  const activeListeners = asArray(
    activeListenersRes?.listeners ?? activeListenersRes
  );
  const headlessTabsRaw = asArray(
    activeListenersRes?.headless_tabs ?? activeListenersRes?.headlessTabs
  );
  const nextWarning = String(
      activeListenersRes?.headless_warning ||
      activeListenersRes?.headlessWarning ||
      prev.headlessWarning ||
      '').trim();
  return {
    ...prev,
    activeListeners: Array.isArray(activeListeners) ? activeListeners : [],
    headlessTabs: Array.isArray(headlessTabsRaw) ? headlessTabsRaw : [],
    headlessWarning: nextWarning,
  };
}

async function refreshListenersPanelOnly(container, dashboardHandler, appState) {
  if (!container || !dashboardHandler ||
      typeof dashboardHandler.getActiveListeners !== 'function') {
    return;
  }
  const listenersEl = container.querySelector('#dbg-listeners');
  if (!listenersEl) {
    return;
  }
  if (container.__dbgListenersRefreshInFlight) {
    container.__dbgListenersRefreshPending = true;
    return;
  }
  container.__dbgListenersRefreshInFlight = true;
  try {
    const activeListenersRes = await dashboardHandler.getActiveListeners();
    const nextData = mergeActiveListenersIntoData(
        container.__dbgDataSnapshot, activeListenersRes);
    container.__dbgDataSnapshot = nextData;
    renderListenersPanel(
        listenersEl, appState, nextData, dashboardHandler,
        _state.headlessScreenshot);
  } catch (err) {
    console.error('Listeners-only refresh failed', err);
  } finally {
    container.__dbgListenersRefreshInFlight = false;
    if (container.__dbgListenersRefreshPending) {
      container.__dbgListenersRefreshPending = false;
      void refreshListenersPanelOnly(container, dashboardHandler, appState);
    }
  }
}

function renderRuler(container, windowStartMs, windowEndMs, rangeMode, data = null, zoomInfo = null) {
  const start = new Date(windowStartMs);
  const end = new Date(windowEndMs);
  const durationMs = windowEndMs - windowStartMs;
  const hours = Math.max(1, Math.round(durationMs / (3600 * 1000)));
  const isZoomed = !!(zoomInfo && zoomInfo.zoomed);
  const title = isZoomed
    ? `Run Timeline (activity window, ${hours}h span)`
    : ((rangeMode === '7d')
      ? 'Run Timeline (last 6d + next 24h)'
      : `Run Timeline (last ${hours}h)`);
  const runCount = Array.isArray(data?.runs) ? data.runs.length : 0;
  const listenerCount = Array.isArray(data?.activeListeners) ? data.activeListeners.length : 0;
  const eventCount = Array.isArray(data?.listenerEvents) ? data.listenerEvents.length : 0;
  const activityCount = Number(zoomInfo?.activityCount || 0);
  const buildHourTicks = () => {
    if (rangeMode !== '24h' || durationMs <= 0) {
      return '';
    }
    const hourMs = 60 * 60 * 1000;
    let tickMs = Math.ceil(windowStartMs / hourMs) * hourMs;
    const ticks = [];
    while (tickMs < windowEndMs) {
      const ratio = (tickMs - windowStartMs) / durationMs;
      const pct = Math.max(0, Math.min(100, ratio * 100));
      ticks.push(
          `<span class="dbg-ruler-hour-tick" style="left:${pct.toFixed(3)}%"></span>`);
      tickMs += hourMs;
    }
    if (!ticks.length) {
      return '';
    }
    return `<div class="dbg-ruler-hour-ticks" aria-hidden="true">${ticks.join('')}</div>`;
  };
  container.innerHTML = `
    <div class="dbg-ruler-top">
      <div class="dbg-ruler-title-wrap">
        <div class="dbg-ruler-title">${title}</div>
        <div class="dbg-ruler-meta">
          <span class="dbg-ruler-chip" title="Run records currently in the selected time window.">${runCount} runs</span>
          <span class="dbg-ruler-chip" title="Listener scripts currently active.">${listenerCount} active listeners</span>
          <span class="dbg-ruler-chip" title="Listener events in the selected time window.">${eventCount} listener events</span>
          ${isZoomed ? `<span class="dbg-ruler-chip" title="Timeline is automatically zoomed to the active event window for readability.">activity zoom (${activityCount} points)</span>` : ''}
        </div>
      </div>
      <div style="display:flex; gap:8px; align-items:center;">
        <button class="btn btn-secondary" id="dbg-range-toggle" title="Toggle range">${rangeMode === '7d' ? '24h' : '7d'}</button>
        <div class="dbg-ruler-range">${formatLocal(start)} → ${formatLocal(end)}</div>
      </div>
    </div>
    <div class="dbg-ruler-bar-wrap">
      <div class="dbg-ruler-bar"></div>
      ${buildHourTicks()}
    </div>
    <div class="dbg-ruler-legend" aria-label="Timeline legend">
      <span class="dbg-legend-group">Runs</span>
      <span class="dbg-legend-item" title="Script run completed successfully.">
        <span class="dbg-legend-run dbg-legend-run-success">✓</span>
        <span>Success</span>
      </span>
      <span class="dbg-legend-item" title="Script run failed with an error.">
        <span class="dbg-legend-run dbg-legend-run-error">⛔</span>
        <span>Error</span>
      </span>
      <span class="dbg-legend-item" title="Script run is currently executing.">
        <span class="dbg-legend-run dbg-legend-run-running">⟳</span>
        <span>Running</span>
      </span>
      <span class="dbg-legend-item" title="Script run is scheduled to start soon.">
        <span class="dbg-legend-run dbg-legend-run-scheduled">⏱</span>
        <span>Scheduled</span>
      </span>
      <span class="dbg-legend-item" title="Run finished with warning-level failures.">
        <span class="dbg-legend-run dbg-legend-run-warning">⚠</span>
        <span>Warning</span>
      </span>

      <span class="dbg-legend-divider"></span>
      <span class="dbg-legend-group">Events</span>
      <span class="dbg-legend-item" title="Listener matched and triggered a downstream script.">
        <span class="dbg-legend-trigger dbg-legend-trigger-trigger"></span>
        <span>Trigger</span>
      </span>
      <span class="dbg-legend-item" title="Listener refresh polling event.">
        <span class="dbg-legend-trigger dbg-legend-trigger-refresh"></span>
        <span>Refresh</span>
      </span>
      <span class="dbg-legend-item" title="Listener error event.">
        <span class="dbg-legend-trigger dbg-legend-trigger-error"></span>
        <span>Error</span>
      </span>
      <span class="dbg-legend-item" title="Markers are bucketed onto a single event line and overlapping events are coalesced.">
        <span>Capped at ${TIMELINE_MAX_MARKERS} markers</span>
      </span>
    </div>
  `;
}

function renderTimeline(container, data, appState, windowStartMs, windowEndMs) {
  const width = container.clientWidth || 1;
  const durationMs = windowEndMs - windowStartMs;
  const pxPerMs = width / durationMs;

  const runItems = [];
  for (const r of data.runs) {
    if (isListenerScript(r.script_id, appState)) continue;
    const startedMs = parseMs(r.started_ms ?? r.startedMs ?? r.started);
    const endedMs = parseMs(r.ended_ms ?? r.endedMs ?? r.ended);
    if (!Number.isFinite(startedMs)) continue;
    runItems.push({
      kind: 'run',
      id: r.run_id,
      scriptId: r.script_id,
      scriptName: r.script_name || (appState.scripts?.[r.script_id]?.name ?? ''),
      status: r.status || 'success',
      error: r.error || '',
      failedStep: r.failed_step ?? -1,
      failedActionIndex: r.failed_action_index || '',
      warningFailures: r.warning_failures ?? 0,
      totalSteps: r.total_steps ?? 0,
      startMs: startedMs,
      endMs: Math.max(Number.isFinite(endedMs) ? endedMs : Date.now(), startedMs + 1000),
    });
  }

  const scheduleItems = [];
  for (const s of data.scheduledScripts) {
    const schedule = s.schedule;
    if (!schedule?.enabled || s.isPaused) continue;
    if (isListenerScript(s.script_id, appState)) continue;
    const intervalMs = intervalToMs(schedule);
    const nextRun = Date.parse(schedule.next_run_iso || '');
    if (!Number.isFinite(nextRun)) continue;
    // Generate occurrences across the window.
    let t = nextRun;
    const cap = 512;
    let n = 0;
    while (t <= windowEndMs && n < cap) {
      if (t >= windowStartMs) {
        scheduleItems.push({
          kind: 'scheduled',
          id: `sched:${s.script_id}:${t}`,
          scriptId: s.script_id,
          scriptName: s.script_name || (appState.scripts?.[s.script_id]?.name ?? ''),
          status: 'scheduled',
          startMs: t,
          endMs: t + 2 * 60 * 1000, // 2-minute marker
        });
      }
      if (!intervalMs) break;
      t += intervalMs;
      n++;
    }
  }

  const allItems = [...runItems, ...scheduleItems];
  const { items, laneCount } = assignLanes(allItems);
  const displayLaneCount = Math.max(5, laneCount);

  const triggerLaneCount = 1;
  const lanesHtml = [];
  lanesHtml.push(`
    <div class="dbg-lane dbg-lane-triggers" data-lane="triggers">
      <div class="dbg-trigger-band-label">Event flags</div>
      <div class="dbg-trigger-grid" aria-hidden="true">
        ${Array.from({ length: triggerLaneCount }).map((_, i) => `<div class="dbg-trigger-row" data-trigger-row="${i}"></div>`).join('')}
      </div>
    </div>
  `);
  for (let lane = 0; lane < displayLaneCount; lane++) {
    lanesHtml.push(`<div class="dbg-lane" data-lane="${lane}"></div>`);
  }
  container.innerHTML = lanesHtml.join('');

  // Listener trigger lane (markers).
  const trigLane = container.querySelector('.dbg-lane-triggers');
  if (trigLane && Array.isArray(data.listenerEvents)) {
    const triggerPoints = [];
    const runById = new Map();
    for (const run of runItems) {
      if (run.id) runById.set(String(run.id), run);
    }
    for (const run of runItems) {
      if (String(run.status || '').toLowerCase() !== 'error')
        continue;
      const ts = parseMs(run.startMs);
      if (!ts) continue;
      if (ts < windowStartMs || ts > windowEndMs) continue;
      const t = clamp(ts, windowStartMs, windowEndMs);
      triggerPoints.push({
        kind: 'run_error',
        action: 'run_error',
        ts,
        t,
        left: Math.round((t - windowStartMs) * pxPerMs),
        run: run,
        selectItem: run,
        eventKey: eventKeyForRun(run.id || ''),
      });
    }
    (data.listenerEvents || []).forEach((ev, index) => {
      const ts = parseMs(ev.ts_ms ?? ev.tsMs ?? ev.ts ?? ev.time_ms ?? ev.timeMs ?? ev.time);
      if (!ts) return;
      const action = String(ev.action || 'trigger').toLowerCase();
      if (!TIMELINE_LISTENER_ACTIONS.has(action))
        return;
      const t = clamp(ts, windowStartMs, windowEndMs);
      const left = Math.round((t - windowStartMs) * pxPerMs);
      const runId = String(ev.run_id || ev.runId || '').trim();
      const listenerId = ev.listener_script_id || ev.listenerScriptId || ev.listener_id || ev.listenerId || '';
      const listenerName =
        ev.listener_script_name || ev.listenerScriptName || ev.listener_name || listenerId || 'Listener';
      const triggerId = ev.trigger_script_id || ev.triggerScriptId || listenerId;
      const triggerName =
        ev.trigger_script_name || ev.triggerScriptName || ev.trigger_name || listenerName || triggerId;
      const detailMsg = String(ev.detail || '').trim();
      if (action === 'error' && runId && runById.has(runId)) {
        return;
      }
      let selectItem = null;
      if (runId) {
        selectItem = runById.get(runId) || {
          kind: 'run',
          id: runId,
          scriptId: triggerId,
          scriptName: triggerName,
          status: action === 'error' ? 'error' : 'running',
          error: detailMsg,
          failedStep: -1,
          startMs: ts,
          endMs: ts + 1000,
        };
      } else {
        selectItem = {
          kind: 'listener_event',
          id: '',
          scriptId: listenerId,
          scriptName: listenerName,
          status: action === 'error' ? 'error' : 'listening',
          error: detailMsg,
          failedStep: -1,
          startMs: ts,
          endMs: ts + 1000,
        };
      }
      triggerPoints.push({
        kind: 'listener_event',
        ev,
        ts,
        t,
        left,
        action,
        run: runId ? (runById.get(runId) || null) : null,
        selectItem,
        eventKey: eventKeyForListenerEvent(ev, index),
      });
    });
    const thinnedPoints = thinPointsByPixelBucket(
      triggerPoints,
      TIMELINE_EVENT_BUCKET_PX,
      TIMELINE_MAX_MARKERS
    );
    const distributed = assignPointLanes(thinnedPoints, triggerLaneCount, 14);
    for (const point of distributed) {
      const ts = point.ts;
      const left = point.left;
      const action = String(point.action || 'trigger').toLowerCase();
      const el = document.createElement('div');
      let actionClass = 'dbg-trigger-trigger';
      if (action === 'error' || action === 'run_error') {
        actionClass = 'dbg-trigger-error';
      } else if (action === 'refresh') {
        actionClass = 'dbg-trigger-refresh';
      } else if (action === 'jitter') {
        actionClass = 'dbg-trigger-jitter';
      } else if (action === 'long_pause') {
        actionClass = 'dbg-trigger-sleep';
      }
      el.className = `dbg-trigger ${actionClass}`;
      el.style.left = `${left}px`;
      el.style.top = '32px';
      if (action === 'refresh') {
        el.classList.add('dbg-trigger-tag');
        el.textContent = 'R';
      }
      if (point.kind === 'run_error' && point.run) {
        const run = point.run;
        const scriptName = run.scriptName || run.scriptId || 'Script';
        const errorText = String(run.error || '').trim();
        el.title = `[run error] ${scriptName}\n${formatLocal(new Date(ts))}${errorText ? `\n${errorText}` : ''}`;
      } else {
        const ev = point.ev || {};
        const listenerName = ev.listener_script_name || ev.listenerScriptName || ev.listener_name || 'Listener';
        const triggerName = ev.trigger_script_name || ev.triggerScriptName || ev.trigger_name || action;
        const cond = ev.condition ? ` (${ev.condition})` : '';
        const detail = ev.detail ? `\n${String(ev.detail)}` : '';
        el.title = `[${action}] ${listenerName} → ${triggerName}${cond}${detail}\n${formatLocal(new Date(ts))}`;
      }
      const coalesced = point.count > 1 ? `\n+${point.count - 1} similar events in this time bucket` : '';
      if (coalesced) el.title += coalesced;
      el.classList.add('dbg-event-focusable');
      el.dataset.eventKey = point.eventKey || '';
      el.addEventListener('click', () => {
        if (point.selectItem) {
          container.dispatchEvent(new CustomEvent('dbg-select', {
            bubbles: true,
            detail: { item: point.selectItem },
          }));
        }
        if (ENABLE_EVENT_STREAM) {
          container.dispatchEvent(new CustomEvent('dbg-focus-event', {
            bubbles: true,
            detail: { eventKey: point.eventKey || '' },
          }));
        }
      });
      trigLane.appendChild(el);
    }
  }

  for (const it of items) {
    const laneEl = container.querySelector(`.dbg-lane[data-lane="${it.lane}"]`);
    if (!laneEl) continue;
    const { left, width } = buildRunBar(it, windowStartMs, windowEndMs, pxPerMs);
    const cls = statusToColor(it.status);
    const title = it.kind === 'scheduled'
      ? `${it.scriptName || it.scriptId} (scheduled)\n${formatLocal(new Date(it.startMs))}`
      : `${it.scriptName || it.scriptId}\n${it.status}\n${formatLocal(new Date(it.startMs))} → ${formatLocal(new Date(it.endMs))}`;
    const label = it.scriptName || it.scriptId || '';
    const badge = it.status === 'error' ? '⛔' :
      it.status === 'success' ? '✓' :
        it.status === 'scheduled' ? '⏱' :
          it.status === 'running' ? '⟳' : '•';
    const el = document.createElement('button');
    el.type = 'button';
    el.className = `dbg-run ${cls}`;
    el.style.left = `${left}px`;
    el.style.width = `${width}px`;
    el.title = title;
    el.dataset.runId = it.id || '';
    el.dataset.scriptId = it.scriptId || '';
    el.dataset.kind = it.kind;
    el.dataset.eventKey = (it.kind === 'run')
      ? eventKeyForRun(it.id || '')
      : eventKeyForScheduled(it.scriptId || '', it.startMs || 0);
    if (it.status) el.dataset.status = it.status;
    if (it.error) el.dataset.error = String(it.error);
    el.innerHTML = `<span class="dbg-run-badge">${badge}</span><span class="dbg-run-label">${label}</span>`;
    el.addEventListener('click', () => {
      _state.selectedRunId = it.kind === 'run' ? (it.id || '') : '';
      _state.selectedContextItem = it.kind === 'run' ? null : it;
      _state.selectedScriptId = it.scriptId || '';
      _state.runDetails = null;
      if (ENABLE_EVENT_STREAM) {
        _state.pendingEventFocusKey = el.dataset.eventKey || '';
        container.dispatchEvent(new CustomEvent('dbg-focus-event', {
          bubbles: true,
          detail: { eventKey: el.dataset.eventKey || '' },
        }));
      }
      container.dispatchEvent(new CustomEvent('dbg-select', { bubbles: true, detail: { item: it } }));
    });
    laneEl.appendChild(el);
  }
}

function renderListenersPanel(container, appState, data, dashboardHandler, screenshotState = null) {
  const scripts = appState.scripts || {};
  const listenerScripts = Object.entries(scripts)
    .filter(([, s]) => (s.kind || 'main') === 'listener')
    .map(([id, s]) => ({ id, name: s.name || id }));
  const listenerNameById = new Map(listenerScripts.map((s) => [s.id, s.name]));

  const activeById = new Map();
  for (const l of (data.activeListeners || [])) {
    const id = l.scriptId || l.script_id;
    if (id) activeById.set(id, l);
  }

  const siteNameFromUrl = (rawUrl) => {
    const url = String(rawUrl || '').trim();
    if (!url) return 'about:blank';
    try {
      const u = new URL(url);
      return u.hostname || u.origin || url;
    } catch (_) {
      return url;
    }
  };

  // Avoid `chrome://favicon2` requests from dashboard. They are blocked by this
  // WebUI's CSP (`img-src 'self' data:`), generate noisy console errors, and
  // add unnecessary work on every listeners refresh.
  const faviconForUrl = (_rawUrl) => '';

  const headlessTabs = [];
  const providedHeadlessTabs = Array.isArray(data?.headlessTabs) ? data.headlessTabs : [];
  if (providedHeadlessTabs.length) {
    for (const rawTab of providedHeadlessTabs) {
      const tab = rawTab && typeof rawTab === 'object' ? rawTab : {};
      const ownerId = String(tab.owner_id || tab.ownerId || '').trim();
      const label = String(tab.label || tab.name || ownerId || 'Headless tab').trim() || 'Headless tab';
      const currentUrl = String(tab.current_url || tab.currentUrl || '').trim();
      const site = String(tab.site_name || tab.siteName || siteNameFromUrl(currentUrl)).trim() ||
        siteNameFromUrl(currentUrl);
      const reason = String(tab.open_reason || tab.openReason || tab.reason || 'listener').trim() || 'listener';
      const closeAction = String(tab.close_action || tab.closeAction || '').trim();
      const closeId = String(tab.close_id || tab.closeId || ownerId).trim();
      const closable =
        (tab.closable !== false) &&
        !!closeAction &&
        closeAction !== 'none' &&
        !!closeId;
      const faviconUrl = faviconForUrl(currentUrl);
      const tooltip =
        `Open reason: ${reason}\n` +
        `Site: ${site}\n` +
        `Target: ${label}${ownerId ? ` (${ownerId})` : ''}`;
      headlessTabs.push({
        id: ownerId || closeId,
        label,
        site,
        reason,
        tooltip,
        faviconUrl,
        closable,
        closeAction,
        closeId,
      });
    }
  } else {
    for (const l of (data.activeListeners || [])) {
      const listenerId = String(l.scriptId || l.script_id || '').trim();
      if (!listenerId) continue;
      const runningKind = String(l.running_kind || l.runningKind || '').trim();
      if (runningKind !== 'headless') continue;
      const currentUrl = String(l.current_url || l.currentUrl || '').trim();
      const site = siteNameFromUrl(currentUrl);
      const reason = String(l.open_reason || l.openReason || 'listener');
      const scriptName = listenerNameById.get(listenerId) || listenerId;
      const faviconUrl = faviconForUrl(currentUrl);
      const tooltip =
        `Open reason: ${reason}\n` +
        `Site: ${site}\n` +
        `Script: ${scriptName} (${listenerId})`;
      headlessTabs.push({
        id: listenerId,
        label: scriptName,
        site,
        reason,
        tooltip,
        faviconUrl,
        closable: true,
        closeAction: 'stop_listener',
        closeId: listenerId,
      });
    }
  }

  const headlessTabsHtml = headlessTabs.map((tab) => {
    const label = tab.label.length > 22 ? `${tab.label.slice(0, 19)}...` : tab.label;
    const tooltipText = String(tab.tooltip || '').replace(/\s*\n+\s*/g, ' • ');
    const fallbackGlyph = String(tab.site || tab.label || 'o').trim().charAt(0) || 'o';
    const favicon = tab.faviconUrl
      ? `<img class="dbg-headless-tab-favicon" src="${escapeHtml(tab.faviconUrl)}" alt="" />`
      : `<span class="dbg-headless-tab-favicon-fallback">${escapeHtml(fallbackGlyph.toLowerCase())}</span>`;
    const refreshBtn = tab.closable ? `
      <button
        class="dbg-headless-tab-refresh"
        type="button"
        title="Reload page and re-check listener against previous match baseline"
        aria-label="Refresh headless tab and listener for ${escapeHtml(tab.label)}"
        data-headless-refresh-id="${escapeHtml(tab.closeId || tab.ownerId || tab.id || '')}"
        data-headless-refresh-kind="${escapeHtml(tab.ownerKind || tab.owner_kind || tab.closeAction || '')}">↻</button>
    ` : '';
    const closeBtn = tab.closable ? `
      <button
        class="dbg-headless-tab-close"
        type="button"
        title="Close tab"
        aria-label="Close headless tab for ${escapeHtml(tab.label)}"
        data-headless-close-id="${escapeHtml(tab.closeId || tab.id || '')}"
        data-headless-close-action="${escapeHtml(tab.closeAction || '')}">x</button>
    ` : '';
    return `
      <div class="dbg-headless-tab"
           title="${escapeHtml(tab.tooltip)}"
           data-tooltip="${escapeHtml(tooltipText)}">
        ${favicon}
        <span class="dbg-headless-tab-label">${escapeHtml(label)}</span>
        ${refreshBtn}
        ${closeBtn}
      </div>
    `;
  }).join('');

  const rows = listenerScripts.map((s) => {
    const activeInfo = activeById.get(s.id) || null;
    const pending = _state.listenerTogglePending?.[s.id] || null;
    const active = pending ? !!pending.desiredEnabled : !!activeInfo;
    const status = pending ? 'starting' : (active ? 'active' : 'off');
    const phaseRaw = String(activeInfo?.status || '').trim().toLowerCase();
    const phase = pending ? 'starting' : (phaseRaw || 'off');
    const phaseClass = `dbg-phase-${phase.replace(/[^a-z0-9_-]/g, '-')}`;
    let statusText = pending
      ? (pending.desiredEnabled ? 'starting...' : 'stopping...')
      : (active ? (activeInfo.status || 'active') : 'disabled');
    const runningKind = activeInfo?.running_kind || activeInfo?.runningKind || '';
    const modeKind = String(
      activeInfo?.mode_kind || activeInfo?.modeKind || activeInfo?.mode || ''
    ).trim();
    const configuredModeLabel = String(data?.listenerModeLabel || '').trim() ||
      'Headless listener host';
    const modeText = active ?
      ((modeKind === 'headless') ? 'Headless listener host' : 'Background tab listener') :
      `${configuredModeLabel} (default)`;
    const execStatus = activeInfo?.execution_status || activeInfo?.executionStatus || null;
    const step = (execStatus && typeof execStatus.current_step === 'number') ? execStatus.current_step : null;
    if (!pending) {
      if (runningKind === 'key_script' && step != null) statusText = `key script step #${step}`;
      if (runningKind === 'trigger' && step != null) statusText = `trigger step #${step}`;
      if (modeKind === 'headless' && (activeInfo?.status || '') === 'listening') {
        statusText = 'headless listening';
      }
      if ((activeInfo?.status || '') === 'error') statusText = `error: ${activeInfo?.status_detail || activeInfo?.last_error || ''}`.trim();
      if ((activeInfo?.status || '') === 'listening') {
        const lastErr = String(activeInfo?.last_error || activeInfo?.lastError || '').trim();
        statusText = lastErr ? `listening (last: ${lastErr.slice(0, 80)})` : 'listening';
        if (modeKind === 'headless') {
          statusText = lastErr ? `headless listening (last: ${lastErr.slice(0, 80)})` : 'headless listening';
        }
      }
    }
    const currentUrl = String(activeInfo?.current_url || activeInfo?.currentUrl || '').trim();
    const currentUrlLabel = currentUrl
      ? (currentUrl.length > 96 ? `${currentUrl.slice(0, 93)}...` : currentUrl)
      : '';
    const isHeadless = modeKind === 'headless';
    const screenshotBusy =
      !!(screenshotState &&
         screenshotState.loading &&
         String(screenshotState.scriptId || '') === s.id);
    const screenshotButton = active ? `
      <button class="dbg-mini-btn dbg-listener-shot-btn"
              data-headless-screenshot-id="${s.id}"
              ${screenshotBusy ? 'disabled' : ''}>
        ${screenshotBusy ? 'Requesting…' : 'Request screenshot'}
      </button>
    ` : '';
    return `
      <div class="dbg-listener-row">
        <div class="dbg-listener-meta">
          <div class="dbg-listener-name">${s.name}<span class="dbg-listener-phase ${phaseClass}">${phase.toUpperCase()}</span></div>
          <div class="dbg-listener-sub">${s.id}</div>
          <div class="dbg-listener-sub">mode: ${modeText}</div>
          <div class="dbg-listener-status">${statusText}</div>
          ${currentUrlLabel ? `<div class="dbg-listener-url" title="${currentUrl}">${currentUrlLabel}</div>` : ''}
        </div>
        <div class="dbg-listener-controls">
          ${screenshotButton}
          <label class="dbg-switch">
            <input type="checkbox" data-listener-id="${s.id}" ${active ? 'checked' : ''} ${pending ? 'disabled' : ''} />
            <span class="dbg-switch-slider" data-status="${status}"></span>
          </label>
        </div>
      </div>
    `;
  }).join('');

  container.innerHTML = `
    <div class="dbg-panel-title">Listeners</div>
    <div class="dbg-headless-tabs-wrap">
      <div class="dbg-headless-tabs-head">
        <span class="dbg-headless-tabs-title">Headless tabs</span>
        <span class="dbg-headless-tabs-count">${headlessTabs.length}</span>
      </div>
      <div class="dbg-headless-tabs" role="tablist" aria-label="Open headless tabs">
        ${headlessTabsHtml || '<div class="dbg-empty">No headless tabs open.</div>'}
      </div>
    </div>
    <div class="dbg-panel-sub">Toggle listener scripts on/off. Current default mode: <strong>${String(data?.listenerModeLabel || 'Headless listener host')}</strong>.</div>
    ${data?.headlessWarning ? `<div class="dbg-panel-sub" style="color:#b45309;">Headless warning: ${String(data.headlessWarning)}</div>` : ''}
    <div class="dbg-listeners">${rows || '<div class="dbg-empty">No listener scripts found.</div>'}</div>
  `;

  for (const input of container.querySelectorAll('input[type="checkbox"][data-listener-id]')) {
    input.addEventListener('change', async (e) => {
      const id = e.target.getAttribute('data-listener-id');
      const enabled = !!e.target.checked;
      if (!id) return;
      const inFlight = _state.listenerTogglePending?.[id];
      if (inFlight) {
        e.target.checked = !!inFlight.desiredEnabled;
        return;
      }
      _state.listenerTogglePending[id] = {
        desiredEnabled: enabled,
        startedAtMs: msNow(),
      };
      container.dispatchEvent(
          new CustomEvent('dbg-refresh-listeners', {bubbles: true}));
      try {
        if (enabled) {
          const res = await dashboardHandler.startListener(id);
          if (res && res.success === false) {
            alert(res.error || 'Failed to start listener');
          } else if (res && res.warning) {
            const mode =
              String(res.mode || res.mode_kind || res.modeKind || '').trim().toLowerCase();
            const warningText = String(res.warning || '');
            const isFallback =
              mode === 'background_tab' ||
              warningText.toLowerCase().includes('falling back');
            if (isFallback) {
              console.warn('[Listeners] start fallback:', warningText);
            } else {
              console.info('[Listeners] start note:', warningText);
            }
          }
        } else {
          const res = await dashboardHandler.stopListener(id);
          if (res && res.success === false) {
            alert(res.error || 'Failed to stop listener');
          }
        }
      } catch (err) {
        console.error('Listener toggle failed', err);
      } finally {
        delete _state.listenerTogglePending[id];
      }
      container.dispatchEvent(
          new CustomEvent('dbg-refresh-listeners', {bubbles: true}));
    });
  }

  for (const button of container.querySelectorAll('button[data-headless-screenshot-id]')) {
    button.addEventListener('click', (e) => {
      const scriptId = e.currentTarget.getAttribute('data-headless-screenshot-id');
      if (!scriptId) {
        return;
      }
      container.dispatchEvent(new CustomEvent('dbg-request-headless-screenshot', {
        bubbles: true,
        detail: {scriptId},
      }));
    });
  }

  for (const refreshBtn of container.querySelectorAll('button[data-headless-refresh-id]')) {
    refreshBtn.addEventListener('click', async (e) => {
      e.preventDefault();
      e.stopPropagation();
      const id = e.currentTarget.getAttribute('data-headless-refresh-id');
      const kind =
        String(e.currentTarget.getAttribute('data-headless-refresh-kind') || '').trim();
      if (!id) return;
      const btn = e.currentTarget;
      btn.disabled = true;
      try {
        const res = await dashboardHandler.refreshHeadlessTab(kind, id);
        if (res && res.success === false) {
          alert(res.error || 'Failed to refresh headless tab');
        }
      } catch (err) {
        console.error('Headless tab refresh failed', err);
      } finally {
        btn.disabled = false;
      }
      container.dispatchEvent(
          new CustomEvent('dbg-refresh-listeners', {bubbles: true}));
    });
  }

  for (const closeBtn of container.querySelectorAll('button[data-headless-close-id]')) {
    closeBtn.addEventListener('click', async (e) => {
      e.preventDefault();
      e.stopPropagation();
      const id = e.currentTarget.getAttribute('data-headless-close-id');
      const closeAction =
        String(e.currentTarget.getAttribute('data-headless-close-action') || '').trim();
      if (!id) return;
      if (closeAction !== 'stop_listener' &&
          closeAction !== 'stop_headless_task') return;
      if (_state.listenerTogglePending?.[id]) return;
      _state.listenerTogglePending[id] = {
        desiredEnabled: false,
        startedAtMs: msNow(),
      };
      container.dispatchEvent(
          new CustomEvent('dbg-refresh-listeners', {bubbles: true}));
      try {
        const res = (closeAction === 'stop_listener')
          ? await dashboardHandler.stopListener(id)
          : await dashboardHandler.stopHeadlessTask(id);
        if (res && res.success === false) {
          alert(res.error || 'Failed to close headless tab');
        }
      } catch (err) {
        console.error('Headless tab close failed', err);
      } finally {
        delete _state.listenerTogglePending[id];
      }
      container.dispatchEvent(
          new CustomEvent('dbg-refresh-listeners', {bubbles: true}));
    });
  }
}

function buildFailureSummary(data, appState, limit = 80) {
  const out = [];
  if (!data || typeof data !== 'object')
    return out;

  const scripts = (appState && appState.scripts && typeof appState.scripts === 'object')
    ? appState.scripts
    : {};
  const scriptNameFor = (id, fallback = '') => {
    const key = String(id || '').trim();
    if (!key) return fallback || '';
    return String((scripts[key] && scripts[key].name) || fallback || key);
  };

  const runIds = new Set();
  for (const run of (data.runs || [])) {
    const runId = String(run?.run_id || '').trim();
    if (runId) {
      runIds.add(runId);
    }
  }

  for (const run of (data.runs || [])) {
    if (isListenerScript(run?.script_id, {scripts})) continue;
    if (String(run?.status || '').toLowerCase() !== 'error')
      continue;
    const runId = String(run.run_id || '').trim();
    const startedMs = parseMs(run.started_ms ?? run.startedMs ?? run.started);
    const endedMs = parseMs(run.ended_ms ?? run.endedMs ?? run.ended);
    out.push({
      kind: 'error',
      key: `run:${runId}`,
      run_id: runId,
      script_id: String(run.script_id || ''),
      script_name: String(run.script_name || scriptNameFor(run.script_id)),
      status: String(run.status || 'error'),
      failed_step: Number(run.failed_step ?? -1),
      failed_action_index: String(run.failed_action_index || ''),
      error: String(run.error || ''),
      started_ms: Number.isFinite(startedMs) ? startedMs : null,
      ended_ms: Number.isFinite(endedMs) ? endedMs : null,
    });
  }

  for (let i = 0; i < (data.listenerEvents || []).length; i++) {
    const ev = data.listenerEvents[i];
    const action = String(ev?.action || '').toLowerCase();
    if (action !== 'error')
      continue;
    const runId = String(ev?.run_id || ev?.runId || '');
    if (runId && runIds.has(runId))
      continue;
    const ts = parseMs(ev?.ts_ms ?? ev?.tsMs ?? ev?.ts ?? ev?.time_ms ?? ev?.timeMs ?? ev?.time);
    const listenerId =
      String(ev?.listener_script_id || ev?.listenerScriptId || ev?.listener_id || ev?.listenerId || '');
    const triggerId =
      String(ev?.trigger_script_id || ev?.triggerScriptId || ev?.trigger_id || ev?.triggerId || '');
    out.push({
      kind: 'error',
      key: eventKeyForListenerEvent(ev, i),
      run_id: runId,
      listener_script_id: listenerId,
      listener_script_name: String(
        ev?.listener_script_name ||
        ev?.listenerScriptName ||
        scriptNameFor(listenerId, 'Listener')),
      trigger_script_id: triggerId,
      trigger_script_name: String(
        ev?.trigger_script_name ||
        ev?.triggerScriptName ||
        scriptNameFor(triggerId, 'Trigger')),
      action: action,
      detail: String(ev?.detail || ''),
      ts_ms: Number.isFinite(ts) ? ts : null,
      raw: toPlain(ev),
    });
  }

  out.sort((a, b) => {
    const ta = Number(a.ts_ms || a.started_ms || 0);
    const tb = Number(b.ts_ms || b.started_ms || 0);
    return tb - ta;
  });
  return out.slice(0, Math.max(1, limit));
}

function buildAutoHealIncidentPayload(data, appState, selectedItem, runDetails = null) {
  if (!selectedItem)
    return null;

  const selected = toPlain(selectedItem) || {};
  const runDetailPlain = toPlain(runDetails) || null;
  const runId = String(selected.id || runDetailPlain?.run_id || '').trim();
  let matchedListenerEvent = null;
  if (selected.kind === 'listener_event' && data && Array.isArray(data.listenerEvents)) {
    const wantScriptId = String(selected.scriptId || '').trim();
    const wantTs = Number(selected.startMs || 0);
    const wantErr = String(selected.error || '').trim();
    for (const ev of data.listenerEvents) {
      const action = String(ev?.action || '').toLowerCase();
      if (action !== 'error')
        continue;
      const listenerId = String(
        ev?.listener_script_id || ev?.listenerScriptId || ev?.listener_id || ev?.listenerId || '');
      const ts = parseMs(ev?.ts_ms ?? ev?.tsMs ?? ev?.ts ?? ev?.time_ms ?? ev?.timeMs ?? ev?.time) || 0;
      const detail = String(ev?.detail || '').trim();
      if (wantScriptId && listenerId && wantScriptId !== listenerId)
        continue;
      if (wantTs > 0 && Math.abs(ts - wantTs) > 2500)
        continue;
      if (wantErr && detail && wantErr !== detail)
        continue;
      matchedListenerEvent = toPlain(ev);
      break;
    }
  }

  const payload = {
    source: 'debugging_tab',
    created_at_ms: Date.now(),
    selected_item: selected,
    selected_run_id: runId,
    selected_script_id: String(selected.scriptId || ''),
    selected_script_name: String(selected.scriptName || ''),
    selected_status: String(selected.status || ''),
    selected_failed_step: Number(selected.failedStep ?? -1),
    selected_failed_action_index: String(
      selected.failedActionIndex ||
      runDetailPlain?.failed_action_index ||
      lookupActionIndexLabelForFailedStep(selected.failedStep, runDetailPlain) ||
      ''
    ),
    selected_error: String(selected.error || ''),
    selected_start_ms: Number(selected.startMs || 0),
    selected_end_ms: Number(selected.endMs || 0),
    run_details: runDetailPlain,
    listener_event: matchedListenerEvent,
    timeline_failures: buildFailureSummary(data, appState, 120),
  };
  return payload;
}

function renderHeadlessScreenshotCard(screenshotState) {
  const state = screenshotState && typeof screenshotState === 'object'
    ? screenshotState
    : null;
  if (!state) {
    return '';
  }
  const scriptId = String(state.scriptId || '').trim();
  if (!scriptId && !state.loading && !state.error && !state.dataUrl) {
    return '';
  }
  const capturedMs = Number(state.capturedAtMs || 0);
  const capturedAt = capturedMs > 0 ? formatLocalWithSeconds(new Date(capturedMs)) : '';
  const path = String(state.path || '').trim();
  const error = String(state.error || '').trim();
  const dataUrlRaw = String(state.dataUrl || '').trim();
  const dataUrl = dataUrlRaw.startsWith('data:') ? dataUrlRaw : '';
  return `
    <div class="dbg-headless-shot">
      <div class="dbg-headless-shot-head">
        <div class="dbg-headless-shot-title">Headless screenshot</div>
        <div class="dbg-headless-shot-meta">${scriptId ? `listener ${escapeHtml(scriptId)}` : ''}${capturedAt ? ` • ${capturedAt}` : ''}</div>
      </div>
      ${state.loading ? '<div class="dbg-headless-shot-sub">Requesting screenshot from headless listener…</div>' : ''}
      ${error ? `<div class="dbg-incident-error">${escapeHtml(error)}</div>` : ''}
      ${dataUrl ? `<img class="dbg-headless-shot-img" src="${dataUrl}" alt="Headless listener screenshot"/>` : ''}
      ${path ? `<div class="dbg-headless-shot-path" title="${escapeHtml(path)}">${escapeHtml(path)}</div>` : ''}
    </div>
  `;
}

function cssAttrEscape(value) {
  return String(value).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

function updateHeadlessScreenshotUi(container, screenshotState) {
  if (!container) {
    return;
  }
  const state = screenshotState && typeof screenshotState === 'object'
    ? screenshotState
    : null;
  const scriptId = String(state?.scriptId || '').trim();
  if (scriptId) {
    const escapedId = cssAttrEscape(scriptId);
    const button = container.querySelector(
      `button[data-headless-screenshot-id="${escapedId}"]`
    );
    if (button) {
      const loading = !!state?.loading;
      button.disabled = loading;
      button.textContent = loading ? 'Requesting…' : 'Request screenshot';
    }
  }
  const shotSlot = container.querySelector('#dbg-headless-shot-slot');
  if (shotSlot) {
    shotSlot.innerHTML = renderHeadlessScreenshotCard(state);
  }
}

function renderRunDetails(container,
                          selectedItem,
                          runDetails = null,
                          queueState = null,
                          screenshotState = null) {
  const screenshotCard = renderHeadlessScreenshotCard(screenshotState);
  if (!selectedItem) {
    container.innerHTML = `
      <div class="dbg-panel-title">Incident</div>
      <div class="dbg-empty">Select a run from the timeline to view details.</div>
      <div id="dbg-headless-shot-slot">${screenshotCard}</div>
    `;
    return;
  }

  const start = selectedItem.startMs ? new Date(selectedItem.startMs) : null;
  const end = selectedItem.endMs ? new Date(selectedItem.endMs) : null;
  const dur = (selectedItem.startMs && selectedItem.endMs) ? (selectedItem.endMs - selectedItem.startMs) : null;
  const status = selectedItem.status || '';
  const failedStep = Number(selectedItem.failedStep ?? runDetails?.failed_step ?? -1);
  const failedActionIndex = String(
    selectedItem.failedActionIndex ||
    runDetails?.failed_action_index ||
    lookupActionIndexLabelForFailedStep(failedStep, runDetails) ||
    ''
  ).trim();
  const runError = String(runDetails?.error || '').trim();
  const selectedError = String(selectedItem.error || '').trim();
  const error = runError || selectedError;
  const taxonomy = runDetails?.failure_taxonomy || selectedItem?.failure_taxonomy || null;
  const taxonomyLabel = taxonomy && typeof taxonomy === 'object'
    ? `${String(taxonomy.category || 'unknown')}/${String(taxonomy.subcategory || 'unknown')}`
    : '';
  const advice = getErrorAdvice(error);
  const steps = Array.isArray(runDetails?.steps) ? runDetails.steps : [];
  let selectedExecutionStep = Number(_state.selectedExecutionStep ?? -1);
  if (!Number.isFinite(selectedExecutionStep) || selectedExecutionStep < 0) {
    selectedExecutionStep = failedStep;
  }
  let selectedStep = steps.find((step) => readExecutionStepIndex(step) === selectedExecutionStep) || null;
  if (!selectedStep && failedStep >= 0) {
    selectedStep = steps.find((step) => readExecutionStepIndex(step) === failedStep) || null;
  }
  if (!selectedStep && steps.length) {
    selectedStep = steps[0];
  }
  _state.selectedExecutionStep = selectedStep ? readExecutionStepIndex(selectedStep) : -1;
  const selectedStepActionIndex = selectedStep ? actionIndexLabelFromStep(selectedStep) : '';
  const selectedStepActionName = String(
    selectedStep?.action?.action || selectedStep?.action?.type || ''
  ).trim();
  const selectedStepStatus = String(selectedStep?.status || '').trim();
  const selectedStepMessage = String(selectedStep?.message || '').trim();
  const selectedStepUrl = String(selectedStep?.url_after || selectedStep?.urlAfter || '').trim();
  const selectedStepJson = selectedStep
    ? escapeHtml(JSON.stringify(toPlain(selectedStep?.action || {}), null, 2))
    : '';
  const q = (queueState && typeof queueState === 'object') ? queueState : {};
  const qRows = Array.isArray(q.queue) ? q.queue : [];
  const qInFlight = !!q.in_flight;
  const qEnabled = q.enabled !== false;
  const qCurrentKey = String(q.current_key || '');
  const qCurrent = (q.current && typeof q.current === 'object') ? q.current : null;
  const qProgress = (q.progress && typeof q.progress === 'object') ? q.progress : null;
  const qStatusClass = !qEnabled ? 'disabled' : (qInFlight ? 'running' : 'idle');
  const qStatusText = !qEnabled ? 'disabled' : (qInFlight ? 'healing' : 'idle');
  const progressAgeMs = Number(qProgress?.updated_at || 0) > 0 ?
    Math.max(0, Date.now() - Number(qProgress.updated_at)) :
    0;
  const progressAgeLabel = progressAgeMs > 0 ?
    (progressAgeMs < 1000 ?
      `${progressAgeMs}ms ago` :
      (progressAgeMs < 60000 ?
        `${Math.round(progressAgeMs / 1000)}s ago` :
        `${Math.round(progressAgeMs / 60000)}m ago`)) :
    '';
  const stuckHint = (qInFlight && progressAgeMs > 90000) ?
    'No progress update for >90s — model may be stuck or very slow.' :
    '';
  const currentCardHtml = (qInFlight && (qCurrent || qProgress)) ? `
    <div class="dbg-autoheal-current">
      <div class="dbg-autoheal-current-title">Currently healing</div>
      <div class="dbg-autoheal-item-title">${escapeHtml(String(
        qCurrent?.script_name || qCurrentKey || '(unknown script)'))}</div>
      <div class="dbg-autoheal-item-meta">${escapeHtml((() => {
        if (!qCurrent) return '';
        const parts = [];
        if (Number.isFinite(Number(qCurrent.failed_step)) && Number(qCurrent.failed_step) >= 0) {
          parts.push(`failed_step=${qCurrent.failed_step}`);
        }
        if (qCurrent.error) {
          parts.push(String(qCurrent.error));
        }
        return parts.join(' • ');
      })())}</div>
      <div class="dbg-autoheal-progress">
        <div><span class="k">Phase</span> ${escapeHtml(String(qProgress?.phase || 'running'))}</div>
        <div><span class="k">Turn</span> ${escapeHtml(String(
          qProgress?.turn ?
            `${qProgress.turn}${qProgress?.max_turns ? `/${qProgress.max_turns}` : ''}` :
            '—'))}</div>
        <div><span class="k">Command</span> ${escapeHtml(String(qProgress?.command || '(waiting)'))}</div>
        <div><span class="k">Detail</span> ${escapeHtml(String(qProgress?.detail || ''))}</div>
        ${progressAgeLabel ? `<div><span class="k">Updated</span> ${escapeHtml(progressAgeLabel)}</div>` : ''}
        ${stuckHint ? `<div class="dbg-incident-error">${escapeHtml(stuckHint)}</div>` : ''}
      </div>
    </div>
  ` : '';
  const queueRowsHtml = qRows.length
    ? qRows.map((row, idx) => {
        const key = String(row?.key || '');
        const isCurrent = key && key === qCurrentKey;
        const title = escapeHtml(String(
          row?.script_name || row?.selected_script_name || row?.listener_script_name || key));
        const err = escapeHtml(String(row?.error || row?.detail || ''));
        const meta = escapeHtml(`idx=${idx + 1} source=${String(row?.source || 'manual')}`);
        return `
          <div class="dbg-autoheal-item ${isCurrent ? 'current' : ''}" data-queue-key="${escapeHtml(key)}">
            <div class="dbg-autoheal-item-top">
              <div class="dbg-autoheal-item-title">${title || '(script unknown)'}</div>
            <div class="dbg-autoheal-item-meta">${meta}</div>
          </div>
          <div class="dbg-autoheal-item-meta">${err || '(no error text)'}</div>
          <div class="dbg-autoheal-item-actions">
              <button class="dbg-mini-btn dbg-icon-btn" data-q-act="heal-now" data-key="${escapeHtml(key)}" title="Heal now" aria-label="Heal now" data-icon="H"><span class="dbg-icon-btn-text">Heal now</span></button>
              <button class="dbg-mini-btn dbg-icon-btn" data-q-act="up" data-key="${escapeHtml(key)}" title="Move up" aria-label="Move up" data-icon="U"><span class="dbg-icon-btn-text">Up</span></button>
              <button class="dbg-mini-btn dbg-icon-btn" data-q-act="down" data-key="${escapeHtml(key)}" title="Move down" aria-label="Move down" data-icon="D"><span class="dbg-icon-btn-text">Down</span></button>
              <button class="dbg-mini-btn dbg-icon-btn" data-q-act="remove" data-key="${escapeHtml(key)}" title="Remove" aria-label="Remove" data-icon="-"><span class="dbg-icon-btn-text">Remove</span></button>
            </div>
          </div>
        `;
      }).join('')
    : '<div class="dbg-empty">Queue is empty.</div>';

  container.innerHTML = `
    <div class="dbg-panel-title">Incident</div>
    <div class="dbg-incident">
      <div class="dbg-incident-row"><span class="k">Script</span><span class="v">${selectedItem.scriptName || selectedItem.scriptId || ''}</span></div>
      <div class="dbg-incident-row"><span class="k">Status</span><span class="v">${status}</span></div>
      <div class="dbg-incident-row"><span class="k">Started</span><span class="v">${start ? formatLocal(start) : ''}</span></div>
      <div class="dbg-incident-row"><span class="k">Ended</span><span class="v">${end ? formatLocal(end) : ''}</span></div>
      <div class="dbg-incident-row"><span class="k">Duration</span><span class="v">${dur != null ? formatDuration(dur) : ''}</span></div>
      ${failedStep >= 0 ? `<div class="dbg-incident-row"><span class="k">Failed Step</span><span class="v">${failedStep}</span></div>` : ''}
      ${failedActionIndex ? `<div class="dbg-incident-row"><span class="k">Action Index</span><span class="v">${escapeHtml(failedActionIndex)}</span></div>` : ''}
      ${taxonomyLabel ? `<div class="dbg-incident-row"><span class="k">Taxonomy</span><span class="v">${escapeHtml(taxonomyLabel)}</span></div>` : ''}
      ${error ? `<div class="dbg-incident-error">${escapeHtml(error)}</div>` : ''}
      ${advice ? `<div class="dbg-incident-advice">${advice}</div>` : ''}
      ${selectedStep ? `
        <div class="dbg-step-detail">
          <div class="dbg-step-detail-head">
            <div class="dbg-step-detail-title">Selected step</div>
            <div class="dbg-step-detail-meta">step ${_state.selectedExecutionStep >= 0 ? _state.selectedExecutionStep : '?'}${selectedStepActionIndex ? ` · action ${escapeHtml(selectedStepActionIndex)}` : ''}${selectedStepActionName ? ` · ${escapeHtml(selectedStepActionName)}` : ''}</div>
          </div>
          <div class="dbg-step-detail-grid">
            <div class="dbg-incident-row"><span class="k">Status</span><span class="v">${escapeHtml(selectedStepStatus || '')}</span></div>
            ${selectedStepUrl ? `<div class="dbg-incident-row"><span class="k">URL</span><span class="v dbg-step-detail-url" title="${escapeHtml(selectedStepUrl)}">${escapeHtml(selectedStepUrl)}</span></div>` : ''}
          </div>
          ${selectedStepMessage ? `<div class="dbg-incident-error">${escapeHtml(selectedStepMessage)}</div>` : ''}
          ${selectedStepJson ? `<pre class="dbg-step-detail-json">${selectedStepJson}</pre>` : ''}
        </div>
      ` : ''}
      <div class="dbg-incident-actions">
        <button class="btn btn-secondary dbg-icon-btn" id="dbg-open-script" title="Open script" aria-label="Open script" data-icon="O"><span class="dbg-icon-btn-text">Open script</span></button>
        <button class="btn btn-primary dbg-icon-btn" id="dbg-resume-step" ${selectedItem.kind !== 'run' ? 'disabled' : ''} title="Resume from step" aria-label="Resume from step" data-icon="R"><span class="dbg-icon-btn-text">Resume from step…</span></button>
        <button class="btn btn-secondary dbg-icon-btn" id="dbg-view-replay" title="View error replay" aria-label="View error replay" data-icon="V"><span class="dbg-icon-btn-text">View error replay</span></button>
        <button class="btn btn-secondary dbg-icon-btn" id="dbg-ask-dashboard-model" title="Ask Dashboard Model" aria-label="Ask Dashboard Model" data-icon="M"><span class="dbg-icon-btn-text">Ask Dashboard Model</span></button>
        <button class="btn btn-secondary dbg-icon-btn" id="dbg-queue-auto-heal" title="Queue Auto-Heal" aria-label="Queue Auto-Heal" data-icon="Q"><span class="dbg-icon-btn-text">Queue Auto-Heal</span></button>
      </div>
      <div class="dbg-autoheal">
        <div class="dbg-autoheal-head">
          <div class="dbg-autoheal-title">Auto-heal Queue</div>
          <div class="dbg-autoheal-status ${qStatusClass}">${qStatusText} • ${qRows.length} queued</div>
        </div>
        ${currentCardHtml}
        <div class="dbg-autoheal-actions">
          <button class="dbg-mini-btn dbg-icon-btn" id="dbg-q-process" title="Process next" aria-label="Process next" data-icon="P"><span class="dbg-icon-btn-text">Process next</span></button>
          <button class="dbg-mini-btn dbg-icon-btn" id="dbg-q-cancel-current" title="Cancel current" aria-label="Cancel current" data-icon="X"><span class="dbg-icon-btn-text">Cancel current</span></button>
          <button class="dbg-mini-btn dbg-icon-btn" id="dbg-q-clear" title="Clear queue" aria-label="Clear queue" data-icon="C"><span class="dbg-icon-btn-text">Clear queue</span></button>
        </div>
        <div class="dbg-autoheal-list" id="dbg-autoheal-list">${queueRowsHtml}</div>
      </div>
      <div id="dbg-replay" class="dbg-replay hidden"></div>
      <div id="dbg-resume-form" class="dbg-resume hidden">
        <div class="dbg-resume-row">
          <input class="input-field" id="dbg-step-index" type="number" min="0" placeholder="Step index (0-based)" />
          <button class="btn btn-primary" id="dbg-resume-go">Resume</button>
        </div>
        <div class="dbg-resume-hint">Tip: Action Index is shown as a 1-based path (for example 4.1), but resume uses execution Step index (0-based).</div>
      </div>
      <div id="dbg-steps" class="dbg-steps"></div>
      <div id="dbg-headless-shot-slot">${screenshotCard}</div>
    </div>
  `;
}

function getErrorAdvice(errorText) {
  const err = String(errorText || '').trim();
  if (!err) return '';
  const lower = err.toLowerCase();
  if (lower.includes('timeout') && lower.includes('chosen=tag_name') &&
      lower.includes('tag=body')) {
    return [
      '<div class="k"><strong>Likely cause</strong>: locator is too broad (`BODY`), so text checks evaluate the whole page.</div>',
      '<div class="k"><strong>Try</strong>: use a narrower locator (for this test page, `#finish` or the specific `h4`), and prefer `text_contains` over `text_equals`.</div>',
      '<div class="k"><strong>Listener behavior</strong>: `listen` is blocking. Actions after `listen` do not run until it matches or times out.</div>',
    ].join('');
  }
  if (lower.includes('no render frame') || lower.includes('target webcontents was destroyed')) {
    return [
      '<div class="k"><strong>Likely cause</strong>: target tab/frame was closed or navigated away during execution.</div>',
      '<div class="k"><strong>Try</strong>: keep the tab alive, or run via listener trigger background tabs.</div>',
    ].join('');
  }
  if (lower.includes('timed out waiting for resume navigation')) {
    return [
      '<div class="k"><strong>Likely cause</strong>: resume URL navigation did not finish in time.</div>',
      '<div class="k"><strong>Try</strong>: rerun from an earlier step, verify network/URL validity, and confirm auth/session state.</div>',
    ].join('');
  }
  if (lower.includes('context url mismatch')) {
    return [
      '<div class="k"><strong>Likely cause</strong>: action expected a different page URL than the tab was currently on.</div>',
      '<div class="k"><strong>Try</strong>: confirm the action Context URL in Advanced settings, or add a navigation/wait step before it.</div>',
    ].join('');
  }
  if (lower.includes('locator') || lower.includes('element') || lower.includes('not found')) {
    return [
      '<div class="k"><strong>Likely cause</strong>: locator drift after page changes.</div>',
      '<div class="k"><strong>Try</strong>: refresh locators in the action editor (prefer `ACCESSIBILITY_NAME` / stable IDs), then retry.</div>',
    ].join('');
  }
  return '';
}

function buildDashboardModelFailurePrompt(selectedItem, runDetails, appState) {
  const item = selectedItem && typeof selectedItem === 'object' ? selectedItem : {};
  const detail = runDetails && typeof runDetails === 'object' ? runDetails : {};
  const scriptId = String(item.scriptId || detail.script_id || '').trim();
  const scriptName = String(
    item.scriptName || detail.script_name || appState?.scripts?.[scriptId]?.name || ''
  ).trim();
  const runId = String(item.id || detail.run_id || '').trim();
  const status = String(item.status || detail.status || '').trim();
  const failedStep = Number(item.failedStep ?? detail.failed_step ?? -1);
  const failedActionIndex = String(
    item.failedActionIndex ||
    detail.failed_action_index ||
    lookupActionIndexLabelForFailedStep(failedStep, detail) ||
    ''
  ).trim();
  const error = String(item.error || detail.error || '').trim();
  const taxonomy = detail.failure_taxonomy || item.failure_taxonomy || null;
  const steps = Array.isArray(detail.steps) ? detail.steps : [];
  const failedSteps = steps.filter((s) => String(s?.status || '').toLowerCase() === 'fail');
  const failedSnippet = failedSteps.slice(0, 6).map((s) => {
    const idx = readExecutionStepIndex(s);
    const actionIdx = actionIndexLabelFromStep(s);
    const action = String(s?.action?.action || s?.action?.type || '').trim();
    const msg = String(s?.message || '').trim();
    const url = String(s?.url_after || s?.urlAfter || '').trim();
    return `- action_index=${actionIdx || '?'} execution_step=${idx} action=${action || '?'} url=${url || '?'} msg=${msg || '?'}`;
  }).join('\n');

  return [
    'You are the Dashboard Assistant. Analyze this failed run and propose the smallest safe fix.',
    '',
    'Context:',
    `- run_id: ${runId || '?'}`,
    `- script_id: ${scriptId || '?'}`,
    `- script_name: ${scriptName || '?'}`,
    `- status: ${status || '?'}`,
    `- failed_step: ${Number.isFinite(failedStep) ? failedStep : -1}`,
    `- failed_action_index: ${failedActionIndex || '?'}`,
    `- error: ${error || '?'}`,
    `- failure_taxonomy: ${taxonomy ? JSON.stringify(taxonomy) : '{}'}`,
    '',
    'Recent failed steps:',
    failedSnippet || '- (no failed step details available)',
    '',
    'Required output format:',
    '1) Root cause (1-2 lines).',
    '2) Exact script edits (action index + field changes).',
    '3) Verification plan (max 3 checks).',
    '4) If uncertain, ask exactly one clarifying question.',
  ].join('\n');
}

function renderSteps(container, runDetails, onPickStep) {
  if (!runDetails?.steps || !Array.isArray(runDetails.steps)) {
    container.innerHTML = '<div class="dbg-empty">No step details available for this run.</div>';
    return;
  }
  const rows = runDetails.steps.map((s) => {
    const executionStep = readExecutionStepIndex(s);
    const executionStepAttr = (executionStep >= 0) ? String(executionStep) : 'NaN';
    const actionIndex = actionIndexLabelFromStep(s);
    const st = s.status || '';
    const msg = s.message || '';
    const url = s.url_after || s.urlAfter || '';
    const isFail = st === 'fail';
    const isSelected = executionStep >= 0 &&
      executionStep === Number(_state.selectedExecutionStep ?? -1);
    return `
      <button class="dbg-step ${isFail ? 'fail' : ''} ${isSelected ? 'selected' : ''}" data-step="${executionStepAttr}">
        <span class="idx" title="Action index">${escapeHtml(actionIndex)}</span>
        <span class="st">${escapeHtml(st)}</span>
        <span class="msg">${escapeHtml(msg)}</span>
        <span class="url">${escapeHtml(url)}</span>
      </button>
    `;
  }).join('');
  container.innerHTML = rows;
  for (const b of container.querySelectorAll('.dbg-step')) {
    b.addEventListener('click', () => {
      const step = Number(b.getAttribute('data-step'));
      if (Number.isFinite(step)) onPickStep(step);
    });
  }
}

export async function renderDebuggingView(container, dashboardHandler, appState, opts = {}) {
  const isActive = () => {
    try {
      if (typeof opts?.isActive === 'function') return !!opts.isActive();
    } catch (_) {}
    return true;
  };
  if (!isActive()) {
    if (_refreshTimer) {
      clearTimeout(_refreshTimer);
      _refreshTimer = null;
    }
    stopReplayTimer();
    return;
  }

  // Any prior replay playback timer must be stopped before we rewrite DOM.
  stopReplayTimer();
  installDebugViewInteractionTracking(container);
  const scrollState = captureDebugViewScrollState(container);
  container.__dbgRendering = true;

  const now = msNow();
  const mode = (_state.rangeMode === '7d') ? '7d' : '24h';
  const dayMs = 24 * 3600 * 1000;
  const baseWindowStartMs = (mode === '7d') ? (now - 6 * dayMs) : (now - dayMs);
  const baseWindowEndMs = (mode === '7d') ? (now + dayMs) : now;

  // Attach resize observer once per container.
  if (_lastContainer !== container) {
    _lastContainer = container;
    if (_resizeObserver) _resizeObserver.disconnect();
    _resizeObserver = new ResizeObserver(() => {
      if (container.__dbgRendering || isDebugViewInteractionActive(container)) {
        return;
      }
      if (container.__dbgResizeRefreshTimer) {
        clearTimeout(container.__dbgResizeRefreshTimer);
      }
      container.__dbgResizeRefreshTimer = setTimeout(() => {
        container.__dbgResizeRefreshTimer = null;
        if (!container.__dbgRendering && !isDebugViewInteractionActive(container)) {
          container.dispatchEvent(new CustomEvent('dbg-refresh', { bubbles: true }));
        }
      }, 250);
    });
    _resizeObserver.observe(container);
  }

  let data = null;
  try {
    if (!dashboardHandler || typeof dashboardHandler.getRunHistory !== 'function') {
      throw new Error('Debugging backend API unavailable (getRunHistory missing). Rebuild/restart Chrome?');
    }
    data = await fetchDebugData(dashboardHandler, baseWindowStartMs, baseWindowEndMs);
  } catch (err) {
    const msg = (err && err.message) ? err.message : String(err || 'Unknown error');
    container.innerHTML = `
      <div class="dbg-root">
        <div class="dbg-header">
          <div class="dbg-header-left">
            <h1 class="dbg-title">Debugging</h1>
            <div class="dbg-empty">Failed to load run history.</div>
          </div>
        </div>
        <div class="dbg-panel" style="max-width:960px;">
          <div class="dbg-panel-title">Details</div>
          <div class="dbg-incident-error">${escapeHtml(msg)}</div>
          <div class="dbg-panel-sub">Tip: open DevTools on this page to see console errors.</div>
        </div>
      </div>
    `;
    restoreDebugViewScrollState(container, scrollState);
    container.__dbgRendering = false;
    return;
  }

  const zoomInfo = computeActivityWindow(mode, baseWindowStartMs, baseWindowEndMs, data, appState);
  const windowStartMs = zoomInfo.startMs;
  const windowEndMs = zoomInfo.endMs;
  container.__dbgDataSnapshot = data;

  container.innerHTML = `
    <div class="dbg-root">
      <div class="dbg-header">
        <div class="dbg-header-left">
          <h1 class="dbg-title">Debugging</h1>
          <label class="dbg-checkbox">
            <input id="dbg-verbose" type="checkbox" ${data.debugLoggingEnabled ? 'checked' : ''}/>
            <span>Enable verbose engine logs</span>
          </label>
        </div>
        <div class="dbg-header-right">
          <button class="btn btn-secondary" id="dbg-refresh">Refresh</button>
        </div>
      </div>

      <div class="dbg-body">
        <div class="dbg-main">
          <div class="dbg-panel dbg-panel-incident" id="dbg-incident"></div>
          <div class="dbg-panel dbg-panel-listeners" id="dbg-listeners"></div>
        </div>
        <div class="dbg-timeline">
          <div class="dbg-ruler" id="dbg-ruler"></div>
          <div class="dbg-lanes" id="dbg-lanes"></div>
        </div>
      </div>
    </div>
    <div class="dbg-hover hidden" id="dbg-hover"></div>
  `;
  restoreDebugViewScrollState(container, scrollState);

  const incidentEl = container.querySelector('#dbg-incident');
  const listenersEl = container.querySelector('#dbg-listeners');
  const rulerEl = container.querySelector('#dbg-ruler');
  const lanesEl = container.querySelector('#dbg-lanes');
  const eventLogEl = ENABLE_EVENT_STREAM
    ? container.querySelector('#dbg-eventlog')
    : null;

  renderRuler(rulerEl, windowStartMs, windowEndMs, mode, data, zoomInfo);
  renderListenersPanel(
    listenersEl,
    appState,
    data,
    dashboardHandler,
    _state.headlessScreenshot
  );

  let selectedItem = null;
  if (_state.selectedRunId) {
    selectedItem = data.runs
      .map((r) => {
        if (isListenerScript(r.script_id, appState)) return null;
        const startedMs = parseMs(r.started_ms ?? r.startedMs ?? r.started);
        const endedMs = parseMs(r.ended_ms ?? r.endedMs ?? r.ended);
        if (!Number.isFinite(startedMs)) return null;
        return {
          kind: 'run',
          id: r.run_id,
          scriptId: r.script_id,
          scriptName: r.script_name || (appState.scripts?.[r.script_id]?.name ?? ''),
          status: r.status || 'success',
          error: r.error || '',
          failure_taxonomy: r.failure_taxonomy || null,
          failedStep: r.failed_step ?? -1,
          failedActionIndex: r.failed_action_index || '',
          startMs: startedMs,
          endMs: Number.isFinite(endedMs) ? endedMs : Date.now(),
        };
      })
      .find((x) => x && x.id === _state.selectedRunId) || null;
  }
  if (!selectedItem && _state.selectedContextItem) {
    selectedItem = _state.selectedContextItem;
  }
  if (!selectedItem) {
    _state.selectedExecutionStep = -1;
  }

  const queueState = (typeof opts?.getAutoHealQueueState === 'function')
    ? (opts.getAutoHealQueueState() || null)
    : null;
  renderRunDetails(
    incidentEl,
    selectedItem,
    _state.runDetails,
    queueState,
    _state.headlessScreenshot
  );
  renderTimeline(lanesEl, data, appState, windowStartMs, windowEndMs);
  if (eventLogEl) {
    renderEventFeed(eventLogEl, data, appState, windowStartMs, windowEndMs);
  }
  if (typeof opts?.onDebugDataSnapshot === 'function') {
    try {
      opts.onDebugDataSnapshot({
        windowStartMs,
        windowEndMs,
        selectedItem: toPlain(selectedItem),
        runDetails: toPlain(_state.runDetails),
        failures: buildFailureSummary(data, appState, 160),
      });
    } catch (_) {}
  }

  const focusEventInLog = (eventKey) => {
    const key = String(eventKey || '').trim();
    if (!key || !eventLogEl) return;
    const focused = focusEventRowByKey(eventLogEl, key, true);
    if (focused) {
      _state.pendingEventFocusKey = '';
      return;
    }
    _state.pendingEventFocusKey = key;
    if (_state.eventFilterScript !== '__all__') {
      _state.eventFilterScript = '__all__';
      container.dispatchEvent(new CustomEvent('dbg-refresh', { bubbles: true }));
    }
  };

  if (eventLogEl) {
    if (container.__dbgFocusHandler) {
      container.removeEventListener('dbg-focus-event', container.__dbgFocusHandler);
    }
    container.__dbgFocusHandler = (e) => {
      focusEventInLog(e?.detail?.eventKey || '');
    };
    container.addEventListener('dbg-focus-event', container.__dbgFocusHandler);
  }

  // Hover bubble: brief failure summary without relying on terminal logs.
  const hoverEl = container.querySelector('#dbg-hover');
  const hideHover = () => {
    if (!hoverEl) return;
    hoverEl.classList.add('hidden');
    hoverEl.textContent = '';
  };
  if (hoverEl) {
    for (const el of container.querySelectorAll('.dbg-run')) {
      el.addEventListener('mouseleave', hideHover);
      el.addEventListener('click', hideHover);
      el.addEventListener('mouseenter', (ev) => {
        const status = el.dataset.status || '';
        const err = el.dataset.error || '';
        if (status !== 'error' || !err) {
          hideHover();
          return;
        }
        const title = (el.querySelector('.dbg-run-label')?.textContent || '').trim();
        const shown = err.length > 220 ? err.slice(0, 220) + '…' : err;
        hoverEl.innerHTML = `<div class="t">${title || 'Failure'}</div><div class="m">${shown}</div>`;
        hoverEl.classList.remove('hidden');
      });
      el.addEventListener('mousemove', (ev) => {
        if (!hoverEl || hoverEl.classList.contains('hidden')) return;
        const pad = 12;
        let x = ev.clientX + pad;
        let y = ev.clientY + pad;
        const w = hoverEl.offsetWidth || 260;
        const h = hoverEl.offsetHeight || 80;
        x = Math.min(x, window.innerWidth - w - pad);
        y = Math.min(y, window.innerHeight - h - pad);
        hoverEl.style.left = `${Math.max(pad, x)}px`;
        hoverEl.style.top = `${Math.max(pad, y)}px`;
      });
    }
  }

  // Event wiring
  const refresh = async () => {
    if (!isActive()) {
      if (_refreshTimer) {
        clearTimeout(_refreshTimer);
        _refreshTimer = null;
      }
      return;
    }
    if (opts?.onRefresh) opts.onRefresh();
    await renderDebuggingView(container, dashboardHandler, appState, opts);
  };

  if (container.__dbgRefreshHandler) {
    container.removeEventListener('dbg-refresh', container.__dbgRefreshHandler);
  }
  container.__dbgRefreshHandler = () => {
    void refresh();
  };
  container.addEventListener('dbg-refresh', container.__dbgRefreshHandler, {
    once: true,
  });

  if (container.__dbgListenersRefreshHandler) {
    container.removeEventListener(
        'dbg-refresh-listeners',
        container.__dbgListenersRefreshHandler);
  }
  container.__dbgListenersRefreshHandler = () => {
    void refreshListenersPanelOnly(container, dashboardHandler, appState);
  };
  container.addEventListener(
      'dbg-refresh-listeners',
      container.__dbgListenersRefreshHandler);

  if (container.__dbgHeadlessScreenshotHandler) {
    container.removeEventListener(
      'dbg-request-headless-screenshot',
      container.__dbgHeadlessScreenshotHandler
    );
  }
  container.__dbgHeadlessScreenshotHandler = async (e) => {
    const scriptId = String(e?.detail?.scriptId || '').trim();
    if (!scriptId) {
      return;
    }
    if (_state.headlessScreenshot.loading &&
        String(_state.headlessScreenshot.scriptId || '') === scriptId) {
      return;
    }
    _state.headlessScreenshot = {
      scriptId,
      loading: true,
      error: '',
      dataUrl: '',
      path: '',
      capturedAtMs: 0,
    };
    updateHeadlessScreenshotUi(container, _state.headlessScreenshot);

    const res = await requestHeadlessScreenshot(dashboardHandler, scriptId);
    const ok = !!(res && res.success);
    const imageDataUrl = String(
      (res && (res.image_data_url || res.imageDataUrl)) || ''
    ).trim();
    const fallbackB64 = String((res && (res.image_base64 || res.imageBase64)) || '').trim();
    const dataUrl = imageDataUrl || (fallbackB64 ? `data:image/png;base64,${fallbackB64}` : '');
    _state.headlessScreenshot = {
      scriptId,
      loading: false,
      error: ok ? '' : String((res && res.error) || 'Failed to capture screenshot.'),
      dataUrl,
      path: String((res && res.path) || ''),
      capturedAtMs: Number((res && (res.ts_ms || res.tsMs)) || 0),
    };
    updateHeadlessScreenshotUi(container, _state.headlessScreenshot);
  };
  container.addEventListener(
    'dbg-request-headless-screenshot',
    container.__dbgHeadlessScreenshotHandler
  );

  const verbose = container.querySelector('#dbg-verbose');
  if (verbose) {
    verbose.addEventListener('change', async (e) => {
      try { await dashboardHandler.setDebugLogging(!!e.target.checked); }
      catch (err) { console.error('Failed to set debug logging', err); }
    });
  }

  const refreshBtn = container.querySelector('#dbg-refresh');
  if (refreshBtn) refreshBtn.addEventListener('click', () => refresh());

  const rangeToggleBtn = container.querySelector('#dbg-range-toggle');
  if (rangeToggleBtn) {
    rangeToggleBtn.addEventListener('click', () => {
      _state.rangeMode = (mode === '7d') ? '24h' : '7d';
      refresh();
    });
  }

  if (container.__dbgSelectHandler) {
    container.removeEventListener('dbg-select', container.__dbgSelectHandler);
  }
  container.__dbgSelectHandler = async (e) => {
    const item = e.detail?.item;
    if (!item) return;
    _state.selectedRunId = item.kind === 'run' ? (item.id || '') : '';
    _state.selectedContextItem = item.kind === 'run' ? null : item;
    _state.selectedScriptId = item.scriptId || '';
    _state.runDetails = null;
    _state.selectedExecutionStep = -1;
    _state.replay = { open: false, runId: '', loading: false, error: '', manifest: null, frameIndex: 0, playing: false };

    // If we have a real run id, fetch details (failures only currently).
    if (_state.selectedRunId) {
      try {
        const res = await dashboardHandler.getRunDetails(_state.selectedRunId);
        const run = toPlain(res?.run ?? res);
        _state.runDetails = run?.ok ? run : null;
      } catch (err) {
        _state.runDetails = null;
      }
    }
    await refresh();
  };
  container.addEventListener('dbg-select', container.__dbgSelectHandler, {
    once: true,
  });

  // Wire incident actions.
  const openScriptBtn = container.querySelector('#dbg-open-script');
  if (openScriptBtn) {
    openScriptBtn.addEventListener('click', () => {
      if (_state.selectedScriptId && opts?.openScript) opts.openScript(_state.selectedScriptId);
    });
  }

  const queueAutoHealBtn = container.querySelector('#dbg-queue-auto-heal');
  if (queueAutoHealBtn) {
    const canQueue = !!selectedItem;
    queueAutoHealBtn.disabled = !canQueue;
    queueAutoHealBtn.addEventListener('click', () => {
      if (!canQueue || typeof opts?.onQueueAutoHeal !== 'function')
        return;
      const incident = buildAutoHealIncidentPayload(
          data, appState, selectedItem, _state.runDetails);
      if (!incident)
        return;
      opts.onQueueAutoHeal(incident);
    });
  }

  const askDashboardModelBtn = container.querySelector('#dbg-ask-dashboard-model');
  if (askDashboardModelBtn) {
    const canAsk = !!selectedItem;
    askDashboardModelBtn.disabled = !canAsk;
    askDashboardModelBtn.addEventListener('click', () => {
      if (!canAsk || typeof opts?.onAskDashboardModel !== 'function')
        return;
      const prompt = buildDashboardModelFailurePrompt(
          selectedItem, _state.runDetails, appState);
      opts.onAskDashboardModel(prompt, {
        selectedItem: toPlain(selectedItem),
        runDetails: toPlain(_state.runDetails),
      });
    });
  }

  const qProcessBtn = container.querySelector('#dbg-q-process');
  if (qProcessBtn) {
    qProcessBtn.addEventListener('click', () => {
      if (typeof opts?.onAutoHealProcessNext === 'function') {
        opts.onAutoHealProcessNext();
      }
    });
  }

  const qCancelCurrentBtn = container.querySelector('#dbg-q-cancel-current');
  if (qCancelCurrentBtn) {
    qCancelCurrentBtn.addEventListener('click', () => {
      if (typeof opts?.onAutoHealCancelCurrent === 'function') {
        opts.onAutoHealCancelCurrent();
      }
    });
  }

  const qClearBtn = container.querySelector('#dbg-q-clear');
  if (qClearBtn) {
    qClearBtn.addEventListener('click', () => {
      if (typeof opts?.onAutoHealClearQueue === 'function') {
        opts.onAutoHealClearQueue();
      }
    });
  }

  const queueList = container.querySelector('#dbg-autoheal-list');
  if (queueList) {
    queueList.addEventListener('click', (event) => {
      const btn = event.target && event.target.closest
        ? event.target.closest('[data-q-act][data-key]')
        : null;
      if (!btn)
        return;
      const act = String(btn.getAttribute('data-q-act') || '');
      const key = String(btn.getAttribute('data-key') || '');
      if (!key)
        return;
      if (act === 'remove' && typeof opts?.onAutoHealRemoveQueueItem === 'function') {
        opts.onAutoHealRemoveQueueItem(key);
        return;
      }
      if (act === 'up' && typeof opts?.onAutoHealMoveQueueItem === 'function') {
        opts.onAutoHealMoveQueueItem(key, -1);
        return;
      }
      if (act === 'down' && typeof opts?.onAutoHealMoveQueueItem === 'function') {
        opts.onAutoHealMoveQueueItem(key, 1);
        return;
      }
      if (act === 'heal-now' && typeof opts?.onAutoHealPrioritizeQueueItem === 'function') {
        opts.onAutoHealPrioritizeQueueItem(key);
      }
    });
  }

  const resumeBtn = container.querySelector('#dbg-resume-step');
  const resumeForm = container.querySelector('#dbg-resume-form');
  if (resumeBtn && resumeForm) {
    resumeBtn.addEventListener('click', () => {
      resumeForm.classList.toggle('hidden');
    });
  }

  const stepsEl = container.querySelector('#dbg-steps');
  if (stepsEl && _state.runDetails) {
    const handleStepPick = (step) => {
      _state.selectedExecutionStep = step;
      const input = container.querySelector('#dbg-step-index');
      if (input) input.value = `${step}`;
      if (resumeForm) resumeForm.classList.remove('hidden');
      void refresh();
    };
    renderSteps(stepsEl, _state.runDetails, handleStepPick);
  }

  const resumeGo = container.querySelector('#dbg-resume-go');
  if (resumeGo) {
    resumeGo.addEventListener('click', async () => {
      const input = container.querySelector('#dbg-step-index');
      const stepIndex = input ? Number(input.value) : NaN;
      if (!_state.selectedScriptId || !Number.isFinite(stepIndex)) return;
      try {
        const res = await dashboardHandler.resumeScriptFromStep(
          _state.selectedScriptId,
          stepIndex,
          _state.selectedRunId || ''
        );
        if (!res?.success) {
          alert(res?.error || 'Resume failed');
        }
      } catch (err) {
        alert(err?.message || 'Resume failed');
      }
    });
  }

  // Replay viewer.
  const replayBtn = container.querySelector('#dbg-view-replay');
  const replayEl = container.querySelector('#dbg-replay');
  const canReplay = !!(selectedItem && selectedItem.kind === 'run' && (selectedItem.status === 'error'));
  if (replayBtn) {
    replayBtn.disabled = !canReplay;
    const openForThisRun = _state.replay.open && _state.replay.runId === _state.selectedRunId;
    replayBtn.textContent = openForThisRun ? 'Hide error replay' : 'View error replay';
    replayBtn.addEventListener('click', async () => {
      if (!canReplay || !_state.selectedRunId) return;
      const runId = _state.selectedRunId;
      const currentlyOpen = _state.replay.open && _state.replay.runId === runId;
      if (currentlyOpen) {
        _state.replay.open = false;
        _state.replay.playing = false;
        await refresh();
        return;
      }

      _state.replay = {
        open: true,
        runId,
        loading: true,
        error: '',
        manifest: null,
        frameIndex: 0,
        playing: false,
      };
      await refresh();
      try {
        // Prefer inlined manifest from GetRunDetails to avoid fetch() issues.
        const inlined = _state.runDetails?.replay_manifest_json || _state.runDetails?.replayManifestJson;
        const manifest = inlined || await fetchReplayManifest(runId);
        _state.replay.manifest = manifest || null;
        _state.replay.loading = false;
      } catch (err) {
        _state.replay.loading = false;
        _state.replay.error = err?.message || 'Failed to load replay';
      }
      await refresh();
    });
  }

  if (replayEl) {
    const openForThisRun = _state.replay.open && _state.replay.runId === _state.selectedRunId && canReplay;
    if (!openForThisRun) {
      replayEl.classList.add('hidden');
    } else {
      replayEl.classList.remove('hidden');
      if (_state.replay.loading) {
        replayEl.innerHTML = `<div class="dbg-replay-title">Error replay</div><div class="dbg-replay-sub">Loading…</div>`;
      } else if (_state.replay.error) {
        replayEl.innerHTML = `<div class="dbg-replay-title">Error replay</div><div class="dbg-replay-error">${_state.replay.error}</div>`;
      } else if (!_state.replay.manifest?.frames?.length) {
        replayEl.innerHTML = `<div class="dbg-replay-title">Error replay</div><div class="dbg-replay-sub">No frames available.</div>`;
      } else {
        const frames = _state.replay.manifest.frames;
        const fps = Number(_state.replay.manifest.fps || 2);
        const maxIndex = frames.length - 1;
        const idx = clamp(_state.replay.frameIndex, 0, maxIndex);
        _state.replay.frameIndex = idx;
        const f = frames[idx];
        const label = `${idx + 1}/${frames.length} • ${Math.round(Number(f.t_ms || 0))}ms`;

        replayEl.innerHTML = `
          <div class="dbg-replay-title">Error replay</div>
          <div class="dbg-replay-player">
            <img id="dbg-replay-img" alt="Replay frame" src="${replayFrameUrl(_state.replay.runId, f.file)}" />
            <div class="dbg-replay-controls">
              <button class="btn btn-secondary" id="dbg-replay-play">${_state.replay.playing ? 'Pause' : 'Play'}</button>
              <input id="dbg-replay-slider" type="range" min="0" max="${maxIndex}" value="${idx}" />
              <div class="dbg-replay-label" id="dbg-replay-label">${label}</div>
            </div>
          </div>
        `;

        const img = container.querySelector('#dbg-replay-img');
        const slider = container.querySelector('#dbg-replay-slider');
        const playBtn = container.querySelector('#dbg-replay-play');
        const labelEl = container.querySelector('#dbg-replay-label');

        const updateFrame = (newIndex) => {
          const i = clamp(Number(newIndex), 0, maxIndex);
          _state.replay.frameIndex = i;
          const fr = frames[i];
          if (img) img.src = replayFrameUrl(_state.replay.runId, fr.file);
          if (slider) slider.value = `${i}`;
          if (labelEl) labelEl.textContent = `${i + 1}/${frames.length} • ${Math.round(Number(fr.t_ms || 0))}ms`;
        };

        if (slider) {
          slider.addEventListener('input', (e) => {
            _state.replay.playing = false;
            stopReplayTimer();
            updateFrame(e.target.value);
            if (playBtn) playBtn.textContent = 'Play';
          });
        }

        if (playBtn) {
          playBtn.addEventListener('click', () => {
            _state.replay.playing = !_state.replay.playing;
            if (!_state.replay.playing) {
              stopReplayTimer();
              playBtn.textContent = 'Play';
              return;
            }
            playBtn.textContent = 'Pause';
            stopReplayTimer();
            const interval = Math.max(50, Math.round(1000 / (Number.isFinite(fps) && fps > 0 ? fps : 2)));
            _replayTimer = setInterval(() => {
              if (!_state.replay.playing) {
                stopReplayTimer();
                return;
              }
              const next = _state.replay.frameIndex + 1;
              if (next > maxIndex) {
                _state.replay.playing = false;
                stopReplayTimer();
                playBtn.textContent = 'Play';
                return;
              }
              updateFrame(next);
            }, interval);
          });
        }
      }
    }
  }

  // Auto-refresh: faster while auto-heal is running so progress stays visible.
  if (_refreshTimer) clearTimeout(_refreshTimer);
  if (!_state.replay.playing && isActive()) {
    const healingActive = !!(opts?.getAutoHealQueueState?.()?.in_flight);
    const refreshMs = healingActive ? 2500 : 10000;
    _refreshTimer = setTimeout(() => {
      if (_lastContainer === container && isActive() &&
          !isDebugViewInteractionActive(container)) {
        container.dispatchEvent(new CustomEvent('dbg-refresh', { bubbles: true }));
      } else {
        _refreshTimer = setTimeout(() => {
          if (_lastContainer === container && isActive() &&
              !isDebugViewInteractionActive(container)) {
            container.dispatchEvent(new CustomEvent('dbg-refresh', { bubbles: true }));
          } else {
            _refreshTimer = null;
          }
        }, refreshMs);
      }
    }, refreshMs);
  }
  container.__dbgRendering = false;
}

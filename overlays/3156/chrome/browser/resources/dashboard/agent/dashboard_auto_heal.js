export function createDashboardAutoHealManager({
  windowRef = window,
  dashboardHandler,
  autoHealState,
  sendPrompt,
  queueMax = 120,
  recentKeysMax = 600,
}) {
  if (!windowRef || typeof windowRef !== 'object') {
    return null;
  }
  if (!dashboardHandler || typeof dashboardHandler !== 'object') {
    return null;
  }
  if (!autoHealState || typeof autoHealState !== 'object') {
    return null;
  }

  const scriptIdForIncident = (failureLike) => {
    if (!failureLike || typeof failureLike !== 'object')
      return '';
    const incident = failureLike.incident || failureLike;
    return String(
        incident.selected_script_id ||
        incident.script_id ||
        incident.playback?.script_id ||
        incident.selected_item?.scriptId ||
        incident.listener_script_id ||
        '').trim();
  };

  // Prefer a stable per-run key so multiple failures from the same script can
  // sit in the queue at once. Exact-key dedupe still prevents spamming the same
  // run. (One-per-script hid everything except the first and made Clear feel
  // like it needed dozens of clicks.)
  const autoHealKeyFor = (failureLike) => {
    if (!failureLike || typeof failureLike !== 'object')
      return '';
    const incident = failureLike.incident || failureLike;
    const runId = String(
        incident.selected_run_id ||
        incident.run_id ||
        incident.id ||
        incident.selected_item?.id ||
        '').trim();
    const scriptId = scriptIdForIncident(failureLike);
    if (runId && scriptId)
      return `run:${runId}:script:${scriptId}`;
    if (runId)
      return `run:${runId}`;
    if (scriptId) {
      const step = Number(
          incident.selected_failed_step ??
          incident.failed_step ??
          incident.selected_item?.failedStep ??
          -1);
      const err = String(
          incident.selected_error ||
          incident.error ||
          incident.selected_item?.error ||
          '').slice(0, 120);
      return `script:${scriptId}:step:${Number.isFinite(step) ? step : -1}:err:${err}`;
    }
    const explicitKey = String(
        incident.failure_key ||
        incident.key ||
        incident.event_key ||
        '').trim();
    if (explicitKey)
      return explicitKey;
    const when = Number(
        incident.selected_start_ms ||
        incident.ts_ms ||
        incident.started_ms ||
        Date.now());
    return `anon:${when}`;
  };

  const rememberAutoHealKey = (key) => {
    const k = String(key || '').trim();
    if (!k) return;
    autoHealState.knownFailureKeys.add(k);
    if (autoHealState.knownFailureKeys.size > recentKeysMax) {
      const iter = autoHealState.knownFailureKeys.values();
      const first = iter.next();
      if (!first.done) {
        autoHealState.knownFailureKeys.delete(first.value);
      }
    }
  };

  const enqueueAutoHealIncident = (incident, source = 'manual') => {
    if (!incident || typeof incident !== 'object')
      return {ok: false, error: 'invalid_incident'};
    const scriptId = scriptIdForIncident(incident);
    const key = autoHealKeyFor(incident);
    if (!key)
      return {ok: false, error: 'missing_incident_key'};

    // Skip exact duplicates (same run / same step+error). Multiple failures
    // from one script are allowed so the Debugging queue lists them all.
    if (autoHealState.knownFailureKeys.has(key)) {
      return {ok: true, queued: false, duplicate: true, key, script_id: scriptId};
    }
    if (autoHealState.queue.some((q) => q && q.key === key)) {
      return {ok: true, queued: false, duplicate: true, key, script_id: scriptId};
    }
    if (autoHealState.inFlight &&
        String(autoHealState.currentTask?.key || '') === key) {
      return {
        ok: true,
        queued: false,
        duplicate: true,
        key,
        script_id: scriptId,
        in_flight: true,
        queue_len: autoHealState.queue.length,
      };
    }
    autoHealState.queue.push({
      key,
      source: String(source || 'manual'),
      queuedAt: Date.now(),
      incident: JSON.parse(JSON.stringify(incident)),
    });
    if (autoHealState.queue.length > queueMax) {
      autoHealState.queue.splice(0, autoHealState.queue.length - queueMax);
    }
    rememberAutoHealKey(key);
    return {
      ok: true,
      queued: true,
      key,
      script_id: scriptId,
      queue_len: autoHealState.queue.length,
    };
  };

  const slimValue = (value, maxChars = 1200) => {
    try {
      const raw = JSON.stringify(value);
      if (raw.length <= maxChars) {
        return raw;
      }
      return `${raw.slice(0, maxChars)}…`;
    } catch (_) {
      return '';
    }
  };

  const slimInvestigationForPrompt = (investigation) => {
    if (!investigation || typeof investigation !== 'object') {
      return null;
    }
    return {
      ok: investigation.ok !== false,
      error: investigation.error ?
        String(investigation.error).slice(0, 240) :
        undefined,
      transport: investigation.transport,
      url: investigation.url,
      title: investigation.title,
      ready_state: investigation.ready_state || investigation.readyState,
      element_count:
          investigation.element_count ?? investigation.elementCount,
      locator_probe:
          investigation.locator_probe || investigation.locatorProbe,
      inner_text_excerpt: String(
          investigation.inner_text_excerpt ||
          investigation.innerTextExcerpt || '').slice(0, 400),
      links: Array.isArray(investigation.links) ?
        investigation.links.slice(0, 6) :
        undefined,
      inputs: Array.isArray(investigation.inputs) ?
        investigation.inputs.slice(0, 6) :
        undefined,
      buttons: Array.isArray(investigation.buttons) ?
        investigation.buttons.slice(0, 6) :
        undefined,
    };
  };

  const buildAutoHealPrompt = (task, investigation = null) => {
    const incident = (task && task.incident && typeof task.incident === 'object')
      ? task.incident
      : {};
    const selected = (incident.selected_item && typeof incident.selected_item === 'object')
      ? incident.selected_item
      : {};
    const playback = (incident.playback && typeof incident.playback === 'object')
      ? incident.playback
      : {};
    const scriptId = String(
        incident.selected_script_id ||
        playback.script_id ||
        selected.scriptId ||
        '');
    const scriptName = String(
        incident.selected_script_name ||
        selected.scriptName ||
        selected.script_name ||
        '');
    const failedStep = Number(
        incident.selected_failed_step ??
        playback.failed_step ??
        selected.failedStep ??
        -1);
    const resumeStep = failedStep >= 0 ? failedStep : 0;
    const error = String(
        incident.selected_error ||
        selected.error ||
        incident.error ||
        '').slice(0, 400);
    const url = resolveIncidentUrl(incident);
    const failedAction = (incident.failed_action &&
                          typeof incident.failed_action === 'object')
      ? incident.failed_action
      : null;
    const probe = (incident.investigation_probe &&
                   typeof incident.investigation_probe === 'object')
      ? incident.investigation_probe
      : null;
    const mismatchFromIncident = (incident.context_url_mismatch &&
                      typeof incident.context_url_mismatch === 'object')
      ? incident.context_url_mismatch
      : null;
    let mismatch = mismatchFromIncident;
    if (!mismatch) {
      const m = error.match(
          /Context URL mismatch \(expected=([^)]*) actual=([^)]*)\)/i);
      if (m) {
        mismatch = {
          mismatch: true,
          expected_context_url: String(m[1] || '').trim() || undefined,
          observed_context_url: String(m[2] || '').trim() || undefined,
          note:
            'Run history URL differs from the action contextUrl. The failure ' +
            'may be from executing on the wrong page.',
        };
      }
    }
    const source = String(incident.source || task?.source || 'unknown');
    const card = {
      source,
      script_id: scriptId || undefined,
      script_name: scriptName || undefined,
      failed_step: failedStep,
      resume_step: resumeStep,
      error: error || undefined,
      url: url || undefined,
      failed_action: failedAction || undefined,
      probe: probe || undefined,
      context_url_mismatch: mismatch || undefined,
      pre_investigate: slimInvestigationForPrompt(investigation) || undefined,
    };
    const lines = [
      'Auto-heal. Fix the failed step only, then verify, then done.',
      `Workflow: select_script -> get_step ${failedStep >= 0 ? failedStep : 'N'} -> optional investigate -> update_step -> resume_script/run_script -> done.`,
      'Headless :9222 only for page probes. No headed tabs. No queue heal.',
    ];
    if (mismatch) {
      lines.push(
          'IMPORTANT: contextUrl mismatch — run history URL differs from the ' +
          'action contextUrl. The step may have run on the wrong page; fix ' +
          'navigation/context before changing locators.',
          `expected_context_url=${String(mismatch.expected_context_url || '')}`,
          `observed_run_url=${String(mismatch.observed_context_url || '')}`,
      );
    }
    lines.push(`Brief:${slimValue(card, 4500)}`);
    return lines.join('\n');
  };

  const setProgress = (patch = {}) => {
    const prev = (autoHealState.progress && typeof autoHealState.progress === 'object')
      ? autoHealState.progress
      : {};
    autoHealState.progress = {
      ...prev,
      ...(patch && typeof patch === 'object' ? patch : {}),
      updated_at: Date.now(),
    };
  };

  const clearProgress = () => {
    autoHealState.progress = null;
  };

  const summarizeTask = (task) => {
    const incident = (task && task.incident && typeof task.incident === 'object')
      ? task.incident
      : {};
    const selected = (incident.selected_item && typeof incident.selected_item === 'object')
      ? incident.selected_item
      : {};
    return {
      key: String(task?.key || ''),
      source: String(task?.source || ''),
      queued_at: Number(task?.queuedAt || 0),
      script_name: String(
        incident.selected_script_name ||
        selected.scriptName ||
        selected.script_name ||
        ''),
      error: String(
        incident.selected_error ||
        selected.error ||
        ''),
      failed_step: Number(incident.selected_failed_step ?? selected.failedStep ?? -1),
    };
  };

  const resolveIncidentUrl = (incident) => {
    if (!incident || typeof incident !== 'object') {
      return '';
    }
    const selected = (incident.selected_item && typeof incident.selected_item === 'object')
      ? incident.selected_item
      : {};
    const playback = (incident.playback && typeof incident.playback === 'object')
      ? incident.playback
      : {};
    const candidates = [
      incident.url,
      incident.page_url,
      incident.pageUrl,
      incident.current_url,
      incident.currentUrl,
      incident.selected_url,
      incident.selectedUrl,
      selected.url,
      selected.current_url,
      selected.currentUrl,
      playback.url,
      playback.start_url,
      playback.startUrl,
      incident.start_url,
      incident.startUrl,
    ];
    for (const value of candidates) {
      const url = String(value || '').trim();
      if (url) {
        return url;
      }
    }
    return '';
  };

  const openHeadlessInvestigationForTask = async (task) => {
    const fn = (typeof windowRef.dashboardAgentInvestigatePage === 'function')
      ? windowRef.dashboardAgentInvestigatePage.bind(windowRef)
      : null;
    if (typeof fn !== 'function') {
      return {ok: false, error: 'dashboard_investigate_api_unavailable'};
    }
    const incident = (task && task.incident && typeof task.incident === 'object')
      ? task.incident
      : {};
    const scriptId = scriptIdForIncident(incident);
    const scriptName = String(
        incident.selected_script_name ||
        incident.script_name ||
        scriptId ||
        'script');
    const probe = (incident.investigation_probe &&
                   typeof incident.investigation_probe === 'object')
      ? incident.investigation_probe
      : null;
    const url = resolveIncidentUrl(incident);
    const actions = [];
    if (url) {
      actions.push({action: 'navigate', params: {url}});
      actions.push({
        action: 'wait',
        params: {waitType: 'time', time: 800, timeout: 0, locators: []},
      });
    }
    // Probe on isolated headless Chromium (:9222). Close the CDP target when
    // done so investigation does not leave tabs in either browser.
    const payload = {
      url: url || undefined,
      actions,
      probe: probe || undefined,
      closeTarget: true,
      close_target: true,
      open_reason: 'auto_heal',
      openReason: 'auto_heal',
      label: `Auto-heal: ${scriptName}`,
      name: `Auto-heal: ${scriptName}`,
      include_screenshot: false,
    };
    try {
      return await Promise.resolve(fn(payload));
    } catch (e) {
      return {ok: false, error: String(e && e.message ? e.message : e)};
    }
  };

  const queueStateSnapshot = () => {
    const progress = (autoHealState.progress && typeof autoHealState.progress === 'object')
      ? {...autoHealState.progress}
      : null;
    const waiting = (autoHealState.queue || []).map(summarizeTask);
    // Surface the in-flight task in the list so Debugging shows the full
    // backlog (current + waiting), not just waiting items.
    const queueForUi = [];
    if (autoHealState.inFlight && autoHealState.currentTask) {
      queueForUi.push({
        ...summarizeTask(autoHealState.currentTask),
        in_flight: true,
      });
    }
    for (const row of waiting) {
      if (row?.key &&
          String(row.key) === String(autoHealState.currentTask?.key || '')) {
        continue;
      }
      queueForUi.push(row);
    }
    return {
      enabled: !!autoHealState.autoQueueEnabled,
      in_flight: !!autoHealState.inFlight,
      current_key: String(autoHealState.currentTask?.key || ''),
      current: autoHealState.currentTask ? summarizeTask(autoHealState.currentTask) : null,
      progress,
      queue: queueForUi,
      queue_waiting_len: waiting.length,
    };
  };

  const startAutoHealTask = async (task) => {
    if (typeof sendPrompt !== 'function') {
      return {ok: false, error: 'agent_console_not_ready'};
    }
    const token = ++autoHealState.operationToken;
    autoHealState.currentTask = task;
    autoHealState.inFlight = true;
    setProgress({
      phase: 'starting',
      turn: 0,
      max_turns: 0,
      command: '',
      detail: 'Starting auto-heal agent…',
      started_at: Date.now(),
      task_key: String(task?.key || ''),
    });

    // Deterministically open a headless (:9222) investigation probe before the
    // model loop so heal does not depend on the model remembering to investigate.
    let investigation = null;
    if (autoHealState.preferHeadlessInvestigation) {
      setProgress({
        phase: 'investigate',
        command: 'dashboard_investigate_page',
        detail: 'Opening headless (:9222) investigation probe…',
      });
      investigation = await openHeadlessInvestigationForTask(task);
      if (investigation && investigation.ok === false) {
        console.warn('[AutoHeal] investigation open failed', investigation);
        setProgress({
          phase: 'investigate',
          command: 'dashboard_investigate_page',
          detail: `Investigation open failed: ${String(
              investigation.error || 'unknown')}`,
        });
      } else if (investigation?.ok) {
        setProgress({
          phase: 'investigate',
          command: 'dashboard_investigate_page',
          detail: `Headless investigation ok transport=${String(
              investigation.transport || 'headless_listener')} target=${String(
              investigation.target_id || investigation.task_id ||
              investigation.taskId || '')}`,
          investigation_task_id: String(
              investigation.target_id || investigation.task_id ||
              investigation.taskId || ''),
        });
      }
    }

    const prompt = buildAutoHealPrompt(task, investigation);
    try {
      const sendRes = await Promise.resolve(sendPrompt(prompt, {
        showContextInChat: false,
        onProgress: (patch) => {
          if (autoHealState.operationToken !== token) {
            return;
          }
          setProgress(patch);
        },
      }));
      if (!sendRes || sendRes.ok === false) {
        if (autoHealState.operationToken === token) {
          autoHealState.inFlight = false;
          autoHealState.currentTask = null;
          setProgress({
            phase: 'failed',
            detail: String(sendRes?.error || 'send_prompt_failed'),
          });
        }
        return {
          ok: false,
          error: String(sendRes?.error || 'send_prompt_failed'),
          investigation,
        };
      }
      if (autoHealState.operationToken === token) {
        setProgress({
          phase: 'completed',
          command: '',
          detail: String(sendRes?.result?.summary || sendRes?.result?.status || 'done'),
        });
      }
      return {
        ok: true,
        task_key: String(task?.key || ''),
        investigation,
      };
    } catch (e) {
      if (autoHealState.operationToken === token) {
        autoHealState.inFlight = false;
        autoHealState.currentTask = null;
        setProgress({
          phase: 'failed',
          detail: String(e && e.message ? e.message : e),
        });
      }
      return {
        ok: false,
        error: String(e && e.message ? e.message : e),
        investigation,
      };
    } finally {
      if (autoHealState.operationToken === token) {
        // Allow a later failure of the same script to re-queue after this heal
        // attempt finishes (resume covers errors during the current attempt).
        const finishedKey = String(task?.key || '').trim();
        if (finishedKey) {
          autoHealState.knownFailureKeys.delete(finishedKey);
        }
        autoHealState.inFlight = false;
        autoHealState.currentTask = null;
        if (autoHealState.autoQueueEnabled && autoHealState.queue.length) {
          void processAutoHealQueue();
        } else {
          // Keep last progress briefly visible, then clear on next idle snapshot.
          globalThis.setTimeout(() => {
            if (!autoHealState.inFlight && autoHealState.operationToken === token) {
              clearProgress();
            }
          }, 8000);
        }
      }
    }
  };

  const cancelCurrentAutoHeal = (reason = 'user_cancel') => {
    if (!autoHealState.inFlight) {
      return {ok: true, canceled: false, reason: 'no_active_task'};
    }
    autoHealState.operationToken += 1;
    const prior = autoHealState.currentTask;
    const priorKey = String(prior?.key || '').trim();
    if (priorKey) {
      autoHealState.knownFailureKeys.delete(priorKey);
    }
    autoHealState.inFlight = false;
    autoHealState.currentTask = null;
    setProgress({
      phase: 'canceled',
      command: '',
      detail: `Canceled (${String(reason || 'user_cancel')})`,
    });
    return {
      ok: true,
      canceled: true,
      reason: String(reason || 'user_cancel'),
      prior_key: priorKey,
    };
  };

  const processAutoHealQueue = async () => {
    if (autoHealState.inFlight) {
      return {ok: true, status: 'busy', queue_len: autoHealState.queue.length};
    }
    if (!autoHealState.queue.length) {
      return {ok: true, status: 'idle', queue_len: 0};
    }
    const task = autoHealState.queue.shift();
    // Start asynchronously so Heal Now / Process next return immediately.
    // Awaiting the full local-model loop made prioritize appear to "fail".
    void startAutoHealTask(task).then((started) => {
      if (!started?.ok) {
        console.warn('[AutoHeal] task failed', started);
      }
    });
    return {
      ok: true,
      status: 'started',
      task_key: task.key,
      queue_len: autoHealState.queue.length,
    };
  };

  const updateAutoHealSettings = (settingsObj) => {
    const autoHeal = (settingsObj && typeof settingsObj === 'object')
      ? (settingsObj.autoHeal || {})
      : {};
    autoHealState.autoQueueEnabled = autoHeal.autoQueueFailures === true;
    autoHealState.preferHeadlessInvestigation =
      autoHeal.investigateInHeadless !== false;
  };

  const removeQueueItemByKey = (key) => {
    const wanted = String(key || '').trim();
    if (!wanted) return {ok: false, error: 'missing_key'};
    const before = autoHealState.queue.length;
    autoHealState.queue = autoHealState.queue.filter((task) => String(task?.key || '') !== wanted);
    if (wanted) {
      autoHealState.knownFailureKeys.delete(wanted);
    }
    return {ok: true, removed: before - autoHealState.queue.length, queue_len: autoHealState.queue.length};
  };

  const moveQueueItemByKey = (key, delta) => {
    const wanted = String(key || '').trim();
    if (!wanted) return {ok: false, error: 'missing_key'};
    const step = Number(delta);
    if (!Number.isFinite(step) || step === 0) return {ok: false, error: 'invalid_delta'};
    const index = autoHealState.queue.findIndex((task) => String(task?.key || '') === wanted);
    if (index < 0) return {ok: false, error: 'queue_item_not_found'};
    const nextIndex = Math.max(0, Math.min(autoHealState.queue.length - 1, index + (step < 0 ? -1 : 1)));
    if (nextIndex === index) return {ok: true, moved: false};
    const [task] = autoHealState.queue.splice(index, 1);
    autoHealState.queue.splice(nextIndex, 0, task);
    return {ok: true, moved: true, queue_len: autoHealState.queue.length};
  };

  const prioritizeQueueItem = async (key) => {
    const wanted = String(key || '').trim();
    if (!wanted) {
      return {ok: false, error: 'missing_key'};
    }
    // Already healing this item — treat as success (don't restart mid-flight).
    if (autoHealState.inFlight &&
        String(autoHealState.currentTask?.key || '') === wanted) {
      return {
        ok: true,
        status: 'already_running',
        task_key: wanted,
        queue_len: autoHealState.queue.length,
      };
    }
    const index = autoHealState.queue.findIndex(
        (task) => String(task?.key || '') === wanted);
    if (index < 0) {
      return {ok: false, error: 'queue_item_not_found'};
    }
    const [task] = autoHealState.queue.splice(index, 1);
    const prior = autoHealState.currentTask;
    if (autoHealState.inFlight) {
      cancelCurrentAutoHeal('prioritize');
    }
    if (prior && prior.key && String(prior.key) !== wanted) {
      autoHealState.queue.unshift(prior);
    }
    autoHealState.queue.unshift(task);
    return processAutoHealQueue();
  };

  const clearQueue = () => {
    // Cancel any in-flight heal and wipe dedupe keys so Clear empties the
    // whole queue in one click (and Debugging snapshot won't immediately
    // re-inject the same incidents on the next refresh).
    autoHealState.operationToken += 1;
    autoHealState.inFlight = false;
    autoHealState.currentTask = null;
    autoHealState.progress = null;
    autoHealState.queue = [];
    autoHealState.knownFailureKeys.clear();
    autoHealState.suppressAutoQueueUntil = Date.now() + 5000;
    return {ok: true, queue_len: 0};
  };

  const buildIncidentFromFailureRow = (rows, row) => {
    const match = (row && typeof row === 'object') ? row : {};
    return {
      source: 'debugging_timeline',
      selected_item: {
        kind: match.kind === 'listener_error' ? 'listener_event' : 'run',
        id: match.run_id || '',
        scriptId: match.script_id || match.listener_script_id || '',
        scriptName: match.script_name || match.listener_script_name || '',
        status: 'error',
        failedStep: Number(match.failed_step ?? -1),
        error: String(match.error || match.detail || ''),
        startMs: Number(match.started_ms || match.ts_ms || 0),
        endMs: Number(match.ended_ms || match.ts_ms || 0),
      },
      selected_run_id: match.run_id || '',
      selected_script_id: match.script_id || match.listener_script_id || '',
      selected_script_name: match.script_name || match.listener_script_name || '',
      selected_status: 'error',
      selected_failed_step: Number(match.failed_step ?? -1),
      selected_error: String(match.error || match.detail || ''),
      selected_start_ms: Number(match.started_ms || match.ts_ms || 0),
      selected_end_ms: Number(match.ended_ms || match.ts_ms || 0),
      run_details: null,
      listener_event: match.kind === 'listener_error' ? (match.raw || match) : null,
      timeline_failures: Array.isArray(rows) ? rows.slice(0, 120) : [],
    };
  };

  const isListenerTimelineNoise = (row) => {
    if (!row || typeof row !== 'object') {
      return false;
    }
    // Listener host/match errors (native_executor_fallback, key_script_*, etc.)
    // show up as timeline rows with listener_script_id and no healable step.
    // Auto-queuing them monopolizes the heal queue and correlates with opening
    // Debugging — they are too common and rarely fixable like a script step.
    const listenerId = String(
        row.listener_script_id || row.listenerScriptId || '').trim();
    if (!listenerId) {
      return false;
    }
    const failedStep = Number(row.failed_step ?? row.failedStep ?? -1);
    if (Number.isFinite(failedStep) && failedStep >= 0) {
      return false;
    }
    return true;
  };

  const autoQueueFromFailureSnapshot = (snapshotFailures) => {
    if (Number(autoHealState.suppressAutoQueueUntil || 0) > Date.now()) {
      return;
    }
    const rows = Array.isArray(snapshotFailures) ? snapshotFailures : [];
    autoHealState.lastFailures = rows.slice(0, 220);
    if (!autoHealState.autoQueueEnabled) {
      return;
    }
    for (const row of autoHealState.lastFailures.slice(0, 60)) {
      if (isListenerTimelineNoise(row)) {
        continue;
      }
      const key = autoHealKeyFor(row);
      if (!key || autoHealState.knownFailureKeys.has(key))
        continue;
      const incident = buildIncidentFromFailureRow(autoHealState.lastFailures, row);
      enqueueAutoHealIncident(incident, 'debugging_timeline');
    }
    if (!autoHealState.inFlight && autoHealState.queue.length) {
      globalThis.setTimeout(() => {
        if (autoHealState.autoQueueEnabled && !autoHealState.inFlight &&
            autoHealState.queue.length) {
          void processAutoHealQueue();
        }
      }, 0);
    }
  };

  const listTimelineFailures = () => {
    const rows = Array.isArray(autoHealState.lastFailures)
      ? autoHealState.lastFailures
      : [];
    return {
      ok: true,
      count: rows.length,
      queue_len: autoHealState.queue.length,
      in_flight: !!autoHealState.inFlight,
      current_key: String(autoHealState.currentTask?.key || ''),
      rows: rows.slice(0, 120),
    };
  };

  const queueAutoHealFromPayload = async (payload = {}) => {
    const failureKey = String(payload?.failure_key || payload?.key || '').trim();
    const incident = (payload && typeof payload.incident === 'object')
      ? payload.incident
      : null;
    if (incident) {
      const enq = enqueueAutoHealIncident(incident, 'agent_command');
      if (enq.ok && autoHealState.autoQueueEnabled) {
        processAutoHealQueue();
      }
      return enq;
    }
    if (!failureKey) {
      return {ok: false, error: 'missing_failure_key_or_incident'};
    }
    const match = (autoHealState.lastFailures || []).find(
        (row) => String(row?.key || '') === failureKey);
    if (!match) {
      return {ok: false, error: `failure_key_not_found:${failureKey}`};
    }
    const incidentFromKey = buildIncidentFromFailureRow(autoHealState.lastFailures, match);
    incidentFromKey.source = 'agent_command';
    const enq = enqueueAutoHealIncident(incidentFromKey, 'agent_command');
    if (enq.ok && autoHealState.autoQueueEnabled) {
      processAutoHealQueue();
    }
    return enq;
  };

  const syncAutoHealSettingsFromBackend = async () => {
    try {
      const res = await dashboardHandler.getEffectiveSettings('');
      const settings = (res && res.settings) ? res.settings : {};
      updateAutoHealSettings(settings);
      if (autoHealState.autoQueueEnabled && autoHealState.queue.length) {
        processAutoHealQueue();
      }
    } catch (e) {
      console.warn('[AutoHeal] failed to load settings', e);
    }
  };

  const installAgentApi = () => {
    windowRef.dashboardAgentListTimelineFailures = () => listTimelineFailures();
    windowRef.dashboardAgentQueueAutoHeal = async (payload = {}) =>
      queueAutoHealFromPayload(payload);
    windowRef.dashboardAgentProcessAutoHealQueue = async () =>
      processAutoHealQueue();
    windowRef.dashboardAgentGetAutoHealQueueState = () => ({
      ok: true,
      ...queueStateSnapshot(),
    });
    windowRef.dashboardAgentCancelCurrentAutoHeal = () =>
      cancelCurrentAutoHeal('agent_cancel');
    windowRef.dashboardAgentRemoveAutoHealQueueItem = (key) =>
      removeQueueItemByKey(key);
    windowRef.dashboardAgentMoveAutoHealQueueItem = (key, delta) =>
      moveQueueItemByKey(key, delta);
    windowRef.dashboardAgentPrioritizeAutoHealQueueItem = (key) =>
      prioritizeQueueItem(key);
  };

  return {
    autoQueueFromFailureSnapshot,
    cancelCurrentAutoHeal,
    clearQueue,
    enqueueAutoHealIncident,
    installAgentApi,
    listTimelineFailures,
    moveQueueItemByKey,
    prioritizeQueueItem,
    processAutoHealQueue,
    queueAutoHealFromPayload,
    queueStateSnapshot,
    removeQueueItemByKey,
    syncAutoHealSettingsFromBackend,
    updateAutoHealSettings,
  };
}

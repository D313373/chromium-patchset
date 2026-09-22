function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));
}

function extractFailedActionFromRunDetails(runDetails, failedStep) {
  if (!runDetails || typeof runDetails !== 'object') {
    return null;
  }
  const steps = Array.isArray(runDetails.steps) ? runDetails.steps : [];
  const step = steps.find((entry) => {
    const stepIndex = Number(entry?.stepIndex ?? entry?.step_index ?? -1);
    return stepIndex === Number(failedStep);
  });
  if (!step || typeof step !== 'object') {
    return null;
  }
  return step.action || step;
}

function extractContextUrlMismatchFromRunDetails(runDetails, failedStep) {
  if (!runDetails || typeof runDetails !== 'object') {
    return null;
  }
  const steps = Array.isArray(runDetails.steps) ? runDetails.steps : [];
  const step = steps.find((entry) => {
    const stepIndex = Number(entry?.stepIndex ?? entry?.step_index ?? -1);
    return stepIndex === Number(failedStep);
  });
  if (!step || typeof step !== 'object') {
    return null;
  }
  const expected = String(
      step.expected_context_url ||
      step.expectedContextUrl ||
      step.action?.contextUrl ||
      step.action?.context_url ||
      '').trim();
  const observed = String(
      step.observed_context_url ||
      step.observedContextUrl ||
      step.url_before ||
      step.urlBefore ||
      step.context_url ||
      step.contextUrl ||
      '').trim();
  const flagged = step.context_url_mismatch === true ||
      step.contextUrlMismatch === true;
  if (!flagged && (!expected || !observed || expected === observed)) {
    return null;
  }
  if (!expected && !observed) {
    return null;
  }
  return {
    mismatch: true,
    expected_context_url: expected || undefined,
    observed_context_url: observed || undefined,
    note:
      'Run history URL differs from the action contextUrl. The failure may be ' +
      'from executing on the wrong page — check navigation / context before ' +
      'changing locators.',
  };
}

function extractProbeFromFailedAction(action) {
  if (!action || typeof action !== 'object') {
    return null;
  }
  const params = (action.params && typeof action.params === 'object') ?
    action.params :
    {};
  const locators = Array.isArray(params.locators) ? params.locators :
    (Array.isArray(action.locators) ? action.locators : []);
  if (!locators.length) {
    return null;
  }
  return {
    locators,
    condition: String(params.condition || action.condition || ''),
    text: String(params.text || action.text || ''),
    attribute: String(params.attribute || action.attribute || ''),
    textUseRegex: !!(params.textUseRegex || params.text_use_regex),
    attributeUseRegex: !!(params.attributeUseRegex || params.attribute_use_regex),
  };
}

export function buildPlaybackFailureIncident({
  scriptId,
  scriptName,
  playbackResult,
  runDetails,
  source = 'script_playback',
}) {
  const failedStep = Number(
      playbackResult?.failed_step ??
      playbackResult?.failedStep ??
      runDetails?.failed_step ??
      -1);
  const error = String(
      playbackResult?.message ||
      playbackResult?.error ||
      runDetails?.error ||
      '');
  const runId = String(
      runDetails?.run_id ||
      runDetails?.runId ||
      '');
  const failedAction = extractFailedActionFromRunDetails(runDetails, failedStep);
  const probe = extractProbeFromFailedAction(failedAction);
  const contextUrlMismatch =
      extractContextUrlMismatchFromRunDetails(runDetails, failedStep);
  const url = String(
      (contextUrlMismatch && contextUrlMismatch.observed_context_url) ||
      runDetails?.start_url ||
      runDetails?.startUrl ||
      '');

  return {
    source,
    playback: {
      execution_id: String(playbackResult?.runId || playbackResult?.run_id || ''),
      success: playbackResult?.success === true,
      failed_step: failedStep,
      failed_action_index: String(
          runDetails?.failed_action_index ||
          runDetails?.failedActionIndex ||
          ''),
      error,
      url,
    },
    selected_item: {
      kind: 'run',
      id: runId,
      scriptId: String(scriptId || ''),
      scriptName: String(scriptName || scriptId || ''),
      status: 'error',
      failedStep,
      error,
      startMs: Number(runDetails?.started_ms || runDetails?.startedMs || 0),
      endMs: Number(runDetails?.ended_ms || runDetails?.endedMs || 0),
    },
    selected_run_id: runId,
    selected_script_id: String(scriptId || ''),
    selected_script_name: String(scriptName || scriptId || ''),
    selected_status: 'error',
    selected_failed_step: failedStep,
    selected_error: error,
    selected_start_ms: Number(runDetails?.started_ms || runDetails?.startedMs || 0),
    selected_end_ms: Number(runDetails?.ended_ms || runDetails?.endedMs || 0),
    failed_action: failedAction,
    investigation_probe: probe,
    context_url_mismatch: contextUrlMismatch || undefined,
    run_details: runDetails || null,
  };
}

async function findLatestRunForScript(dashboardHandler, scriptId, startedAfterMs) {
  const res = await dashboardHandler.getRunHistory(scriptId, '', '', 40);
  const runs = Array.isArray(res?.runs) ? res.runs : [];
  const threshold = Number(startedAfterMs || 0) - 5000;
  let best = null;
  for (const run of runs) {
    const startedMs = Number(run?.started_ms || run?.startedMs || 0);
    if (threshold > 0 && startedMs > 0 && startedMs < threshold) {
      continue;
    }
    if (!best || startedMs > Number(best?.started_ms || best?.startedMs || 0)) {
      best = run;
    }
  }
  return best;
}

export async function fetchRunDetailsForPlayback(
    dashboardHandler, scriptId, startedAfterMs) {
  const summary = await findLatestRunForScript(
      dashboardHandler, scriptId, startedAfterMs);
  if (!summary) {
    return null;
  }
  const runId = String(summary.run_id || summary.runId || '');
  if (!runId) {
    return summary;
  }
  const detailRes = await dashboardHandler.getRunDetails(runId);
  const run = (detailRes && detailRes.run) ? detailRes.run : detailRes;
  if (run && typeof run === 'object') {
    return run;
  }
  return summary;
}

export async function playScriptAndWait(dashboardHandler, scriptId, options = {}) {
  const id = String(scriptId || '').trim();
  if (!id) {
    return {ok: false, error: 'missing_script_id'};
  }
  const waitForCompletion = options.waitForCompletion !== false;
  const res = await dashboardHandler.playScript(id, {
    waitForCompletion,
  });
  return res || {ok: false, error: 'empty_playback_response'};
}

export async function handlePlaybackResultForAutoHeal({
  dashboardHandler,
  appState,
  autoHealState,
  enqueueAutoHealIncident,
  processAutoHealQueue,
  scriptId,
  playbackResult,
  startedAfterMs,
}) {
  if (!autoHealState?.autoQueueEnabled) {
    return {ok: true, skipped: true, reason: 'auto_heal_disabled'};
  }
  if (!playbackResult || playbackResult.success === true) {
    return {ok: true, skipped: true, reason: 'playback_succeeded'};
  }

  const script = appState?.scripts?.[scriptId];
  const scriptName = String(script?.name || scriptId || '');
  let runDetails = null;
  try {
    runDetails = await fetchRunDetailsForPlayback(
        dashboardHandler, scriptId, startedAfterMs);
  } catch (e) {
    console.warn('[AutoHeal] failed to load run details for playback failure', e);
  }

  const incident = buildPlaybackFailureIncident({
    scriptId,
    scriptName,
    playbackResult,
    runDetails,
    source: 'script_playback',
  });
  const failureRow = {
    key: incident.selected_run_id ? `run:${incident.selected_run_id}` : `playback:${scriptId}:${Date.now()}`,
    run_id: incident.selected_run_id,
    script_id: scriptId,
    script_name: scriptName,
    failed_step: incident.selected_failed_step,
    error: incident.selected_error,
    started_ms: incident.selected_start_ms,
    ended_ms: incident.selected_end_ms,
    kind: 'run',
    source: 'script_playback',
  };
  autoHealState.lastFailures = [
    failureRow,
    ...(Array.isArray(autoHealState.lastFailures) ? autoHealState.lastFailures : []),
  ].slice(0, 220);
  const enq = enqueueAutoHealIncident(incident, 'script_playback');
  if (enq?.ok && enq?.queued && typeof processAutoHealQueue === 'function') {
    processAutoHealQueue();
  }
  return {ok: true, queued: !!enq?.queued, duplicate: !!enq?.duplicate, incident};
}

export async function watchPlaybackAndAutoHeal({
  dashboardHandler,
  appState,
  autoHealState,
  enqueueAutoHealIncident,
  processAutoHealQueue,
  scriptId,
  options = {},
}) {
  const startedAfterMs = Date.now();
  const playbackResult = await playScriptAndWait(dashboardHandler, scriptId, options);
  if (!playbackResult?.ok && playbackResult?.success !== false) {
    return {ok: false, playback: playbackResult};
  }
  if (!playbackResult.waited) {
    await sleep(1500);
    const polled = await fetchRunDetailsForPlayback(
        dashboardHandler, scriptId, startedAfterMs);
    if (polled && String(polled.status || '').toLowerCase() === 'error') {
      playbackResult.success = false;
      playbackResult.failed_step = Number(polled.failed_step ?? -1);
      playbackResult.message = String(polled.error || '');
    } else if (polled && String(polled.status || '').toLowerCase() === 'success') {
      playbackResult.success = true;
    }
  }
  const healRes = await handlePlaybackResultForAutoHeal({
    dashboardHandler,
    appState,
    autoHealState,
    enqueueAutoHealIncident,
    processAutoHealQueue,
    scriptId,
    playbackResult,
    startedAfterMs,
  });
  return {ok: true, playback: playbackResult, auto_heal: healRes};
}

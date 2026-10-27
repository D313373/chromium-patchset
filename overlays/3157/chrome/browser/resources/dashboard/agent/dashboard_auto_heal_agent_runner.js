import {
  buildDashboardTurnResponseFormat,
  unwrapStructuredAction,
} from './action_output_schema.js';
import {getAgentMementoStore} from './agent_memento_store.js';
import {createDashboardCommandDispatcher} from './command_dispatcher.js';
import {
  buildSystemPromptForLoopProfile,
  normalizeAgentCommandForExecution,
} from './loop_profiles.js';
import {slimInvestigateResult, applyInvestigateContextDiff} from './investigate_result_slim.js';
import {
  buildAssistantApplySystemPrompt,
  buildInvestigatorDiagnoseSystemPrompt,
  findingsFromAgentCommand,
  formatFindingsBriefForAssistant,
  normalizeInvestigatorFindings,
} from './investigator_findings.js';

const DEFAULT_MAX_TURNS = 10;
const DEFAULT_DIAGNOSE_TURNS = 6;
const DEFAULT_APPLY_TURNS = 6;
const MAX_TOOL_RESULT_CHARS = 3500;
const MAX_ASSISTANT_CHARS = 1200;
const MAX_HISTORY_MESSAGES = 8;  // pinned user + sliding window
const STAGNANT_FINGERPRINT_LIMIT = 3;

function actionFingerprint(command) {
  const payload = (command && command.payload && typeof command.payload === 'object') ?
    command.payload :
    {};
  const point = payload.coordinate || payload.coordinates || {};
  const x = Number(point.x);
  const y = Number(point.y);
  const coord = Number.isFinite(x) && Number.isFinite(y) ?
    `${Math.round(x)},${Math.round(y)}` :
    '';
  const target = payload.target_id ?? payload.targetId ?? payload.script_id ??
      payload.step_index ?? '';
  return [
    String(command?.command || ''),
    String(target),
    coord,
    String(payload.url || ''),
    String(payload.key || ''),
    String(payload.direction || ''),
    String(payload.text || '').slice(0, 32),
  ].join('|');
}

/** Round float coordinates and accept [x, y] before the strict integer schema. */
function coerceModelAction(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) {
    return raw;
  }
  const copy = {...raw};
  let point = copy.coordinate ?? copy.coordinates;
  if (typeof point === 'string') {
    const match = point.match(/(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)/);
    if (match) {
      point = {x: Number(match[1]), y: Number(match[2])};
    }
  }
  if (Array.isArray(point) && point.length >= 2) {
    point = {x: point[0], y: point[1]};
  }
  if (point && typeof point === 'object') {
    const x = Number(point.x);
    const y = Number(point.y);
    if (Number.isFinite(x) && Number.isFinite(y)) {
      copy.coordinate = {
        x: Math.max(0, Math.min(4000, Math.round(x))),
        y: Math.max(0, Math.min(4000, Math.round(y))),
      };
      delete copy.coordinates;
    }
  }
  return copy;
}

function fallbackForStagnation(nudge) {
  const slot = ((nudge % 3) + 3) % 3;
  if (slot === 1) {
    return {action: 'wait', timeout: 1};
  }
  if (slot === 2) {
    return {
      action: 'complete',
      status: 'blocked',
      summary: 'Stopped after repeated actions left the page unchanged.',
    };
  }
  return {action: 'scroll', direction: 'down', distance: 600};
}

function extractJsonCommandFromText(text) {
  const raw = String(text || '');
  let searchPos = 0;
  while (searchPos < raw.length) {
    const blockStart = raw.indexOf('```json', searchPos);
    if (blockStart < 0) {
      break;
    }
    const payloadStart = raw.indexOf('\n', blockStart);
    if (payloadStart < 0) {
      break;
    }
    const blockEnd = raw.indexOf('```', payloadStart + 1);
    if (blockEnd < 0) {
      break;
    }
    const candidate = raw.slice(payloadStart + 1, blockEnd).trim();
    try {
      const parsed = JSON.parse(candidate);
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
        return parsed;
      }
    } catch (_) {}
    searchPos = blockEnd + 3;
  }
  const trimmed = raw.trim();
  if (!trimmed) {
    return null;
  }
  const start = trimmed.indexOf('{');
  const end = trimmed.lastIndexOf('}');
  if (start >= 0 && end > start) {
    try {
      const parsed = JSON.parse(trimmed.slice(start, end + 1));
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
        return parsed;
      }
    } catch (_) {}
  }
  return null;
}

function extractAssistantText(payload, provider) {
  if (!payload || typeof payload !== 'object') {
    return '';
  }
  const p = String(provider || 'ollama').toLowerCase();
  if (p === 'anthropic') {
    const content = payload.content;
    if (Array.isArray(content)) {
      return content
          .filter((part) => part && part.type === 'text')
          .map((part) => String(part.text || ''))
          .join('\n')
          .trim();
    }
    return '';
  }
  if (p === 'openai' || p === 'cerebras' || p === 'local') {
    const choice = Array.isArray(payload.choices) ? payload.choices[0] : null;
    const message = choice && choice.message;
    return String((message && message.content) || '');
  }
  if (p === 'gemini') {
    const cand = Array.isArray(payload.candidates) ? payload.candidates[0] : null;
    const parts = cand && cand.content && Array.isArray(cand.content.parts) ?
      cand.content.parts :
      [];
    return parts.map((part) => String(part?.text || '')).join('\n').trim();
  }
  const message = payload.message;
  if (message && typeof message === 'object') {
    return String(message.content || '');
  }
  return String(payload.response || '');
}

function compactJson(value, maxChars) {
  let raw = '{}';
  try {
    raw = JSON.stringify(value ?? {});
  } catch (_) {
    raw = '{}';
  }
  if (raw.length <= maxChars) {
    return raw;
  }
  return `${raw.slice(0, maxChars)}\n...[truncated ${raw.length - maxChars} chars]`;
}

/** Strip bulky fields before feeding tool results back into the model. */
export function slimToolResultForModel(commandName, result) {
  if (!result || typeof result !== 'object') {
    return {ok: false, error: 'empty_result'};
  }
  const cmd = String(commandName || '');
  const base = {
    ok: result.ok !== false,
  };
  if (result.error) {
    base.error = String(result.error).slice(0, 400);
  }
  if (result.message) {
    base.message = String(result.message).slice(0, 400);
  }

  if (cmd === 'dashboard_investigate_page' || cmd === 'investigate' ||
      cmd === 'probe' || cmd === 'execute_actions' || cmd === 'run_script' ||
      cmd === 'resume_from_step' || cmd === 'call_action') {
    return applyInvestigateContextDiff(result) ||
        slimInvestigateResult(result) || base;
  }

  if (cmd === 'dashboard_handoff_to_assistant' || cmd === 'handoff') {
    return {
      ...base,
      handoff: true,
      findings: result.findings || result,
      status: result.status || 'handoff',
      summary: result.summary,
    };
  }

  if (cmd === 'dashboard_get_script_step' || cmd === 'get_step') {
    return {
      ...base,
      script_id: result.script_id || result.scriptId,
      step_index: result.step_index ?? result.stepIndex,
      step: result.step || result.action || result,
    };
  }

  if (cmd === 'dashboard_update_script_step' || cmd === 'update_step' ||
      cmd === 'dashboard_select_script' || cmd === 'select_script' ||
      cmd === 'dashboard_resume_script_from_step' || cmd === 'resume_script' ||
      cmd === 'dashboard_run_script' || cmd === 'run_script') {
    return {
      ...base,
      script_id: result.script_id || result.scriptId,
      step_index: result.step_index ?? result.stepIndex,
      success: result.success,
      failed_step: result.failed_step ?? result.failedStep,
      status: result.status,
    };
  }

  if (cmd === 'dashboard_get_script_json' || cmd === 'get_script' ||
      cmd === 'dashboard_save_script_json' || cmd === 'save_script') {
    const script = result.script || result.script_json || result.scriptJson;
    const actions = script && Array.isArray(script.actions) ? script.actions : null;
    return {
      ...base,
      script_id: result.script_id || result.scriptId ||
          (script && (script.id || script.script_id)),
      name: script && script.name,
      action_count: actions ? actions.length : undefined,
    };
  }

  // Generic: drop known heavy keys.
  const out = {...result};
  for (const key of [
    'page_map', 'pageMap', 'screenshot_image_base64', 'screenshotImageBase64',
    'screenshot_path', 'create_result', 'action_result', 'stop_result',
    'script_json', 'scriptJson', 'timeline', 'run_details', 'runDetails',
    'trace',
  ]) {
    delete out[key];
  }
  return out;
}

function trimMessageWindow(messages, maxMessages = MAX_HISTORY_MESSAGES) {
  if (!Array.isArray(messages) || messages.length <= maxMessages) {
    return messages;
  }
  // Keep the original heal brief (messages[0]) + the most recent turns.
  const head = messages[0];
  const tail = messages.slice(-(maxMessages - 1));
  return [head, ...tail];
}

function buildHealSystemPrompt() {
  // Legacy single-prompt kept for tests/compat; two-phase runner uses
  // investigator_findings prompts instead.
  return buildInvestigatorDiagnoseSystemPrompt(buildSystemPromptForLoopProfile);
}

function withLoopProfile(windowRef, profile, fn) {
  const prev = windowRef.__DASH_LOOP_PROFILE;
  windowRef.__DASH_LOOP_PROFILE = profile;
  return Promise.resolve()
      .then(() => fn())
      .finally(() => {
        if (prev == null) {
          delete windowRef.__DASH_LOOP_PROFILE;
        } else {
          windowRef.__DASH_LOOP_PROFILE = prev;
        }
      });
}

function withHeadlessScriptRuns(windowRef, enabled, fn) {
  const prev = windowRef.__DASH_HEADLESS_SCRIPT_RUNS;
  if (enabled) {
    windowRef.__DASH_HEADLESS_SCRIPT_RUNS = true;
  } else {
    delete windowRef.__DASH_HEADLESS_SCRIPT_RUNS;
  }
  return Promise.resolve()
      .then(() => fn())
      .finally(() => {
        if (prev) {
          windowRef.__DASH_HEADLESS_SCRIPT_RUNS = prev;
        } else {
          delete windowRef.__DASH_HEADLESS_SCRIPT_RUNS;
        }
      });
}

function classifyModelRequestFailure(httpStatus, bodyText) {
  const status = Number(httpStatus || 0);
  const body = String(bodyText || '').toLowerCase();
  const schemaRejected = status >= 400 && status < 500 &&
      /schema|response_format|json_schema|output_config|structured/.test(body);
  if (schemaRejected) {
    return 'unsupported_schema_or_api_error';
  }
  if (status === 0) {
    return 'request_failed';
  }
  return `model_http_${status}`;
}

async function requestModelChat({
  provider,
  endpoint,
  model,
  systemPrompt,
  messages,
  dashboardHandler,
  apiKeyName,
}) {
  const payload = {
    provider: String(provider || 'ollama'),
    endpoint: String(endpoint || '').replace(/\/+$/, ''),
    model: String(model || ''),
    system_prompt: String(systemPrompt || ''),
    messages: Array.isArray(messages) ? messages : [],
    api_key_name: apiKeyName || '',
    max_tokens: 1024,
    response_format: buildDashboardTurnResponseFormat(),
  };
  if (dashboardHandler && typeof dashboardHandler.agentOllamaChat === 'function') {
    const res = await dashboardHandler.agentOllamaChat(payload);
    const httpStatus = Number(res?.http_status || res?.httpStatus || 0);
    const bodyText = String(res?.body || res?.error || '');
    if (!res?.ok) {
      const kind = classifyModelRequestFailure(httpStatus, bodyText);
      throw new Error(`${kind}:${bodyText.slice(0, 240)}`);
    }
    let parsed = null;
    try {
      parsed = JSON.parse(bodyText);
    } catch (e) {
      throw new Error(`model_invalid_json:${String(e && e.message ? e.message : e)}`);
    }
    return extractAssistantText(parsed, payload.provider);
  }

  if (String(provider || '').toLowerCase() !== 'ollama') {
    throw new Error('hosted_provider_requires_dashboard_handler');
  }

  const base = payload.endpoint || 'http://127.0.0.1:11434';
  const url = `${base}/api/chat`;
  const body = {
    model: payload.model || 'qwen3-coder-next',
    stream: false,
    messages: [
      {role: 'system', content: payload.system_prompt},
      ...payload.messages,
    ],
    options: {num_predict: 1024},
    format: payload.response_format?.json_schema?.schema || undefined,
  };
  const response = await fetch(url, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'Accept': 'application/json'},
    body: JSON.stringify(body),
  });
  const text = await response.text();
  if (!response.ok) {
    const kind = classifyModelRequestFailure(response.status, text);
    throw new Error(`${kind}:${text.slice(0, 240)}`);
  }
  let parsed = null;
  try {
    parsed = JSON.parse(text);
  } catch (e) {
    throw new Error(`ollama_invalid_json:${String(e && e.message ? e.message : e)}`);
  }
  return extractAssistantText(parsed, 'ollama');
}

const ANTHROPIC_MODEL_ALIASES = {
  'claude-haiku-4-5-20251001': 'claude-haiku-4-5',
  'claude-3-5-haiku-latest': 'claude-haiku-4-5',
  'claude-3-5-sonnet-latest': 'claude-sonnet-4-5',
  'claude-sonnet-4-20250514': 'claude-sonnet-4-5',
};

function normalizeAutoHealModel(provider, model) {
  let out = String(model || '').trim();
  if (provider === 'anthropic' && ANTHROPIC_MODEL_ALIASES[out]) {
    out = ANTHROPIC_MODEL_ALIASES[out];
  }
  return out;
}

function resolveProviderConfig(agent, windowRef) {
  // Settings + bootstrap set __DASH_PROVIDER / __DASH_MODEL (not the older
  // __DASH_AGENT_PROVIDER name). Prefer those so auto-heal tracks the Settings
  // Anthropic/model selection even if a settings read is partial.
  let provider = String(
      agent.provider || windowRef.__DASH_PROVIDER ||
      windowRef.__DASH_AGENT_PROVIDER || 'anthropic')
      .trim()
      .toLowerCase() || 'anthropic';
  const endpoints =
      (agent.providerEndpoints && typeof agent.providerEndpoints === 'object') ?
        agent.providerEndpoints :
        {};
  let endpoint = '';
  let model = String(
      agent.model || windowRef.__DASH_MODEL || '').trim();
  let apiKeyName = '';
  if (provider === 'ollama') {
    provider = 'local';
  }
  if (provider === 'local') {
    endpoint = String(
        agent.localEndpoint || agent.ollamaEndpoint ||
        windowRef.__DASH_OLLAMA_ENDPOINT ||
        'http://127.0.0.1:11434/v1/chat/completions');
    model = model || windowRef.__DASH_OLLAMA_MODEL || 'local-model';
    apiKeyName = 'LOCAL_API_KEY';
  } else if (provider === 'anthropic') {
    endpoint = String(
        endpoints.anthropic || agent.endpoint ||
        'https://api.anthropic.com/v1/messages');
    model = normalizeAutoHealModel(provider, model) || 'claude-haiku-4-5';
    apiKeyName = 'ANTHROPIC_API_KEY';
  } else if (provider === 'openai') {
    endpoint = String(
        endpoints.openai || agent.endpoint ||
        'https://api.openai.com/v1/chat/completions');
    model = model || 'gpt-4.1-mini';
    apiKeyName = 'OPENAI_API_KEY';
  } else if (provider === 'grok') {
    endpoint = String(
        endpoints.grok || agent.endpoint ||
        'https://api.x.ai/v1/chat/completions');
    model = model || 'grok-4';
    apiKeyName = 'XAI_API_KEY';
  } else if (provider === 'cerebras') {
    endpoint = String(
        endpoints.cerebras || agent.endpoint ||
        'https://api.cerebras.ai/v1/chat/completions');
    model = model || 'qwen-3-235b-a22b-instruct-2507';
    apiKeyName = 'CEREBRAS_API_KEY';
  } else if (provider === 'gemini') {
    endpoint = String(
        endpoints.gemini || agent.endpoint ||
        'https://generativelanguage.googleapis.com/v1beta');
    model = model || 'gemini-2.5-flash';
    apiKeyName = 'GEMINI_API_KEY';
  } else {
    endpoint = String(
        agent.localEndpoint || agent.ollamaEndpoint ||
        'http://127.0.0.1:11434/v1/chat/completions');
    model = model || 'local-model';
    provider = 'local';
    apiKeyName = 'LOCAL_API_KEY';
  }
  return {provider, endpoint, model, apiKeyName};
}

export function createDashboardAutoHealAgentRunner({
  windowRef = window,
  dashboardHandler,
  chromeSend,
  maxTurns = DEFAULT_MAX_TURNS,
}) {
  const noop = () => {};
  const mementos = getAgentMementoStore();
  const dispatcher = createDashboardCommandDispatcher({
    appendMessage: noop,
    historyEl: null,
    chromeSend: chromeSend || ((name, args) => windowRef.chrome.send(name, args)),
    parseVisualCoords: () => null,
    formatVisualCoords: () => '',
    cubbyRead: (taskId) => mementos.readText(taskId),
    cubbyWrite: (text, meta) => mementos.write(text, meta),
    onResetAgent: noop,
    onRefreshPageMapRequested: noop,
    windowRef,
  });
  if (!dispatcher || typeof dispatcher.sendCommandToAgent !== 'function') {
    return null;
  }

  /**
   * Generic two-phase loop: investigator diagnose → assistant apply/explain.
   * Used by auto-heal and other investigative/support tasks.
   */
  const runInvestigatorAssistantHandoff = async (userPrompt, options = {}) => {
    const onProgress = typeof options?.onProgress === 'function' ?
      options.onProgress :
      null;
    const report = (patch) => {
      try {
        if (onProgress) {
          onProgress(patch);
        }
      } catch (_) {}
    };
    const settingsRes = await dashboardHandler.getEffectiveSettings('');
    const settings = (settingsRes && settingsRes.settings) ? settingsRes.settings : {};
    const agent = (settings && settings.agent && typeof settings.agent === 'object') ?
      settings.agent :
      {};
    const {provider, endpoint, model, apiKeyName} =
        resolveProviderConfig(agent, windowRef);

    const diagnoseTurns = Math.max(
        2,
        Math.min(
            Number(options.diagnoseTurns) || DEFAULT_DIAGNOSE_TURNS,
            12));
    const applyTurns = Math.max(
        2,
        Math.min(
            Number(options.applyTurns) || DEFAULT_APPLY_TURNS,
            12));
    const seedFindings = normalizeInvestigatorFindings(options.findings || null);
    const trace = [];
    const taskId = String(options.taskId || options.taskKey || '').trim();
    if (taskId) {
      mementos.setActiveTask(taskId);
    }

    const runPhase = async ({
      phaseName,
      systemPrompt,
      prompt,
      turnBudget,
      loopProfile,
      headlessScriptRuns,
      stopOnHandoff,
    }) => {
      let messages = [{role: 'user', content: String(prompt || '')}];
      let findings = null;
      let lastFinish = null;
      let stagnantFingerprint = '';
      let stagnantStreak = 0;
      let stagnationNudges = 0;

      const executeTurns = async () => {
        for (let turn = 0; turn < turnBudget; turn += 1) {
          report({
            phase: phaseName === 'diagnose' ? 'diagnose_waiting_model' :
                                           'apply_waiting_model',
            turn: turn + 1,
            max_turns: turnBudget,
            command: '',
            detail:
                `${phaseName}: waiting on ${provider}/${model} (turn ${turn + 1}/${turnBudget})…`,
          });
          let assistantText = '';
          try {
            assistantText = await requestModelChat({
              provider,
              endpoint,
              model,
              systemPrompt,
              messages: trimMessageWindow(messages),
              dashboardHandler,
              apiKeyName,
            });
          } catch (e) {
            const err = String(e && e.message ? e.message : e);
            report({
              phase: 'failed',
              turn: turn + 1,
              max_turns: turnBudget,
              command: '',
              detail: `${phaseName} ${provider}/${model} failed: ${err.slice(0, 280)}`,
            });
            return {
              ok: false,
              error: err,
              findings,
              lastFinish,
              messages,
            };
          }
          const clippedAssistant =
              String(assistantText || '').slice(0, MAX_ASSISTANT_CHARS);
          messages.push({role: 'assistant', content: clippedAssistant});
          const resolveCommand = (text) => {
            const extracted = extractJsonCommandFromText(text);
            const rawCommand = coerceModelAction(
                extracted ? unwrapStructuredAction(extracted) : null);
            return rawCommand ? normalizeAgentCommandForExecution(rawCommand) : null;
          };
          let command = resolveCommand(assistantText);
          if (!command || !command.command) {
            messages.push({
              role: 'user',
              content: [
                'That reply was not a valid action object.',
                'Reply with one JSON object only.',
                'coordinate must be {"x": <integer 0-4000>, "y": <integer 0-4000>}.',
                'No markdown.',
              ].join(' '),
            });
            messages = trimMessageWindow(messages);
            let repairedText = '';
            try {
              repairedText = await requestModelChat({
                provider,
                endpoint,
                model,
                systemPrompt,
                messages: trimMessageWindow(messages),
                dashboardHandler,
                apiKeyName,
              });
            } catch (e) {
              const err = String(e && e.message ? e.message : e);
              return {
                ok: false,
                error: err,
                findings,
                lastFinish,
                messages,
              };
            }
            const clippedRepair =
                String(repairedText || '').slice(0, MAX_ASSISTANT_CHARS);
            messages.push({role: 'assistant', content: clippedRepair});
            command = resolveCommand(repairedText);
            if (!command || !command.command) {
              trace.push({
                phase: phaseName,
                turn,
                type: 'assistant_text',
                text: clippedRepair || clippedAssistant,
              });
              return {
                ok: true,
                status: 'completed_without_command',
                findings,
                lastFinish: {
                  ok: true,
                  status: 'completed_without_command',
                  summary: clippedRepair || clippedAssistant,
                },
                messages,
              };
            }
          }

          const fingerprint = actionFingerprint(command);
          if (fingerprint && fingerprint === stagnantFingerprint) {
            stagnantStreak += 1;
          } else {
            stagnantFingerprint = fingerprint;
            stagnantStreak = 1;
          }
          if (stagnantStreak > STAGNANT_FINGERPRINT_LIMIT &&
              command.command !== 'agent_finish') {
            stagnationNudges += 1;
            const forced = fallbackForStagnation(stagnationNudges);
            command = normalizeAgentCommandForExecution(forced);
            stagnantStreak = 0;
            stagnantFingerprint = '';
            report({
              phase: 'stagnation_breaker',
              turn: turn + 1,
              max_turns: turnBudget,
              command: String(command?.command || ''),
              detail: `Same action fingerprint repeated ${STAGNANT_FINGERPRINT_LIMIT} times. Forcing ${forced.action}.`,
            });
          }

          const handoffFromCmd = findingsFromAgentCommand(command);
          if (handoffFromCmd && stopOnHandoff) {
            findings = handoffFromCmd;
            trace.push({
              phase: phaseName,
              turn,
              command: command.command,
              handoff: findings,
            });
            report({
              phase: 'handoff',
              turn: turn + 1,
              max_turns: turnBudget,
              command: String(command.command || ''),
              detail: `Handoff: ${String(findings.diagnosis || findings.user_summary || 'ready').slice(0, 200)}`,
            });
            // Still dispatch handoff so dispatcher ack is consistent.
            await dispatcher.sendCommandToAgent(command);
            return {
              ok: true,
              status: 'handoff',
              findings,
              lastFinish: {
                ok: true,
                status: 'handoff',
                summary: findings.user_summary || findings.diagnosis || '',
              },
              messages,
            };
          }

          report({
            phase: phaseName === 'diagnose' ? 'diagnose_executing' :
                                           'apply_executing',
            turn: turn + 1,
            max_turns: turnBudget,
            command: String(command.command || ''),
            detail: `${phaseName}: executing ${command.command}…`,
          });
          const commandResult = await dispatcher.sendCommandToAgent(command);
          const slimResult = slimToolResultForModel(command.command, commandResult);
          trace.push({
            phase: phaseName,
            turn,
            command: command.command,
            commandResult: slimResult,
          });
          report({
            phase: 'command_result',
            turn: turn + 1,
            max_turns: turnBudget,
            command: String(command.command || ''),
            detail: slimResult?.ok === false ?
              `Command ${command.command} failed: ${String(slimResult?.error || 'error')}` :
              `Command ${command.command} ok`,
          });

          if (command.command === 'dashboard_handoff_to_assistant' ||
              (commandResult && commandResult.handoff)) {
            findings = normalizeInvestigatorFindings(
                           commandResult?.findings || command.payload) ||
                findings;
            if (stopOnHandoff) {
              return {
                ok: true,
                status: 'handoff',
                findings,
                lastFinish: {
                  ok: true,
                  status: 'handoff',
                  summary: findings?.user_summary || findings?.diagnosis || '',
                },
                messages,
              };
            }
          }

          if (command.command === 'agent_finish') {
            lastFinish = {
              ok: commandResult?.ok !== false,
              status: String(
                  command.payload?.status || commandResult?.status || 'done'),
              summary: String(
                  command.payload?.summary || commandResult?.summary ||
                  command.payload?.why || ''),
            };
            const finishFindings = findingsFromAgentCommand(command);
            if (finishFindings) {
              findings = finishFindings;
              if (stopOnHandoff) {
                return {
                  ok: true,
                  status: 'handoff',
                  findings,
                  lastFinish,
                  messages,
                };
              }
            }
            return {
              ok: lastFinish.ok,
              status: lastFinish.status,
              findings,
              lastFinish,
              messages,
            };
          }

          if (slimResult?.ok !== false) {
            stagnantStreak = 0;
            stagnantFingerprint = '';
          }
          const memory = mementos.promptBlock(taskId);
          messages.push({
            role: 'user',
            content: [
              `${command.command} =>`,
              compactJson(slimResult, MAX_TOOL_RESULT_CHARS),
              memory,
              stopOnHandoff ?
                'Next action, or {"action":"handoff",...} with findings.' :
                'Next action, or {"action":"complete","status":"solved","summary":"..."}.',
            ].filter(Boolean).join('\n'),
          });
          messages = trimMessageWindow(messages);
        }
        return {
          ok: false,
          error: `${phaseName}_turn_budget_exceeded`,
          findings,
          lastFinish,
          messages,
        };
      };

      return withLoopProfile(windowRef, loopProfile, () =>
        withHeadlessScriptRuns(windowRef, headlessScriptRuns, executeTurns));
    };

    // --- Phase 1: investigator diagnose (unless findings already provided) ---
    let findings = seedFindings;
    let diagnoseResult = null;
    if (!findings) {
      report({
        phase: 'diagnose',
        turn: 0,
        max_turns: diagnoseTurns,
        command: '',
        detail: 'Investigator diagnose phase…',
      });
      diagnoseResult = await runPhase({
        phaseName: 'diagnose',
        systemPrompt: buildInvestigatorDiagnoseSystemPrompt(
            buildSystemPromptForLoopProfile),
        prompt: userPrompt,
        turnBudget: diagnoseTurns,
        loopProfile: 'investigator',
        headlessScriptRuns: true,
        stopOnHandoff: true,
      });
      findings = diagnoseResult.findings;
      if (!findings) {
        // Soft fallback: if diagnose finished without handoff, synthesize
        // from last finish summary so assistant can still try.
        if (diagnoseResult.lastFinish?.summary) {
          findings = normalizeInvestigatorFindings({
            diagnosis: diagnoseResult.lastFinish.summary,
            status: diagnoseResult.lastFinish.status || 'handoff',
            user_summary: diagnoseResult.lastFinish.summary,
          });
        }
      }
      if (!findings) {
        return {
          ok: false,
          error: diagnoseResult.error || 'diagnose_missing_handoff',
          provider,
          model,
          endpoint,
          trace,
          phase: 'diagnose',
          diagnose: diagnoseResult,
        };
      }
    }

    // --- Phase 2: dashboard assistant apply / explain / verify ---
    report({
      phase: 'apply',
      turn: 0,
      max_turns: applyTurns,
      command: '',
      detail: 'Assistant apply/explain phase…',
    });
    const applyPrompt = formatFindingsBriefForAssistant(findings, {
      task_brief: String(options.taskBrief || '').slice(0, 800) || undefined,
    });
    const applyResult = await runPhase({
      phaseName: 'apply',
      systemPrompt:
          buildAssistantApplySystemPrompt(buildSystemPromptForLoopProfile),
      prompt: applyPrompt,
      turnBudget: applyTurns,
      loopProfile: 'dashboard_assistant',
      headlessScriptRuns: true,  // verify resume/run on headless
      stopOnHandoff: false,
    });

    const summary = String(
        applyResult.lastFinish?.summary || findings.user_summary ||
        findings.diagnosis || '');
    const status = String(
        applyResult.lastFinish?.status ||
        (applyResult.ok ? 'solved' : 'blocked'));
    if (status === 'solved' || status === 'blocked') {
      mementos.pruneCompletedTask(taskId);
    }
    return {
      ok: applyResult.ok !== false && !applyResult.error,
      status,
      summary,
      findings,
      provider,
      model,
      endpoint,
      trace,
      diagnose: diagnoseResult,
      apply: applyResult,
      error: applyResult.error,
    };
  };

  const runAutoHealPrompt = async (prompt, options = {}) => {
    return runInvestigatorAssistantHandoff(prompt, {
      ...options,
      diagnoseTurns: options.diagnoseTurns || DEFAULT_DIAGNOSE_TURNS,
      applyTurns: options.applyTurns || DEFAULT_APPLY_TURNS,
    });
  };

  /** Support / tech-help entry: same handoff without heal-specific wrapping. */
  const runSupportHandoffPrompt = async (prompt, options = {}) => {
    return runInvestigatorAssistantHandoff(prompt, options);
  };

  return {
    runAutoHealPrompt,
    runSupportHandoffPrompt,
    runInvestigatorAssistantHandoff,
  };
}

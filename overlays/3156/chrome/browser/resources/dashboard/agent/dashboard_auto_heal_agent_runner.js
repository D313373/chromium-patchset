import {createDashboardCommandDispatcher} from './command_dispatcher.js';
import {normalizeAgentCommandForExecution} from './loop_profiles.js';

const DEFAULT_MAX_TURNS = 10;
const MAX_TOOL_RESULT_CHARS = 3500;
const MAX_ASSISTANT_CHARS = 1200;
const MAX_HISTORY_MESSAGES = 8;  // pinned user + sliding window

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

  if (cmd === 'dashboard_investigate_page' || cmd === 'investigate') {
    return {
      ...base,
      transport: result.transport,
      url: result.url,
      title: result.title,
      ready_state: result.ready_state || result.readyState,
      element_count: result.element_count ?? result.elementCount,
      locator_probe: result.locator_probe || result.locatorProbe,
      links: Array.isArray(result.links) ? result.links.slice(0, 8) : undefined,
      inputs: Array.isArray(result.inputs) ? result.inputs.slice(0, 8) : undefined,
      buttons: Array.isArray(result.buttons) ? result.buttons.slice(0, 8) : undefined,
      inner_text_excerpt: String(
          result.inner_text_excerpt || result.innerTextExcerpt || '')
          .slice(0, 500),
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
  return [
    'Auto-heal agent. Fix the failing script step with the fewest tools.',
    'One JSON object per turn (markdown fence optional):',
    '{"do":"select_script","script_id":"..."}',
    '{"do":"get_step","step":N}',
    '{"do":"update_step","step":N,"sel":"..."} or {"do":"update_step","step":N,"id":"..."}',
    '{"do":"investigate","url":"..."}  // headless :9222 only',
    '{"do":"resume_script","step":N} or {"do":"run_script"}',
    '{"do":"done","why":"short summary","status":"solved|blocked"}',
    'Rules: surgical edits only; never headed tabs; never queue heal; prefer get_step/update_step over get_script/save_script.',
  ].join('\n');
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
  };
  if (dashboardHandler && typeof dashboardHandler.agentOllamaChat === 'function') {
    const res = await dashboardHandler.agentOllamaChat(payload);
    const httpStatus = Number(res?.http_status || res?.httpStatus || 0);
    const bodyText = String(res?.body || res?.error || '');
    if (!res?.ok) {
      throw new Error(`model_http_${httpStatus || 0}:${bodyText.slice(0, 240)}`);
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
  };
  const response = await fetch(url, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'Accept': 'application/json'},
    body: JSON.stringify(body),
  });
  const text = await response.text();
  if (!response.ok) {
    throw new Error(`ollama_http_${response.status}:${text.slice(0, 240)}`);
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
  const dispatcher = createDashboardCommandDispatcher({
    appendMessage: noop,
    historyEl: null,
    chromeSend: chromeSend || ((name, args) => windowRef.chrome.send(name, args)),
    parseVisualCoords: () => null,
    formatVisualCoords: () => '',
    cubbyRead: () => '',
    cubbyWrite: noop,
    onResetAgent: noop,
    onRefreshPageMapRequested: noop,
    windowRef,
  });
  if (!dispatcher || typeof dispatcher.sendCommandToAgent !== 'function') {
    return null;
  }

  const runAutoHealPrompt = async (prompt, options = {}) => {
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
    const systemPrompt = buildHealSystemPrompt();

    let messages = [{
      role: 'user',
      content: String(prompt || ''),
    }];
    const trace = [];
    const turnBudget = Math.max(3, Math.min(Number(maxTurns) || DEFAULT_MAX_TURNS, 16));

    for (let turn = 0; turn < turnBudget; turn += 1) {
      report({
        phase: 'waiting_model',
        turn: turn + 1,
        max_turns: turnBudget,
        command: '',
        detail: `Waiting on ${provider}/${model} (turn ${turn + 1}/${turnBudget})…`,
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
          detail: `${provider}/${model} failed: ${err.slice(0, 280)}`,
        });
        return {
          ok: false,
          error: err,
          provider,
          model,
          endpoint,
          trace,
        };
      }
      const clippedAssistant = String(assistantText || '').slice(0, MAX_ASSISTANT_CHARS);
      messages.push({role: 'assistant', content: clippedAssistant});
      const rawCommand = extractJsonCommandFromText(assistantText);
      const command = rawCommand ?
        normalizeAgentCommandForExecution(rawCommand) :
        null;
      if (!command || !command.command) {
        trace.push({turn, type: 'assistant_text', text: clippedAssistant});
        report({
          phase: 'completed',
          turn: turn + 1,
          max_turns: turnBudget,
          command: '',
          detail: 'Model returned no command; ending heal.',
        });
        return {
          ok: true,
          status: 'completed_without_command',
          assistant_text: clippedAssistant,
          provider,
          model,
          trace,
        };
      }
      report({
        phase: 'executing_command',
        turn: turn + 1,
        max_turns: turnBudget,
        command: String(command.command || ''),
        detail: `Executing ${command.command}…`,
      });
      const commandResult = await dispatcher.sendCommandToAgent(command);
      const slimResult = slimToolResultForModel(command.command, commandResult);
      trace.push({turn, command: command.command, commandResult: slimResult});
      report({
        phase: 'command_result',
        turn: turn + 1,
        max_turns: turnBudget,
        command: String(command.command || ''),
        detail: slimResult?.ok === false ?
          `Command ${command.command} failed: ${String(slimResult?.error || 'error')}` :
          `Command ${command.command} ok`,
      });
      if (command.command === 'agent_finish') {
        return {
          ok: commandResult?.ok !== false,
          status: String(command.payload?.status || commandResult?.status || 'done'),
          summary: String(command.payload?.summary || commandResult?.summary || ''),
          provider,
          model,
          trace,
        };
      }
      messages.push({
        role: 'user',
        content: [
          `${command.command} =>`,
          compactJson(slimResult, MAX_TOOL_RESULT_CHARS),
          'Next JSON command or {"do":"done","why":"..."}.',
        ].join('\n'),
      });
      messages = trimMessageWindow(messages);
    }
    report({
      phase: 'failed',
      turn: turnBudget,
      max_turns: turnBudget,
      command: '',
      detail: 'Turn budget exceeded',
    });
    return {
      ok: false,
      error: 'auto_heal_turn_budget_exceeded',
      provider,
      model,
      trace,
    };
  };

  return {runAutoHealPrompt};
}

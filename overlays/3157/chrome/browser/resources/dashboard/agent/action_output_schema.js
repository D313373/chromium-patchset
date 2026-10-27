// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * One flat action per turn, as a discriminated union.
 *
 * Each action is its own object with additionalProperties: false. Choosing
 * "click" makes text, url, distance, and key unrepresentable. Optional keys
 * are omitted, not nulled.
 */

const PIXEL = {type: 'integer', minimum: 0, maximum: 4000};

const COORDINATE_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['x', 'y'],
  properties: {
    x: PIXEL,
    y: PIXEL,
  },
};

function actionEnum(name) {
  return {type: 'string', enum: [name]};
}

function closed(action, properties, required) {
  return {
    type: 'object',
    additionalProperties: false,
    properties: {action: actionEnum(action), ...properties},
    required: ['action', ...required],
  };
}

export const FLAT_AGENT_ACTIONS = Object.freeze([
  'click',
  'type',
  'navigate',
  'scroll',
  'key_press',
  'wait',
  'request_screenshot',
  'complete',
]);

export function buildAgentTurnJsonSchema() {
  return {
    anyOf: [
      closed('click', {
        target_id: {type: 'integer', minimum: 0},
        coordinate: COORDINATE_SCHEMA,
      }, []),
      closed('type', {
        target_id: {type: 'integer', minimum: 0},
        text: {type: 'string'},
      }, ['target_id', 'text']),
      closed('navigate', {
        url: {type: 'string'},
      }, ['url']),
      closed('scroll', {
        direction: {type: 'string', enum: ['up', 'down']},
        distance: PIXEL,
        scope: {type: 'string', enum: ['page', 'iframe']},
        target_id: {type: 'integer', minimum: 0},
      }, ['direction', 'distance']),
      closed('key_press', {
        key: {type: 'string'},
      }, ['key']),
      closed('wait', {
        timeout: {type: 'integer', minimum: 0},
      }, ['timeout']),
      closed('request_screenshot', {}, []),
      closed('complete', {
        status: {type: 'string', enum: ['solved', 'blocked']},
        summary: {type: 'string'},
      }, ['status', 'summary']),
    ],
  };
}

const LOCATOR_TYPES = [
  'CSS_SELECTOR', 'XPATH', 'ID', 'NAME', 'CLASS_NAME', 'TAG_NAME',
  'LINK_TEXT', 'PARTIAL_LINK_TEXT', 'TEXT_CONTENT', 'ACCESSIBILITY_NAME',
];

const LOCATORS_SCHEMA = {
  type: 'array',
  minItems: 1,
  items: {
    type: 'object',
    additionalProperties: false,
    required: ['type', 'value'],
    properties: {
      type: {type: 'string', enum: LOCATOR_TYPES},
      value: {type: 'string'},
    },
  },
};

const STEP_INDEX = {type: 'integer', minimum: 0};

export const DASHBOARD_AGENT_ACTIONS = Object.freeze([
  'click',
  'type',
  'navigate',
  'scroll',
  'key_press',
  'wait',
  'investigate',
  'get_script',
  'get_step',
  'update_step',
  'run_script',
  'resume_script',
  'handoff',
  'complete',
]);

/**
 * Investigator and dashboard assistant turns. Page moves target headless
 * elements by locator because those loops have no page-map ids.
 */
export function buildDashboardTurnJsonSchema() {
  return {
    anyOf: [
      closed('click', {
        locators: LOCATORS_SCHEMA,
      }, ['locators']),
      closed('type', {
        locators: LOCATORS_SCHEMA,
        text: {type: 'string'},
      }, ['locators', 'text']),
      closed('navigate', {
        url: {type: 'string'},
      }, ['url']),
      closed('scroll', {
        direction: {type: 'string', enum: ['up', 'down']},
        distance: PIXEL,
      }, ['direction', 'distance']),
      closed('key_press', {
        key: {type: 'string'},
      }, ['key']),
      closed('wait', {
        timeout: {type: 'integer', minimum: 0},
      }, ['timeout']),
      closed('investigate', {
        url: {type: 'string'},
      }, []),
      closed('get_script', {
        script_id: {type: 'string'},
      }, ['script_id']),
      closed('get_step', {
        script_id: {type: 'string'},
        step_index: STEP_INDEX,
      }, ['script_id', 'step_index']),
      closed('update_step', {
        script_id: {type: 'string'},
        step_index: STEP_INDEX,
        locators: LOCATORS_SCHEMA,
      }, ['script_id', 'step_index', 'locators']),
      closed('run_script', {
        script_id: {type: 'string'},
      }, ['script_id']),
      closed('resume_script', {
        script_id: {type: 'string'},
        step_index: STEP_INDEX,
      }, ['script_id', 'step_index']),
      closed('handoff', {
        script_id: {type: 'string'},
        step_index: STEP_INDEX,
        diagnosis: {type: 'string'},
        user_summary: {type: 'string'},
        confidence: {type: 'string', enum: ['high', 'medium', 'low']},
        evidence: {type: 'string'},
        proposed_locators: LOCATORS_SCHEMA,
      }, ['script_id', 'step_index', 'diagnosis', 'user_summary',
          'confidence', 'evidence']),
      closed('complete', {
        status: {type: 'string', enum: ['solved', 'blocked']},
        summary: {type: 'string'},
      }, ['status', 'summary']),
    ],
  };
}

/** Post-task script review: keep selected steps, or keep the draft. */
export function buildScriptReviewJsonSchema() {
  return {
    anyOf: [
      closed('save_task_script', {
        indexes: {type: 'array', minItems: 1, items: STEP_INDEX},
        name: {type: 'string'},
      }, ['indexes', 'name']),
      closed('complete', {
        status: {type: 'string', enum: ['solved', 'blocked']},
        summary: {type: 'string'},
      }, ['status', 'summary']),
    ],
  };
}

function responseFormat(name, schema) {
  return {
    type: 'json_schema',
    json_schema: {name, strict: true, schema},
  };
}

/** OpenAI and xAI Grok chat-completions `response_format` body. */
export function buildAgentTurnResponseFormat() {
  return responseFormat('simulachrome_agent_action', buildAgentTurnJsonSchema());
}

export function buildDashboardTurnResponseFormat() {
  return responseFormat(
      'simulachrome_dashboard_action', buildDashboardTurnJsonSchema());
}

export function buildScriptReviewResponseFormat() {
  return responseFormat(
      'simulachrome_script_review', buildScriptReviewJsonSchema());
}

const OPENAI_STRICT_STRIPPED = new Set([
  'minimum',
  'maximum',
  'minLength',
  'maxLength',
  'minItems',
  'maxItems',
  'pattern',
  'format',
]);

function schemaFingerprint(value) {
  const text = JSON.stringify(value);
  let hash = 5381;
  for (let i = 0; i < text.length; i++) {
    hash = Math.imul(hash, 33) + text.charCodeAt(i);
  }
  return (hash >>> 0).toString();
}

function lowerSchemaForOpenAIStrict(node, remember) {
  if (Array.isArray(node)) {
    return node.map((item) => lowerSchemaForOpenAIStrict(item, remember));
  }
  if (!node || typeof node !== 'object') {
    return node;
  }
  const required = new Set(Array.isArray(node.required) ? node.required : []);
  const out = {};
  for (const [key, value] of Object.entries(node)) {
    if (OPENAI_STRICT_STRIPPED.has(key)) {
      remember(key);
      continue;
    }
    if (key === 'properties' && value && typeof value === 'object') {
      continue;
    }
    if (key === 'anyOf' || key === 'oneOf' || key === 'items' ||
        key === 'required' || key === 'additionalProperties') {
      continue;
    }
    out[key] = lowerSchemaForOpenAIStrict(value, remember);
  }
  for (const key of ['anyOf', 'oneOf']) {
    if (Array.isArray(node[key])) {
      out[key] = node[key].map(
          (item) => lowerSchemaForOpenAIStrict(item, remember));
    }
  }
  if (node.items) {
    out.items = lowerSchemaForOpenAIStrict(node.items, remember);
  }
  if (node.properties && typeof node.properties === 'object') {
    const properties = {};
    const nextRequired = [];
    let madeNullable = false;
    for (const [name, property] of Object.entries(node.properties)) {
      const lowered = lowerSchemaForOpenAIStrict(property, remember);
      if (required.has(name)) {
        properties[name] = lowered;
      } else {
        properties[name] = {anyOf: [lowered, {type: 'null'}]};
        madeNullable = true;
      }
      nextRequired.push(name);
    }
    if (madeNullable) {
      remember('optional_to_nullable');
    }
    out.properties = properties;
    out.required = nextRequired;
    out.additionalProperties = false;
  } else if ('additionalProperties' in node) {
    out.additionalProperties = false;
  }
  return out;
}

function wrapResultObject(schema, remember) {
  if (schema && schema.type === 'object' && schema.properties?.result &&
      Array.isArray(schema.required) && schema.required.includes('result')) {
    return schema;
  }
  remember('root_result_wrapper');
  return {
    type: 'object',
    additionalProperties: false,
    required: ['result'],
    properties: {result: schema},
  };
}

function canonicalSchemaFromFormat(format) {
  return format?.json_schema?.schema || format?.schema || null;
}

/**
 * Compile the canonical action union for one provider transport.
 * `verified` stays false until a live endpoint test records enforcement.
 */
export function compileStructuredOutput(provider, transport, canonicalFormat) {
  const format = canonicalFormat || buildAgentTurnResponseFormat();
  const schema = canonicalSchemaFromFormat(format);
  const name = format?.json_schema?.name || 'simulachrome_agent_action';
  const lowered = [];
  const remember = (key) => {
    if (!lowered.includes(key)) {
      lowered.push(key);
    }
  };
  const diagnostics = {
    provider,
    transport,
    verified: false,
    schema_hash: schemaFingerprint(schema),
    mode: 'prompt_only',
    strict: false,
    fallback_reason: '',
    lowered,
  };
  const plan = {diagnostics};
  const finish = (mode, reason, strict) => {
    diagnostics.mode = mode;
    diagnostics.fallback_reason = reason;
    diagnostics.strict = strict;
    return plan;
  };
  if (!schema) {
    return finish('prompt_only', 'missing_schema', false);
  }
  if (transport === 'ollama_format') {
    plan.format = schema;
    return finish(
        'schema_supported_non_strict', 'ollama_format_enforcement_unverified',
        false);
  }
  if (transport === 'openai_response_format') {
    const strictOpenAI = provider === 'openai';
    const wireSchema = wrapResultObject(
        strictOpenAI ? lowerSchemaForOpenAIStrict(schema, remember) : schema,
        remember);
    plan.response_format = {
      type: 'json_schema',
      json_schema: {name, strict: strictOpenAI, schema: wireSchema},
    };
    return finish(
        strictOpenAI ? 'strict_schema_requested' : 'schema_supported_non_strict',
        strictOpenAI ? '' : 'strict_subset_unverified', strictOpenAI);
  }
  if (transport === 'anthropic_output_config') {
    plan.output_config = {
      format: {type: 'json_schema', schema: wrapResultObject(schema, remember)},
    };
    return finish(
        'schema_supported_non_strict',
        'anthropic_structured_outputs_unverified', false);
  }
  if (transport === 'gemini_response_json_schema') {
    plan.generation_config = {
      responseMimeType: 'application/json',
      responseJsonSchema: wrapResultObject(schema, remember),
    };
    return finish(
        'schema_supported_non_strict', 'gemini_response_json_schema_unverified',
        false);
  }
  if (!transport) {
    return finish('prompt_only', 'no_structured_transport', false);
  }
  return finish('unsupported', 'unknown_structured_transport', false);
}

/** Pull a bare action out of a provider result wrapper. */
export function unwrapStructuredAction(parsed) {
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return parsed;
  }
  if (typeof parsed.action === 'string' && parsed.action) {
    return parsed;
  }
  const result = parsed.result;
  if (result && typeof result === 'object' && !Array.isArray(result) &&
      typeof result.action === 'string' && result.action) {
    return result;
  }
  return parsed;
}

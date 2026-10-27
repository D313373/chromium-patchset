// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run: node --test chrome/browser/resources/dashboard/agent/action_output_schema_test.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath, pathToFileURL} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const tmp = path.join(
    fs.mkdtempSync(path.join(os.tmpdir(), 'schema-')),
    'action_output_schema.mjs');
fs.copyFileSync(path.join(here, 'action_output_schema.js'), tmp);
const schema = await import(pathToFileURL(tmp).href);

function embeddedSchema(filename) {
  const raw = fs.readFileSync(
      path.resolve(here, '../../../ui/webui/dashboard', filename), 'utf8');
  const open = raw.indexOf('(');
  const close = raw.lastIndexOf(')schema');
  assert.ok(open > 0 && close > open, filename);
  return raw.slice(open + 1, close);
}

function clickBranch(wireSchema) {
  const branches = wireSchema.properties.result.anyOf;
  return branches.find((branch) => branch.properties.action.enum[0] === 'click');
}

test('embedded agent and script schemas match the canonical builders', () => {
  assert.equal(
      embeddedSchema('agent_turn_schema.inc'),
      JSON.stringify(schema.buildAgentTurnResponseFormat()));
  assert.equal(
      embeddedSchema('script_review_schema.inc'),
      JSON.stringify(schema.buildScriptReviewResponseFormat()));
});

test('openai strict wrapper lowers optionals and does not claim verification', () => {
  const canonical = schema.buildAgentTurnResponseFormat();
  const plan = schema.compileStructuredOutput(
      'openai', 'openai_response_format', canonical);
  assert.equal(plan.diagnostics.mode, 'strict_schema_requested');
  assert.equal(plan.diagnostics.verified, false);
  assert.equal(plan.diagnostics.strict, true);
  assert.notEqual(plan.diagnostics.mode, 'strict_schema_verified');
  assert.ok(plan.diagnostics.lowered.includes('root_result_wrapper'));
  assert.ok(plan.diagnostics.lowered.includes('minimum'));
  const wire = plan.response_format.json_schema;
  assert.equal(wire.strict, true);
  assert.equal(wire.schema.type, 'object');
  assert.deepEqual(wire.schema.required, ['result']);
  assert.equal(wire.schema.additionalProperties, false);
  assert.equal(wire.schema.properties.result.anyOf.length, 8);
  const click = clickBranch(wire.schema);
  assert.equal(click.properties.text, undefined);
  assert.equal(click.properties.url, undefined);
  assert.equal(click.properties.target_id.anyOf[1].type, 'null');
  assert.ok(click.required.includes('target_id'));
  const coordinate = click.properties.coordinate.anyOf[0];
  assert.equal(coordinate.properties.x.type, 'integer');
  assert.equal(coordinate.properties.x.minimum, undefined);
  assert.equal(coordinate.properties.x.maximum, undefined);
});

test('compatible providers keep distinct unverified transports', () => {
  const canonical = schema.buildAgentTurnResponseFormat();
  const grok = schema.compileStructuredOutput(
      'grok', 'openai_response_format', canonical);
  assert.equal(grok.diagnostics.mode, 'schema_supported_non_strict');
  assert.equal(grok.response_format.json_schema.strict, false);
  assert.equal(grok.response_format.json_schema.schema.type, 'object');
  assert.equal(
      grok.response_format.json_schema.schema.properties.result.anyOf.length, 8);
  assert.equal(
      grok.response_format.json_schema.schema.properties.result.anyOf[0]
          .properties.coordinate.minimum,
      undefined);
  assert.equal(
      grok.response_format.json_schema.schema.properties.result.anyOf[0]
          .properties.coordinate.properties.x.minimum,
      0);

  const ollama = schema.compileStructuredOutput(
      'ollama', 'ollama_format', canonical);
  assert.equal(ollama.format.anyOf.length, 8);
  assert.equal(ollama.format.properties, undefined);
  assert.equal(ollama.response_format, undefined);
  assert.equal(ollama.diagnostics.fallback_reason,
               'ollama_format_enforcement_unverified');

  const anthropic = schema.compileStructuredOutput(
      'anthropic', 'anthropic_output_config', canonical);
  assert.equal(anthropic.output_config.format.type, 'json_schema');
  assert.equal(anthropic.output_config.format.schema.type, 'object');
  assert.equal(anthropic.response_format, undefined);

  const gemini = schema.compileStructuredOutput(
      'gemini', 'gemini_response_json_schema', canonical);
  assert.equal(gemini.generation_config.responseMimeType, 'application/json');
  assert.equal(gemini.generation_config.responseJsonSchema.type, 'object');
  assert.equal(gemini.response_format, undefined);

  const unknown = schema.compileStructuredOutput('other', 'mystery', canonical);
  assert.equal(unknown.diagnostics.mode, 'unsupported');
  assert.equal(unknown.response_format, undefined);
});

test('unwrap keeps bare actions and extracts result wrappers', () => {
  assert.deepEqual(
      schema.unwrapStructuredAction({
        result: {action: 'navigate', url: 'https://example.com/'},
      }),
      {action: 'navigate', url: 'https://example.com/'});
  assert.deepEqual(
      schema.unwrapStructuredAction({action: 'wait', timeout: 1}),
      {action: 'wait', timeout: 1});
  assert.deepEqual(schema.unwrapStructuredAction({result: {url: 'x'}}),
                   {result: {url: 'x'}});
});

test('script review compiles without merging into the agent action enum', () => {
  const review = schema.buildScriptReviewResponseFormat();
  const plan = schema.compileStructuredOutput(
      'openai', 'openai_response_format', review);
  const names = plan.response_format.json_schema.schema.properties.result.anyOf
      .map((branch) => branch.properties.action.enum[0]);
  assert.deepEqual(names, ['save_task_script', 'complete']);
});

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run: node --test chrome/browser/resources/glic/glic_panel_transport_test.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createRequire} from 'node:module';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const clientPath = path.resolve(
    here, '../../../test/data/webui/glic/test_client/ollama_client.ts');
const source = fs.readFileSync(clientPath, 'utf8');
const start = source.indexOf('export function resolveGlicClientPrefsUrl');
const end = source.indexOf('class OllamaWebClient');
assert.ok(start > 0 && end > start, 'transport helpers missing from client');

const require = createRequire(
    path.resolve(here, '../../../../third_party/node/node_modules/typescript/lib/typescript.js'));
const ts = require(
    path.resolve(here, '../../../../third_party/node/node_modules/typescript/lib/typescript.js'));
const js = ts.transpileModule(source.slice(start, end), {
  compilerOptions: {module: ts.ModuleKind.ES2022, target: ts.ScriptTarget.ES2022},
}).outputText;
const helpers = await import(
    'data:text/javascript,' + encodeURIComponent(js));

test('blob and chrome guests do not fetch a root-relative glic-prefs URL', () => {
  assert.equal(helpers.resolveGlicClientPrefsUrl({
    protocol: 'blob:',
    origin: 'chrome://glic',
  }), null);
  assert.equal(helpers.resolveGlicClientPrefsUrl({
    protocol: 'chrome:',
    origin: 'chrome://glic',
  }), null);
  assert.equal(helpers.resolveGlicClientPrefsUrl({
    protocol: 'blob:',
    origin: 'null',
  }), null);
  assert.equal(helpers.resolveGlicClientPrefsUrl(null), null);
});

test('the dev HTTP client still uses serve.py /glic-prefs', () => {
  assert.equal(helpers.resolveGlicClientPrefsUrl({
    protocol: 'http:',
    origin: 'http://localhost:8000',
  }), 'http://localhost:8000/glic-prefs');
  assert.equal(helpers.resolveGlicClientPrefsUrl({
    protocol: 'https:',
    origin: 'https://example.test/',
  }), 'https://example.test/glic-prefs');
});

test('an empty host does not count as an enabled tab-context permission', async () => {
  assert.equal(await helpers.requestTabContextPermission({}, true), 'unavailable');
  assert.equal(await helpers.requestTabContextPermission(null, true), 'unavailable');
});

test('a real host without the method rejects instead of succeeding', async () => {
  await assert.rejects(
      helpers.requestTabContextPermission({closePanel() {}}, true),
      /setTabContextPermissionState is unavailable/);
});

test('the supported host method is invoked and a rejection stays a failure', async () => {
  let enabled = null;
  const outcome = await helpers.requestTabContextPermission({
    setTabContextPermissionState(value) {
      enabled = value;
      return Promise.resolve();
    },
  }, true);
  assert.equal(outcome, 'enabled');
  assert.equal(enabled, true);

  await assert.rejects(helpers.requestTabContextPermission({
    async setTabContextPermissionState() {
      throw new Error('pref write failed');
    },
  }, true), /pref write failed/);
});

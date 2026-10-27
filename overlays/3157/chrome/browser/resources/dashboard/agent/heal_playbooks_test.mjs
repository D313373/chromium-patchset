// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Run: node --test chrome/browser/resources/dashboard/agent/heal_playbooks_test.mjs
// The module under test is a browser ES module with a .js extension, so copy it
// to a temp .mjs before importing it.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath, pathToFileURL} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const tmp = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'heal-')), 'heal_playbooks.mjs');
fs.copyFileSync(path.join(here, 'heal_playbooks.js'), tmp);
const m = await import(pathToFileURL(tmp).href);
const {HEAL_CATEGORIES: C} = m;

const click = (params, extra = {}) => ({action: 'click', params, ...extra});

test('classifies by message, action kind and context', () => {
  const cls = (error, failedAction, contextUrlMismatch) =>
      m.classifyHealFailure({error, failedAction, contextUrlMismatch}).category;
  assert.equal(cls('Element not found', click({})), C.LOCATOR_NOT_FOUND);
  assert.equal(cls('Wait timeout after 15000ms', click({})), C.LOCATOR_NOT_FOUND);
  assert.equal(cls('Navigation timed out', {action: 'navigate'}), C.NAVIGATION_TIMEOUT);
  assert.equal(cls('Listen timed out', {action: 'listen'}), C.WAIT_TIMEOUT);
  assert.equal(cls('timeout', {action: 'wait'}), C.WAIT_TIMEOUT);
  assert.equal(cls('Every enabled login method failed.', {action: 'authenticate'}), C.AUTH_WALL);
  assert.equal(cls('Key Script finished but the login wall is still up', click({})), C.AUTH_WALL);
  assert.equal(cls('WebContents destroyed', click({})), C.FRAME_LOST);
  assert.equal(cls('click suppressed', click({})), C.NOT_INTERACTABLE);
  assert.equal(cls('element is covered by overlay', click({})), C.NOT_INTERACTABLE);
  assert.equal(cls('403 Forbidden', {action: 'navigate'}), C.BLOCKED);
  assert.equal(cls('Context URL mismatch (expected=a actual=b)', click({})), C.CONTEXT_MISMATCH);
  assert.equal(cls('', click({}), {mismatch: true}), C.CONTEXT_MISMATCH);
  assert.equal(cls('Invalid text regex: (', click({})), C.INVALID_INPUT);
  assert.equal(cls('something odd', click({})), C.UNKNOWN);
});

test('context mismatch outranks everything else', () => {
  const r = m.classifyHealFailure({
    error: 'Element not found | Context URL mismatch (expected=x actual=y)',
    failedAction: click({}),
  });
  assert.equal(r.category, C.CONTEXT_MISMATCH);
});

test('adjacent context: the failed step is a bridge between two good steps', () => {
  const runDetails = {steps: [
    {stepIndex: 1, status: 'success', url_before: 'https://shop.test/cart?x=1',
     url_after: 'https://shop.test/cart#top', action: {action: 'click', params: {}}},
    {stepIndex: 2, status: 'fail', url_before: 'https://shop.test/cart',
     action: click({coordinates: {x: 10, y: 20}})},
    {stepIndex: 3, status: 'success', url_before: 'https://shop.test/checkout?token=abc',
     action: {action: 'type', params: {text: 'hunter2!'}}},
  ]};
  const adj = m.buildAdjacentStepContext({runDetails, failedStep: 2});
  assert.equal(adj.prev.url_after, 'https://shop.test/cart');  // query/hash stripped
  assert.equal(adj.expected_url_after, 'https://shop.test/checkout');
  assert.equal(adj.step_probably_navigates, true);
  // literal typed text must never survive into a prompt
  assert.equal(adj.next.action.text, '<literal len=8>');
});

test('tokens are kept, literals are masked', () => {
  const t = (text) => m.summarizeActionSafe({action: 'type', params: {text}}).text;
  assert.equal(t('{Password}'), '{Password}');
  assert.equal(t('p@ssw0rd'), '<literal len=8>');
});

test('falls back to the saved script when the run never reached later steps', () => {
  const adj = m.buildAdjacentStepContext({
    runDetails: {steps: [{stepIndex: 0, status: 'success', url_after: 'https://a.test/'},
                         {stepIndex: 1, status: 'fail', url_before: 'https://a.test/'}]},
    scriptActions: [
      {action: 'navigate', params: {url: 'https://a.test/'}},
      click({locators: [{type: 'ID', value: 'go'}]}),
      {action: 'navigate', params: {url: 'https://a.test/done?s=1'}},
    ],
    failedStep: 1,
  });
  assert.equal(adj.next.status, 'not_run');
  assert.equal(adj.expected_url_after, 'https://a.test/done');
  assert.equal(adj.step_probably_navigates, true);
});

test('evidence levels', () => {
  const lvl = (a) => m.buildEvidenceProfile({failedAction: a, adjacent: null}).evidence_level;
  assert.equal(lvl(click({locators: [{type: 'ID', value: 'x'}], ax_name: 'Go'})), 'rich');
  assert.equal(lvl(click({locators: [{type: 'ID', value: 'x'}]})), 'partial');
  assert.equal(lvl(click({coordinates: {x: 1, y: 2}})), 'sparse');
  assert.equal(lvl(null), 'sparse');
});

test('probe results feed the evidence profile', () => {
  const p = m.buildEvidenceProfile({
    failedAction: click({locators: [{type: 'ID', value: 'a'}, {type: 'CSS_SELECTOR', value: '.b'}]}),
    adjacent: null,
    probe: {per_locator: [{matches: 0}, {matches: 2}]},
  });
  assert.equal(p.locators_probed, 2);
  assert.equal(p.locators_still_matching, 1);
});

test('sparse click with a navigating next step hunts hrefs', () => {
  const plan = m.planHealInvestigation({
    error: 'Element not found',
    failedAction: click({coordinates: {x: 100, y: 200}}),
    failedStep: 1,
    runDetails: {steps: [
      {stepIndex: 0, status: 'success', url_after: 'https://a.test/list'},
      {stepIndex: 1, status: 'fail', url_before: 'https://a.test/list'},
      {stepIndex: 2, status: 'success', url_before: 'https://a.test/item/42'},
    ]},
  });
  const text = plan.lines.join('\n');
  assert.equal(plan.profile.evidence_level, 'sparse');
  assert.match(text, /anchors on the page whose href equals/);
  assert.match(text, /https:\/\/a\.test\/item\/42/);
  assert.match(text, /Evidence is sparse/);
  assert.match(text, /coordinates/i);
  assert.match(text, /"confidence"/);
});

test('rich click with no navigation does not invent a bridge', () => {
  const plan = m.planHealInvestigation({
    error: 'Element not found',
    failedAction: click({locators: [{type: 'ID', value: 'save'}], ax_name: 'Save'}),
    failedStep: 0,
    runDetails: {steps: [{stepIndex: 0, status: 'fail', url_before: 'https://a.test/'}]},
  });
  const text = plan.lines.join('\n');
  assert.doesNotMatch(text, /Bridge hint/);
  assert.match(text, /Probe every saved locator/);
  assert.match(text, /role \+ accessible name/);
});

test('type failures check tokens and autofill, not just locators', () => {
  const plan = m.planHealInvestigation({
    error: 'Element not found',
    failedAction: {action: 'type', params: {text: '{Email}', locators: [{type: 'ID', value: 'email'}]}},
    failedStep: 3,
  });
  const text = plan.lines.join('\n');
  assert.match(text, /unresolved token is a data problem/);
  assert.match(text, /:autofill/);
});

test('auth playbook encodes the login-state and login-success heuristics', () => {
  const plan = m.planHealInvestigation({
    error: 'Key Script finished but the login wall is still up',
    failedAction: click({}),
    failedStep: 2,
    runDetails: {steps: [{stepIndex: 1, status: 'success', url_after: 'https://a.test/login?next=/x'}]},
  });
  const text = plan.lines.join('\n');
  assert.equal(plan.classification.category, C.AUTH_WALL);
  assert.match(text, /LOGGED OUT looks like/);
  assert.match(text, /ALL frames and shadow roots/);
  assert.match(text, /URL left the login path/);
  assert.match(text, /human_required/);
  assert.match(text, /https:\/\/a\.test\/login/);
  assert.doesNotMatch(text, /next=/);
});

test('blocked runs never plan to defeat the check', () => {
  const plan = m.planHealInvestigation({error: 'blocked: captcha challenge', failedAction: click({}), failedStep: 0});
  assert.match(plan.lines.join('\n'), /Do not attempt to defeat a captcha/);
});

test('every category yields a non-empty plan with a handoff contract', () => {
  for (const category of Object.values(C)) {
    const pb = m.buildHealPlaybook({
      classification: {category, confidence: 0.5, signals: []},
      profile: m.buildEvidenceProfile({failedAction: click({}), adjacent: null}),
      adjacent: null,
    });
    assert.ok(pb.steps.length > 0, category);
    assert.ok(pb.handoff.length > 0, category);
    assert.ok(pb.avoid.some((a) => /Do not edit the script/.test(a)), category);
  }
});

test('urlForPrompt drops queries and fragments', () => {
  assert.equal(m.urlForPrompt('https://a.test/p?token=SECRET#frag'), 'https://a.test/p');
  assert.equal(m.urlForPrompt('not a url?x=1'), 'not a url');
});

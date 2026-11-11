// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {
  CAPTCHA_BRANDS,
  THIRD_PARTY_FILL,
  THIRD_PARTY_LABEL,
  captchaBrand,
  createCustomLoginMethod,
  defaultAuthenticateParams,
  keysForTags,
  tokenForKey,
} from './authenticate_model.js';

let authDrag = null;

function ensureMethods(action) {
  if (!action.params || typeof action.params !== 'object') {
    action.params = {};
  }
  if (!Array.isArray(action.params.methods) || !action.params.methods.length) {
    action.params.methods = defaultAuthenticateParams().methods;
  }
  for (const method of action.params.methods) {
    if (!method.captcha || typeof method.captcha !== 'object') {
      method.captcha = {enabled: false, brand: 'recaptcha', challenge: 'checkbox'};
    }
    if (method.custom === true) {
      if (!Array.isArray(method.actions)) method.actions = [];
    } else if (!Array.isArray(method.steps)) {
      method.steps = [];
    }
  }
  return action.params.methods;
}

function moveItem(list, fromId, toId, idOf) {
  const from = list.findIndex((item) => idOf(item) === fromId);
  const to = list.findIndex((item) => idOf(item) === toId);
  if (from < 0 || to < 0 || from === to) {
    return false;
  }
  const [item] = list.splice(from, 1);
  list.splice(to, 0, item);
  return true;
}

function fillSelect(select, options, value) {
  select.replaceChildren();
  for (const option of options) {
    const node = document.createElement('option');
    node.value = option.value;
    node.textContent = option.label;
    if (option.title) {
      node.title = option.title;
    }
    select.appendChild(node);
  }
  select.value = value || '';
  if (value && select.value !== value) {
    const extra = document.createElement('option');
    extra.value = value;
    extra.textContent = value;
    select.appendChild(extra);
    select.value = value;
  }
}

function credentialOptions(source, tags) {
  const keys = keysForTags(source, tags);
  const bases = new Map();
  for (const raw of keys) {
    const base = raw.split('#')[0];
    bases.set(base, (bases.get(base) || 0) + 1);
  }
  return [
    {value: '', label: 'Select a saved key'},
    {value: THIRD_PARTY_FILL, label: THIRD_PARTY_LABEL},
    ...keys.map((raw) => {
      const base = raw.split('#')[0];
      return {
        value: tokenForKey(raw),
        label: bases.get(base) > 1 ? raw : base,
        title: raw,
      };
    }),
  ];
}

function emailOptions() {
  return credentialOptions('variable', ['email'])
      .filter((option) => option.value !== THIRD_PARTY_FILL);
}

function stopControlDrag(event) {
  if (event.target.closest('input, select, button, label, option')) {
    event.preventDefault();
    event.stopPropagation();
  }
}

export function appendAuthenticateBranches(action, container, hooks) {
  const methods = ensureMethods(action);
  const flow = document.createElement('div');
  flow.className = 'auth-flow';
  const loop = document.createElement('div');
  loop.className = 'auth-loop';
  const loopHead = document.createElement('div');
  loopHead.className = 'auth-loop-head';
  loopHead.textContent = 'While login has not succeeded and a method has not been tried';
  const row = document.createElement('div');
  row.className = 'auth-branches';
  loop.appendChild(loopHead);
  loop.appendChild(row);
  const firstEnabled = methods.findIndex((method) => method.enabled !== false);

  const persist = () => {
    hooks.scheduleAutoSave();
    hooks.renderAll();
  };

  methods.forEach((method, index) => {
    const branch = document.createElement('div');
    branch.className = 'auth-branch' + (method.enabled === false ? ' is-off' : '');
    branch.dataset.methodId = method.id;

    const head = document.createElement('div');
    head.className = 'auth-branch-head';

    const title = document.createElement('div');
    title.className = 'auth-row';
    const grip = document.createElement('span');
    grip.className = 'auth-grip';
    grip.textContent = '⋮⋮';
    grip.draggable = true;
    grip.title = 'Drag to change priority. Far left is tried first.';
    grip.addEventListener('dragstart', (event) => {
      authDrag = {kind: 'method', id: method.id};
      event.stopPropagation();
      event.dataTransfer.effectAllowed = 'move';
      event.dataTransfer.setData('application/x-simulachrome-auth', method.id);
    });
    grip.addEventListener('dragend', () => {
      authDrag = null;
    });
    const label = document.createElement('span');
    label.className = 'auth-label';
    const rank = index === firstEnabled ? 'Primary' : `Priority ${index + 1}`;
    if (method.custom === true) {
      const name = document.createElement('input');
      name.type = 'text';
      name.className = 'input-field auth-name';
      name.value = method.label || '';
      name.placeholder = 'Login with X';
      name.title = rank;
      name.addEventListener('click', (event) => event.stopPropagation());
      name.addEventListener('input', (event) => {
        event.stopPropagation();
        method.label = name.value;
        hooks.scheduleAutoSave();
      });
      label.appendChild(document.createTextNode(`${rank} · `));
      label.appendChild(name);
    } else {
      label.textContent = `${rank} · ${method.label || method.id}`;
    }
    title.appendChild(grip);
    title.appendChild(label);
    if (!method.builtin) {
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.className = 'delete-action-btn';
      remove.textContent = 'X';
      remove.title = 'Remove method';
      remove.addEventListener('click', (event) => {
        event.stopPropagation();
        action.params.methods =
            methods.filter((candidate) => candidate.id !== method.id);
        persist();
      });
      title.appendChild(remove);
    }
    head.appendChild(title);

    head.appendChild(checkbox(
        'Use this method',
        method.enabled !== false,
        (checked) => {
          method.enabled = checked;
          persist();
        }));
    if (method.custom !== true) {
    head.appendChild(checkbox(
        'Captcha on this path',
        !!method.captcha.enabled,
        (checked) => {
          method.captcha.enabled = checked;
          const captchaStep = (method.steps || []).find((item) => item.kind === 'captcha');
          if (captchaStep) {
            captchaStep.enabled = checked;
          }
          persist();
        }));

    if (method.captcha.enabled) {
      const brandSelect = document.createElement('select');
      brandSelect.className = 'input-field';
      fillSelect(
          brandSelect,
          CAPTCHA_BRANDS.map((brand) => ({value: brand.id, label: brand.label})),
          method.captcha.brand || 'recaptcha');
      brandSelect.addEventListener('change', (event) => {
        event.stopPropagation();
        method.captcha.brand = brandSelect.value;
        const brand = captchaBrand(brandSelect.value);
        const challenges = brand ? brand.challenges : [];
        if (!challenges.some((item) => item.id === method.captcha.challenge)) {
          method.captcha.challenge = challenges[0] ? challenges[0].id : '';
        }
        persist();
      });
      const challengeSelect = document.createElement('select');
      challengeSelect.className = 'input-field';
      const brand = captchaBrand(method.captcha.brand) || CAPTCHA_BRANDS[0];
      fillSelect(
          challengeSelect,
          brand.challenges.map((item) => ({value: item.id, label: item.label})),
          method.captcha.challenge || brand.challenges[0].id);
      challengeSelect.addEventListener('change', (event) => {
        event.stopPropagation();
        method.captcha.challenge = challengeSelect.value;
        persist();
      });
      head.appendChild(brandSelect);
      head.appendChild(challengeSelect);
    }
    }
    branch.appendChild(head);

    if (method.custom === true) {
      if (typeof hooks.renderMethodTimeline === 'function') {
        hooks.renderMethodTimeline(method, branch);
      }
    } else {
      for (const loginStep of method.steps || []) {
        branch.appendChild(renderStep(method, loginStep, persist));
      }
    }

    branch.addEventListener('dragover', (event) => {
      if (!authDrag) {
        return;
      }
      event.preventDefault();
      event.stopPropagation();
    });
    branch.addEventListener('drop', (event) => {
      if (!authDrag || authDrag.kind !== 'method') {
        return;
      }
      event.preventDefault();
      event.stopPropagation();
      const moved = moveItem(methods, authDrag.id, method.id, (item) => item.id);
      authDrag = null;
      if (moved) {
        persist();
      }
    });
    row.appendChild(branch);
  });

  const merge = document.createElement('div');
  merge.className = 'auth-merge';
  merge.textContent = 'Login paths rejoin here';

  const addRow = document.createElement('form');
  addRow.className = 'auth-add-row';
  const addName = document.createElement('input');
  addName.type = 'text';
  addName.className = 'input-field';
  addName.placeholder = 'Login with X';
  const addButton = document.createElement('button');
  addButton.type = 'submit';
  addButton.className = 'btn';
  addButton.textContent = 'Add login method';
  addRow.appendChild(addName);
  addRow.appendChild(addButton);
  addRow.addEventListener('submit', (event) => {
    event.preventDefault();
    event.stopPropagation();
    const label = addName.value.trim();
    if (!label) return;
    methods.push(createCustomLoginMethod(label, methods));
    persist();
  });
  flow.appendChild(loop);
  flow.appendChild(merge);
  flow.appendChild(addRow);
  container.appendChild(flow);
}

function checkbox(text, checked, onChange) {
  const label = document.createElement('label');
  label.className = 'auth-check';
  const input = document.createElement('input');
  input.type = 'checkbox';
  input.checked = !!checked;
  input.addEventListener('click', (event) => event.stopPropagation());
  input.addEventListener('change', (event) => {
    event.stopPropagation();
    onChange(input.checked);
  });
  const span = document.createElement('span');
  span.textContent = text;
  label.appendChild(input);
  label.appendChild(span);
  return label;
}

function renderStep(method, loginStep, persist) {
  const row = document.createElement('div');
  row.className = 'auth-step' + (loginStep.enabled === false ? ' is-off' : '');
  const title = document.createElement('div');
  title.className = 'auth-row';
  const grip = document.createElement('span');
  grip.className = 'auth-grip';
  grip.textContent = '⋮⋮';
  grip.draggable = true;
  grip.title = 'Drag to reorder this step';
  grip.addEventListener('dragstart', (event) => {
    stopControlDrag(event);
    if (event.defaultPrevented) {
      return;
    }
    authDrag = {kind: 'step', methodId: method.id, id: loginStep.id};
    event.stopPropagation();
    event.dataTransfer.effectAllowed = 'move';
    event.dataTransfer.setData('application/x-simulachrome-auth-step', loginStep.id);
  });
  grip.addEventListener('dragend', () => {
    authDrag = null;
  });
  title.appendChild(grip);
  title.appendChild(checkbox(
      loginStep.label || loginStep.kind,
      loginStep.enabled !== false,
      (checked) => {
        loginStep.enabled = checked;
        if (loginStep.kind === 'captcha') {
          method.captcha.enabled = checked;
        }
        persist();
      }));
  row.appendChild(title);

  if (loginStep.kind === 'username' || loginStep.kind === 'password') {
    const select = document.createElement('select');
    select.className = 'input-field';
    const options = loginStep.kind === 'username' ?
      credentialOptions('variable', ['username', 'email']) :
      credentialOptions('secret', ['password', 'pw']);
    fillSelect(select, options, loginStep.token || '');
    select.addEventListener('change', (event) => {
      event.stopPropagation();
      loginStep.token = select.value;
      persist();
    });
    row.appendChild(select);
  }
  if (loginStep.kind === 'two_factor') {
    const select = document.createElement('select');
    select.className = 'input-field';
    fillSelect(select, emailOptions(), loginStep.emailToken || '');
    select.addEventListener('change', (event) => {
      event.stopPropagation();
      loginStep.emailToken = select.value;
      persist();
    });
    row.appendChild(select);
  }

  row.addEventListener('dragover', (event) => {
    if (!authDrag || authDrag.kind !== 'step' || authDrag.methodId !== method.id) {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
  });
  row.addEventListener('drop', (event) => {
    if (!authDrag || authDrag.kind !== 'step' || authDrag.methodId !== method.id) {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    const moved = moveItem(
        method.steps, authDrag.id, loginStep.id, (item) => item.id);
    authDrag = null;
    if (moved) {
      persist();
    }
  });
  return row;
}

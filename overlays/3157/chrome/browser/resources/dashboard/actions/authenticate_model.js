// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Login methods are an ordered list. Index 0 is the far-left, highest-priority
// method. Adding a method appends it and the timeline splits the row evenly.
// Providers that are not in use are not inserted until the user adds them.

export const THIRD_PARTY_FILL = 'third_party';
export const THIRD_PARTY_LABEL =
    'Fill this field using a third party password manager or service.';

export const CAPTCHA_BRANDS = [
  {
    id: 'recaptcha',
    label: 'reCAPTCHA',
    challenges: [
      {id: 'checkbox', label: 'Checkbox'},
      {id: 'image_grid', label: 'Image grid'},
      {id: 'audio', label: 'Audio'},
    ],
  },
  {
    id: 'hcaptcha',
    label: 'hCaptcha',
    challenges: [
      {id: 'checkbox', label: 'Checkbox'},
      {id: 'image_grid', label: 'Image grid'},
    ],
  },
  {
    id: 'turnstile',
    label: 'Cloudflare Turnstile',
    challenges: [
      {id: 'checkbox', label: 'Checkbox'},
      {id: 'managed', label: 'Managed challenge'},
      {id: 'interactive', label: 'Interactive challenge'},
    ],
  },
  {
    id: 'funcaptcha',
    label: 'FunCaptcha',
    challenges: [
      {id: 'rotate', label: 'Rotate'},
      {id: 'tile_match', label: 'Tile match'},
    ],
  },
  {
    id: 'geetest',
    label: 'GeeTest',
    challenges: [
      {id: 'slide', label: 'Slide'},
      {id: 'icon_click', label: 'Icon click'},
      {id: 'space', label: 'Spatial'},
    ],
  },
  {
    id: 'iconcaptcha',
    label: 'IconCaptcha',
    challenges: [
      {id: 'icon_match', label: 'Icon match'},
      {id: 'odd_one_out', label: 'Odd one out'},
    ],
  },
];

function step(id, kind, label, enabled, extra = {}) {
  return {id, kind, label, enabled: !!enabled, ...extra};
}

function providerSteps(providerName) {
  return [
    step('open_menu', 'open_menu', 'Open login menu', true),
    step('provider_click', 'provider_click', `Continue with ${providerName}`, true),
    step('captcha', 'captcha', 'Captcha', false),
    step('two_factor', 'two_factor', '2FA code', false, {emailToken: ''}),
    step('submit', 'submit', 'Submit login', true),
  ];
}

function credentialSteps() {
  return [
    step('open_menu', 'open_menu', 'Open login menu', true),
    step('username', 'username', 'Username or email', true, {token: ''}),
    step('password', 'password', 'Password', true, {token: ''}),
    step('captcha', 'captcha', 'Captcha', false),
    step('two_factor', 'two_factor', '2FA code', false, {emailToken: ''}),
    step('submit', 'submit', 'Submit login', true),
  ];
}

// Registry of method templates. Only the entries returned by
// defaultAuthenticateParams() are placed on a new action.
const METHOD_TEMPLATES = [
  {
    id: 'credentials',
    label: 'Credentials',
    builtin: true,
    enabled: true,
    steps: credentialSteps,
  },
  {
    id: 'google',
    label: 'Log in with Google',
    builtin: true,
    enabled: true,
    steps: () => providerSteps('Google'),
  },
  {
    id: 'passkey',
    label: 'Sign in with passkey',
    builtin: true,
    enabled: false,
    steps: () => providerSteps('passkey'),
  },
];

export function createLoginMethod(template) {
  return {
    id: template.id,
    label: template.label,
    builtin: !!template.builtin,
    enabled: template.enabled !== false,
    captcha: {enabled: false, brand: 'recaptcha', challenge: 'checkbox'},
    steps: template.steps(),
  };
}

export function defaultAuthenticateParams() {
  const ids = ['credentials', 'google', 'passkey'];
  return {
    // The method row is a while loop: each enabled method is tried, in order,
    // until one completes or every enabled method has been tried.
    loopMode: 'while_untried_login_methods',
    methods: ids.map((id) => createLoginMethod(
        METHOD_TEMPLATES.find((template) => template.id === id))),
  };
}

export function createCustomLoginMethod(label, methods) {
  const used = new Set((methods || []).map((method) => method && method.id));
  let base = String(label || '').trim().toLowerCase()
      .replace(/[^a-z0-9]+/g, '_')
      .replace(/^_+|_+$/g, '');
  if (!base) {
    base = 'login_method';
  }
  let id = base;
  let suffix = 2;
  while (used.has(id)) {
    id = `${base}_${suffix}`;
    suffix += 1;
  }
  return {
    id,
    label: String(label || '').trim() || 'Login method',
    builtin: false,
    custom: true,
    enabled: true,
    actions: [],
  };
}

export function captchaBrand(id) {
  return CAPTCHA_BRANDS.find((brand) => brand.id === id) || null;
}

export function credentialRef(value) {
  const text = String(value || '').trim();
  if (!text || text === THIRD_PARTY_FILL) {
    return text;
  }
  if (/^\{[^{}]+\}$/.test(text)) {
    return text;
  }
  return '';
}

function tagNames(rawKey) {
  const parts = String(rawKey || '').split('#').map((part) => part.trim()).filter(Boolean);
  return parts.slice(1).map((tag) => {
    const match = tag.match(/^(.*?)(\d+)$/);
    return (match && match[1] ? match[1] : tag).toLowerCase();
  });
}

function keysFromTable(elementId) {
  return [...document.querySelectorAll(`#${elementId} .data-key`)]
      .map((el) => String(el.value || '').trim())
      .filter(Boolean);
}

export function keysForTags(source, wantedTags) {
  const wanted = new Set(wantedTags.map((tag) => tag.toLowerCase()));
  const catalog = window.__dashboardSmartInputCatalog || {};
  const listed = source === 'secret' ? catalog.secretKeys : catalog.variableKeys;
  const keys = Array.isArray(listed) ? listed :
      keysFromTable(source === 'secret' ? 'data-secrets' : 'data-vars');
  return keys.filter((raw) => tagNames(raw).some((tag) => wanted.has(tag)));
}

export function tokenForKey(rawKey) {
  const base = String(rawKey || '').split('#')[0].trim();
  return base ? `{${base}}` : '';
}

import type {FocusedTabData, GlicHostRegistry, GlicWebClient, GlicBrowserHost, OpenPanelInfo, PanelOpeningData, PanelState, PinCandidate, TabContextResult, TabData, UnifiedAgentProfile} from '/glic/glic_api/glic_api.js';
import {HostCapability, PanelStateKind, WebClientMode} from '/glic/glic_api/glic_api.js';

import {createGlicHostRegistryOnLoad} from '../api_boot.js';

type ChatMessage = {role: 'user'|'assistant'|'system', content: string};

type ChatContentPart =
    {type: 'text', text: string}|
    {type: 'image_url', image_url: {url: string}};

type WireChatMessage = {
  role: 'user'|'assistant'|'system',
  content: string|ChatContentPart[],
};

type TaskItem = {
  id: string,
  url: string,
  goal: string,
  username?: string,
  password?: string,
};

type TaskParse = {
  url: string,
  goal: string,
  username?: string,
  password?: string,
};

type LocatorCandidate = {
  type: string,
  value: string,
};

type ContextNode = {
  id: number,
  label: string,
  role?: string,
  formType?: string,
  fieldName?: string,
  placeholder?: string,
  editable?: boolean,
  focusable?: boolean,
  clickable?: boolean,
  required?: boolean,
  disabled?: boolean,
  visible?: boolean,
  bounds?: {x: number, y: number, width: number, height: number},
  locators?: LocatorCandidate[],
};

type PageContextSummary = {
  tabId?: string,
  tabIdInt?: number,
  url?: string,
  title?: string,
  innerText?: string,
  documentId?: string,
  /** CSS/DIP viewport size from APC (click coordinate space). */
  viewportCss?: {width: number, height: number},
  elements: ContextNode[],
};

type ContextLockMode = 'locked'|'follow_active_tab';

type ActionHandlingResult = {
  handled: boolean,
  requestContext: boolean,
};

type DashboardDataTokenCatalog = {
  tokenNames: string[],
  variableTokenNames?: string[],
  secretTokenNames?: string[],
  tokenCount?: number,
  secretTokenCount?: number,
};

type DashboardRecorderBridge = {
  recordAction?: (payload: any) => any,
  saveCurrentScript?: () => any,
  executeAction?: (payload: any) => any,
};

type GlicRecorderScaffoldEvent = {
  taskId?: string,
  timestampMs: number,
  status: 'ok'|'unknown',
  url?: string,
  documentId?: string,
  sourceAction: any,
  timelineAction: any,
  /** Compact step shown to the model for prune/edit on success. */
  brief?: {
    action: string,
    target_id?: number,
    desc?: string,
    text?: string,
    key?: string,
    url?: string,
  },
};

declare global {
  interface Window {
    __dashboardGlicRecorderBridge?: DashboardRecorderBridge,
    __glicRecorderScaffoldBuffer?: GlicRecorderScaffoldEvent[],
    __DASH_MODEL?: string,
    __DASH_PROVIDER?: string,
    __DASH_OLLAMA_ENDPOINT?: string,
    __DASH_LOOP_PROFILE?: string,
    __DASH_KEEP_WARM_HEADLESS?: boolean,
    __DASH_HEADLESS_MODEL?: string,
  }
}

const ACTION_RESULT = {
  OK: 0,
  TAB_WENT_AWAY: 13,
  FRAME_WENT_AWAY: 17,
  INVALID_DOM_NODE_ID: 18,
  ELEMENT_OFFSCREEN: 20,
  COORDINATES_OUT_OF_BOUNDS: 21,
  FRAME_LOCATION_CHANGED: 26,
  OBSERVED_TARGET_DESTROYED: 30,
  OBSERVED_TARGET_CHANGED: 31,
  TARGET_OBSCURED: 32,
  CLICK_SUPPRESSED: 200,
} as const;

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}


type AgentProvider = 'ollama'|'cerebras'|'gemini'|'openai'|'anthropic';

type ModelOption = {
  value: string,
  endpoint?: string,
  label?: string,
};

const AGENT_PROVIDERS: AgentProvider[] = [
  'ollama',
  'cerebras',
  'gemini',
  'openai',
  'anthropic',
];

const DEFAULT_MODELS_BY_PROVIDER: Record<AgentProvider, string> = {
  ollama: 'qwen3-coder-next',
  cerebras: 'qwen-3-235b-a22b-instruct-2507',
  gemini: 'gemini-2.5-pro',
  openai: 'gpt-4.1',
  anthropic: 'claude-haiku-4-5',
};

const DEFAULT_ENDPOINTS_BY_PROVIDER: Record<AgentProvider, string> = {
  ollama: 'http://127.0.0.1:11434',
  cerebras: 'https://api.cerebras.ai/v1/chat/completions',
  gemini: 'https://generativelanguage.googleapis.com/v1beta',
  openai: 'https://api.openai.com/v1/chat/completions',
  anthropic: 'https://api.anthropic.com/v1/messages',
};

const PROVIDER_MODEL_OPTIONS: Record<AgentProvider, ModelOption[]> = {
  ollama: [
    {value: 'qwen3-coder-next', endpoint: 'http://127.0.0.1:11434'},
    {value: 'qwen3.5:9b', endpoint: 'http://127.0.0.1:11434'},
    {value: 'qwen3-vl:2b', endpoint: 'http://127.0.0.1:11434'},
    {value: 'gemma4:e2b', endpoint: 'http://127.0.0.1:11434'},
    {value: 'qwen3.5:0.8b', endpoint: 'http://127.0.0.1:11434'},
    {value: 'functiongemma:latest', endpoint: 'http://127.0.0.1:11434'},
    {value: 'qwen2.5vl:3b', endpoint: 'http://127.0.0.1:11434'},
    {value: 'simula-vision-q8:latest', endpoint: 'http://127.0.0.1:11434'},
    {value: 'qwen3-coder-next:latest', endpoint: 'http://127.0.0.1:11434'},
    {value: 'qwen3:0.6b', endpoint: 'http://127.0.0.1:11434'},
    {value: 'deepseek-r1:1.5b', endpoint: 'http://127.0.0.1:11434'},
    {value: 'llama3:latest', endpoint: 'http://127.0.0.1:11434'},
    {value: 'gemma4:e4b', endpoint: 'http://127.0.0.1:11435'},
  ],
  cerebras: [
    {value: 'qwen-3-235b-a22b-instruct-2507'},
    {value: 'llama3.1-8b'},
  ],
  gemini: [
    {value: 'gemini-2.5-pro'},
    {value: 'gemini-2.5-flash'},
  ],
  openai: [
    {value: 'gpt-4.1'},
    {value: 'gpt-4.1-mini'},
    {value: 'o4-mini'},
  ],
  anthropic: [
    {value: 'claude-haiku-4-5', label: 'claude-haiku-4-5 (cheap, default)'},
    {value: 'claude-sonnet-4-5', label: 'claude-sonnet-4-5 (higher quality)'},
    {value: 'claude-sonnet-5', label: 'claude-sonnet-5 (higher quality)'},
  ],
};

function normalizeAgentProvider(raw: string|undefined|null): AgentProvider {
  const provider = String(raw || '').trim().toLowerCase();
  return (AGENT_PROVIDERS as string[]).includes(provider) ?
      provider as AgentProvider :
      'ollama';
}

const ANTHROPIC_MODEL_ALIASES: Record<string, string> = {
  'claude-haiku-4-5-20251001': 'claude-haiku-4-5',
  'claude-3-5-haiku-latest': 'claude-haiku-4-5',
  'claude-3-5-sonnet-latest': 'claude-sonnet-4-5',
  'claude-sonnet-4-20250514': 'claude-sonnet-4-5',
};

function normalizeModelForProvider(provider: AgentProvider, model: string): string {
  let candidate = String(model || '').trim();
  if (provider === 'anthropic') {
    const aliased = ANTHROPIC_MODEL_ALIASES[candidate];
    if (aliased) {
      candidate = aliased;
    }
  }
  const options = PROVIDER_MODEL_OPTIONS[provider] || [];
  if (options.some((opt) => opt.value === candidate)) {
    return candidate;
  }
  return DEFAULT_MODELS_BY_PROVIDER[provider] || DEFAULT_MODELS_BY_PROVIDER.ollama;
}

function sanitizeModelForProvider(provider: AgentProvider, model: string): string {
  return normalizeModelForProvider(provider, model);
}

function modelOptionLabel(provider: AgentProvider, option: ModelOption): string {
  if (option.label) {
    return option.label;
  }
  if (provider === 'ollama' && option.endpoint === 'http://127.0.0.1:11435') {
    return `${option.value} via 11435`;
  }
  return option.value;
}


function parseCsv(text: string): string[][] {
  const rows: string[][] = [];
  let row: string[] = [];
  let field = '';
  let inQuotes = false;

  for (let i = 0; i < text.length; i++) {
    const ch = text[i];

    if (inQuotes) {
      if (ch === '"') {
        if (text[i + 1] === '"') {
          field += '"';
          i++;
        } else {
          inQuotes = false;
        }
      } else {
        field += ch;
      }
      continue;
    }

    if (ch === '"') {
      inQuotes = true;
      continue;
    }

    if (ch === ',') {
      row.push(field);
      field = '';
      continue;
    }

    if (ch === '\n') {
      row.push(field);
      rows.push(row);
      row = [];
      field = '';
      continue;
    }

    if (ch === '\r') {
      continue;
    }

    field += ch;
  }

  row.push(field);
  const firstCell = row[0] ?? '';
  if (row.length > 1 || firstCell.trim() !== '') {
    rows.push(row);
  }

  return rows;
}

function parseTasksFromCsv(text: string): TaskParse[] {
  const rows = parseCsv(text).filter((row) =>
    row.some((cell) => cell.trim() !== ''));

  if (!rows.length) {
    return [];
  }

  const headerRow = rows[0] ?? [];
  const header = headerRow.map((cell) => cell.trim().toLowerCase());
  const hasHeader = header.includes('url') && header.includes('goal');
  const offsets = {
    url: hasHeader ? header.indexOf('url') : 0,
    goal: hasHeader ? header.indexOf('goal') : 1,
    username: hasHeader ? header.indexOf('username') : 2,
    password: hasHeader ? header.indexOf('password') : 3,
  };

  const startIndex = hasHeader ? 1 : 0;
  const tasks: TaskParse[] = [];
  for (const row of rows.slice(startIndex)) {
    const url = (row[offsets.url] ?? '').trim();
    const goal = (row[offsets.goal] ?? '').trim();
    if (!url || !goal) {
      continue;
    }
    const username = (row[offsets.username] ?? '').trim();
    const password = (row[offsets.password] ?? '').trim();
    tasks.push({
      url,
      goal,
      username: username || undefined,
      password: password || undefined,
    });
  }

  return tasks;
}

function encodeTaskHeader(task: TaskItem): string {
  const json = JSON.stringify(task);
  const bytes = new TextEncoder().encode(json);
  let binary = '';
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary);
}

function decodeBase64ToBytes(base64: string): Uint8Array {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

function encodeBytesToBase64(bytes: Uint8Array): string {
  let binary = '';
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary);
}

async function readStream(stream: ReadableStream<Uint8Array>):
    Promise<Uint8Array> {
  const response = new Response(stream);
  return new Uint8Array(await response.arrayBuffer());
}

function getField(obj: any, keys: string[]): any {
  for (const key of keys) {
    if (obj && Object.prototype.hasOwnProperty.call(obj, key)) {
      return obj[key];
    }
  }
  return undefined;
}

function normalizeText(text: string): string {
  return text.replace(/\s+/g, ' ').trim();
}

/** Prefer visible label, then form hints, then locator names for prompt text. */
function bestContextNodeText(node: ContextNode): string {
  const direct = normalizeText(
      String(node.label || node.placeholder || node.fieldName || ''));
  if (direct) {
    return direct;
  }
  const preferred = new Set([
    'ACCESSIBILITY_NAME',
    'NAME',
    'TEXT_CONTENT',
    'LINK_TEXT',
  ]);
  for (const loc of node.locators || []) {
    const type = canonicalizeLocatorType(String(loc?.type || ''));
    if (!preferred.has(type)) {
      continue;
    }
    const value = normalizeText(String(loc?.value || ''));
    // Skip pure geometry / CSS noise.
    if (!value || value.startsWith('{') || value.length > 80) {
      continue;
    }
    return value;
  }
  return '';
}

function scoreContextNode(node: ContextNode): number {
  let score = 0;
  const labelUpper = node.label.toUpperCase();

  if (node.disabled) {
    score -= 30;
  }
  if (node.bounds && node.bounds.width > 0 && node.bounds.height > 0) {
    score += 10;
  }
  if (node.clickable) {
    score += 60;
  }
  if (node.focusable) {
    score += 40;
  }
  if (node.editable) {
    score += 120;
  }

  if (node.formType) {
    if (node.formType.includes('INPUT_PASSWORD')) {
      score += 140;
    } else if (node.formType.includes('INPUT_TEXT') || node.formType.includes('INPUT_EMAIL')) {
      score += 120;
    } else if (node.formType.includes('INPUT')) {
      score += 100;
    } else if (node.formType.includes('BUTTON_SUBMIT')) {
      score += 110;
    } else if (node.formType.includes('BUTTON')) {
      score += 90;
    } else if (node.formType.includes('SELECT')) {
      score += 70;
    }
  }

  if (node.required) {
    score += 10;
  }
  if (node.placeholder) {
    score += 15;
  }
  if (node.fieldName) {
    score += 10;
  }

  if (node.label) {
    score += Math.min(30, Math.floor(node.label.length / 2));
  } else if (!node.placeholder && !node.fieldName) {
    score -= 25;
  }

  const keywordBoosts: Array<[RegExp, number]> = [
    [/\b(LOG IN|LOGIN|SIGN IN|SIGN-IN|SIGNIN)\b/i, 120],
    [/\b(SIGN UP|REGISTER|CREATE ACCOUNT)\b/i, 60],
    [/\b(USERNAME|EMAIL|E-MAIL)\b/i, 90],
    [/\b(PASSWORD|PASSCODE|PIN)\b/i, 90],
    [/\b(SUBMIT|CONTINUE|NEXT)\b/i, 60],
    [/\b(IMAGES|IMAGE)\b/i, 70],
    [/\b(CLAIM|BONUS|REWARD|SPIN|WHEEL)\b/i, 40],
  ];
  for (const [re, boost] of keywordBoosts) {
    if (re.test(labelUpper)) {
      score += boost;
      break;
    }
  }

  return score;
}

function describeNodeKind(node: ContextNode): string|undefined {
  if (node.formType) {
    if (node.formType.includes('INPUT_PASSWORD')) {
      return 'input_password';
    }
    if (node.formType.includes('INPUT_EMAIL')) {
      return 'input_email';
    }
    if (node.formType.includes('INPUT_TEXT')) {
      return 'input_text';
    }
    if (node.formType.includes('INPUT')) {
      return 'input';
    }
    if (node.formType.includes('BUTTON_SUBMIT')) {
      return 'button_submit';
    }
    if (node.formType.includes('BUTTON')) {
      return 'button';
    }
    if (node.formType.includes('SELECT')) {
      return 'select';
    }
  }
  if (node.editable) {
    return 'editable';
  }
  if (node.clickable) {
    return 'clickable';
  }
  if (node.focusable) {
    return 'focusable';
  }
  return undefined;
}

function tryParseUrl(url: string|undefined): URL|undefined {
  const text = String(url || '').trim();
  if (!text) {
    return undefined;
  }
  try {
    return new URL(text);
  } catch (_) {
    return undefined;
  }
}

function isYouTubeResultsUrl(url: string|undefined): boolean {
  const parsed = tryParseUrl(url);
  return !!parsed && parsed.hostname.includes('youtube.com') &&
      parsed.pathname === '/results';
}

function isGenericSearchResultsUrl(url: string|undefined): boolean {
  const parsed = tryParseUrl(url);
  if (!parsed) {
    return false;
  }
  const host = parsed.hostname.toLowerCase();
  const path = parsed.pathname;
  const hasQuery = parsed.searchParams.has('q') || parsed.searchParams.has('p');

  if (host.includes('google.') && path === '/search' && hasQuery) {
    return true;
  }
  if (host.includes('bing.com') && path === '/search' && hasQuery) {
    return true;
  }
  if (host.includes('duckduckgo.com') &&
      (path === '/' || path === '/html' || path === '/lite') && hasQuery) {
    return true;
  }
  if (host.includes('search.yahoo.com') && path === '/search' && hasQuery) {
    return true;
  }
  if (host.includes('search.brave.com') && path === '/search' && hasQuery) {
    return true;
  }
  if (host.includes('startpage.com') && path.includes('/sp/search') && hasQuery) {
    return true;
  }
  if (host.includes('ecosia.org') && path === '/search' && hasQuery) {
    return true;
  }
  return false;
}

function shouldSuppressContextNodeForPrompt(
    node: ContextNode, pageUrl: string|undefined): boolean {
  const label = normalizeText(
      `${node.label || ''} ${node.placeholder || ''} ${node.fieldName || ''}`)
                    .toLowerCase();
  if (!label) {
    return false;
  }

  if (isYouTubeResultsUrl(pageUrl)) {
    if (/^(sponsored|my ad center|action menu|more actions|learn more)$/.test(label)) {
      return true;
    }
  }
  if (isGenericSearchResultsUrl(pageUrl)) {
    if (/^(sponsored|ads|ad|advertisement|about this ad|why this ad\??|ad choices|my ad center|learn more|feedback|action menu|more actions)$/.test(label)) {
      return true;
    }
  }

  return false;
}

function sanitizeContextInnerTextForPrompt(
    pageUrl: string|undefined, innerText: string|undefined): string|undefined {
  const text = String(innerText || '');
  if (!text) {
    return innerText;
  }
  const isYouTubeResults = isYouTubeResultsUrl(pageUrl);
  const isGenericResults = isGenericSearchResultsUrl(pageUrl);
  if (!isYouTubeResults && !isGenericResults) {
    return innerText;
  }

  const lines = text.split('\n');
  const sanitized: string[] = [];
  let skippingSponsoredBlock = false;

  for (const rawLine of lines) {
    const line = rawLine.trim();
    const upper = line.toUpperCase();
    if (!skippingSponsoredBlock && isYouTubeResults && upper === 'SPONSORED') {
      skippingSponsoredBlock = true;
      continue;
    }
    if (skippingSponsoredBlock) {
      if (/^\d{1,2}:\d{2}(?::\d{2})?$/.test(line) || upper === 'NOW PLAYING') {
        skippingSponsoredBlock = false;
        sanitized.push(rawLine);
      }
      continue;
    }
    if (upper === 'MY AD CENTER') {
      continue;
    }
    if (isGenericResults &&
        /^(SPONSORED|ADS|AD|ADVERTISEMENT|ABOUT THIS AD|WHY THIS AD\??|AD CHOICES|FEEDBACK|LEARN MORE)$/.test(upper)) {
      continue;
    }
    sanitized.push(rawLine);
  }

  return sanitized.join('\n');
}

type MinimalElementDescriptor = {
  text?: string,
  kind?: string,
  role?: string,
  pos?: [number, number],
};

function quantize(value: number, step: number): number {
  if (!Number.isFinite(value) || !Number.isFinite(step) || step <= 0) {
    return 0;
  }
  return Math.round(value / step) * step;
}

function getElementCenter(bounds?: {x: number, y: number, width: number, height: number}):
    {x: number, y: number}|undefined {
  if (!bounds) {
    return undefined;
  }
  if (!Number.isFinite(bounds.x) || !Number.isFinite(bounds.y) ||
      !Number.isFinite(bounds.width) || !Number.isFinite(bounds.height)) {
    return undefined;
  }
  if (bounds.width <= 0 || bounds.height <= 0) {
    return undefined;
  }
  return {x: bounds.x + bounds.width / 2, y: bounds.y + bounds.height / 2};
}

function getActionablePoint(node?: ContextNode|null): {x: number, y: number}|undefined {
  if (!node) {
    return undefined;
  }
  const center = getElementCenter(node.bounds);
  if (center) {
    return center;
  }
  const rawNode: any = node;
  // LLM summary may stash a coarse center on desc.pos; ContextNode itself
  // does not normally carry desc, but tolerate either shape.
  const pos = rawNode?.desc?.pos;
  if (Array.isArray(pos) && pos.length >= 2) {
    const x = Math.round(Number(pos[0]));
    const y = Math.round(Number(pos[1]));
    if (Number.isFinite(x) && Number.isFinite(y)) {
      return {x, y};
    }
  }
  const locs = Array.isArray(rawNode?.locators) ? rawNode.locators : [];
  for (const loc of locs) {
    const type = String(loc?.type || '').toUpperCase();
    if (type !== 'VISUAL_COORDS' && type !== 'VISUALCOORDS') {
      continue;
    }
    const value = String(loc?.value || '');
    const match = value.match(/(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)/);
    if (!match) {
      continue;
    }
    const x = Math.round(Number(match[1]));
    const y = Math.round(Number(match[2]));
    if (Number.isFinite(x) && Number.isFinite(y)) {
      return {x, y};
    }
  }
  return undefined;
}

function positionKey(bounds?: {x: number, y: number, width: number, height: number}):
    {key: string, pos: [number, number]}|undefined {
  const center = getElementCenter(bounds);
  if (!center) {
    return undefined;
  }
  const x = quantize(center.x, 20);
  const y = quantize(center.y, 20);
  return {key: `${x},${y}`, pos: [x, y]};
}

function countBy<T>(items: T[], keyFn: (item: T) => string|undefined): Map<string, number> {
  const counts = new Map<string, number>();
  for (const item of items) {
    const key = keyFn(item);
    if (!key) {
      continue;
    }
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }
  return counts;
}

function canonicalizeLocatorType(type: string): string {
  const key = String(type || '').trim().toUpperCase();
  const aliases: Record<string, string> = {
    CSS: 'CSS_SELECTOR',
    CSSSELECTOR: 'CSS_SELECTOR',
    CSS_SELECTOR: 'CSS_SELECTOR',
    XPATH: 'XPATH',
    ID: 'ID',
    NAME: 'NAME',
    CLASS: 'CLASS_NAME',
    CLASS_NAME: 'CLASS_NAME',
    TAG: 'TAG_NAME',
    TAG_NAME: 'TAG_NAME',
    LINKTEXT: 'LINK_TEXT',
    LINK_TEXT: 'LINK_TEXT',
    PARTIALLINKTEXT: 'PARTIAL_LINK_TEXT',
    PARTIAL_LINK_TEXT: 'PARTIAL_LINK_TEXT',
    TEXT: 'TEXT_CONTENT',
    TEXT_CONTENT: 'TEXT_CONTENT',
    ACCESSIBILITY_NAME: 'ACCESSIBILITY_NAME',
    ARIA: 'ACCESSIBILITY_NAME',
    VISUAL_COORDS: 'VISUAL_COORDS',
    COORDINATES: 'VISUAL_COORDS',
  };
  return aliases[key] || key;
}

function addLocator(
    out: LocatorCandidate[], seen: Set<string>, typeRaw: string, valueRaw: unknown,
    max = 10): void {
  if (out.length >= max) {
    return;
  }
  const type = canonicalizeLocatorType(typeRaw);
  if (!type) {
    return;
  }
  const value = String(valueRaw ?? '').trim();
  if (!value) {
    return;
  }
  if (value.length > 220) {
    return;
  }
  const normalizedValue = (type === 'TAG_NAME') ? value.toLowerCase() : value;
  const key = `${type}::${normalizedValue}`;
  if (seen.has(key)) {
    return;
  }
  seen.add(key);
  out.push({type, value: normalizedValue});
}

function extractLocatorCandidates(attrs: any, entry: {
  labelText: string,
  placeholderText: string,
  fieldNameText: string,
  roleText: string,
  clickable: boolean,
  bounds?: {x: number, y: number, width: number, height: number},
}): LocatorCandidate[] {
  const out: LocatorCandidate[] = [];
  const seen = new Set<string>();
  const add = (type: string, value: unknown) => addLocator(out, seen, type, value);

  const cssSelector = getField(attrs, ['cssSelector', 'css_selector', 'selector']);
  add('CSS_SELECTOR', cssSelector);
  const xpath = getField(attrs, ['xPath', 'xpath']);
  add('XPATH', xpath);
  const domId = getField(attrs, [
    'domId',
    'dom_id',
    'id',
    'idAttribute',
    'id_attribute',
    'idAttr',
    'id_attr',
  ]);
  add('ID', domId);
  const nameAttr = getField(attrs, ['name', 'nameAttribute', 'name_attribute']);
  add('NAME', nameAttr);
  const className = getField(attrs, ['className', 'class_name']);
  add('CLASS_NAME', className);
  const tagName = getField(attrs, ['tagName', 'tag_name']);
  add('TAG_NAME', tagName);

  const rawLocators = getField(attrs, [
    'locators',
    'locatorCandidates',
    'locator_candidates',
    'selectors',
    'selectorCandidates',
    'selector_candidates',
  ]);
  if (Array.isArray(rawLocators)) {
    for (const loc of rawLocators) {
      if (!loc || typeof loc !== 'object') {
        continue;
      }
      add(
          String(getField(loc, ['type', 'locatorType', 'locator_type']) || ''),
          getField(loc, ['value', 'selector', 'text']));
    }
  }

  if (entry.fieldNameText &&
      /^[a-zA-Z0-9_.:-]{1,80}$/.test(entry.fieldNameText)) {
    add('NAME', entry.fieldNameText);
  }
  if (entry.labelText) {
    add('ACCESSIBILITY_NAME', entry.labelText);
    if (entry.clickable) {
      add('TEXT_CONTENT', entry.labelText);
    }
  }
  if (entry.placeholderText) {
    add('TEXT_CONTENT', entry.placeholderText);
  }

  const roleUpper = String(entry.roleText || '').toUpperCase();
  if (entry.labelText && (roleUpper.includes('LINK') || roleUpper.includes('ANCHOR'))) {
    add('LINK_TEXT', entry.labelText);
  }

  const center = getElementCenter(entry.bounds);
  if (center) {
    add('VISUAL_COORDS', JSON.stringify({
      x: Math.round(center.x),
      y: Math.round(center.y),
    }));
  }

  return out;
}

function selectContextNodes(nodes: ContextNode[], limit: number): ContextNode[] {
  if (nodes.length <= limit) {
    return nodes;
  }

  const scored = nodes
      .map((node) => ({node, score: scoreContextNode(node)}))
      .sort((a, b) => b.score - a.score || a.node.id - b.node.id);

  const selected = new Map<number, ContextNode>();

  for (const {node} of scored) {
    if (node.formType?.includes('INPUT_PASSWORD')) {
      selected.set(node.id, node);
    }
  }
  for (const {node} of scored) {
    if (node.editable) {
      selected.set(node.id, node);
    }
  }
  for (const {node} of scored) {
    const text = (node.label || node.placeholder || '').toLowerCase();
    if (/(log in|login|sign in|username|email|password)/.test(text)) {
      selected.set(node.id, node);
    }
  }

  for (const {node} of scored) {
    if (selected.size >= limit) {
      break;
    }
    selected.set(node.id, node);
  }

  const chosen = Array.from(selected.values());
  chosen.sort((a, b) => scoreContextNode(b) - scoreContextNode(a) || a.id - b.id);
  return chosen.slice(0, limit);
}

function readApcBoundingBox(geometry: any):
    {x: number, y: number, width: number, height: number}|undefined {
  if (!geometry || typeof geometry !== 'object') {
    return undefined;
  }
  const visibleBox = getField(geometry, ['visibleBoundingBox', 'visible_bounding_box']);
  const outerBox = getField(geometry, ['outerBoundingBox', 'outer_bounding_box']);
  const box = visibleBox || outerBox;
  if (!box || typeof box !== 'object') {
    return undefined;
  }
  const width = Number(box.width || 0);
  const height = Number(box.height || 0);
  if (!Number.isFinite(width) || !Number.isFinite(height)) {
    return undefined;
  }
  return {
    x: Number(box.x || 0),
    y: Number(box.y || 0),
    width,
    height,
  };
}

// APC element geometry is often in device/screenshot pixels, while
// viewport_geometry and SetPositionInWidget clicks use DIPs. Scale element
// bounds into DIP space before deriving click centers.
function extractViewportCssFromApc(apcJson: any):
    {width: number, height: number}|undefined {
  const viewport = getField(apcJson, ['viewportGeometry', 'viewport_geometry']);
  const width = Number(getField(viewport, ['width']));
  const height = Number(getField(viewport, ['height']));
  if (!Number.isFinite(width) || width <= 0 || !Number.isFinite(height) ||
      height <= 0) {
    return undefined;
  }
  return {width: Math.round(width), height: Math.round(height)};
}

function computeApcToDipScale(apcJson: any): number {
  const viewport = getField(apcJson, ['viewportGeometry', 'viewport_geometry']);
  const vpW = Number(getField(viewport, ['width']));
  if (!Number.isFinite(vpW) || vpW <= 0) {
    return 1;
  }
  const root = getField(apcJson, ['rootNode', 'root_node']);
  const rootAttrs = getField(root, ['contentAttributes', 'content_attributes']) || {};
  const rootGeom = getField(rootAttrs, ['geometry']) || {};
  // Prefer outer box for the root size (full page surface in APC pixels).
  const outer = getField(rootGeom, ['outerBoundingBox', 'outer_bounding_box']);
  const visible = getField(rootGeom, ['visibleBoundingBox', 'visible_bounding_box']);
  const rootBox = outer || visible;
  const rootW = Number(getField(rootBox, ['width']));
  if (!Number.isFinite(rootW) || rootW <= 0) {
    return 1;
  }
  // Only downscale when APC pixels are meaningfully larger than DIP viewport
  // (classic retina / screenshot capture mismatch).
  if (rootW <= vpW * 1.05) {
    return 1;
  }
  const scale = vpW / rootW;
  if (!Number.isFinite(scale) || scale <= 0.2 || scale > 1) {
    return 1;
  }
  return scale;
}

function scaleBoundsToDip(
    bounds: {x: number, y: number, width: number, height: number}|undefined,
    scale: number): {x: number, y: number, width: number, height: number}|undefined {
  if (!bounds) {
    return undefined;
  }
  if (!Number.isFinite(scale) || Math.abs(scale - 1) < 0.001) {
    return bounds;
  }
  return {
    x: bounds.x * scale,
    y: bounds.y * scale,
    width: bounds.width * scale,
    height: bounds.height * scale,
  };
}

function extractNodesFromApc(apcJson: any, limit: number, pageUrl?: string): ContextNode[] {
  const byId = new Map<number, ContextNode>();
  // Nearest button/clickable id -> ordered unique child text fragments.
  const buttonTextParts = new Map<number, string[]>();
  const root = getField(apcJson, ['rootNode', 'root_node']);
  if (!root) {
    return [];
  }

  function upsertNode(entry: ContextNode) {
    const existing = byId.get(entry.id);
    if (existing) {
      if (!existing.label && entry.label) {
        existing.label = entry.label;
      }
      if (entry.role && !existing.role) {
        existing.role = entry.role;
      }
      if (typeof entry.disabled === 'boolean' && existing.disabled === undefined) {
        existing.disabled = entry.disabled;
      }
      if (typeof entry.visible === 'boolean' && existing.visible === undefined) {
        existing.visible = entry.visible;
      }
      if (entry.bounds && !existing.bounds) {
        existing.bounds = entry.bounds;
      }
      if (entry.placeholder && !existing.placeholder) {
        existing.placeholder = entry.placeholder;
      }
      if (entry.fieldName && !existing.fieldName) {
        existing.fieldName = entry.fieldName;
      }
      if (entry.formType && !existing.formType) {
        existing.formType = entry.formType;
      }
      if (typeof entry.editable === 'boolean' && existing.editable === undefined) {
        existing.editable = entry.editable;
      }
      if (typeof entry.focusable === 'boolean' && existing.focusable === undefined) {
        existing.focusable = entry.focusable;
      }
      if (typeof entry.clickable === 'boolean' && existing.clickable === undefined) {
        existing.clickable = entry.clickable;
      }
      if (typeof entry.required === 'boolean' && existing.required === undefined) {
        existing.required = entry.required;
      }
      if (Array.isArray(entry.locators) && entry.locators.length) {
        const merged = Array.isArray(existing.locators) ? existing.locators.slice() : [];
        const seen = new Set<string>(
            merged.map((loc) => `${canonicalizeLocatorType(loc.type)}::${loc.value}`));
        for (const loc of entry.locators) {
          addLocator(merged, seen, loc.type, loc.value, 12);
        }
        existing.locators = merged;
      }
      return;
    }
    byId.set(entry.id, entry);
  }

  const dipScale = computeApcToDipScale(apcJson);
  type ApcQueueItem = {node: any, actionableAncestorIds: number[]};
  const queue: ApcQueueItem[] = [{node: root, actionableAncestorIds: []}];
  let visited = 0;
  const maxVisited = 6000;
  while (queue.length && visited < maxVisited) {
    visited++;
    const item = queue.shift()!;
    const node = item.node;
    const actionableAncestorIds = item.actionableAncestorIds;
    const attrs = getField(node, ['contentAttributes', 'content_attributes']) || {};
    const id = getField(attrs, ['commonAncestorDomNodeId', 'common_ancestor_dom_node_id']);
    const interaction = getField(attrs, ['interactionInfo', 'interaction_info']) || {};
    const clickReasons = getField(interaction, ['clickabilityReasons', 'clickability_reasons']) || [];
    const isFocusable = getField(interaction, ['isFocusable', 'is_focusable']);
    const isEditable = getField(interaction, ['isEditable', 'is_editable']);
    const isDisabled = getField(interaction, ['isDisabled', 'is_disabled']);

    const geometry = getField(attrs, ['geometry']);
    const rawBounds = readApcBoundingBox(geometry);
    const bounds = scaleBoundsToDip(rawBounds, dipScale);
    const visible = bounds ? bounds.width > 0 && bounds.height > 0 : undefined;

    const label = getField(attrs, [
      'label',
      'accessibleName',
      'accessible_name',
      'ariaLabel',
      'aria_label',
      'name',
      'title',
      'tooltip',
      'text',
      'textContent',
      'text_content',
    ]);
    const textData = getField(attrs, ['textData', 'text_data']);
    const textContent = textData ? getField(textData, ['textContent', 'text_content']) : undefined;
    const formControl = getField(attrs, ['formControlData', 'form_control_data']);
    const placeholder = formControl
        ? getField(formControl, ['placeholder', 'placeholderText', 'placeholder_text'])
        : undefined;
    const fieldName = formControl ? getField(formControl, ['fieldName', 'field_name']) : undefined;
    const formControlType = formControl
        ? getField(formControl, ['formControlType', 'form_control_type'])
        : undefined;
    const isRequired = formControl ? getField(formControl, ['isRequired', 'is_required']) : undefined;
    const role = getField(attrs, ['ariaRole', 'aria_role']);

    const labelText = normalizeText((label || textContent || '').toString());
    const placeholderText = normalizeText((placeholder || '').toString());
    const fieldNameText = normalizeText((fieldName || '').toString());
    const textForPrompt = normalizeText((labelText || placeholderText || fieldNameText));
    const roleText = typeof role === 'string' ? role : '';
    const hasMeaningfulRole = Boolean(
      roleText &&
      roleText !== 'AX_ROLE_UNKNOWN' &&
      roleText !== 'AX_ROLE_GENERIC_CONTAINER' &&
      roleText !== 'AX_ROLE_GROUP' &&
      roleText !== 'AX_ROLE_SECTION',
    );
    const isControl = Boolean(clickReasons.length || isFocusable || isEditable || formControlType);
    const isActionable = Boolean(isControl || textForPrompt || hasMeaningfulRole);
    const locators = extractLocatorCandidates(attrs, {
      labelText,
      placeholderText,
      fieldNameText,
      roleText,
      clickable: Boolean(clickReasons.length),
      bounds,
    });

    if (typeof id === 'number' && isActionable) {
      upsertNode({
        id,
        label: textForPrompt,
        role: hasMeaningfulRole ? roleText : undefined,
        formType: typeof formControlType === 'string' ? formControlType : undefined,
        fieldName: fieldNameText || undefined,
        placeholder: placeholderText || undefined,
        editable: Boolean(isEditable),
        focusable: Boolean(isFocusable),
        clickable: Boolean(clickReasons.length),
        required: Boolean(isRequired),
        disabled: Boolean(isDisabled),
        visible,
        bounds,
        locators,
      });
    }

    // Collect short child text onto the nearest button/clickable ancestor.
    // Icon/"Get Coins" buttons often have empty labels on the control itself.
    if (textForPrompt && actionableAncestorIds.length &&
        textForPrompt.length <= 48) {
      const nearestId =
          actionableAncestorIds[actionableAncestorIds.length - 1]!;
      const nearest = byId.get(nearestId);
      const buttonLike = !!nearest &&
          (nearest.clickable ||
           /BUTTON/i.test(String(nearest.formType || '')) ||
           /BUTTON|LINK|TAB|MENU_ITEM/i.test(String(nearest.role || '')));
      if (buttonLike) {
        const parts = buttonTextParts.get(nearestId) ?? [];
        const lower = textForPrompt.toLowerCase();
        if (!parts.some((p) => p.toLowerCase() === lower)) {
          parts.push(textForPrompt);
          buttonTextParts.set(nearestId, parts);
        }
      }
    }

    const labelForId = getField(attrs, ['labelForDomNodeId', 'label_for_dom_node_id']);
    if (typeof labelForId === 'number' && textForPrompt) {
      const labelEntry: ContextNode = {
        id: labelForId,
        label: textForPrompt,
        fieldName: fieldNameText || undefined,
        placeholder: placeholderText || undefined,
        formType: typeof formControlType === 'string' ? formControlType : undefined,
        visible,
        bounds,
        locators,
      };
      upsertNode(labelEntry);
    }

    const nextAncestors = actionableAncestorIds.slice();
    if (typeof id === 'number' && isActionable &&
        (clickReasons.length || formControlType || isFocusable)) {
      nextAncestors.push(id);
    }
    const children = getField(node, ['childrenNodes', 'children_nodes']) || [];
    for (const child of children) {
      queue.push({node: child, actionableAncestorIds: nextAncestors});
    }
  }

  for (const [id, parts] of buttonTextParts) {
    const node = byId.get(id);
    if (!node || !parts.length) {
      continue;
    }
    const joined = normalizeText(parts.join(' ')).slice(0, 80);
    if (!joined) {
      continue;
    }
    if (!node.label) {
      node.label = joined;
    }
    if (!Array.isArray(node.locators)) {
      node.locators = [];
    }
    const seen = new Set(
        node.locators.map(
            (loc) => `${canonicalizeLocatorType(loc.type)}::${loc.value}`));
    addLocator(node.locators, seen, 'TEXT_CONTENT', joined, 12);
    addLocator(node.locators, seen, 'ACCESSIBILITY_NAME', joined, 12);
  }

  const candidates = Array.from(byId.values()).filter((node) => {
    const hasText = Boolean(node.label || node.placeholder || node.fieldName);
    const hasAction = Boolean(node.formType || node.editable || node.focusable || node.clickable);
    return (hasText || hasAction) &&
        !shouldSuppressContextNodeForPrompt(node, pageUrl);
  });
  return selectContextNodes(candidates, limit);
}

function extractDocumentIdFromApc(apcJson: any): string|undefined {
  const mainFrame = getField(apcJson, ['mainFrameData', 'main_frame_data']);
  const doc = getField(mainFrame, ['documentIdentifier', 'document_identifier']);
  const token = getField(doc, ['serializedToken', 'serialized_token']);
  if (typeof token === 'string' && token.trim()) {
    return token;
  }
  return undefined;
}

function stripModelNoise(text: string): string {
  let raw = String(text || '');
  // Drop common chain-of-thought / tool wrappers that surround the JSON payload.
  raw = raw.replace(/<think>[\s\S]*?<\/think>/gi, ' ');
  raw = raw.replace(/<reasoning>[\s\S]*?<\/reasoning>/gi, ' ');
  raw = raw.replace(/```(?:json)?\s*/gi, '\n').replace(/```/g, '\n');
  return raw;
}

function repairCommonModelJson(candidate: string): string {
  let body = String(candidate || '').trim();
  if (!body) {
    return body;
  }
  // Quote bare dashboard tokens: "text":{GmailAlt} -> "text":"{GmailAlt}"
  body = body.replace(
      /("(?:text|value|url|key|reason|target_profile|username|password|goal|name)"\s*:\s*)(\{[A-Za-z_][A-Za-z0-9_]*\})/g,
      '$1"$2"');
  // Single-quoted strings -> double quotes for simple action objects.
  if (body.includes("'") && !body.includes('"')) {
    body = body.replace(/'/g, '"');
  }
  // Trailing commas before } or ]
  body = body.replace(/,\s*([}\]])/g, '$1');
  return body;
}

function tryParseJsonObject(candidate: string): any|null {
  const repaired = repairCommonModelJson(candidate);
  if (!repaired) {
    return null;
  }
  try {
    const parsed = JSON.parse(repaired);
    if (parsed && typeof parsed === 'object') {
      return parsed;
    }
  } catch (_) {}
  return null;
}

function extractBalancedJsonObjects(text: string): string[] {
  const raw = String(text || '');
  const out: string[] = [];
  for (let i = 0; i < raw.length; i++) {
    if (raw[i] !== '{') {
      continue;
    }
    let depth = 0;
    let inString = false;
    let escaped = false;
    for (let j = i; j < raw.length; j++) {
      const ch = raw[j];
      if (inString) {
        if (escaped) {
          escaped = false;
          continue;
        }
        if (ch === '\\') {
          escaped = true;
          continue;
        }
        if (ch === '"') {
          inString = false;
        }
        continue;
      }
      if (ch === '"') {
        inString = true;
        continue;
      }
      if (ch === '{') {
        depth++;
      } else if (ch === '}') {
        depth--;
        if (depth === 0) {
          out.push(raw.slice(i, j + 1));
          i = j;
          break;
        }
      }
    }
  }
  return out;
}

function extractJsonObjects(text: string): any[] {
  const cleaned = stripModelNoise(text);
  const parsed: any[] = [];
  const seen = new Set<string>();

  const push = (obj: any) => {
    if (!obj || typeof obj !== 'object') {
      return;
    }
    // Prefer objects that look like actions/scripts; still keep others as fallback.
    try {
      const key = JSON.stringify(obj);
      if (seen.has(key)) {
        return;
      }
      seen.add(key);
      parsed.push(obj);
    } catch (_) {}
  };

  // Whole response first (common when the model returns pure JSON).
  const whole = tryParseJsonObject(cleaned.trim());
  if (whole) {
    push(whole);
  }

  for (const candidate of extractBalancedJsonObjects(cleaned)) {
    const obj = tryParseJsonObject(candidate);
    if (obj) {
      push(obj);
    }
  }

  // Prefer actionable objects when mixed noise is present.
  const actionable = parsed.filter(
      (obj) => typeof (obj as any)?.action === 'string' ||
          Array.isArray((obj as any)?.actions));
  return actionable.length ? actionable : parsed;
}

function looksLikeTokenPlaceholder(value: string|undefined): boolean {
  const text = String(value ?? '').trim();
  if (text.length < 3) {
    return false;
  }
  // {Email} or mustache-style {{Email}}
  return /^\{\{?[A-Za-z0-9_]+\}\}?$/.test(text);
}

/** Collapse {{Token}} → {Token} so dashboard secret/variable expansion matches. */
function normalizeTokenPlaceholdersInText(value: string): string {
  return String(value ?? '').replace(
      /\{\{([A-Za-z0-9_]+)\}\}/g, '{$1}');
}

function tokenNameFromPlaceholder(value: string): string|null {
  const text = String(value ?? '').trim();
  const m = text.match(/^\{([A-Za-z0-9_]+)\}$/);
  return m ? m[1]! : null;
}

function looksLikeLiteralEmail(value: string): boolean {
  const text = String(value ?? '').trim();
  return text.length >= 5 && text.includes('@') && !text.includes(' ');
}

const MODEL_BUSY_MESSAGE =
    'Model request already in flight; skipping overlapping prompt.';
const MANUAL_CONTEXT_ELEMENT_LIMIT = 12;

class OllamaWebClient implements GlicWebClient {
  private browser: GlicBrowserHost|undefined;
  private messages: ChatMessage[] = [];
  private taskQueue: TaskItem[] = [];
  private currentTask: TaskItem|null = null;
  private queueRunning = false;
  /** True while runTaskLoop is on the stack. */
  private taskLoopRunning = false;
  /** Set by Pause/Stop; checked between steps and before acting. */
  private taskLoopStopRequested = false;
  private taskCounter = 0;
  private queueStatusEl: HTMLElement|null = null;
  private taskInfoEl: HTMLElement|null = null;
  private actorTaskId: number|null = null;
  private unifiedRunId: string|null = null;
  private warnedNoActorTask = false;
  private lastContextError: string|null = null;
  private lastPageContext: PageContextSummary|null = null;
  private lastActionSummary: string|null = null;
  private maxStepsPerTask = 20;
  private currentTabId: string|null = null;
  private focusedTabId: string|null = null;
  private candidateTabId: string|null = null;
  private tabReopenAttempts = 0;
  private maxTabReopenAttempts = 1;
  private maxLoadWaitMs = 120000;
  private taskStartDelayMs = 2500;
  private pendingTaskStartTimer: number|null = null;
  private warnedExtraMetaKeys = false;
  private typedUsername = false;
  private llmRequestInFlight = false;
  private llmAbortController: AbortController|null = null;
  private sendBtnEl: HTMLButtonElement|null = null;
  private stopBtnEl: HTMLButtonElement|null = null;
  private initialized = false;
  private requestContextNextTurn = false;
  /** Element ids from the last full task-tab snapshot (survives list_tabs turns). */
  private lastTaskElementIds: number[] = [];
  /** Compact labeled targets from the last full snapshot (id + text). */
  private lastTaskLabeledElements: Array<{id: number, text: string}> = [];
  private hybridExecutionEnabled = true;
  private nonVisualContextMode = true;
  private attachContextForManualPrompts = false;
  private promptOptimizerEnabled = false;
  private promptHistory: string[] = [];
  /** -1 = live draft; 0 = most recent history entry. */
  private promptHistoryIndex = -1;
  private promptHistoryDraft = '';
  private promptInputEl: HTMLInputElement|null = null;
  private contextLockMode: ContextLockMode = 'locked';
  private taskTabId: string|null = null;
  private workingTabId: string|null = null;
  private workingTabUrl: string|null = null;
  private workingTabTitle: string|null = null;
  private currentTaskStepIndex = 0;
  private queueWidgetToggleEl: HTMLButtonElement|null = null;
  private queueWidgetPanelEl: HTMLElement|null = null;
  private queueWidgetSubtitleEl: HTMLElement|null = null;
  private queueWorkingContextEl: HTMLElement|null = null;
  private queueActiveTaskEl: HTMLElement|null = null;
  private queueWidgetListEl: HTMLElement|null = null;
  private panelModeEl: HTMLElement|null = null;
  private panelAttachToggleEl: HTMLButtonElement|null = null;
  private canAttachPanelNow = true;
  private attachContextToggleEl: HTMLInputElement|null = null;
  private promptOptimizerToggleEl: HTMLInputElement|null = null;
  private modeSelectEl: HTMLSelectElement|null = null;
  private providerSelectEl: HTMLSelectElement|null = null;
  private modelSelectEl: HTMLSelectElement|null = null;
  private selectedProvider: AgentProvider = 'ollama';
  private selectedModel = DEFAULT_MODELS_BY_PROVIDER.ollama;
  private selectedOllamaEndpoint = DEFAULT_ENDPOINTS_BY_PROVIDER.ollama;
  private dashboardDataTokenCatalog: DashboardDataTokenCatalog|null = null;
  private tabContextPermissionRecoveryAttempted = false;
  private hasCoreAssistantContextPrompt = false;
  private lastContextEvidenceKey = '';
  private unifiedProfile: UnifiedAgentProfile = 'browser_operator';
  private unifiedProfileSource = 'default';
  private unifiedProfileHopUsed = false;
  private lastHeadlessInvestigationSummary = '';
  private activeHeadlessInvestigationTaskId: string|null = null;
  private keepWarmHeadlessInvestigationTab = true;
  private lastManualHeadlessInvestigatedUrl = '';
  private lastManualHeadlessInvestigationAtMs = 0;
  /** Anthropic-only: viewport screenshot attached on the next model turn. */
  private pendingScreenshot: {
    base64: string,
    mimeType: string,
    cssWidth?: number,
    cssHeight?: number,
    note?: string,
  }|null = null;



  private resolveConfiguredProvider(): AgentProvider {
    try {
      const fromStorage = String(localStorage.getItem('glic.provider') || '').trim();
      if (fromStorage) {
        return normalizeAgentProvider(fromStorage);
      }
    } catch (_) {}
    return normalizeAgentProvider(window.__DASH_PROVIDER || 'ollama');
  }

  private resolveConfiguredModel(provider: AgentProvider): string {
    try {
      const fromStorage = String(localStorage.getItem('glic.model') || '').trim();
      if (fromStorage) {
        return sanitizeModelForProvider(provider, fromStorage);
      }
    } catch (_) {}
    const fromWindow = String(window.__DASH_MODEL || '').trim();
    return sanitizeModelForProvider(provider, fromWindow);
  }

  private resolveConfiguredOllamaEndpoint(model: string): string {
    const options = PROVIDER_MODEL_OPTIONS.ollama;
    const match = options.find((option) => option.value === model);
    if (match?.endpoint) {
      return match.endpoint;
    }
    const fromWindow = String(window.__DASH_OLLAMA_ENDPOINT || '').trim();
    return fromWindow || DEFAULT_ENDPOINTS_BY_PROVIDER.ollama;
  }

  private persistProviderModelSelection(): void {
    try {
      localStorage.setItem('glic.provider', this.selectedProvider);
      localStorage.setItem('glic.model', this.selectedModel);
      if (this.selectedProvider === 'ollama') {
        localStorage.setItem('glic.ollama_endpoint', this.selectedOllamaEndpoint);
      }
    } catch (_) {}
    window.__DASH_PROVIDER = this.selectedProvider;
    window.__DASH_MODEL = this.selectedModel;
    if (this.selectedProvider === 'ollama') {
      window.__DASH_OLLAMA_ENDPOINT = this.selectedOllamaEndpoint;
    }
    // Disk-backed via serve.py so provider/model survive DevProfile browser resets.
    void this.persistClientPrefsToServer({
      provider: this.selectedProvider,
      model: this.selectedModel,
      ollama_endpoint: this.selectedProvider === 'ollama' ?
          this.selectedOllamaEndpoint :
          undefined,
    });
  }

  private clientPrefsUrl(): string {
    try {
      const origin = String(window.location?.origin || '').replace(/\/$/, '');
      if (origin && origin !== 'null' && !origin.startsWith('chrome')) {
        return `${origin}/glic-prefs`;
      }
    } catch (_) {
    }
    return '/glic-prefs';
  }

  private async persistClientPrefsToServer(partial: Record<string, unknown>):
      Promise<void> {
    try {
      await fetch(this.clientPrefsUrl(), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(partial),
      });
    } catch (_) {
    }
  }

  private async restoreClientPrefsFromServer(): Promise<void> {
    try {
      const res = await fetch(this.clientPrefsUrl());
      if (!res.ok) {
        this.addSystemMessage(
            `Disk prefs restore failed (HTTP ${res.status}); prompt history may be empty until you send a prompt.`);
        return;
      }
      const prefs = await res.json();
      if (!prefs || typeof prefs !== 'object') {
        return;
      }
      const providerRaw = String(prefs.provider || '').trim();
      const modelRaw = String(prefs.model || '').trim();
      if (providerRaw) {
        const endpoint = String(prefs.ollama_endpoint || '').trim();
        if (endpoint) {
          try {
            localStorage.setItem('glic.ollama_endpoint', endpoint);
          } catch (_) {
          }
          this.selectedOllamaEndpoint = endpoint;
        }
        this.applyProviderModelSelection(providerRaw, modelRaw || undefined, false);
        try {
          localStorage.setItem('glic.provider', this.selectedProvider);
          localStorage.setItem('glic.model', this.selectedModel);
        } catch (_) {
        }
        window.__DASH_PROVIDER = this.selectedProvider;
        window.__DASH_MODEL = this.selectedModel;
        this.addSystemMessage(
            `Restored provider ${this.selectedProvider} / model ${this.selectedModel} from disk prefs.`);
      }
      if (Array.isArray(prefs.prompt_history)) {
        const restored = prefs.prompt_history
                             .map((v: unknown) => String(v || '').trim())
                             .filter((v: string) => !!v)
                             .slice(0, OllamaWebClient.PROMPT_HISTORY_MAX);
        if (restored.length) {
          // Merge server + local, newest-first unique.
          const merged: string[] = [];
          const seen = new Set<string>();
          for (const entry of [...restored, ...this.promptHistory]) {
            if (!entry || seen.has(entry)) {
              continue;
            }
            seen.add(entry);
            merged.push(entry);
            if (merged.length >= OllamaWebClient.PROMPT_HISTORY_MAX) {
              break;
            }
          }
          this.promptHistory = merged;
          this.promptHistoryIndex = -1;
          try {
            localStorage.setItem(
                OllamaWebClient.PROMPT_HISTORY_KEY,
                JSON.stringify(this.promptHistory));
          } catch (_) {
          }
          this.addSystemMessage(
              `Restored ${this.promptHistory.length} prompt(s) from disk (↑/↓ history).`);
        }
      }
      if (!providerRaw && this.selectedProvider) {
        void this.persistClientPrefsToServer({
          provider: this.selectedProvider,
          model: this.selectedModel,
          ollama_endpoint: this.selectedProvider === 'ollama' ?
              this.selectedOllamaEndpoint :
              undefined,
          prompt_history: this.promptHistory,
        });
      }
    } catch (e) {
      this.addSystemMessage(`Disk prefs restore error: ${e}`);
    }
  }

  private syncModelSelectOptions(preferredModel?: string): void {
    if (!this.modelSelectEl) {
      return;
    }
    const provider = this.selectedProvider;
    const options = PROVIDER_MODEL_OPTIONS[provider] || [];
    const desired = sanitizeModelForProvider(
        provider, preferredModel || this.selectedModel || this.modelSelectEl.value);
    this.modelSelectEl.innerHTML = '';
    for (const option of options) {
      const el = document.createElement('option');
      el.value = option.value;
      el.textContent = modelOptionLabel(provider, option);
      if (option.endpoint) {
        el.dataset['endpoint'] = option.endpoint;
      }
      if (option.value === desired) {
        el.selected = true;
      }
      this.modelSelectEl.appendChild(el);
    }
    this.selectedModel = sanitizeModelForProvider(provider, this.modelSelectEl.value);
    this.modelSelectEl.value = this.selectedModel;
    if (provider === 'ollama') {
      const selectedOption =
          this.modelSelectEl.options[this.modelSelectEl.selectedIndex] || null;
      this.selectedOllamaEndpoint =
          selectedOption?.dataset?.['endpoint'] ||
          this.resolveConfiguredOllamaEndpoint(this.selectedModel);
      this.modelSelectEl.title =
          'Local models discovered on the running Ollama runtimes.';
    } else {
      this.modelSelectEl.title =
          'Hosted provider model list. API keys come from Dashboard Data > Secrets automatically.';
    }
    if (this.providerSelectEl) {
      this.providerSelectEl.value = provider;
    }
  }

  private applyProviderModelSelection(
      providerRaw: string, modelRaw?: string, persist = true): void {
    this.selectedProvider = normalizeAgentProvider(providerRaw);
    this.selectedModel =
        sanitizeModelForProvider(this.selectedProvider, modelRaw || '');
    if (this.selectedProvider === 'ollama') {
      this.selectedOllamaEndpoint =
          this.resolveConfiguredOllamaEndpoint(this.selectedModel);
    }
    this.syncModelSelectOptions(this.selectedModel);
    if (persist) {
      this.persistProviderModelSelection();
    }
  }

  private getSelectedProviderAndModel():
      {provider: AgentProvider, model: string, endpoint: string} {
    const provider = normalizeAgentProvider(
        this.providerSelectEl?.value || this.selectedProvider);
    const model = sanitizeModelForProvider(
        provider, this.modelSelectEl?.value || this.selectedModel);
    let endpoint = DEFAULT_ENDPOINTS_BY_PROVIDER[provider];
    if (provider === 'ollama') {
      const selectedOption = this.modelSelectEl?.options
          ?.[this.modelSelectEl.selectedIndex] ||
          null;
      endpoint = selectedOption?.dataset?.['endpoint'] ||
          this.selectedOllamaEndpoint || endpoint;
    }
    this.selectedProvider = provider;
    this.selectedModel = model;
    if (provider === 'ollama') {
      this.selectedOllamaEndpoint = endpoint;
    }
    return {provider, model, endpoint};
  }

  private buildChatRequestHeaders(extra?: Record<string, string>):
      Record<string, string> {
    const {provider, endpoint} = this.getSelectedProviderAndModel();
    const headers: Record<string, string> = {
      'Content-Type': 'application/json',
      'X-Glic-Provider': provider,
      ...(extra || {}),
    };
    if (provider === 'ollama') {
      headers['X-Glic-Ollama-Endpoint'] = endpoint;
    } else {
      headers['X-Glic-Provider-Endpoint'] =
          DEFAULT_ENDPOINTS_BY_PROVIDER[provider];
    }
    return headers;
  }

  private normalizeDashboardLoopProfileName(raw: string): string {
    const value = String(raw || '').trim().toLowerCase();
    if (!value) {
      return 'glic_task';
    }
    if (value === 'task' || value === 'glic' || value === 'glic-task' ||
        value === 'task_completer' || value === 'task-completer') {
      return 'glic_task';
    }
    if (value === 'assistant' || value === 'dashboard' ||
        value === 'dashboard-assistant') {
      return 'dashboard_assistant';
    }
    if (value === 'investigate' || value === 'error_investigator' ||
        value === 'error-investigator') {
      return 'investigator';
    }
    return value;
  }

  private mapLoopProfileToUnifiedProfile(
      loopProfile: string): UnifiedAgentProfile {
    const normalized = this.normalizeDashboardLoopProfileName(loopProfile);
    if (normalized === 'dashboard_assistant') {
      return 'script_builder';
    }
    if (normalized === 'investigator') {
      return 'script_debugger';
    }
    return 'browser_operator';
  }

  private normalizeUnifiedProfile(value: any): UnifiedAgentProfile|null {
    const raw = String(value || '').trim().toLowerCase();
    if (!raw) {
      return null;
    }
    if (raw === 'browser_operator' || raw === 'browser-operator' ||
        raw === 'operator' || raw === 'glic_task' || raw === 'glic-task' ||
        raw === 'task' || raw === 'task_completer' || raw === 'task-completer') {
      return 'browser_operator';
    }
    if (raw === 'script_builder' || raw === 'script-builder' ||
        raw === 'builder' || raw === 'dashboard_assistant' ||
        raw === 'dashboard-assistant' || raw === 'assistant') {
      return 'script_builder';
    }
    if (raw === 'script_debugger' || raw === 'script-debugger' ||
        raw === 'debugger' || raw === 'investigator' ||
        raw === 'error_investigator' || raw === 'error-investigator') {
      return 'script_debugger';
    }
    if (raw === 'auto_heal' || raw === 'auto-heal' || raw === 'healer') {
      return 'auto_heal';
    }
    return null;
  }

  private resolveUnifiedProfileFromConfiguration():
      {profile: UnifiedAgentProfile, source: string} {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = this.normalizeUnifiedProfile(
          params.get('unifiedProfile') || params.get('profile') || '');
      if (fromQuery) {
        return {profile: fromQuery, source: 'query'};
      }
      const loopFromQuery = String(params.get('loopProfile') || '').trim();
      if (loopFromQuery) {
        return {
          profile: this.mapLoopProfileToUnifiedProfile(loopFromQuery),
          source: 'query_loopProfile',
        };
      }
    } catch (_) {}

    try {
      const fromStorage = this.normalizeUnifiedProfile(
          localStorage.getItem('glic.unified_profile'));
      if (fromStorage) {
        return {profile: fromStorage, source: 'local_storage'};
      }
      const loopFromStorage =
          String(localStorage.getItem('glic.loop_profile') || '').trim();
      if (loopFromStorage) {
        return {
          profile: this.mapLoopProfileToUnifiedProfile(loopFromStorage),
          source: 'local_storage_loopProfile',
        };
      }
    } catch (_) {}

    const loopFromWindow = String(window.__DASH_LOOP_PROFILE || '').trim();
    if (loopFromWindow) {
      return {
        profile: this.mapLoopProfileToUnifiedProfile(loopFromWindow),
        source: 'dashboard_settings',
      };
    }

    return {profile: 'browser_operator', source: 'default'};
  }

  private setUnifiedProfile(profile: UnifiedAgentProfile,
                            source: string,
                            options?: {persist?: boolean, emitMessage?: boolean}) {
    const previous = this.unifiedProfile;
    this.unifiedProfile = profile;
    this.unifiedProfileSource = source;
    if (options?.persist) {
      try {
        localStorage.setItem('glic.unified_profile', profile);
      } catch (_) {}
    }
    if (previous !== profile) {
      this.unifiedRunId = null;
      if (options?.emitMessage !== false) {
        this.addSystemMessage(
            `Unified profile set to ${profile} (${source}).`);
      }
    } else if (options?.emitMessage) {
      this.addSystemMessage(
          `Unified profile remains ${profile} (${source}).`);
    }
    if (this.modeSelectEl) {
      this.modeSelectEl.value = this.unifiedProfile;
    }
  }

  private inferUrlFromPrompt(prompt: string): string {
    const text = String(prompt || '');
    if (!text) {
      return '';
    }
    const fullUrlMatch = text.match(/\bhttps?:\/\/[^\s"'`<>]+/i);
    if (fullUrlMatch?.[0]) {
      return fullUrlMatch[0].replace(/[),.;:!?]+$/, '');
    }
    const hostMatch =
        text.match(/\b(?:[a-z0-9-]+\.)+[a-z]{2,}(?:\/[^\s"'`<>]*)?/i);
    if (!hostMatch?.[0]) {
      return '';
    }
    const host = hostMatch[0].replace(/[),.;:!?]+$/, '');
    if (!host || host.includes('{') || host.includes('}')) {
      return '';
    }
    return `https://${host}`;
  }

  private isWebNavigableUrl(url: string): boolean {
    const value = String(url || '').trim().toLowerCase();
    if (!value) {
      return false;
    }
    return value.startsWith('http://') || value.startsWith('https://');
  }

  private isNonActuableBrowserUrl(url: string): boolean {
    const value = String(url || '').trim().toLowerCase();
    if (!value || value === 'about:blank') {
      return true;
    }
    return value.startsWith('chrome://') ||
        value.startsWith('chrome-untrusted://') ||
        value.startsWith('chrome-extension://') ||
        value.startsWith('devtools://') ||
        value.startsWith('edge://') ||
        value.startsWith('data:');
  }

  private sameSite(a: string, b: string): boolean {
    try {
      const ua = new URL(a);
      const ub = new URL(b);
      return ua.protocol === ub.protocol && ua.host === ub.host;
    } catch (_) {
      return false;
    }
  }

  /** Prefer an already-open destination tab over createTab (avoids tab storms). */
  private async rebindToExistingDestinationTab(taskUrl: string):
      Promise<boolean> {
    const url = String(taskUrl || '').trim();
    if (!this.isWebNavigableUrl(url)) {
      return false;
    }
    let tabs: any[] = [];
    try {
      tabs = await this.listBrowserTabs?.() || [];
    } catch (_) {
      tabs = [];
    }
    const match = (tabs || []).find((t) => {
      const tabUrl = String(t?.url || '').trim();
      return this.isWebNavigableUrl(tabUrl) && this.sameSite(tabUrl, url) &&
          !this.isNonActuableBrowserUrl(tabUrl);
    });
    if (!match) {
      return false;
    }
    const tabId = String(match.tabId ?? match.id ?? '').trim();
    if (!tabId) {
      return false;
    }
    this.bindTaskTab(tabId, String(match.url || url), match.title || this.currentTask?.goal || null);
    try {
      this.browser?.activateTab?.(tabId);
    } catch (_) {
    }
    this.addSystemMessage(
        `Rebound task to existing tab ${tabId} (${String(match.url || url)}).`);
    this.lastActionSummary = `rebind_existing_tab ${tabId}`;
    return true;
  }

  private async ensureTaskOnDestinationUrl(taskUrl: string, reason: string):
      Promise<boolean> {
    const url = String(taskUrl || '').trim();
    if (!this.isWebNavigableUrl(url)) {
      return false;
    }
    // Never open a duplicate when Lonestar (or the destination) is already open.
    if (await this.rebindToExistingDestinationTab(url)) {
      this.addSystemMessage(`${reason}: reused existing tab for ${url}`);
      await sleep(400);
      return true;
    }
    this.addSystemMessage(`${reason}: opening ${url}`);
    try {
      await this.openTaskUrl(url);
      this.lastActionSummary = `auto_navigate ${url} -> opened`;
      await sleep(1500);
      return true;
    } catch (e) {
      this.addSystemMessage(`Auto-navigate failed: ${e}`);
      return false;
    }
  }

  private resolveManualHeadlessInvestigationUrl(
      userText: string,
      context?: PageContextSummary): string {
    const fromPrompt = this.inferUrlFromPrompt(userText);
    if (this.isWebNavigableUrl(fromPrompt)) {
      return fromPrompt;
    }
    const fromContext = String(context?.url || '').trim();
    if (this.isWebNavigableUrl(fromContext)) {
      return fromContext;
    }
    const fromWorking = String(this.workingTabUrl || '').trim();
    if (this.isWebNavigableUrl(fromWorking)) {
      return fromWorking;
    }
    return '';
  }

  private async maybePrimeManualHeadlessInvestigation(
      userText: string,
      context?: PageContextSummary): Promise<void> {
    if (this.currentTask) {
      return;
    }
    if (!this.isHeadlessPreferredUnifiedProfile()) {
      return;
    }
    if (!this.supportsDashboardInvestigation()) {
      return;
    }
    const url = this.resolveManualHeadlessInvestigationUrl(userText, context);
    const now = Date.now();
    if (url && this.lastManualHeadlessInvestigatedUrl === url &&
        (now - this.lastManualHeadlessInvestigationAtMs) < 20000) {
      return;
    }
    await this.requestHeadlessInvestigation(url ? {url} : {});
    this.lastManualHeadlessInvestigatedUrl = url;
    this.lastManualHeadlessInvestigationAtMs = now;
  }

  private isHeadlessPreferredUnifiedProfile(
      profile: UnifiedAgentProfile = this.unifiedProfile): boolean {
    return profile === 'script_debugger' || profile === 'auto_heal';
  }

  private shouldEnforceHeadlessOnlyForCurrentTask(): boolean {
    return !!this.currentTask && this.isHeadlessPreferredUnifiedProfile();
  }

  async initialize(browser: GlicBrowserHost): Promise<void> {
    this.browser = browser;
    const configuredProfile = this.resolveUnifiedProfileFromConfiguration();
    this.setUnifiedProfile(configuredProfile.profile,
                           configuredProfile.source,
                           {emitMessage: false});
    this.hybridExecutionEnabled = this.resolveHybridExecutionSetting();
    this.nonVisualContextMode = this.resolveNonVisualContextMode();
    this.attachContextForManualPrompts = this.resolveAttachContextMode();
    this.promptOptimizerEnabled = this.resolvePromptOptimizerEnabled();
    this.keepWarmHeadlessInvestigationTab =
        this.resolveKeepWarmHeadlessInvestigationTab();
    if (this.initialized) {
      await this.enablePermissions();
      return;
    }
    this.initialized = true;
    await this.enablePermissions();
    await this.refreshDashboardDataTokenCatalog();
    this.addSystemMessage(
        `Unified execution profile: ${this.unifiedProfile} (${this.unifiedProfileSource}).`);

    const close = document.getElementById('close') as HTMLButtonElement | null;
    close?.addEventListener('click', () => {
      this.browser?.closePanel?.();
    });
    this.panelModeEl = document.getElementById('panelMode');
    this.panelAttachToggleEl =
        document.getElementById('panelAttachToggle') as HTMLButtonElement | null;
    if (!this.browser.attachPanel || !this.browser.detachPanel) {
      this.panelAttachToggleEl?.setAttribute('hidden', 'hidden');
      if (this.panelModeEl) {
        this.panelModeEl.textContent = 'Attached';
      }
    }
    this.panelAttachToggleEl?.addEventListener('click', () => {
      const label = (this.panelAttachToggleEl?.dataset['mode'] || '').toLowerCase();
      if (label === 'detached') {
        this.browser?.attachPanel?.();
      } else {
        this.browser?.detachPanel?.();
      }
    });
    this.browser.getPanelState?.()?.subscribe((state) => {
      this.updatePanelChrome(state);
    });
    this.browser.getFocusedTabStateV2?.()?.subscribe((state) => {
      this.onFocusedTabStateChanged(state);
    });
    this.browser.canAttachPanel?.()?.subscribe((canAttach) => {
      this.updatePanelAttachability(canAttach);
    });

    const send = document.getElementById('send') as HTMLButtonElement | null;
    const stop = document.getElementById('stop') as HTMLButtonElement | null;
    const prompt = document.getElementById('prompt') as HTMLInputElement | null;
    const modeSelect =
        document.getElementById('modeSelect') as HTMLSelectElement | null;
    const providerSelect =
        document.getElementById('provider') as HTMLSelectElement | null;
    const modelSelect =
        document.getElementById('model') as HTMLSelectElement | null;
    const attachContextToggle =
        document.getElementById('attachContextToggle') as HTMLInputElement | null;
    const promptOptimizerToggle =
        document.getElementById('promptOptimizerToggle') as HTMLInputElement | null;
    this.sendBtnEl = send;
    this.stopBtnEl = stop;
    this.modeSelectEl = modeSelect;
    this.providerSelectEl = providerSelect;
    this.modelSelectEl = modelSelect;
    this.attachContextToggleEl = attachContextToggle;
    this.promptOptimizerToggleEl = promptOptimizerToggle;
    {
      const initialProvider = this.resolveConfiguredProvider();
      const initialModel = this.resolveConfiguredModel(initialProvider);
      // localStorage / window only — do not POST yet or a fresh DevProfile
      // would overwrite disk prefs with the Ollama default before restore.
      this.applyProviderModelSelection(initialProvider, initialModel, false);
      window.__DASH_PROVIDER = this.selectedProvider;
      window.__DASH_MODEL = this.selectedModel;
      if (this.selectedProvider === 'ollama') {
        window.__DASH_OLLAMA_ENDPOINT = this.selectedOllamaEndpoint;
      }
    }
    this.providerSelectEl?.addEventListener('change', () => {
      const provider = normalizeAgentProvider(this.providerSelectEl?.value || '');
      this.applyProviderModelSelection(provider, this.modelSelectEl?.value || '');
      this.addSystemMessage(
          `Provider set to ${this.selectedProvider}; model ${this.selectedModel}.`);
    });
    this.modelSelectEl?.addEventListener('change', () => {
      const {provider, model} = this.getSelectedProviderAndModel();
      this.applyProviderModelSelection(provider, model, true);
      this.addSystemMessage(`Model set to ${model} (${provider}).`);
    });
    if (this.modeSelectEl) {
      this.modeSelectEl.value = this.unifiedProfile;
      this.modeSelectEl.addEventListener('change', () => {
        const next = this.normalizeUnifiedProfile(this.modeSelectEl?.value || '');
        if (!next) {
          if (this.modeSelectEl) {
            this.modeSelectEl.value = this.unifiedProfile;
          }
          return;
        }
        this.setUnifiedProfile(next, 'mode_select', {persist: true});
        if (this.isHeadlessPreferredUnifiedProfile()) {
          this.addSystemMessage(
              'Headless mode selected. Manual prompts will preflight with headless investigation when possible.');
        }
      });
    }
    if (this.attachContextToggleEl) {
      this.attachContextToggleEl.checked = this.attachContextForManualPrompts;
    }
    this.attachContextToggleEl?.addEventListener('change', () => {
      this.attachContextForManualPrompts = !!this.attachContextToggleEl?.checked;
      try {
        localStorage.setItem(
            'glic.attach_context',
            this.attachContextForManualPrompts ? '1' : '0');
      } catch (_) {}
    });
    if (this.promptOptimizerToggleEl) {
      this.promptOptimizerToggleEl.checked = this.promptOptimizerEnabled;
    }
    this.promptOptimizerToggleEl?.addEventListener('change', () => {
      this.promptOptimizerEnabled = !!this.promptOptimizerToggleEl?.checked;
      try {
        localStorage.setItem(
            'glic.prompt_optimizer',
            this.promptOptimizerEnabled ? '1' : '0');
      } catch (_) {}
    });
    this.updateLlmUiState();

    this.promptInputEl = prompt;
    this.loadPromptHistory();
    void this.restoreClientPrefsFromServer();

    const submit = async () => {
      const text = (prompt?.value ?? '').trim();
      if (!text) {
        return;
      }
      await this.submitPromptText(text, {clearInput: true});
    };

    send?.addEventListener('click', () => void submit());
    stop?.addEventListener('click', () => {
      this.pauseQueue('stopped by user');
    });
    prompt?.addEventListener('keydown', (e: KeyboardEvent) => {
      // Prompt history is independent of provider/model; ↑/↓ browse local+disk history.
      if (e.key === 'ArrowUp') {
        if (!this.promptHistory.length) {
          return;
        }
        e.preventDefault();
        this.browsePromptHistory('up');
        return;
      }
      if (e.key === 'ArrowDown') {
        if (this.promptHistoryIndex < 0 && !this.promptHistory.length) {
          return;
        }
        e.preventDefault();
        this.browsePromptHistory('down');
        return;
      }
      if (e.key === 'Enter') {
        e.preventDefault();
        void submit();
      }
    }, true);

    this.queueStatusEl = document.getElementById('queueStatus');
    this.taskInfoEl = document.getElementById('taskInfo');
    this.queueWidgetToggleEl =
        document.getElementById('queueWidgetToggle') as HTMLButtonElement | null;
    this.queueWidgetPanelEl = document.getElementById('queueWidgetPanel');
    this.queueWidgetSubtitleEl = document.getElementById('queueWidgetSubtitle');
    this.queueWorkingContextEl = document.getElementById('queueWorkingContext');
    this.queueActiveTaskEl = document.getElementById('queueActiveTask');
    this.queueWidgetListEl = document.getElementById('queueWidgetList');

    const csvPath = document.getElementById('csvPath') as HTMLInputElement | null;
    const loadPath = document.getElementById('loadPath') as HTMLButtonElement | null;
    const csvText = document.getElementById('csvText') as HTMLTextAreaElement | null;
    const loadText = document.getElementById('loadText') as HTMLButtonElement | null;
    const startQueue = document.getElementById('startQueue') as HTMLButtonElement | null;
    const pauseQueue = document.getElementById('pauseQueue') as HTMLButtonElement | null;
    const nextTask = document.getElementById('nextTask') as HTMLButtonElement | null;
    const queueWidgetClose =
        document.getElementById('queueWidgetClose') as HTMLButtonElement | null;
    const queueResume =
        document.getElementById('queueResume') as HTMLButtonElement | null;
    const queuePause =
        document.getElementById('queuePause') as HTMLButtonElement | null;
    const queueNext = document.getElementById('queueNext') as HTMLButtonElement | null;
    const queueClear = document.getElementById('queueClear') as HTMLButtonElement | null;

    if (csvPath && !csvPath.value.trim()) {
      csvPath.value = this.resolveLastValidCsvPath();
    }

    const loadFromPath = async () => {
      const path = (csvPath?.value ?? '').trim();
      if (!path) {
        this.addSystemMessage('Provide a CSV path first.');
        return;
      }
      try {
        const res = await fetch('/task/load_csv', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({path}),
        });
        if (!res.ok) {
          let detail = '';
          try {
            const payload = await res.json();
            const error = String(payload?.error || '').trim();
            const allowedRoot = String(payload?.allowed_root || '').trim();
            if (error && allowedRoot) {
              detail = `${error}. Allowed CSV directory: ${allowedRoot}`;
            } else if (error) {
              detail = error;
            }
          } catch (_) {}
          this.addSystemMessage(
              `CSV load failed: ${res.status}${
                  detail ? ` ${detail}` : ` ${res.statusText}`}`);
          return;
        }
        const payload = await res.json();
        const content = typeof payload?.content === 'string' ? payload.content : '';
        const resolvedPath =
            typeof payload?.resolved_path === 'string' ? payload.resolved_path.trim() : '';
        if (!content) {
          this.addSystemMessage('CSV load returned empty content.');
          return;
        }
        if (resolvedPath) {
          this.persistLastValidCsvPath(resolvedPath);
          if (csvPath) {
            csvPath.value = resolvedPath;
          }
        }
        this.loadTasks(content);
      } catch (e) {
        this.addSystemMessage(`CSV load failed: ${e}`);
      }
    };

    const loadFromText = async () => {
      const text = (csvText?.value ?? '').trim();
      if (!text) {
        this.addSystemMessage('Paste CSV content first.');
        return;
      }
      this.loadTasks(text);
    };

    loadPath?.addEventListener('click', () => void loadFromPath());
    loadText?.addEventListener('click', () => void loadFromText());
    startQueue?.addEventListener('click', () => void this.startQueue());
    pauseQueue?.addEventListener('click', () => this.pauseQueue());
    nextTask?.addEventListener('click', () => void this.skipToNextTask());
    this.queueWidgetToggleEl?.addEventListener('click', () => {
      this.toggleQueueWidget();
    });
    queueWidgetClose?.addEventListener('click', () => this.toggleQueueWidget(false));
    queueResume?.addEventListener('click', () => void this.startQueue());
    queuePause?.addEventListener('click', () => this.pauseQueue());
    queueNext?.addEventListener('click', () => void this.skipToNextTask());
    queueClear?.addEventListener('click', () => this.clearQueuedTasks());

    this.render();
    this.updateQueueStatus();
    this.updateTaskInfo();
    this.renderQueueWidget();
  }

  private updatePanelChrome(state: PanelState|undefined) {
    if (!this.panelModeEl || !this.panelAttachToggleEl) {
      return;
    }
    const kind = state?.kind ?? PanelStateKind.HIDDEN;
    if (kind === PanelStateKind.DETACHED) {
      this.panelModeEl.textContent = 'Floating';
      this.panelAttachToggleEl.textContent = 'Dock';
      this.panelAttachToggleEl.dataset['mode'] = 'detached';
      this.panelAttachToggleEl.hidden = false;
      this.updatePanelAttachability(this.canAttachPanelNow);
      return;
    }
    if (kind === PanelStateKind.ATTACHED) {
      this.panelModeEl.textContent = 'Attached';
      this.panelAttachToggleEl.textContent = 'Pop out';
      this.panelAttachToggleEl.dataset['mode'] = 'attached';
      this.panelAttachToggleEl.hidden = false;
      this.updatePanelAttachability(this.canAttachPanelNow);
      return;
    }
    this.panelModeEl.textContent = 'Hidden';
    this.panelAttachToggleEl.textContent = 'Pop out';
    this.panelAttachToggleEl.dataset['mode'] = 'attached';
    this.updatePanelAttachability(this.canAttachPanelNow);
  }

  private updatePanelAttachability(canAttach: boolean) {
    this.canAttachPanelNow = canAttach;
    if (!this.panelAttachToggleEl) {
      return;
    }
    const mode = this.panelAttachToggleEl.dataset['mode'] || 'attached';
    // Detaching should always remain available when the API is exposed.
    this.panelAttachToggleEl.disabled = mode === 'detached' ? !canAttach : false;
  }

  private resolveHybridExecutionSetting(): boolean {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = params.get('hybridExecution');
      if (fromQuery !== null) {
        const v = fromQuery.trim().toLowerCase();
        return !(v === '0' || v === 'false' || v === 'off');
      }
    } catch (_) {}
    try {
      const fromStorage = localStorage.getItem('glic.hybrid_execution');
      if (typeof fromStorage === 'string' && fromStorage.trim()) {
        const v = fromStorage.trim().toLowerCase();
        return !(v === '0' || v === 'false' || v === 'off');
      }
    } catch (_) {}
    return true;
  }

  private resolveNonVisualContextMode(): boolean {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = params.get('nonVisualContext');
      if (fromQuery !== null) {
        const v = fromQuery.trim().toLowerCase();
        return !(v === '0' || v === 'false' || v === 'off');
      }
    } catch (_) {}
    try {
      const fromStorage = localStorage.getItem('glic.non_visual_context');
      if (typeof fromStorage === 'string' && fromStorage.trim()) {
        const v = fromStorage.trim().toLowerCase();
        return !(v === '0' || v === 'false' || v === 'off');
      }
    } catch (_) {}
    // Default to non-visual context for local/text-only models.
    return true;
  }

  private resolveAttachContextMode(): boolean {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = params.get('attachContext');
      if (fromQuery !== null) {
        const v = fromQuery.trim().toLowerCase();
        return !(v === '0' || v === 'false' || v === 'off');
      }
    } catch (_) {}
    try {
      const fromStorage = localStorage.getItem('glic.attach_context');
      if (typeof fromStorage === 'string' && fromStorage.trim()) {
        const v = fromStorage.trim().toLowerCase();
        return v === '1' || v === 'true' || v === 'on';
      }
    } catch (_) {}
    // Default to detached context for normal chat unless user enables it.
    return false;
  }

  private resolvePromptOptimizerEnabled(): boolean {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = params.get('promptOptimizer');
      if (fromQuery !== null) {
        const v = fromQuery.trim().toLowerCase();
        return v === '1' || v === 'true' || v === 'on';
      }
    } catch (_) {}
    try {
      const fromStorage = localStorage.getItem('glic.prompt_optimizer');
      if (typeof fromStorage === 'string' && fromStorage.trim()) {
        const v = fromStorage.trim().toLowerCase();
        return v === '1' || v === 'true' || v === 'on';
      }
    } catch (_) {}
    return false;
  }

  private defaultCsvPath(): string {
    return '/Users/matcauthon/notrainnogain/config.csv';
  }

  private resolveLastValidCsvPath(): string {
    try {
      const fromStorage =
          String(localStorage.getItem('glic.last_valid_csv_path') || '').trim();
      if (fromStorage) {
        return fromStorage;
      }
    } catch (_) {}
    return this.defaultCsvPath();
  }

  private persistLastValidCsvPath(path: string): void {
    const trimmed = String(path || '').trim();
    if (!trimmed) {
      return;
    }
    try {
      localStorage.setItem('glic.last_valid_csv_path', trimmed);
    } catch (_) {}
  }

  private resolveHeadlessModelOverride(): string {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = String(params.get('headlessModel') || '').trim();
      if (fromQuery) {
        return fromQuery;
      }
    } catch (_) {}
    try {
      const fromStorage = String(localStorage.getItem('glic.headless_model') || '')
                              .trim();
      if (fromStorage) {
        return fromStorage;
      }
    } catch (_) {}
    return String(window.__DASH_HEADLESS_MODEL || '').trim();
  }

  private resolveKeepWarmHeadlessInvestigationTab(): boolean {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = params.get('keepWarmHeadlessInvestigationTab');
      if (fromQuery !== null) {
        const v = fromQuery.trim().toLowerCase();
        return !(v === '0' || v === 'false' || v === 'off');
      }
    } catch (_) {}
    try {
      const fromStorage =
          localStorage.getItem('glic.keep_warm_headless_investigation_tab');
      if (typeof fromStorage === 'string' && fromStorage.trim()) {
        const v = fromStorage.trim().toLowerCase();
        return !(v === '0' || v === 'false' || v === 'off');
      }
    } catch (_) {}
    return window.__DASH_KEEP_WARM_HEADLESS !== false;
  }

  private async refreshDashboardDataTokenCatalog(): Promise<void> {
    if (!this.browser?.getDashboardDataTokenCatalog) {
      return;
    }
    try {
      const raw = await this.browser.getDashboardDataTokenCatalog();
      const parsed = JSON.parse(String(raw || ''));
      const normalizeList = (value: any): string[] =>
        Array.isArray(value) ? value.map((item) => String(item || '').trim())
                                 .filter(Boolean) :
                               [];
      this.dashboardDataTokenCatalog = {
        tokenNames: normalizeList(parsed?.tokenNames),
        variableTokenNames: normalizeList(parsed?.variableTokenNames),
        secretTokenNames: normalizeList(parsed?.secretTokenNames),
        tokenCount: Number(parsed?.tokenCount) || undefined,
        secretTokenCount: Number(parsed?.secretTokenCount) || undefined,
      };
    } catch (e) {
      console.warn('[Glic] Failed to load dashboard data-token catalog', e);
    }
  }

  private async enablePermissions() {
    if (!this.browser) {
      return;
    }
    try {
      await this.browser.setTabContextPermissionState(true);
      this.addSystemMessage('Enabled tab context permission.');
    } catch (e) {
      this.addSystemMessage(`Failed to enable tab context permission: ${e}`);
    }
    if (this.browser.setActuationOnWebSetting) {
      try {
        await this.browser.setActuationOnWebSetting(true);
        this.addSystemMessage('Enabled actuation on web.');
      } catch (e) {
        this.addSystemMessage(`Failed to enable actuation on web: ${e}`);
      }
    }
  }

  private async requestConsoleLogs(
      level = 'error',
      limit = 20): Promise<void> {
    if (!this.browser?.getDashboardDevToolsConsoleLogs) {
      this.addSystemMessage('Console log retrieval is unavailable in this host.');
      return;
    }
    try {
      const raw = await this.browser.getDashboardDevToolsConsoleLogs(level, limit);
      const parsed = JSON.parse(String(raw || '[]'));
      const entries = Array.isArray(parsed) ? parsed : [];
      if (!entries.length) {
        this.addSystemMessage(
            `No buffered console ${level === 'all' ? 'entries' : level + ' entries'} were available.`);
        return;
      }
      const rendered = entries.slice(0, 20).map((entry) => {
        const lvl = String(entry?.level || 'log');
        const msg = String(entry?.message || '').trim();
        const src = String(entry?.source || entry?.frame_url || '').trim();
        const line = Number(entry?.line);
        const suffix =
            src ? ` @ ${src}${Number.isFinite(line) && line > 0 ? ':' + line : ''}` : '';
        return `[${lvl}] ${msg}${suffix}`;
      }).join('\n');
      this.addSystemMessage(`Buffered console logs:\n${rendered}`);
    } catch (e) {
      this.addSystemMessage(`Failed to retrieve console logs: ${e}`);
    }
  }

  private supportsScreenshotAction(): boolean {
    return this.getSelectedProviderAndModel().provider === 'anthropic';
  }

  /** Anthropic: capture viewport for the next model turn (used by request_screenshot
   *  and temporarily also by request_context to verify the vision path). */
  private async captureScreenshotForNextTurn(reason: string): Promise<boolean> {
    if (!this.supportsScreenshotAction()) {
      return false;
    }
    // Skip quietly when this guest has no actuable tab binding (common for a
    // secondary/unfocused Glic webview). Only surface failures for explicit
    // user-facing request_screenshot.
    const hasTabHint = !!(this.currentTask || this.currentTabId ||
                          this.workingTabUrl || this.getAuthoritativeTaskTabId?.());
    const shot = await this.captureTaskViewportScreenshot();
    if (!shot) {
      if (reason === 'request_screenshot' ||
          (hasTabHint && !String(reason).startsWith('hotspot-test'))) {
        this.addSystemMessage(
            `request_screenshot failed (${reason}): could not capture a viewport image` +
            (hasTabHint ? '.' : ' (no task tab bound in this Glic guest).'));
      }
      return false;
    }
    const annotated = await this.annotateScreenshotForCoordinateClicks(shot);
    this.pendingScreenshot = annotated || shot;
    const ready = this.pendingScreenshot;
    const kb = Math.max(1, Math.round(ready.base64.length / 1024));
    const dim = (ready.cssWidth && ready.cssHeight) ?
        ` ${ready.cssWidth}x${ready.cssHeight} CSS px + grid` :
        '';
    this.addSystemMessage(
        `Captured viewport screenshot (${ready.mimeType}, ~${kb}KB${dim}) for the next Anthropic turn (${reason}).`);
    return true;
  }

  private supportsDashboardInvestigation(): boolean {
    return !!this.browser?.investigateDashboardPage;
  }

  private currentTaskPrefersHeadlessInvestigation(): boolean {
    const goal = String(this.currentTask?.goal || '').toLowerCase();
    if (!goal) {
      return false;
    }
    return /\b(debug|diagnos|troubleshoot|investigat|inspect|heal|repair|fix|error|failure|broken|why)\b/.test(
        goal);
  }

  private headlessInvestigationLabel(): string {
    const byProfile: Record<UnifiedAgentProfile, string> = {
      browser_operator: 'Browser Operator',
      script_builder: 'Script Builder',
      script_debugger: 'Script Debugger',
      auto_heal: 'Auto Heal',
    };
    return byProfile[this.unifiedProfile] || 'Investigation';
  }

  private headlessInvestigationReason(): string {
    const activeGoal = String(this.currentTask?.goal || '').trim();
    if (activeGoal) {
      return `glic:${this.unifiedProfile}:${activeGoal}`;
    }
    return `glic:${this.unifiedProfile}`;
  }

  private async requestHeadlessInvestigation(raw: any): Promise<void> {
    if (!this.browser?.investigateDashboardPage) {
      this.addSystemMessage('Headless investigation is unavailable in this host.');
      return;
    }
    this.keepWarmHeadlessInvestigationTab =
        this.resolveKeepWarmHeadlessInvestigationTab();
    const requestedCloseTarget =
        raw?.closeTarget ?? raw?.close_target ?? undefined;
    const closeTarget = requestedCloseTarget === undefined ?
        !this.keepWarmHeadlessInvestigationTab :
        !!requestedCloseTarget;
    const requestedUrl = String(raw?.url || '').trim();
    const url = requestedUrl ||
        (closeTarget ? '' : (this.currentTask?.url || this.workingTabUrl || ''));
    // Text-only / non-visual guest: never request encoded screenshots.
    const includeScreenshot = this.nonVisualContextMode
        ? false
        : (!!raw?.include_screenshot || !!raw?.includeScreenshot);
    const request: any = {
      includeScreenshot,
      closeTarget,
      openReason: this.headlessInvestigationReason(),
      label: this.headlessInvestigationLabel(),
    };
    if (url) {
      request.url = url;
    }
    const requestedTaskId = String(raw?.taskId || raw?.task_id || '').trim();
    const taskId = requestedTaskId || this.activeHeadlessInvestigationTaskId || '';
    if (taskId) {
      request.taskId = taskId;
    }
    try {
      const rawResult = await this.browser.investigateDashboardPage(request);
      const result = JSON.parse(String(rawResult || '{}'));
      const returnedTaskId =
          String(result?.target_id || result?.targetId ||
                 result?.task_id || result?.taskId || '').trim();
      if (closeTarget) {
        if (!returnedTaskId || returnedTaskId === this.activeHeadlessInvestigationTaskId) {
          this.activeHeadlessInvestigationTaskId = null;
        }
      } else if (returnedTaskId) {
        this.activeHeadlessInvestigationTaskId = returnedTaskId;
      }
      const lines: string[] = [];
      lines.push('Headless investigation result:');
      lines.push(`ok=${!!result?.ok}`);
      if (returnedTaskId) {
        lines.push(`task_id=${returnedTaskId}`);
      }
      if (result?.url) {
        lines.push(`url=${String(result.url)}`);
      }
      if (result?.title) {
        lines.push(`title=${String(result.title)}`);
      }
      if (result?.ready_state) {
        lines.push(`ready_state=${String(result.ready_state)}`);
      }
      const elementCount = Number(result?.element_count ?? 0);
      if (Number.isFinite(elementCount)) {
        lines.push(`element_count=${elementCount}`);
      }
      const actionError =
          String(result?.action_result?.error || result?.error || '').trim();
      if (actionError) {
        lines.push(`error=${actionError}`);
      }
      const pageMap = result?.page_map ?? result?.pageMap;
      const interactiveCount = Number(
          pageMap?.metadata?.interactiveCount ??
          pageMap?.metadata?.interactive_count ?? NaN);
      if (Number.isFinite(interactiveCount)) {
        lines.push(`page_map_interactive_count=${interactiveCount}`);
      }
      this.lastHeadlessInvestigationSummary = lines.join('\n');
      this.addSystemMessage(this.lastHeadlessInvestigationSummary);
    } catch (e) {
      if (closeTarget) {
        this.activeHeadlessInvestigationTaskId = null;
      } else {
        this.lastHeadlessInvestigationSummary = '';
        this.addSystemMessage(`Headless investigation failed: ${e}`);
      }
    }
  }

  private async recoverTabContextPermissionIfNeeded(
      error: unknown): Promise<boolean> {
    const message = String(error || '');
    if (!message.includes('tabContext failed: permission denied')) {
      return false;
    }
    if (this.tabContextPermissionRecoveryAttempted) {
      return false;
    }
    this.tabContextPermissionRecoveryAttempted = true;
    this.addSystemMessage(
        'Tab context permission was denied by the host. Re-enabling and retrying once.');
    await this.enablePermissions();
    await sleep(180);
    return true;
  }

  async notifyPanelWillOpen(panelOpeningData: PanelOpeningData&PanelState):
      Promise<OpenPanelInfo> {
    const suggestion =
        String((panelOpeningData as any)?.promptSuggestion || '').trim();
    if (suggestion) {
      const promptEl =
          document.getElementById('prompt') as HTMLInputElement | null;
      if (promptEl) {
        promptEl.value = suggestion;
      }
      // Match a user Send: prefill Ask..., then submit after the panel settles.
      void this.autoSubmitPromptLikeUser(suggestion);
    }
    return {
      startingMode: WebClientMode.TEXT,
      resizeParams: {
        width: 420,
        height: 560,
        options: {durationMs: 0},
      },
    };
  }

  private async autoSubmitPromptLikeUser(text: string): Promise<void> {
    const trimmed = String(text || '').trim();
    if (!trimmed) {
      return;
    }
    await sleep(300);
    const promptEl =
        document.getElementById('prompt') as HTMLInputElement | null;
    if (promptEl && !String(promptEl.value || '').trim()) {
      promptEl.value = trimmed;
    }
    this.addSystemMessage('Startup prompt received from host; sending like a user.');
    await this.submitPromptText(trimmed, {clearInput: true});
  }

  async checkResponsive(): Promise<void> {
    return;
  }

  private render() {
    const messagesEl = document.getElementById('messages');
    if (!messagesEl) {
      return;
    }
    messagesEl.textContent = '';
    for (const m of this.messages) {
      const div = document.createElement('div');
      div.className = `msg ${m.role}`;
      div.textContent = m.content;
      messagesEl.appendChild(div);
    }
    messagesEl.scrollTop = messagesEl.scrollHeight;
  }

  private updateQueueStatus() {
    if (this.queueStatusEl) {
      const running = this.queueRunning ? 'running' : 'paused';
      this.queueStatusEl.textContent = `Queue: ${this.taskQueue.length} (${running})`;
    }
    this.renderQueueWidget();
  }

  private addSystemMessage(message: string) {
    const last = this.messages[this.messages.length - 1];
    if (last?.role === 'system' && last.content === message) {
      return;
    }
    this.messages.push({role: 'system', content: message});
    this.render();
  }

  private ensureCoreAssistantContextPrompt() {
    if (this.hasCoreAssistantContextPrompt) {
      return;
    }
    const lines: string[] = [];
    lines.push('You are the Simulachrome in-product automation assistant.');
    lines.push(
        'When a USER GOAL / user message is present, it outranks default habits and tips in system prompts. Follow the user\'s stated constraints first.');
    lines.push('Do not default to Selenium/Playwright/Puppeteer guidance unless the user explicitly asks for those frameworks.');
    lines.push('Assume user requests refer to Simulachrome dashboard scripts, actions, and run diagnostics.');
    lines.push('When asked to create automation, prefer Simulachrome script/action JSON over external code.');
    lines.push('Hard constraint: if the user did NOT explicitly request Selenium/Playwright/Puppeteer/WebDriver code, do NOT output those frameworks, Python automation code, package-install commands, or chromedriver setup steps.');
    lines.push('When the user asks to create a script, return Simulachrome-native JSON (script object or action JSON), not external framework snippets.');
    lines.push('');
    lines.push('Runtime tool actions available from this chat host:');
    lines.push('{"action":"request_context"}');
    lines.push('{"action":"request_console_logs","level":"error|warning|all","limit":20}');
    lines.push('{"action":"delegate_profile","target_profile":"browser_operator|script_builder|script_debugger|auto_heal","reason":"..."}');
    if (this.supportsScreenshotAction()) {
      lines.push('{"action":"request_screenshot"}');
    }
    if (this.supportsDashboardInvestigation()) {
      lines.push('{"action":"investigate_page"}');
      lines.push('{"action":"investigate_page","url":"https://example.com/"}');
      lines.push(
          '{"action":"investigate_page","url":"https://example.com/","closeTarget":false}');
      lines.push(
          '{"action":"investigate_page","task_id":"<existing_task_id>","closeTarget":false}');
    }
    lines.push('');
    lines.push('During an active queued task loop, executable tab actions are available:');
    lines.push('{"action":"click","target_id":123}');
    lines.push('{"action":"click","coordinate":[x,y]}');
    lines.push('{"action":"hover","target_id":123}');
    lines.push('{"action":"type","target_id":123,"text":"..."}');
    lines.push('{"action":"type","coordinate":[x,y],"text":"..."}');
    lines.push('{"action":"scroll","target_id":123,"distance":600}');
    lines.push('{"action":"scroll_to","target_id":123}');
    lines.push('{"action":"key_press","key":"Enter"}');
    lines.push('{"action":"wait","timeout":2}');
    lines.push('{"action":"complete"}');
    lines.push('');
    lines.push('Profile guidance: browser_operator=site interaction, script_builder=script/settings/data editing, script_debugger=investigation/failure diagnosis, auto_heal=healing workflows.');
    lines.push('At most one delegate_profile switch is allowed per active task loop.');
    lines.push('For active queued tasks, script_debugger and auto_heal are headless-first and do not execute foreground tab actions.');
    lines.push('');
    lines.push('For clickable/editable actions, prefer target_id from current context elements[].id.');
    lines.push(
        'If a visible control is missing from elements[] (canvas/image overlay/popup), use click/type with {"coordinate":[x,y]} (viewport CSS pixels from a screenshot).');
    if (this.supportsDashboardInvestigation()) {
      lines.push(
          'For persistent headless handoff, call investigate_page with closeTarget:false; then reuse task_id on later investigate_page calls.');
      lines.push(
          'investigate_page optionally accepts actions:[...] (Simulachrome timeline steps) to run against the headless task tab.');
    }
    lines.push('If page context is sparse/empty, request_context or investigate_page before making locator claims.');
    if (this.supportsScreenshotAction()) {
      lines.push(
          'If a click/type does not produce a noticeable page change, use request_screenshot to inspect the viewport.');
      lines.push(
          'For screenshot-visible controls absent from elements[], use {"action":"click","coordinate":[x,y]} from the cyan CSS-pixel grid on the screenshot.');
    }
    lines.push('If there is no active queued task and the user asks to create a script, return one Simulachrome script JSON object (not action JSON, and not framework code).');
    lines.push('');
    lines.push('Simulachrome script JSON reference shape (same AgentModelContract as dashboard/Glic):');
    lines.push('{"name":"Script Name","actions":[{"action":"navigate","params":{"url":"https://example.com"}},{"action":"click","params":{"locators":[{"type":"CSS_SELECTOR","value":"button[type=submit]"}],"clickCount":1}},{"action":"wait","params":{"waitType":"element","locators":[{"type":"CSS_SELECTOR","value":"#table"}],"timeout":10000}}]}');
    lines.push('Do not emit Playwright shapes ({"type":"goto","url":...} or top-level "selector"). Always {"action","params"} with params.locators. waitType is only time|element.');
    lines.push('Output contract: return JSON only (no markdown/code fences).');
    lines.push('If user asks to create/generate/build a script in manual chat, return one script JSON object: {"name":"...","actions":[...]}');
    lines.push('If user asks for tool execution/inspection, return one action JSON object from the allowed runtime actions.');
    this.messages.push({role: 'system', content: lines.join('\n')});
    this.hasCoreAssistantContextPrompt = true;
  }

  private userExplicitlyRequestedExternalFramework(userPrompt: string): boolean {
    const text = String(userPrompt || '').toLowerCase();
    return /\b(selenium|playwright|puppeteer|webdriver|appium|cypress)\b/.test(
        text);
  }

  private responseLooksLikeExternalFrameworkCode(assistantText: string): boolean {
    const text = String(assistantText || '');
    if (!text.trim()) {
      return false;
    }
    return /\bselenium\b|\bplaywright\b|\bpuppeteer\b|\bwebdriver\b|chromedriver|pip install|npm install|from\s+selenium|webdriver\.chrome|webdriver\.firefox/i
        .test(text);
  }

  private userLikelyWantsSimulachromeScript(userPrompt: string): boolean {
    const text = String(userPrompt || '').toLowerCase();
    if (!text) {
      return false;
    }
    return /\b(make|create|build|generate|write)\b[\s\S]{0,80}\b(script|automation|workflow)\b/.test(
               text) ||
        /\bscript\b[\s\S]{0,80}\b(login|log in|signup|sign up|checkout|scrape|navigate)\b/.test(
            text);
  }

  private profileSupportsManualTaskLoop(
      profile: UnifiedAgentProfile = this.unifiedProfile): boolean {
    return profile === 'browser_operator' || profile === 'script_debugger' ||
        profile === 'auto_heal';
  }

  private userLikelyWantsImmediateExecution(
      userPrompt: string,
      profile: UnifiedAgentProfile = this.unifiedProfile): boolean {
    const text = String(userPrompt || '').trim().toLowerCase();
    if (!text) {
      return false;
    }
    if (this.userLikelyWantsSimulachromeScript(text)) {
      return false;
    }
    const investigationIntent =
        /\b(investigate|debug|diagnose|diagnosis|troubleshoot|auto[-\s]?heal|heal|repair|fix)\b/
        .test(text);
    const directExecutionIntent =
        /\b(log in|login|sign in|click|type|fill|enter|submit|drag|drop|hover|scroll|navigate|go to|open|press|run|execute)\b/
            .test(text);
    const explicitExecution =
        /\b(now|please|go ahead|run|execute|start|do it)\b/.test(text);
    if (/^\s*(how|why|what|which|who|when)\b/.test(text) &&
        !investigationIntent && !explicitExecution) {
      return false;
    }
    if (/^\s*(can|could|would|should|is|are|do|does|did)\b/.test(text) &&
        !investigationIntent && !explicitExecution) {
      return false;
    }
    if (investigationIntent &&
        (profile === 'script_debugger' || profile === 'auto_heal')) {
      return true;
    }
    return directExecutionIntent || investigationIntent;
  }

  private resolveManualTaskExecutionUrl(userText: string,
                                        context?: PageContextSummary): string {
    const fromPrompt = this.inferUrlFromPrompt(userText);
    if (this.isWebNavigableUrl(fromPrompt)) {
      return fromPrompt;
    }
    // Goals often say "go to moonspin.us" without scheme; also try goal-only hosts.
    const fromGoalHosts = this.inferUrlFromPrompt(
        String(userText || '').replace(/(go to|open|visit|navigate to)/gi, ' '));
    if (this.isWebNavigableUrl(fromGoalHosts)) {
      return fromGoalHosts;
    }
    const fromContext = String(context?.url || '').trim();
    if (this.isWebNavigableUrl(fromContext)) {
      return fromContext;
    }
    const fromWorking = String(this.workingTabUrl || '').trim();
    if (this.isWebNavigableUrl(fromWorking)) {
      return fromWorking;
    }
    return 'about:blank';
  }

  private async maybeStartManualTaskLoop(
      userText: string,
      context?: PageContextSummary): Promise<boolean> {
    if (this.currentTask || this.queueRunning || this.llmRequestInFlight) {
      return false;
    }
    if (!this.profileSupportsManualTaskLoop()) {
      return false;
    }
    if (!this.userLikelyWantsImmediateExecution(userText, this.unifiedProfile)) {
      return false;
    }

    const url = this.resolveManualTaskExecutionUrl(userText, context);
    const temporaryTask: TaskItem = {
      id: `manual-${Date.now()}`,
      url,
      goal: userText,
    };
    this.currentTask = temporaryTask;
    this.unifiedProfileHopUsed = false;
    this.lastHeadlessInvestigationSummary = '';
    this.actorTaskId = null;
    this.unifiedRunId = null;
    this.tabReopenAttempts = 0;
    this.warnedExtraMetaKeys = false;
    this.typedUsername = false;
    this.requestContextNextTurn = false;
    this.lastTaskElementIds = [];
    this.lastTaskLabeledElements = [];
    this.currentTaskStepIndex = 0;
    this.clearTaskTabBinding();
    this.currentTabId = null;
    this.setWorkingContext(null, temporaryTask.url, temporaryTask.goal);
    await this.refreshDashboardDataTokenCatalog();
    this.messages = [];
    this.hasCoreAssistantContextPrompt = false;
    this.lastContextEvidenceKey = '';
    this.addTaskSystemPrompt(temporaryTask);
    this.addSystemMessage(
        `Started temporary task loop from manual prompt (${this.unifiedProfile}).`);
    // So Pause/Resume treat manual loops like queued runs.
    this.taskLoopStopRequested = false;
    this.queueRunning = true;
    this.updateQueueStatus();
    this.updateTaskInfo();
    this.render();

    if (!this.supportsUnifiedAgentOrchestrator()) {
      await this.ensureActorTask();
    }
    const contextTabId = String(context?.tabId || '').trim();
    const contextUrl = String(context?.url || '').trim();
    if (!this.shouldEnforceHeadlessOnlyForCurrentTask()) {
      if (this.isWebNavigableUrl(url)) {
        const alreadyThere = contextTabId && this.isWebNavigableUrl(contextUrl) &&
            !this.isNonActuableBrowserUrl(contextUrl) &&
            this.resolveManualTaskExecutionUrl('', context) === url;
        if (alreadyThere) {
          this.bindTaskTab(contextTabId, url, temporaryTask.goal);
        } else {
          await this.openTaskUrl(url, {forceNew: !contextTabId});
        }
      } else if (contextTabId) {
        this.bindTaskTab(
            contextTabId, contextUrl || url, temporaryTask.goal);
      }
    } else if (this.supportsDashboardInvestigation()) {
      await this.requestHeadlessInvestigation({url});
    }

    void this.runTaskLoop();
    return true;
  }

  private parseFirstJsonObjectFromText(text: string): any|null {
    const raw = String(text || '').trim();
    if (!raw) {
      return null;
    }
    const fencedMatch = raw.match(/```(?:json)?\s*([\s\S]*?)```/i);
    if (fencedMatch && fencedMatch[1]) {
      const candidate = fencedMatch[1].trim();
      try {
        return JSON.parse(candidate);
      } catch (_) {}
    }
    try {
      return JSON.parse(raw);
    } catch (_) {}
    const start = raw.indexOf('{');
    const end = raw.lastIndexOf('}');
    if (start >= 0 && end > start) {
      const candidate = raw.slice(start, end + 1).trim();
      try {
        return JSON.parse(candidate);
      } catch (_) {}
    }
    return null;
  }

  private normalizeLocatorType(rawType: any): string {
    const t = String(rawType || '').trim().toUpperCase();
    if (!t) return '';
    if (t === 'CSS' || t === 'CSS_SELECTOR') return 'CSS_SELECTOR';
    if (t === 'XPATH') return 'XPATH';
    if (t === 'VISUAL_COORDS' || t === 'COORDS' || t === 'COORDINATES') {
      return 'VISUAL_COORDS';
    }
    if (t === 'TEXT' || t === 'TEXT_CONTENT') return 'TEXT_CONTENT';
    if (t === 'ACCESSIBILITY' || t === 'ACCESSIBILITY_NAME') {
      return 'ACCESSIBILITY_NAME';
    }
    return t;
  }

  private normalizeLocatorArray(rawLocators: any): Array<{type: string, value: string}> {
    if (!Array.isArray(rawLocators)) {
      return [];
    }
    const out: Array<{type: string, value: string}> = [];
    for (const locator of rawLocators) {
      if (!locator || typeof locator !== 'object') continue;
      const type = this.normalizeLocatorType(locator.type);
      const value = String(locator.value ?? '').trim();
      if (!type || !value) continue;
      out.push({type, value});
    }
    return out;
  }

  private collectLocatorsFromStep(step: any): Array<{type: string, value: string}> {
    const direct = this.normalizeLocatorArray(step?.locators);
    if (direct.length) return direct;
    const inParams = this.normalizeLocatorArray(step?.params?.locators);
    if (inParams.length) return inParams;
    const locator = step?.locator;
    if (Array.isArray(locator)) {
      return this.normalizeLocatorArray(locator);
    }
    if (locator && typeof locator === 'object') {
      const out: Array<{type: string, value: string}> = [];
      const primary = locator.primary;
      if (primary && typeof primary === 'object') {
        const type = this.normalizeLocatorType(primary.type);
        const value = String(primary.value ?? '').trim();
        if (type && value) out.push({type, value});
      }
      const fallbacks = Array.isArray(locator.fallbacks) ? locator.fallbacks : [];
      out.push(...this.normalizeLocatorArray(fallbacks));
      return out;
    }
    return [];
  }

  private aliasModelActionName(raw: string): string {
    const lower = String(raw || '').trim().toLowerCase();
    // Models sometimes reuse element kind names (input_text) as action verbs.
    if (lower === 'fill' || lower === 'input' || lower === 'enter_text' ||
        lower === 'input_text' || lower === 'type_text' || lower === 'fill_text' ||
        lower === 'set_text' || lower === 'set_value' || lower === 'enter_text_into' ||
        lower === 'type_into' || lower === 'send_keys' || lower === 'sendkeys') {
      return 'type';
    }
    if (lower === 'click_element' || lower === 'clickelement' ||
        lower === 'left_click' || lower === 'tap') {
      return 'click';
    }
    if (lower === 'goto' || lower === 'open' || lower === 'open_url' ||
        lower === 'navigate_to') {
      return 'navigate';
    }
    if (lower === 'wait_for_element' || lower === 'waitforelement' ||
        lower === 'wait_for' || lower === 'waitfor' || lower === 'pause' ||
        lower === 'sleep' || lower === 'delay') {
      return 'wait';
    }
    if (lower === 'keypress' || lower === 'press_key' || lower === 'press' ||
        lower === 'keydown' || lower === 'key_down') {
      return 'key_press';
    }
    return lower;
  }

  /**
   * Align with dashboard AgentModelContract / model_prompt_contract.js:
   * canonical script steps are {action, params} with params.locators.
   * Also accepts Playwright-ish {type,url,selector}.
   */
  private normalizeGeneratedScriptAction(step: any): any|null {
    if (!step || typeof step !== 'object') {
      return null;
    }
    const action = this.aliasModelActionName(
        String(step.action || step.type || '').trim());
    if (!action) {
      return null;
    }

    const params: any =
        (step.params && typeof step.params === 'object') ? {...step.params} : {};
    if (!params.url && typeof step.url === 'string' && step.url.trim()) {
      params.url = step.url.trim();
    }
    if (params.text == null) {
      if (typeof step.text === 'string') {
        params.text = step.text;
      } else if (typeof step.value === 'string') {
        params.text = step.value;
      }
    }
    for (const key of [
      'selector', 'css', 'cssSelector', 'css_selector', 'xpath', 'timeout',
      'timeout_ms', 'time', 'duration', 'waitType', 'wait_type',
    ]) {
      if (params[key] == null && step[key] != null) {
        params[key] = step[key];
      }
    }

    if (action === 'navigate') {
      const url = String(params.url || '').trim();
      if (!url) {
        return null;
      }
      return {action: 'navigate', params: {url}};
    }

    let locators = this.collectLocatorsFromStep({...step, params});
    if (!locators.length) {
      const selector = String(
          params.selector || params.css || params.cssSelector ||
          params.css_selector || '')
                           .trim();
      if (selector) {
        locators = [{type: 'CSS_SELECTOR', value: selector}];
      } else {
        const xpath = String(params.xpath || '').trim();
        if (xpath) {
          locators = [{type: 'XPATH', value: xpath}];
        }
      }
    }
    delete params.selector;
    delete params.css;
    delete params.cssSelector;
    delete params.css_selector;
    delete params.xpath;

    if (action === 'wait') {
      let waitType =
          String(params.waitType || params.wait_type || '').trim().toLowerCase();
      if (waitType === 'visible' || waitType === 'attached' ||
          waitType === 'present' || waitType === 'selector') {
        waitType = 'element';
      } else if (!waitType) {
        waitType = locators.length ? 'element' : 'time';
      }
      const out: any = {waitType};
      if (waitType === 'time') {
        const raw = Number(params.time ?? params.timeout ?? params.duration ?? 1000);
        out.time = raw > 1000 ? Math.round(raw) : Math.round(raw * (raw <= 120 ? 1000 : 1));
        if (!Number.isFinite(out.time) || out.time <= 0) {
          out.time = 1000;
        }
      } else {
        if (locators.length) {
          out.locators = locators;
        }
        out.timeout = Number(params.timeout ?? params.timeout_ms ?? params.time ?? 5000);
        if (!Number.isFinite(out.timeout) || out.timeout <= 0) {
          out.timeout = 5000;
        }
      }
      return {action: 'wait', params: out};
    }

    if (action === 'type' || action === 'click' || action === 'hover') {
      const out: any = {};
      if (locators.length) {
        out.locators = locators;
      }
      if (action === 'type') {
        const text = String(params.text ?? '').trim();
        if (!text) {
          return null;
        }
        out.text = text;
      }
      if (action === 'click') {
        const clickCountRaw = Number(params.clickCount ?? params.click_count ?? 1);
        out.clickCount = Number.isFinite(clickCountRaw) ?
            Math.max(1, Math.min(3, Math.round(clickCountRaw))) :
            1;
      }
      return {action, params: out};
    }

    if (locators.length) {
      params.locators = locators;
    }
    return {action, params};
  }

  private normalizeGeneratedScriptObject(
      originalUserPrompt: string,
      assistantText: string): string {
    if (!this.userLikelyWantsSimulachromeScript(originalUserPrompt)) {
      return assistantText;
    }
    const parsed = this.parseFirstJsonObjectFromText(assistantText);
    if (!parsed || typeof parsed !== 'object') {
      return assistantText;
    }
    const hasActionsArray =
        Array.isArray((parsed as any).actions) || Array.isArray((parsed as any).script);
    if (!hasActionsArray) {
      return assistantText;
    }
    const rawActions =
        Array.isArray((parsed as any).actions) ? (parsed as any).actions :
                                                 (parsed as any).script;
    const normalizedActions = rawActions
                                  .map((step: any) =>
                                           this.normalizeGeneratedScriptAction(step))
                                  .filter(Boolean);
    if (!normalizedActions.length) {
      return assistantText;
    }
    const name =
        String((parsed as any).name || (parsed as any).title || 'Generated Script')
            .trim() || 'Generated Script';
    const normalizedScript = {
      name,
      actions: normalizedActions,
    };
    return JSON.stringify(normalizedScript, null, 2);
  }

  private buildWireMessages(
      messagesPayload: Array<{role: 'user'|'assistant'|'system', content: string}>,
  ): WireChatMessage[] {
    const wire: WireChatMessage[] =
        messagesPayload.map((m) => ({role: m.role, content: m.content}));
    const pending = this.pendingScreenshot;
    if (!pending || !this.supportsScreenshotAction()) {
      return wire;
    }
    this.pendingScreenshot = null;
    const dataUrl = `data:${pending.mimeType};base64,${pending.base64}`;
    const note = pending.note ||
        ('[Attached viewport screenshot. Click with ' +
         '{"action":"click","coordinate":[x,y]} using CSS pixels; origin ' +
         'top-left (0,0), x right, y down.]');
    for (let i = wire.length - 1; i >= 0; --i) {
      const msg = wire[i];
      if (!msg || msg.role !== 'user') {
        continue;
      }
      const existing = msg.content;
      const text = typeof existing === 'string' ? existing : note;
      wire[i] = {
        role: 'user',
        content: [
          {type: 'text', text: text ? `${text}\n\n${note}` : note},
          {type: 'image_url', image_url: {url: dataUrl}},
        ],
      };
      break;
    }
    return wire;
  }

  private async fetchAssistantText(
      model: string,
      headers: Record<string, string>,
      messagesPayload: Array<{role: 'user'|'assistant'|'system', content: string}>,
  ): Promise<string> {
    const body = {
      model,
      stream: false,
      messages: this.buildWireMessages(messagesPayload),
    };
    try {
      const res = await fetch('/openai-chat', {
        method: 'POST',
        headers,
        body: JSON.stringify(body),
        signal: this.llmAbortController?.signal,
      });
      if (!res.ok) {
        return `Error: ${res.status} ${res.statusText}\n${await res.text()}`;
      }
      const json = await res.json();
      return json?.choices?.[0]?.message?.content ?? JSON.stringify(json);
    } catch (e) {
      const msg = String(e);
      if ((e instanceof DOMException && e.name === 'AbortError') ||
          msg.includes('AbortError')) {
        return '[Canceled]';
      }
      return `Error: ${e}`;
    }
  }

  private async maybeRepairExternalFrameworkResponse(
      originalUserPrompt: string,
      assistantText: string,
      model: string,
      headers: Record<string, string>,
  ): Promise<string> {
    if (this.userExplicitlyRequestedExternalFramework(originalUserPrompt)) {
      return assistantText;
    }
    if (!this.responseLooksLikeExternalFrameworkCode(assistantText)) {
      return assistantText;
    }

    const correctionSystem =
        'Previous reply violated host contract by using external automation frameworks. '
        + 'Re-answer using Simulachrome-native output only.';
    const scriptHint = this.userLikelyWantsSimulachromeScript(originalUserPrompt)
      ? 'Return ONE Simulachrome script JSON object only (no markdown), shape: '
          + '{"name":"...","actions":[{"action":"navigate","params":{"url":"..."}},...]}'
      : 'Return ONE valid host action JSON object only (no markdown).';
    const correctionUser =
        'Re-answer the previous user request using only Simulachrome-native formats. '
        + 'Do NOT mention Selenium/Playwright/Puppeteer/WebDriver, package installs, '
        + 'or external code frameworks. '
        + scriptHint;
    const retryMessages = [
      ...this.messages.map((m) => ({role: m.role, content: m.content})),
      {role: 'assistant' as const, content: assistantText},
      {role: 'system' as const, content: correctionSystem},
      {role: 'user' as const, content: correctionUser},
    ];
    const repaired = await this.fetchAssistantText(model, headers, retryMessages);
    if (repaired && repaired !== '[Canceled]') {
      this.addSystemMessage(
          'Adjusted model response to Simulachrome-native format (external framework output suppressed).');
      return repaired;
    }
    return assistantText;
  }

  private async maybeOptimizePrompt(
      originalPrompt: string,
      model: string,
      headers: Record<string, string>,
      context?: PageContextSummary): Promise<string> {
    if (!this.promptOptimizerEnabled) {
      return originalPrompt;
    }
    const trimmed = String(originalPrompt || '').trim();
    if (!trimmed) {
      return originalPrompt;
    }

    const optimizerMessages: Array<{role: 'system'|'user', content: string}> = [
      {
        role: 'system',
        content:
            'You are a prompt optimizer for the Simulachrome Glic client. '
            + 'Rewrite the user request into a clearer, tighter prompt for the acting model. '
            + 'Preserve user intent, URLs, credentials placeholders, and explicit constraints exactly. '
            + 'Return JSON only with shape '
            + '{"prompt":"...","preferred_profile":"browser_operator|script_builder|script_debugger|auto_heal","reason":"..."}. '
            + 'Do not add tools, markdown, or commentary.',
      },
      {
        role: 'user',
        content: JSON.stringify({
          prompt: trimmed,
          include_page_context: this.attachContextForManualPrompts,
          current_profile: this.unifiedProfile,
          current_url: context?.url || undefined,
          current_title: context?.title || undefined,
        }),
      },
    ];
    const raw = await this.fetchAssistantText(model, headers, optimizerMessages);
    const parsed = this.parseFirstJsonObjectFromText(raw);
    if (!parsed || typeof parsed !== 'object') {
      this.addSystemMessage(
          'Prompt optimizer returned invalid output; using the original prompt.');
      return originalPrompt;
    }
    const optimizedPrompt = String((parsed as any).prompt || '').trim();
    const preferredProfile = this.normalizeUnifiedProfile(
        (parsed as any).preferred_profile || '');
    if (preferredProfile && preferredProfile !== this.unifiedProfile) {
      this.setUnifiedProfile(preferredProfile, 'prompt_optimizer',
                             {persist: false, emitMessage: false});
      this.addSystemMessage(`Prompt optimizer routed to ${preferredProfile}.`);
    }
    if (optimizedPrompt && optimizedPrompt !== trimmed) {
      this.addSystemMessage('Prompt optimizer refined the outgoing prompt.');
      return optimizedPrompt;
    }
    return originalPrompt;
  }

  private maybeEmitContextEvidence(
      options?: {context?: PageContextSummary, handleActions?: boolean}) {
    const hasLiveTaskContext =
        !!(this.currentTask && options?.handleActions && options?.context);
    const hasManualAttachedContext =
        !!(!hasLiveTaskContext && options?.context);
    const nextKey = hasLiveTaskContext ? 'task_loop_context' :
        hasManualAttachedContext ? 'manual_context' :
                                   'chat_only_context';
    if (this.lastContextEvidenceKey === nextKey) {
      return;
    }
    this.lastContextEvidenceKey = nextKey;

    if (hasManualAttachedContext) {
      const context = options?.context;
      const elementCount = context?.elements?.length ?? 0;
      const url = String(context?.url || '').trim();
      const urlClause = url ? ` url=${url}` : '';
      const error = String(this.lastContextError || '').trim();
      const errorClause = error ? ` error=${error}` : '';
      this.addSystemMessage(
          `[Simulachrome Context] Manual chat prompt with live page context attached (${elementCount} candidate elements.${urlClause}${errorClause})`);
      return;
    }

    if (!hasLiveTaskContext) {
      this.addSystemMessage(
          '[Simulachrome Context] Chat-only prompt. No live tab/page context snapshot is attached to this turn.');
      return;
    }

    const context = options?.context;
    const elementCount = context?.elements?.length ?? 0;
    const url = String(context?.url || '').trim();
    const urlClause = url ? ` url=${url}` : '';
    this.addSystemMessage(
        `[Simulachrome Context] Task-loop prompt with live page context attached (${elementCount} candidate elements.${urlClause}${this.getAuthoritativeTaskTabId() ? ` tab=${this.getAuthoritativeTaskTabId()}` : ''}${this.currentTaskStepIndex ? ` step=${this.currentTaskStepIndex}` : ''})`);
  }

  private updateLlmUiState() {
    if (this.sendBtnEl) {
      this.sendBtnEl.disabled = this.llmRequestInFlight;
    }
    if (this.stopBtnEl) {
      // Allow Stop whenever a task loop can still spend tokens/actions.
      this.stopBtnEl.disabled =
          !(this.llmRequestInFlight || this.taskLoopRunning || !!this.currentTask);
    }
  }

  private cancelModelRequest(reason: string): boolean {
    if (!this.llmRequestInFlight || !this.llmAbortController) {
      return false;
    }
    this.llmAbortController.abort();
    this.addSystemMessage(`Canceled in-flight model request (${reason}).`);
    return true;
  }

  private updateTaskInfo() {
    if (this.taskInfoEl) {
      if (!this.currentTask) {
        this.taskInfoEl.textContent = 'No active task.';
      } else {
        const {url, goal, username} = this.currentTask;
        const userText = username ? ` | user: ${username}` : '';
        const stepText =
            this.currentTaskStepIndex > 0 ? ` | step ${this.currentTaskStepIndex}` : '';
        const tabText = this.getAuthoritativeTaskTabId() ?
            ` | tab ${this.getAuthoritativeTaskTabId()}` :
            '';
        this.taskInfoEl.textContent =
            `Task: ${goal} | ${url}${userText}${stepText}${tabText}`;
      }
    }
    this.renderQueueWidget();
  }

  private nextTaskId(): string {
    const stamp = Date.now();
    this.taskCounter += 1;
    return `task-${stamp}-${this.taskCounter}`;
  }

  private loadTasks(csvText: string) {
    const parsed = parseTasksFromCsv(csvText);
    this.taskQueue = parsed.map((task) => ({
      id: this.nextTaskId(),
      ...task,
    }));
    this.updateQueueStatus();
    this.renderQueueWidget();
  }

  private async startQueue() {
    this.taskLoopStopRequested = false;
    this.queueRunning = true;
    this.updateQueueStatus();
    // Resume a paused-but-still-present task loop if needed.
    if (this.currentTask && !this.taskLoopRunning) {
      this.addSystemMessage('Resuming agent task loop.');
      void this.runTaskLoop();
      return;
    }
    if (!this.currentTask) {
      this.scheduleNextTaskStart();
    }
  }

  private pauseQueue(reason: string = 'queue paused') {
    this.taskLoopStopRequested = true;
    this.queueRunning = false;
    this.clearPendingTaskStart();
    const canceledLlm = this.cancelModelRequest(reason);
    void this.abortActiveTooling(reason);
    this.updateQueueStatus();
    this.updateTaskInfo();
    this.addSystemMessage(
        canceledLlm ?
            `Paused: canceled in-flight model request and stopping the agent loop (${reason}).` :
            `Paused: stopping the agent loop (${reason}).`);
    // If the loop isn't currently on-stack (stuck between turns / already
    // exited), clear the active task immediately so it can't keep spending.
    if (this.currentTask && !this.taskLoopRunning) {
      void this.finishTask('paused', reason);
    }
  }

  private async abortActiveTooling(reason: string): Promise<void> {
    if (this.unifiedRunId && this.browser?.cancelAgentRun) {
      try {
        await this.browser.cancelAgentRun(this.unifiedRunId);
      } catch (_) {
      }
    }
    this.unifiedRunId = null;
    if (this.actorTaskId && this.browser?.stopActorTask) {
      try {
        this.browser.stopActorTask(this.actorTaskId);
      } catch (_) {
      }
    }
    this.actorTaskId = null;
    void reason;
  }

  private async skipToNextTask() {
    this.taskLoopStopRequested = true;
    this.cancelModelRequest('skip to next task');
    void this.abortActiveTooling('skip to next task');
    this.clearPendingTaskStart();
    if (this.currentTask) {
      // Drop the active task; runTaskLoop will observe null/stop and exit.
      const skipped = this.currentTask;
      this.currentTask = null;
      this.currentTaskStepIndex = 0;
      this.clearTaskTabBinding();
      this.clearWorkingContext();
      this.updateTaskInfo();
      await this.logTaskEnd(skipped, 'skipped');
    } else {
      const skipped = this.taskQueue.shift();
      if (skipped) {
        await this.logTaskEnd(skipped, 'skipped');
      }
      this.updateQueueStatus();
    }
    if (this.queueRunning) {
      this.scheduleNextTaskStart();
    }
  }

  private clearPendingTaskStart() {
    if (this.pendingTaskStartTimer === null) {
      return;
    }
    clearTimeout(this.pendingTaskStartTimer);
    this.pendingTaskStartTimer = null;
  }

  private scheduleNextTaskStart() {
    if (!this.queueRunning || this.currentTask) {
      return;
    }
    this.clearPendingTaskStart();
    this.pendingTaskStartTimer = window.setTimeout(() => {
      this.pendingTaskStartTimer = null;
      void this.startNextTask();
    }, this.taskStartDelayMs);
  }

  private async startNextTask() {
    this.clearPendingTaskStart();
    if (this.currentTask) {
      return;
    }
    const next = this.taskQueue.shift();
    if (!next) {
      this.queueRunning = false;
      this.updateQueueStatus();
      this.updateTaskInfo();
      return;
    }
    this.currentTask = next;
    this.unifiedProfileHopUsed = false;
    this.lastHeadlessInvestigationSummary = '';
    const configuredProfile = this.resolveUnifiedProfileFromConfiguration();
    this.setUnifiedProfile(configuredProfile.profile,
                           configuredProfile.source,
                           {emitMessage: false});
    this.actorTaskId = null;
    this.unifiedRunId = null;
    this.tabReopenAttempts = 0;
    this.warnedExtraMetaKeys = false;
    this.typedUsername = false;
    this.requestContextNextTurn = false;
    this.currentTaskStepIndex = 0;
    this.clearTaskTabBinding();
    this.currentTabId = null;
    this.setWorkingContext(null, next.url, next.goal);
    await this.refreshDashboardDataTokenCatalog();
    await this.logTaskStart(next);
    this.messages = [];
    this.hasCoreAssistantContextPrompt = false;
    this.lastContextEvidenceKey = '';
    this.addTaskSystemPrompt(next);
    this.render();
    if (!this.supportsUnifiedAgentOrchestrator()) {
      await this.ensureActorTask();
    }
    if (this.shouldEnforceHeadlessOnlyForCurrentTask()) {
      this.currentTabId = null;
      this.clearTaskTabBinding();
      this.setWorkingContext(null, next.url, next.goal);
      if (this.supportsDashboardInvestigation()) {
        await this.requestHeadlessInvestigation({url: next.url});
      } else {
        this.addSystemMessage(
            `Headless-only profile ${this.unifiedProfile} selected, but investigateDashboardPage is unavailable in this host.`);
      }
    } else {
      await this.openTaskUrl(next.url, {forceNew: true});
      if (this.currentTaskPrefersHeadlessInvestigation() &&
          this.supportsDashboardInvestigation()) {
        await this.requestHeadlessInvestigation({url: next.url});
      }
    }
    await this.runTaskLoop();
  }

  private getCurrentTabIdInt(): number|undefined {
    const parsed = Number.parseInt(
        String(this.getAuthoritativeTaskTabId() ?? this.currentTabId ?? ''), 10);
    if (Number.isFinite(parsed)) {
      return parsed;
    }
    return undefined;
  }

  private supportsUnifiedAgentOrchestrator(): boolean {
    const capabilities = this.browser?.getHostCapabilities?.();
    return !!this.browser?.executeUnifiedTool &&
        !!capabilities &&
        capabilities.has(HostCapability.UNIFIED_AGENT_ORCHESTRATOR);
  }

  private summarizeUnifiedTraceSummary(traceSummaryJson?: string): string|undefined {
    if (!traceSummaryJson) {
      return undefined;
    }
    try {
      const summary = JSON.parse(traceSummaryJson);
      if (!summary || typeof summary !== 'object') {
        return undefined;
      }
      const failureStage =
          String(summary.failureStage ?? summary.failure_stage ?? '').trim();
      const errorClass =
          String(summary.errorClass ?? summary.error_class ?? '').trim();
      const retryCount =
          Number(summary.retryCount ?? summary.retry_count ?? 0);
      const refreshCount =
          Number(summary.contextRefreshCount ?? summary.context_refresh_count ?? 0);
      const bits = [
        failureStage ? `stage=${failureStage}` : '',
        errorClass ? `class=${errorClass}` : '',
        Number.isFinite(retryCount) ? `retries=${retryCount}` : '',
        Number.isFinite(refreshCount) ? `refreshes=${refreshCount}` : '',
      ].filter(Boolean);
      return bits.length ? bits.join(' ') : undefined;
    } catch (_) {
      return undefined;
    }
  }

  private async getUnifiedFailureDiagnostic(
      runId: string|null,
      traceSummaryJson?: string): Promise<string|undefined> {
    const summaryLine = this.summarizeUnifiedTraceSummary(traceSummaryJson);
    if (!runId || !this.browser?.getAgentRunTrace) {
      return summaryLine;
    }
    try {
      const traceJson = await this.browser.getAgentRunTrace(runId);
      if (!traceJson) {
        return summaryLine;
      }
      const trace = JSON.parse(traceJson);
      const results = Array.isArray(trace?.commandResults) ?
        trace.commandResults :
        (Array.isArray(trace?.command_results) ? trace.command_results : []);
      const last = results.length ? results[results.length - 1] : null;
      const detailBits = [
        summaryLine || '',
        String(last?.summary ?? '').trim() ? `summary=${String(last.summary).trim()}` : '',
        String(last?.verificationSummary ?? last?.verification_summary ?? '').trim() ?
          `verify=${String(last.verificationSummary ?? last.verification_summary).trim()}` :
          '',
      ].filter(Boolean);
      return detailBits.length ? detailBits.join(' | ') : summaryLine;
    } catch (_) {
      return summaryLine;
    }
  }

  private async executeUnifiedExecutorTool(
      tool: 'executor.run_action'|'executor.run_timeline',
      payload: any,
      options?: {forceContextRefresh?: boolean}):
      Promise<{ok: boolean, message: string}> {
    if (this.shouldEnforceHeadlessOnlyForCurrentTask()) {
      return {
        ok: false,
        message:
            `Profile ${this.unifiedProfile} is configured headless-only for active tasks. Use investigate_page or delegate_profile to browser_operator.`,
      };
    }
    if (!this.browser?.executeUnifiedTool) {
      return {ok: false, message: 'executeUnifiedTool unavailable in host.'};
    }
    try {
      const result = await this.browser.executeUnifiedTool({
        tool,
        payload,
        runId: this.unifiedRunId ?? undefined,
        goal: this.currentTask?.goal ?? tool,
        profile: this.unifiedProfile,
        tabId: this.currentTabId ?? undefined,
        allowActorFallback: false,
        forceContextRefresh: options?.forceContextRefresh ?? false,
      });
      this.unifiedRunId = result.runId || this.unifiedRunId;
      const turn = result.turnResponse;
      const ok = turn.failureStage === 'none' && !turn.errorClass;
      const diagnostic = ok ? undefined : await this.getUnifiedFailureDiagnostic(
          this.unifiedRunId, turn.traceSummaryJson);
      return {
        ok,
        message: turn.assistantText ||
            (ok ? 'Executor run completed.' :
                  diagnostic || turn.errorClass || 'executor_run_failed'),
      };
    } catch (e) {
      return {ok: false, message: String(e)};
    }
  }

  private async performTabControlAction(action: any, tabId: number):
      Promise<number|undefined> {
    if (this.supportsUnifiedAgentOrchestrator()) {
      void this.logTaskAction('requested', {
        engine: 'unified_executor',
        tab_id: tabId,
        action,
        run_id: this.unifiedRunId,
      });
      const timelineAction = action?.action === 'navigate' ? {
        action: 'navigate',
        params: {url: action.url},
      } : {
        action: String(action?.action || 'unknown'),
        params: action?.params || {},
      };
      const result = await this.executeUnifiedExecutorTool('executor.run_action', {
        action: timelineAction,
        tab_id: tabId,
        wait_for_completion: true,
      });
      await this.logTaskAction(result.ok ? 'result' : 'error', {
        engine: 'unified_executor',
        tab_id: tabId,
        action,
        run_id: this.unifiedRunId,
        message: result.message,
      });
      if (!result.ok) {
        this.addSystemMessage(`Tab action execution failed: ${result.message}`);
        return undefined;
      }
      return ACTION_RESULT.OK;
    }
    if (!this.browser?.performActions) {
      return undefined;
    }
    if (!this.actorTaskId) {
      return undefined;
    }
    const payload = {
      task_id: this.actorTaskId,
      tab_id: tabId,
      action,
    };
    void this.logTaskAction('requested', payload);
    try {
      const res = await fetch('/task/build_action', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload),
      });
      if (!res.ok) {
        this.addSystemMessage(`Tab action build failed: ${res.status} ${res.statusText}`);
        return undefined;
      }
      const data = await res.json();
      const base64 = data?.base64;
      if (typeof base64 !== 'string') {
        this.addSystemMessage('Tab action build returned empty payload.');
        return undefined;
      }
      const bytes = decodeBase64ToBytes(base64);
      const resultBytes = await this.browser.performActions(bytes.buffer as ArrayBuffer);
      const resultArray = new Uint8Array(resultBytes as ArrayBuffer);
      const logged = await this.logTaskAction('result', {
        task_id: this.actorTaskId,
        tab_id: tabId,
        action,
        base64: encodeBytesToBase64(resultArray),
      });
      if (typeof logged?.action_result_code === 'number') {
        return logged.action_result_code;
      }
      return ACTION_RESULT.OK;
    } catch (e) {
      await this.logTaskAction('error', {
        task_id: this.actorTaskId,
        tab_id: tabId,
        action,
        error: String(e),
      });
      this.addSystemMessage(`Tab action execution failed: ${e}`);
      return undefined;
    }
  }

  private async navigateCurrentTab(url: string): Promise<boolean> {
    const authoritativeTabId = this.getAuthoritativeTaskTabId();
    if (!authoritativeTabId) {
      return false;
    }
    const tabId = this.getCurrentTabIdInt();
    if (!Number.isFinite(tabId)) {
      return false;
    }
    if (!this.supportsUnifiedAgentOrchestrator() && !this.actorTaskId) {
      await this.ensureActorTask();
    }
    if (!this.supportsUnifiedAgentOrchestrator() && !this.actorTaskId) {
      return false;
    }
    this.browser?.activateTab?.(authoritativeTabId);
    const code = await this.performTabControlAction(
        {action: 'navigate', url},
        tabId as number);
    if (typeof code !== 'number' || code === ACTION_RESULT.OK) {
      this.bindTaskTab(authoritativeTabId, url, this.currentTask?.goal ?? this.workingTabTitle);
      await sleep(1500);
      return true;
    }
    if (code === ACTION_RESULT.TAB_WENT_AWAY) {
      this.currentTabId = null;
      this.clearTaskTabBinding();
    }
    this.addSystemMessage(`Tab reuse navigate failed: ${this.describeActionResult(code)}.`);
    return false;
  }

  private async openTaskUrl(url: string, options?: {forceNew?: boolean}) {
    if (!options?.forceNew && this.getAuthoritativeTaskTabId()) {
      if (await this.navigateCurrentTab(url)) {
        this.bindTaskTab(
            this.getAuthoritativeTaskTabId(),
            url,
            this.currentTask?.goal ?? null);
        return;
      }
      this.addSystemMessage('Opening a fresh tab because tab reuse failed.');
    }
    if (this.browser?.createTab) {
      try {
        const tabData = await this.browser.createTab(url, {openInBackground: false});
        if (tabData?.tabId) {
          this.bindTaskTab(tabData.tabId, url, this.currentTask?.goal ?? null);
        } else {
          this.setWorkingContext(
              this.getAuthoritativeTaskTabId(), url, this.currentTask?.goal ?? null);
        }
        await new Promise((resolve) => setTimeout(resolve, 2000));
        return;
      } catch (e) {
        this.messages.push({role: 'assistant', content: `Failed to open ${url}: ${e}`});
      }
    }
    this.setWorkingContext(this.getAuthoritativeTaskTabId(), url, this.currentTask?.goal ?? null);
    window.open(url, '_blank');
  }

  private addTaskSystemPrompt(task: TaskItem) {
    const lines: string[] = [];
    lines.push('You are working on a task.');
    lines.push(
        'PRIORITY: The USER GOAL below is the highest-priority instruction. ' +
        'It overrides default habits and procedural tips in this prompt and in ' +
        'other system messages. Hard format rules (integer target_id, JSON shapes) ' +
        'still apply. If a default conflicts with the goal, follow the goal.');
    lines.push(`USER GOAL: ${task.goal}`);
    lines.push(`URL: ${task.url}`);
    lines.push(`Current unified profile: ${this.unifiedProfile}.`);
    lines.push(
        'This task owns a dedicated working tab for click/type/navigate. Stay bound to that task tab for actuation; use list_tabs/peek_tab/request_context(tab_id, reload) to read helper tabs (e.g. Gmail for 2FA) without opening fresh tabs.');
    lines.push(
        'If the live task context URL is about:blank, chrome://, a New Tab page, or any host that is not the task destination, your FIRST action must be navigate to the task URL (or the goal destination).');
    lines.push(
        'Do not click New Tab shortcuts or chrome:// UI to reach a site — use {"action":"navigate","url":"https://..."}.');
    lines.push(
        'After you are already on the correct site, do not navigate again just because the original task URL is known. Only navigate when the live context shows the wrong page or the task explicitly requires a new destination.');
    if (task.username) {
      lines.push(
          `Username value (literal or token placeholder; preserve exactly): ${
            task.username}`);
    }
    if (task.password) {
      lines.push(
          `Password value (literal or token placeholder; preserve exactly): ${
            task.password}`);
    }
    const identityTokens = this.listIdentityVariableTokens();
    const passwordTokens = this.listPasswordSecretTokens();
    lines.push(
        'CREDENTIAL TYPING RULES (hard constraints for every site):');
    lines.push(
        '1) Email/username/login fields: text MUST be a Dashboard VARIABLE token in {TokenName} form. ' +
        'Never type field labels/placeholders or bare names.');
    lines.push(
        '2) Password fields: text MUST be a Dashboard SECRET token in {TokenName} form. ' +
        'Never type the word Password or plaintext secrets.');
    lines.push(
        '3) Exact format: {TokenName} with ONE pair of braces. Never {{TokenName}}. ' +
        'Executor resolves tokens at run time — do not invent values.');
    lines.push(
        '4) Choose from the token lists below (prefer any token named in the USER GOAL). ' +
        'Labels only identify which field to target.');
    lines.push(
        identityTokens.length ?
            `Variable tokens for email/username: ${
                identityTokens.map((n) => `{${n}}`).join(', ')}` :
            'Variable tokens for email/username: (none loaded from Data > Variables).');
    lines.push(
        passwordTokens.length ?
            `Secret tokens for password fields: ${
                passwordTokens.map((n) => `{${n}}`).join(', ')}` :
            'Secret tokens for password fields: (none loaded from Data > Secrets).');
    lines.push(
        'Do not ask the user to paste secrets in plaintext if a {token} can be used.');
    lines.push('Use the integer element ids from the current Page context when selecting elements.');
    lines.push('IMPORTANT: target_id MUST be an integer taken from the current Page context elements[].id list.');
    lines.push('Do NOT invent ids (e.g. "password_field"). Do NOT use strings. Any non-integer or unknown id will be rejected.');
    lines.push(
        'For typing, prefer form_type FORM_CONTROL_TYPE_INPUT_* (or a matching placeholder/field_name).');
    lines.push(
        'A type action text field may be either literal text or a {token} placeholder.');
    const knownTokenNames =
        this.dashboardDataTokenCatalog?.tokenNames?.slice(0, 48) ?? [];
    if (knownTokenNames.length) {
      const rendered = knownTokenNames.map((name) => `{${name}}`).join(', ');
      const more =
          (this.dashboardDataTokenCatalog?.tokenNames?.length ?? 0) >
          knownTokenNames.length;
      lines.push(
          `Known dashboard token placeholders: ${rendered}${more ? ' ...' : ''}`);
    }
    const secretTokenNames =
        this.dashboardDataTokenCatalog?.secretTokenNames?.slice(0, 24) ?? [];
    if (secretTokenNames.length) {
      const rendered = secretTokenNames.map((name) => `{${name}}`).join(', ');
      const more =
          (this.dashboardDataTokenCatalog?.secretTokenNames?.length ?? 0) >
          secretTokenNames.length;
      lines.push(
          `Secret-backed token names include: ${rendered}${more ? ' ...' : ''}`);
    }
    lines.push(
        'DEFAULT habits (lower priority than USER GOAL — skip or invert when the goal says otherwise):');
    lines.push(
        '- Login: if fields look empty, type username/email then password then Enter/Log In; if already filled or the goal says not to type, just submit.');
    lines.push(
        '- Search: after typing a query, prefer Enter over clicking a search button.');
    lines.push(
        '- Prefer acting with current element ids; request_context when UI changed (incl. same-URL popup/modal) or needed ids are missing.');
    lines.push(
        '- Canvas/image popups often omit buttons from elements[]; after request_screenshot, click with coordinate [x,y] instead of looping request_context.');
    lines.push('Do NOT include action results (e.g., success=true/false, status, reason) in click/hover/type/scroll/key_press JSON.');
    lines.push('Respond with a single JSON object only, using EXACTLY one of the following shapes (no extra keys):');
    lines.push('{"action":"navigate","url":"https://example.com/"}');
    lines.push('{"action":"click","target_id":123}');
    lines.push('{"action":"click","coordinate":[x,y]}');
    lines.push('{"action":"hover","target_id":123}');
    lines.push('{"action":"type","target_id":123,"text":"..."}');
    lines.push('{"action":"type","coordinate":[x,y],"text":"..."}');
    lines.push(
        'Do not use element kind names as actions (input_text/input_email). Typing is always {"action":"type",...}.');
    lines.push('{"action":"scroll","target_id":123,"distance":600}');
    lines.push('{"action":"key_press","key":"Enter"}');
    lines.push('{"action":"complete"}');
    lines.push('{"action":"complete","success":false,"reason":"..."}');
    lines.push('{"action":"request_context"}');
    lines.push('{"action":"request_context","tab_id":"<id>","reload":true}');
    lines.push('{"action":"list_tabs"}');
    lines.push('{"action":"list_tabs","query":"gmail"}');
    lines.push('{"action":"peek_tab","tab_id":"<id>"}');
    lines.push('{"action":"peek_tab","url_host":"mail.google.com","reload":true}');
    lines.push('{"action":"refresh_tab","tab_id":"<id>"}');
    lines.push('{"action":"request_console_logs","level":"error","limit":20}');
    lines.push('{"action":"delegate_profile","target_profile":"browser_operator|script_builder|script_debugger|auto_heal"}');
    if (this.supportsScreenshotAction()) {
      lines.push('{"action":"request_screenshot"}');
    }
    if (this.supportsDashboardInvestigation()) {
      lines.push('{"action":"investigate_page"}');
      lines.push('{"action":"investigate_page","url":"https://example.com/"}');
      lines.push(
          '{"action":"investigate_page","url":"https://example.com/","closeTarget":false}');
      lines.push(
          '{"action":"investigate_page","task_id":"<existing_task_id>","closeTarget":false}');
    }
    lines.push(
        'For 2FA/email codes in another open tab: list_tabs → peek_tab/request_context on that tab. Use reload:true (or refresh_tab) when the inbox is inactive and may not have fetched yet. Then type the code on the task tab.');
    lines.push(
        'peek_tab / cross-tab request_context do NOT move the task binding; click/type still target the authoritative task tab.');
    lines.push('Use {"action":"request_console_logs"} when page warnings/errors may explain a failure.');
    if (this.supportsScreenshotAction()) {
      lines.push(
          'If a click/type does not produce a noticeable page change, use {"action":"request_screenshot"} to inspect the viewport (overlays/popups APC may miss).');
      lines.push(
          'For screenshot-visible controls absent from elements[], use {"action":"click","coordinate":[x,y]} reading the cyan CSS-pixel grid on the screenshot (1 image px ≈ 1 CSS px).');
    }
    lines.push('Use {"action":"delegate_profile",...} only when another unified profile is clearly better for the next step. Only one switch is allowed per task.');
    if (this.supportsDashboardInvestigation()) {
      lines.push(
          'Use {"action":"investigate_page"} when you need independent headless page inspection that should not interfere with the user tab.');
      lines.push(
          'Use closeTarget:false to keep the same headless task alive across turns, and pass task_id to continue on that same headless tab.');
      lines.push(
          'You may include actions:[...] in investigate_page to run Simulachrome timeline steps inside that headless task.');
      if (this.currentTaskPrefersHeadlessInvestigation()) {
        lines.push(
            'This task is a debugging/healing style task. Prefer investigate_page before interacting with the user-visible tab.');
      }
    }
    lines.push(
        'When finished, use the same JSON action shape as other steps: {"action":"complete"} ' +
        '(same field name as click/type; aliases mark_task_complete/task_complete/done also accepted).');
    lines.push(
        'Optional: {"action":"complete","reason":"..."}. On failure: {"action":"complete","success":false,"reason":"..."}.');
    if (looksLikeTokenPlaceholder(task.username) ||
        looksLikeTokenPlaceholder(task.password)) {
      lines.push(
          'This task already includes credential token placeholders. Use them ' +
          'exactly as provided; do not rewrite or resolve them yourself.');
    }
    lines.push('Do not include any other text.');
    this.messages.push({role: 'system', content: lines.join('\n')});
  }

  private onFocusedTabStateChanged(state: FocusedTabData|undefined): void {
    if (!state) {
      this.focusedTabId = null;
      this.candidateTabId = null;
      return;
    }
    if (state.hasFocus?.tabData?.tabId) {
      this.focusedTabId = state.hasFocus.tabData.tabId;
      this.candidateTabId = null;
      return;
    }
    this.focusedTabId = null;
    this.candidateTabId = state.hasNoFocus?.tabFocusCandidateData?.tabId || null;
  }

  private getAuthoritativeTaskTabId(): string|null {
    const taskBound = String(this.taskTabId || '').trim();
    if (this.currentTask && taskBound) {
      return taskBound;
    }
    if (this.currentTask) {
      return null;
    }
    const current = String(this.currentTabId || '').trim();
    if (current) {
      return current;
    }
    const working = String(this.workingTabId || '').trim();
    if (working) {
      return working;
    }
    return null;
  }

  private bindTaskTab(tabId: string|null, url?: string|null, title?: string|null): void {
    const trimmed = String(tabId || '').trim();
    if (trimmed) {
      this.taskTabId = trimmed;
      this.currentTabId = trimmed;
    }
    this.setWorkingContext(
        this.taskTabId || this.currentTabId,
        url ?? this.workingTabUrl ?? null,
        title ?? this.workingTabTitle ?? this.currentTask?.goal ?? null);
  }

  private clearTaskTabBinding(): void {
    this.taskTabId = null;
  }

  private getContextTabCandidates(): string[] {
    const ordered: Array<string|null> =
        (this.currentTask && !this.shouldEnforceHeadlessOnlyForCurrentTask())
        ? [
            this.taskTabId,
          ]
        : this.contextLockMode === 'follow_active_tab'
        ? [
            this.focusedTabId,
            this.candidateTabId,
            this.currentTabId,
            this.workingTabId,
          ]
        : [
            this.currentTabId,
            this.workingTabId,
            this.focusedTabId,
            this.candidateTabId,
          ];
    const seen = new Set<string>();
    const out: string[] = [];
    for (const raw of ordered) {
      const tabId = String(raw || '').trim();
      if (!tabId || seen.has(tabId)) {
        continue;
      }
      seen.add(tabId);
      out.push(tabId);
    }
    return out;
  }

  private async ensureActorTask() {
    if (this.supportsUnifiedAgentOrchestrator()) {
      return;
    }
    if (this.actorTaskId) {
      return;
    }
    if (!this.browser?.createTask) {
      if (!this.warnedNoActorTask) {
        this.addSystemMessage('Actor tasks are unavailable in this host.');
        this.warnedNoActorTask = true;
      }
      return;
    }
    try {
      const taskId = await this.browser.createTask({title: this.currentTask?.goal});
      if (!taskId) {
        this.addSystemMessage('Actor task creation returned empty id.');
        return;
      }
      this.actorTaskId = taskId;
    } catch (e) {
      this.addSystemMessage(`Actor task creation failed: ${e}`);
    }
  }

  private async logTaskStart(task: TaskItem) {
    try {
      await fetch('/task/start', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(task),
      });
    } catch (_) {
      return;
    }
  }

  private async logTaskEnd(task: TaskItem, status: string, reason?: string) {
    try {
      await fetch('/task/end', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          task_id: task.id,
          status,
          reason,
        }),
      });
    } catch (_) {
      return;
    }
  }

  private maybeCompleteTask(responseText: string) {
    const parsed = extractJsonObjects(responseText);
    for (const obj of parsed) {
      if (!obj || typeof obj !== 'object') {
        continue;
      }
      const verb = String(
                       obj.action ?? obj.action_type ?? obj.actionType ??
                       obj.type ?? obj.name ?? '')
                       .toLowerCase()
                       .trim();
      if (verb === 'complete' || verb === 'task_complete' ||
          verb === 'mark_task_complete' || verb === 'mark_complete' ||
          verb === 'task_done' || verb === 'finish_task' || verb === 'done') {
        const success = obj.success !== false;
        const reason = typeof obj.reason === 'string' ?
            obj.reason :
            (typeof obj.message === 'string' ? obj.message : undefined);
        void this.finishTask(success ? 'success' : 'failure', reason);
        return true;
      }
    }
    return false;
  }

  private async finishTask(status: string, reason?: string) {
    if (!this.currentTask) {
      return;
    }
    this.taskLoopStopRequested = true;
    const completed = this.currentTask;
    this.currentTask = null;
    this.currentTaskStepIndex = 0;
    this.clearTaskTabBinding();
    this.clearWorkingContext();
    this.updateTaskInfo();
    await this.logTaskEnd(completed, status, reason);
    await this.flushRecorderScaffoldForTask(status, completed);
    if (status === 'success') {
      this.addSystemMessage(
          reason ? `Task complete: ${reason}` : 'Task marked complete.');
    }
    if (this.unifiedRunId && this.browser?.cancelAgentRun) {
      try {
        await this.browser.cancelAgentRun(this.unifiedRunId);
      } catch (_) {}
    }
    this.unifiedRunId = null;
    if (this.actorTaskId && this.browser?.stopActorTask) {
      this.browser.stopActorTask(this.actorTaskId);
    }
    this.actorTaskId = null;
    if (this.queueRunning) {
      this.scheduleNextTaskStart();
    }
  }

  private toggleQueueWidget(forceOpen?: boolean) {
    if (!this.queueWidgetPanelEl) {
      return;
    }
    const shouldOpen = forceOpen ?? this.queueWidgetPanelEl.hidden;
    this.queueWidgetPanelEl.hidden = !shouldOpen;
    if (shouldOpen) {
      this.renderQueueWidget();
    }
  }

  private clearQueuedTasks() {
    this.taskQueue = [];
    this.updateQueueStatus();
  }

  private moveQueuedTask(index: number, direction: -1|1) {
    const target = index + direction;
    if (index < 0 || index >= this.taskQueue.length ||
        target < 0 || target >= this.taskQueue.length) {
      return;
    }
    const [task] = this.taskQueue.splice(index, 1);
    if (!task) {
      return;
    }
    this.taskQueue.splice(target, 0, task);
    this.updateQueueStatus();
  }

  private removeQueuedTask(index: number) {
    if (index < 0 || index >= this.taskQueue.length) {
      return;
    }
    this.taskQueue.splice(index, 1);
    this.updateQueueStatus();
  }

  private prioritizeQueuedTask(index: number) {
    if (index < 0 || index >= this.taskQueue.length) {
      return;
    }
    const [task] = this.taskQueue.splice(index, 1);
    if (!task) {
      return;
    }
    this.taskQueue.unshift(task);
    this.updateQueueStatus();
    if (this.queueRunning && !this.currentTask) {
      this.scheduleNextTaskStart();
    }
  }

  private setWorkingContext(tabId: string|null, url: string|null, title?: string|null) {
    this.workingTabId = tabId;
    this.workingTabUrl = url;
    this.workingTabTitle = title ?? null;
    this.renderQueueWidget();
  }

  private clearWorkingContext() {
    this.setWorkingContext(null, null, null);
  }

  private renderQueueWidget() {
    const totalCount = this.taskQueue.length + (this.currentTask ? 1 : 0);
    if (this.queueWidgetToggleEl) {
      this.queueWidgetToggleEl.dataset['hasItems'] =
          totalCount > 0 ? 'true' : 'false';
      this.queueWidgetToggleEl.dataset['queueCount'] = String(totalCount);
      this.queueWidgetToggleEl.textContent =
          totalCount > 0 ? `Queue (${totalCount})` : 'Queue';
    }

    if (this.queueWidgetSubtitleEl) {
      if (this.currentTask) {
        const state = this.queueRunning ? 'running' : 'paused';
        this.queueWidgetSubtitleEl.textContent =
            `${state} · ${this.taskQueue.length} waiting`;
      } else if (this.taskQueue.length > 0) {
        const state = this.queueRunning ? 'armed' : 'paused';
        this.queueWidgetSubtitleEl.textContent =
            `${this.taskQueue.length} queued · ${state}`;
      } else {
        this.queueWidgetSubtitleEl.textContent = 'No queued work.';
      }
    }

    if (this.queueWorkingContextEl) {
      if (this.workingTabUrl) {
        const title = this.workingTabTitle ? `${this.workingTabTitle} · ` : '';
        const tab = this.workingTabId ? ` · tab ${this.workingTabId}` : '';
        this.queueWorkingContextEl.textContent =
            `${title}${this.workingTabUrl}${tab} · ${this.contextLockMode}`;
      } else {
        this.queueWorkingContextEl.textContent = 'No locked task context.';
      }
    }

    if (this.queueActiveTaskEl) {
      if (!this.currentTask) {
        this.queueActiveTaskEl.textContent = 'No active task.';
      } else {
        const user = this.currentTask.username ? ` · ${this.currentTask.username}` : '';
        this.queueActiveTaskEl.textContent =
            `${this.currentTask.goal} · ${this.currentTask.url}${user}`;
      }
    }

    if (!this.queueWidgetListEl) {
      return;
    }
    this.queueWidgetListEl.textContent = '';
    if (!this.taskQueue.length) {
      const empty = document.createElement('div');
      empty.className = 'queue-empty';
      empty.textContent = 'Nothing waiting in the queue.';
      this.queueWidgetListEl.appendChild(empty);
      return;
    }

    this.taskQueue.forEach((task, index) => {
      const card = document.createElement('div');
      card.className = 'queue-item';

      const top = document.createElement('div');
      top.className = 'queue-item-top';

      const text = document.createElement('div');
      const title = document.createElement('div');
      title.className = 'queue-widget-title';
      title.textContent = task.goal;
      const meta = document.createElement('div');
      meta.className = 'queue-widget-meta';
      meta.textContent = task.url;
      text.appendChild(title);
      text.appendChild(meta);

      const position = document.createElement('div');
      position.className = 'queue-widget-meta';
      position.textContent = `#${index + 1}`;

      top.appendChild(text);
      top.appendChild(position);
      card.appendChild(top);

      const actions = document.createElement('div');
      actions.className = 'queue-item-actions';
      actions.appendChild(this.createQueueButton('Run Next', () => {
        this.prioritizeQueuedTask(index);
      }));
      actions.appendChild(this.createQueueButton('Up', () => {
        this.moveQueuedTask(index, -1);
      }));
      actions.appendChild(this.createQueueButton('Down', () => {
        this.moveQueuedTask(index, 1);
      }));
      actions.appendChild(this.createQueueButton('Remove', () => {
        this.removeQueuedTask(index);
      }));
      card.appendChild(actions);
      this.queueWidgetListEl?.appendChild(card);
    });
  }

  private createQueueButton(label: string, handler: () => void): HTMLButtonElement {
    const button = document.createElement('button');
    button.textContent = label;
    button.addEventListener('click', handler);
    return button;
  }

  private trimMessages() {
    if (this.messages.length <= 12) {
      return;
    }
    const systemMessages = this.messages.filter((msg) => msg.role === 'system');
    const tail = this.messages.slice(-8);
    this.messages = [...systemMessages.slice(0, 2), ...tail];
  }

  private summarizeTabData(tab: TabData): {
    tabId: string,
    title: string,
    url: string,
    isActiveInWindow?: boolean,
    isWindowActive?: boolean,
  } {
    return {
      tabId: String(tab.tabId || '').trim(),
      title: String(tab.title || '').trim(),
      url: String(tab.url || '').trim(),
      isActiveInWindow: tab.isActiveInWindow,
      isWindowActive: tab.isWindowActive,
    };
  }

  private async listBrowserTabs(query?: string): Promise<Array<{
    tabId: string,
    title: string,
    url: string,
    isActiveInWindow?: boolean,
    isWindowActive?: boolean,
    source: string,
  }>> {
    const q = String(query || '').trim().toLowerCase();
    const byId = new Map<string, {
      tabId: string,
      title: string,
      url: string,
      isActiveInWindow?: boolean,
      isWindowActive?: boolean,
      source: string,
    }>();
    const upsert = (tab: TabData|undefined|null, source: string) => {
      if (!tab?.tabId) {
        return;
      }
      const summary = this.summarizeTabData(tab);
      if (!summary.tabId) {
        return;
      }
      if (q) {
        const hay = `${summary.title} ${summary.url}`.toLowerCase();
        if (!hay.includes(q)) {
          return;
        }
      }
      const existing = byId.get(summary.tabId);
      if (!existing) {
        byId.set(summary.tabId, {...summary, source});
        return;
      }
      // Prefer richer titles/urls when merging sources.
      if (!existing.title && summary.title) {
        existing.title = summary.title;
      }
      if (!existing.url && summary.url) {
        existing.url = summary.url;
      }
      if (!existing.source.includes(source)) {
        existing.source = `${existing.source}+${source}`;
      }
    };

    const taskTabId = this.getAuthoritativeTaskTabId();
    if (taskTabId) {
      try {
        const obs = this.browser?.getTabById?.(taskTabId);
        const tab = obs?.getCurrentValue?.();
        if (tab) {
          upsert(tab, 'task');
        } else {
          byId.set(taskTabId, {
            tabId: taskTabId,
            title: String(this.workingTabTitle || this.currentTask?.goal || ''),
            url: String(this.workingTabUrl || this.currentTask?.url || ''),
            source: 'task',
          });
        }
      } catch (_) {
        byId.set(taskTabId, {
          tabId: taskTabId,
          title: String(this.workingTabTitle || this.currentTask?.goal || ''),
          url: String(this.workingTabUrl || this.currentTask?.url || ''),
          source: 'task',
        });
      }
    }

    try {
      const pinnedObs = this.browser?.getPinnedTabs?.();
      const pinned = pinnedObs?.getCurrentValue?.() ?? [];
      for (const tab of pinned) {
        upsert(tab, 'pinned');
      }
    } catch (_) {
    }

    if (this.focusedTabId) {
      try {
        const obs = this.browser?.getTabById?.(this.focusedTabId);
        upsert(obs?.getCurrentValue?.(), 'focused');
      } catch (_) {
      }
    }

    try {
      const candidatesObs = this.browser?.getPinCandidates?.({
        maxCandidates: 20,
        query: query || undefined,
      });
      // First emission may be async; wait briefly for subscribe + current value.
      const waited = await new Promise<PinCandidate[]>((resolve) => {
        let resolved = false;
        const finish = (value: PinCandidate[]) => {
          if (resolved) {
            return;
          }
          resolved = true;
          try {
            sub?.unsubscribe?.();
          } catch (_) {
          }
          resolve(value || []);
        };
        let sub: {unsubscribe?: () => void}|undefined;
        try {
          sub = candidatesObs?.subscribe?.((value) => finish(value || []));
        } catch (_) {
          finish([]);
          return;
        }
        const current = candidatesObs?.getCurrentValue?.();
        if (current && current.length) {
          finish(current);
          return;
        }
        setTimeout(() => finish(candidatesObs?.getCurrentValue?.() || []), 350);
      });
      for (const candidate of waited) {
        upsert(candidate?.tabData, 'candidate');
      }
    } catch (_) {
    }

    return Array.from(byId.values()).sort((a, b) => {
      const aTask = a.tabId === taskTabId ? 0 : 1;
      const bTask = b.tabId === taskTabId ? 0 : 1;
      if (aTask !== bTask) {
        return aTask - bTask;
      }
      return a.tabId.localeCompare(b.tabId);
    });
  }

  private async resolveBrowserTabId(spec: {
    tab_id?: string,
    url?: string,
    url_host?: string,
    query?: string,
  }): Promise<{tabId: string, title: string, url: string}|null> {
    const directId = String(spec.tab_id || '').trim();
    if (directId) {
      const tabs = await this.listBrowserTabs();
      const hit = tabs.find((t) => t.tabId === directId);
      if (hit) {
        return {tabId: hit.tabId, title: hit.title, url: hit.url};
      }
      return {tabId: directId, title: '', url: ''};
    }
    const urlExact = String(spec.url || '').trim().toLowerCase();
    const host = String(spec.url_host || '').trim().toLowerCase();
    const query = String(spec.query || '').trim().toLowerCase();
    const tabs = await this.listBrowserTabs(query || host || undefined);
    if (!tabs.length) {
      return null;
    }
    const scored = tabs.map((t) => {
      const u = t.url.toLowerCase();
      const title = t.title.toLowerCase();
      let score = 0;
      if (urlExact && (u === urlExact || u.startsWith(urlExact))) {
        score += 100;
      }
      if (host && u.includes(host)) {
        score += 50;
      }
      if (query && (u.includes(query) || title.includes(query))) {
        score += 20;
      }
      if (t.source.includes('pinned')) {
        score += 5;
      }
      return {t, score};
    }).filter((x) => x.score > 0);
    scored.sort((a, b) => b.score - a.score);
    const best = scored[0]?.t;
    if (!best) {
      return null;
    }
    return {tabId: best.tabId, title: best.title, url: best.url};
  }

  private async collectContextFromTabId(
      tabId: string,
      options?: {rebindTaskTab?: boolean}): Promise<PageContextSummary> {
    const summary: PageContextSummary = {elements: []};
    const trimmed = String(tabId || '').trim();
    if (!this.browser || !trimmed) {
      return summary;
    }
    const contextOptions = {
      innerText: true,
      innerTextBytesLimit: 12000,
      viewportScreenshot: false,
      annotatedPageContent: true,
      annotatedPageContentMode: 1,
      maxMetaTags: 0,
    };
    let context: TabContextResult|undefined;
    let lastError = '';
    try {
      if (this.browser.getContextForActorFromTab) {
        context = await this.browser.getContextForActorFromTab(
            trimmed, contextOptions);
      }
    } catch (e) {
      lastError = String(e || '');
    }
    if (!context && this.browser.getContextFromTab) {
      try {
        context = await this.browser.getContextFromTab(trimmed, contextOptions);
      } catch (e) {
        lastError = String(e || '');
      }
    }
    if (!context) {
      if (lastError) {
        this.addSystemMessage(`Peek/context failed for tab ${trimmed}: ${lastError}`);
      }
      return summary;
    }
    summary.tabId = context.tabData.tabId;
    summary.tabIdInt = Number.parseInt(context.tabData.tabId, 10);
    summary.url = context.tabData.url;
    summary.title = context.tabData.title;
    summary.innerText = context.webPageData?.mainDocument?.innerText;
    if (options?.rebindTaskTab) {
      this.bindTaskTab(
          String(summary.tabId || trimmed),
          summary.url ?? null,
          summary.title ?? null);
    }
    const apcStream = context.annotatedPageData?.annotatedPageContent;
    if (apcStream) {
      try {
        const bytes = await readStream(apcStream);
        const buffer = bytes.buffer as ArrayBuffer;
        const res = await fetch('/parse-apc', {
          method: 'POST',
          headers: {'Content-Type': 'application/octet-stream'},
          body: buffer,
        });
        if (res.ok) {
          const apcJson = await res.json();
          summary.elements = extractNodesFromApc(apcJson, 60, summary.url);
          summary.documentId = extractDocumentIdFromApc(apcJson);
          const vpCss = extractViewportCssFromApc(apcJson);
          if (vpCss) {
            summary.viewportCss = vpCss;
          }
        }
      } catch (e) {
        this.addSystemMessage(
            `Peek APC parse failed for tab ${trimmed}: ${e}` +
            (String(e).includes('Failed to fetch')
                 ? ' — local glic serve.py looks down (restart start_glic_dev / proxy on :8000).'
                 : ''));
      }
    }
    summary.elements = (summary.elements ?? []).filter(
        (node) => !shouldSuppressContextNodeForPrompt(node, summary.url));
    // For helper tabs (email), keep more innerText so OTP codes are visible.
    summary.innerText = String(summary.innerText || '').slice(0, 8000);
    return summary;
  }

  private formatPeekedTabContext(summary: PageContextSummary): string {
    const compactElements = (summary.elements ?? []).slice(0, 50).map((el) => ({
      id: el.id,
      label: el.label,
      role: el.role,
      formType: el.formType,
      clickable: !!el.clickable,
      editable: !!el.editable,
    }));
    const payload = {
      peeked: true,
      tabId: summary.tabId,
      tabIdInt: summary.tabIdInt,
      url: summary.url,
      title: summary.title,
      documentId: summary.documentId,
      elementCount: summary.elements?.length ?? 0,
      elements: compactElements,
      innerText: summary.innerText || '',
      note:
          'This is a helper-tab peek. Task actions (click/type/navigate) still target the authoritative task tab unless you explicitly refresh/peek again. Extract values (e.g. 2FA codes) from this snapshot, then act on the task tab.',
    };
    return 'Peeked helper-tab context:\n' + JSON.stringify(payload, null, 2);
  }

  private async reloadBrowserTab(tabId: string): Promise<boolean> {
    const trimmed = String(tabId || '').trim();
    if (!trimmed) {
      return false;
    }
    let url = '';
    try {
      const obs = this.browser?.getTabById?.(trimmed);
      url = String(obs?.getCurrentValue?.()?.url || '').trim();
    } catch (_) {
    }
    if (!url) {
      const tabs = await this.listBrowserTabs();
      url = String(tabs.find((t) => t.tabId === trimmed)?.url || '').trim();
    }
    if (!url || !/^https?:/i.test(url)) {
      this.addSystemMessage(
          `Cannot reload tab ${trimmed}: no http(s) URL available.`);
      return false;
    }
    const tabIdInt = Number.parseInt(trimmed, 10);
    if (!Number.isFinite(tabIdInt)) {
      this.addSystemMessage(`Cannot reload tab ${trimmed}: invalid tab id.`);
      return false;
    }
    // Navigate to the same URL on that tab (works for background tabs without
    // stealing focus from the task tab).
    const code = await this.performTabControlAction(
        {action: 'navigate', url},
        tabIdInt);
    if (typeof code === 'number' && code !== ACTION_RESULT.OK) {
      this.addSystemMessage(
          `Reload/navigate failed for tab ${trimmed}: ${this.describeActionResult(code)}`);
      return false;
    }
    if (code === undefined) {
      return false;
    }
    await sleep(1800);
    return true;
  }

  private async handleCrossTabContextAction(action: any): Promise<{
    handled: boolean,
    requestContext: boolean,
  }> {
    const wantsReload = !!action?.reload;

    if (action?.action === 'list_tabs') {
      const tabs = await this.listBrowserTabs(
          String(action.query || action.url_host || '').trim() || undefined);
      const taskTabId = this.getAuthoritativeTaskTabId();
      const priorIds = this.lastTaskElementIds.slice(0, 80);
      this.addSystemMessage(
          'Open tabs (use tabId with peek_tab / request_context):\n' +
          JSON.stringify({
            taskTabId,
            tabs,
            priorTaskElementIds: priorIds.length ? priorIds : undefined,
            hint:
                'For inactive inboxes (e.g. Gmail), call peek_tab/request_context with reload:true on that tabId before reading codes. Do NOT re-request task-tab context just to recover element ids — priorTaskElementIds remain valid unless the task DOM changed.',
          }, null, 2));
      return {handled: true, requestContext: false};
    }

    const taskTabId = this.getAuthoritativeTaskTabId();
    let resolved = await this.resolveBrowserTabId({
      tab_id: action?.tab_id,
      url: action?.url,
      url_host: action?.url_host,
      query: action?.query,
    });
    // Bare reload / refresh with no selector targets the task tab.
    if (!resolved?.tabId && taskTabId &&
        (wantsReload || action?.action === 'refresh_tab' ||
         action?.action === 'request_context')) {
      resolved = {
        tabId: taskTabId,
        title: String(this.workingTabTitle || ''),
        url: String(this.workingTabUrl || this.currentTask?.url || ''),
      };
    }
    if (!resolved?.tabId) {
      this.addSystemMessage(
          'Could not resolve target tab. Call {"action":"list_tabs"} first, or pass tab_id / url_host (e.g. mail.google.com).');
      return {handled: true, requestContext: false};
    }

    const isTaskTab = !!taskTabId && resolved.tabId === taskTabId;

    if (wantsReload || action?.action === 'refresh_tab') {
      this.addSystemMessage(
          `Reloading tab ${resolved.tabId}` +
          (resolved.url ? ` (${resolved.url})` : '') + '…');
      const ok = await this.reloadBrowserTab(resolved.tabId);
      if (!ok) {
        return {handled: true, requestContext: isTaskTab};
      }
    }

    if (isTaskTab &&
        (action?.action === 'request_context' ||
         action?.action === 'refresh_tab')) {
      // Task-tab reload/refresh: next turn collects a normal task snapshot.
      return {handled: true, requestContext: true};
    }

    // Peek helper tab without rebinding the task tab.
    const peeked = await this.collectContextFromTabId(resolved.tabId, {
      rebindTaskTab: false,
    });
    if (!(peeked.elements?.length) && !(peeked.innerText || '').trim()) {
      this.addSystemMessage(
          `Peeked tab ${resolved.tabId} returned empty context` +
          (resolved.url ? ` (${resolved.url})` : '') +
          '. If this is Gmail/Outlook, retry with reload:true.');
      return {handled: true, requestContext: false};
    }
    this.addSystemMessage(this.formatPeekedTabContext(peeked));
    // Also refresh task-tab context on the next model turn so element ids for
    // the login/2FA form remain available.
    return {handled: true, requestContext: !!this.currentTask};
  }

  private resolveScreenshotCssSize(
      imageWidth: number,
      imageHeight: number,
  ): {width: number, height: number, scale: number} {
    const fromContext = this.lastPageContext?.viewportCss;
    if (fromContext && fromContext.width > 0 && fromContext.height > 0) {
      const scaleX = imageWidth / fromContext.width;
      const scaleY = imageHeight / fromContext.height;
      const scale = (scaleX + scaleY) / 2;
      if (Number.isFinite(scale) && scale > 0.5 && scale < 5) {
        return {
          width: fromContext.width,
          height: fromContext.height,
          scale,
        };
      }
    }
    // Retina captures are typically exact 2x/3x device pixels.
    for (const candidate of [2, 3, 1.5, 1]) {
      const w = imageWidth / candidate;
      const h = imageHeight / candidate;
      if (Math.abs(w - Math.round(w)) < 0.51 &&
          Math.abs(h - Math.round(h)) < 0.51 && w >= 320 && h >= 240) {
        return {
          width: Math.round(w),
          height: Math.round(h),
          scale: candidate,
        };
      }
    }
    return {width: imageWidth, height: imageHeight, scale: 1};
  }

  /**
   * Find large saturated CTA-like regions (e.g. green COLLECT bars) and return
   * their CSS-pixel centers for painting exact click labels on the screenshot.
   */
  private findScreenshotClickHotspots(
      ctx: CanvasRenderingContext2D,
      cssW: number,
      cssH: number,
  ): Array<{x: number, y: number, w: number, h: number, kind: string}> {
    let data: ImageData;
    try {
      data = ctx.getImageData(0, 0, cssW, cssH);
    } catch (_) {
      return [];
    }
    const px = data.data;
    const step = 2;
    // Row runs of "CTA green" pixels → merge into blobs.
    type Blob = {minX: number, maxX: number, minY: number, maxY: number, n: number};
    const blobs: Blob[] = [];
    const isCta = (i: number): boolean => {
      const r = px[i] ?? 0;
      const g = px[i + 1] ?? 0;
      const b = px[i + 2] ?? 0;
      const a = px[i + 3] ?? 0;
      if (a < 200) {
        return false;
      }
      // Bright green / teal CTAs (Lonestar COLLECT, etc.). JPEG+downsample
      // washes these out, so keep thresholds loose.
      return g > 115 && g >= r + 12 && g >= b + 8 && r < 220 && b < 180;
    };
    for (let y = 0; y < cssH; y += step) {
      let runStart = -1;
      for (let x = 0; x <= cssW; x += step) {
        const inside = x < cssW && isCta(((y * cssW) + x) * 4);
        if (inside && runStart < 0) {
          runStart = x;
        } else if (!inside && runStart >= 0) {
          const runEnd = x - step;
          const runW = runEnd - runStart;
          if (runW >= 60) {
            // Merge with nearby blob
            let merged = false;
            for (const b of blobs) {
              if (Math.abs(y - b.maxY) <= 14 &&
                  !(runEnd < b.minX - 12 || runStart > b.maxX + 12)) {
                b.minX = Math.min(b.minX, runStart);
                b.maxX = Math.max(b.maxX, runEnd);
                b.maxY = y;
                b.n += runW;
                merged = true;
                break;
              }
            }
            if (!merged) {
              blobs.push({
                minX: runStart,
                maxX: runEnd,
                minY: y,
                maxY: y,
                n: runW,
              });
            }
          }
          runStart = -1;
        }
      }
    }
    const out: Array<{x: number, y: number, w: number, h: number, kind: string}> =
        [];
    for (const b of blobs) {
      const w = b.maxX - b.minX + 1;
      const h = b.maxY - b.minY + 1;
      // Prefer wide button-like bars; skip tiny noise / tall side chrome.
      if (w < 120 || h < 6 || h > 140 || w * h < 1200) {
        continue;
      }
      out.push({
        x: Math.round((b.minX + b.maxX) / 2),
        y: Math.round((b.minY + b.maxY) / 2),
        w,
        h,
        kind: 'green_cta',
      });
    }
    // Largest first; keep a few so the note stays short.
    out.sort((a, b) => (b.w * b.h) - (a.w * a.h));
    return out.slice(0, 5);
  }

  /**
   * Resample the screenshot to CSS/DIP pixels, draw a labeled grid, and paint
   * exact [x,y] markers on detected CTA hotspots for coordinate clicks.
   */
  private async annotateScreenshotForCoordinateClicks(shot: {
    base64: string,
    mimeType: string,
    widthPixels?: number,
    heightPixels?: number,
  }): Promise<{
    base64: string,
    mimeType: string,
    cssWidth: number,
    cssHeight: number,
    note: string,
  }|null> {
    try {
      const srcUrl = `data:${shot.mimeType};base64,${shot.base64}`;
      const img = await new Promise<HTMLImageElement>((resolve, reject) => {
        const el = new Image();
        el.onload = () => resolve(el);
        el.onerror = () => reject(new Error('screenshot decode failed'));
        el.src = srcUrl;
      });
      const imageWidth = shot.widthPixels || img.naturalWidth || img.width;
      const imageHeight = shot.heightPixels || img.naturalHeight || img.height;
      if (!imageWidth || !imageHeight) {
        return null;
      }
      const css = this.resolveScreenshotCssSize(imageWidth, imageHeight);
      const cssW = css.width;
      const cssH = css.height;
      const canvas = document.createElement('canvas');
      canvas.width = cssW;
      canvas.height = cssH;
      const ctx = canvas.getContext('2d');
      if (!ctx) {
        return null;
      }
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = 'high';
      ctx.drawImage(img, 0, 0, cssW, cssH);

      // Detect large CTA-colored hotspots BEFORE drawing the grid so labels
      // sit on top. These give the model exact readable click coords.
      const hotspots = this.findScreenshotClickHotspots(ctx, cssW, cssH);

      // Dim edges slightly so grid labels stay readable on busy UIs.
      const step = cssW >= 1600 ? 100 : (cssW >= 900 ? 50 : 25);
      ctx.save();
      ctx.lineWidth = 1;
      ctx.font = `${Math.max(11, Math.round(step / 6))}px monospace`;
      ctx.textBaseline = 'top';

      for (let x = 0; x <= cssW; x += step) {
        ctx.beginPath();
        ctx.moveTo(x + 0.5, 0);
        ctx.lineTo(x + 0.5, cssH);
        ctx.strokeStyle = x % (step * 2) === 0 ? 'rgba(0, 255, 255, 0.55)' :
                                                'rgba(0, 220, 255, 0.22)';
        ctx.stroke();
        if (x > 0 && x < cssW) {
          const label = String(x);
          const tw = ctx.measureText(label).width;
          ctx.fillStyle = 'rgba(0, 0, 0, 0.6)';
          ctx.fillRect(x + 2, 2, tw + 6, 14);
          ctx.fillStyle = 'rgba(0, 255, 255, 0.95)';
          ctx.fillText(label, x + 5, 4);
        }
      }
      for (let y = 0; y <= cssH; y += step) {
        ctx.beginPath();
        ctx.moveTo(0, y + 0.5);
        ctx.lineTo(cssW, y + 0.5);
        ctx.strokeStyle = y % (step * 2) === 0 ? 'rgba(0, 255, 255, 0.55)' :
                                                'rgba(0, 220, 255, 0.22)';
        ctx.stroke();
        if (y > 0 && y < cssH) {
          const label = String(y);
          const tw = ctx.measureText(label).width;
          ctx.fillStyle = 'rgba(0, 0, 0, 0.6)';
          ctx.fillRect(2, y + 2, tw + 6, 14);
          ctx.fillStyle = 'rgba(0, 255, 255, 0.95)';
          ctx.fillText(label, 5, y + 4);
        }
      }

      // Paint hotspot markers with EXACT click coordinates on the image.
      ctx.font = 'bold 16px monospace';
      ctx.textBaseline = 'middle';
      hotspots.forEach((h, i) => {
        const tag = `#${i + 1} [${h.x},${h.y}]`;
        const tw = ctx.measureText(tag).width;
        // Magenta crosshair at exact click point.
        ctx.strokeStyle = 'rgba(255, 0, 200, 0.95)';
        ctx.lineWidth = 3;
        ctx.beginPath();
        ctx.moveTo(h.x - 22, h.y);
        ctx.lineTo(h.x + 22, h.y);
        ctx.moveTo(h.x, h.y - 22);
        ctx.lineTo(h.x, h.y + 22);
        ctx.stroke();
        ctx.strokeRect(h.x - h.w / 2, h.y - h.h / 2, h.w, h.h);
        const labelX = Math.min(cssW - tw - 12, Math.max(8, h.x - tw / 2));
        const labelY = Math.max(18, h.y - h.h / 2 - 18);
        ctx.fillStyle = 'rgba(0, 0, 0, 0.78)';
        ctx.fillRect(labelX - 4, labelY - 12, tw + 8, 24);
        ctx.fillStyle = 'rgba(255, 80, 220, 1)';
        ctx.fillText(tag, labelX, labelY);
      });

      // Origin / size legend.
      const legend =
          `CSS click px (0,0) top-left → (${cssW},${cssH}) grid=${step}px ` +
          `scale=${css.scale.toFixed(2)} | tab viewport ONLY (Glic panel not in image)`;
      ctx.font = '13px monospace';
      ctx.textBaseline = 'top';
      const lw = ctx.measureText(legend).width;
      ctx.fillStyle = 'rgba(0, 0, 0, 0.72)';
      ctx.fillRect(8, cssH - 28, Math.min(cssW - 16, lw + 16), 22);
      ctx.fillStyle = 'rgba(0, 255, 180, 0.98)';
      ctx.fillText(legend, 16, cssH - 23);
      ctx.restore();

      const mimeType = 'image/jpeg';
      const dataUrl = canvas.toDataURL(mimeType, 0.82);
      const comma = dataUrl.indexOf(',');
      if (comma < 0) {
        return null;
      }
      const base64 = dataUrl.slice(comma + 1);
      const hotspotLines = hotspots.length ?
          hotspots
              .map(
                  (h, i) =>
                      `  #${i + 1} ${h.kind} → {"action":"click","coordinate":[${h.x},${h.y}]} ` +
                      `(~${h.w}x${h.h}px)`)
              .join('\n') :
          '  (no large CTA-colored hotspots auto-detected)';
      const note =
          `[Viewport screenshot — CSS click coordinates]\n` +
          `- This image is the TASK TAB viewport only. The Glic side panel is NOT ` +
          `in the image and must not be used when estimating positions.\n` +
          `- Coordinate space: CSS/DIP pixels matching executor clicks. ` +
          `Origin (0,0)=top-left of this image; x→right, y→down.\n` +
          `- Image resampled to ${cssW}x${cssH} (1 image pixel ≈ 1 click CSS pixel).\n` +
          `- Magenta markers (#N [x,y]) are EXACT click coordinates painted on ` +
          `detected CTA/hotspot regions — prefer copying those numbers.\n` +
          `- Cyan grid every ${step}px is a secondary ruler if no marker fits.\n` +
          `Painted hotspots:\n${hotspotLines}`;
      return {base64, mimeType, cssWidth: cssW, cssHeight: cssH, note};
    } catch (e) {
      console.warn('[glic] annotateScreenshotForCoordinateClicks failed', e);
      return null;
    }
  }

  private async captureTaskViewportScreenshot():
      Promise<{
        base64: string,
        mimeType: string,
        widthPixels?: number,
        heightPixels?: number,
      }|null> {
    if (!this.browser) {
      return null;
    }
    const options = {
      innerText: false,
      innerTextBytesLimit: 0,
      viewportScreenshot: true,
      annotatedPageContent: false,
      annotatedPageContentMode: 1,
      maxMetaTags: 0,
    };
    let context: TabContextResult|undefined;
    const lockedTaskTabId =
        this.currentTask ? this.getAuthoritativeTaskTabId() : null;
    const candidateTabIds = lockedTaskTabId ?
        [lockedTaskTabId] :
        this.getContextTabCandidates();
    for (const tabId of candidateTabIds) {
      if (!tabId) {
        continue;
      }
      try {
        if (this.browser.getContextForActorFromTab) {
          context =
              await this.browser.getContextForActorFromTab(tabId, options);
        }
      } catch (_e) {
        context = undefined;
      }
      if (!context && this.browser.getContextFromTab) {
        try {
          context = await this.browser.getContextFromTab(tabId, options);
        } catch (_e) {
          context = undefined;
        }
      }
      if (context?.viewportScreenshot?.data) {
        break;
      }
      context = undefined;
    }
    if (!context?.viewportScreenshot?.data &&
        this.browser.getContextFromFocusedTab) {
      try {
        context = await this.browser.getContextFromFocusedTab(options);
      } catch (_e) {
        context = undefined;
      }
    }
    const shot = context?.viewportScreenshot;
    if (!shot?.data) {
      return null;
    }
    const bytes = new Uint8Array(shot.data);
    if (!bytes.length) {
      return null;
    }
    const mimeType = String(shot.mimeType || 'image/jpeg').trim() || 'image/jpeg';
    const widthPixels = Number(shot.widthPixels || 0);
    const heightPixels = Number(shot.heightPixels || 0);
    return {
      base64: encodeBytesToBase64(bytes),
      mimeType,
      widthPixels: Number.isFinite(widthPixels) && widthPixels > 0 ?
          Math.round(widthPixels) :
          undefined,
      heightPixels: Number.isFinite(heightPixels) && heightPixels > 0 ?
          Math.round(heightPixels) :
          undefined,
    };
  }

  private async collectContext(): Promise<PageContextSummary> {
    const summary: PageContextSummary = {elements: []};
    if (!this.browser) {
      return summary;
    }
    // Non-vision local agents: never pull screenshots or full HTML/innerText.
    // Annotated page content stays accessibility/actionable only, off the
    // renderer critical path (see multi_source_page_context_fetcher).
    const options = {
      innerText: false,
      innerTextBytesLimit: 0,
      viewportScreenshot: false,
      annotatedPageContent: true,
      annotatedPageContentMode: 1,
      maxMetaTags: 0,
    };
    for (let attempt = 0; attempt < 2; ++attempt) {
      try {
        this.lastContextError = null;
        let context: TabContextResult|undefined;
        let lastError = '';
        const candidateTabIds = this.getContextTabCandidates();
        const lockedTaskTabId = this.currentTask ? this.getAuthoritativeTaskTabId() : null;
        let resolvedCandidateTabId: string|null = null;

        for (const tabId of candidateTabIds) {
          const preferActorContext = !!lockedTaskTabId;
          if (preferActorContext && this.browser.getContextForActorFromTab) {
            try {
              context = await this.browser.getContextForActorFromTab(tabId, options);
            } catch (e) {
              lastError = String(e || '');
            }
          }
          if (!context && this.browser.getContextFromTab) {
            try {
              context = await this.browser.getContextFromTab(tabId, options);
            } catch (e) {
              lastError = String(e || '');
            }
          }
          if (!context && !preferActorContext &&
              this.browser.getContextForActorFromTab) {
            try {
              context = await this.browser.getContextForActorFromTab(tabId, options);
            } catch (e) {
              lastError = String(e || '');
            }
          }
          if (!context) {
            continue;
          }
          const returnedTabId = String(context.tabData?.tabId || '').trim();
          if (lockedTaskTabId && returnedTabId &&
              returnedTabId !== lockedTaskTabId) {
            lastError =
                `Locked task tab mismatch: expected ${lockedTaskTabId}, got ${returnedTabId}`;
            context = undefined;
            continue;
          }
          resolvedCandidateTabId = tabId;
          break;
        }

        const allowFocusedTabFallback =
            !this.currentTask || candidateTabIds.length === 0;
        if (!context && allowFocusedTabFallback &&
            this.browser.getContextFromFocusedTab) {
          try {
            context = await this.browser.getContextFromFocusedTab(options);
          } catch (e) {
            lastError = String(e || '');
          }
        }

        if (!context) {
          if (lastError) {
            throw new Error(lastError);
          }
          return summary;
        }
        this.tabContextPermissionRecoveryAttempted = false;
        summary.tabId = context.tabData.tabId;
        summary.tabIdInt = Number.parseInt(context.tabData.tabId, 10);
        summary.url = context.tabData.url;
        summary.title = context.tabData.title;
        summary.innerText = context.webPageData?.mainDocument?.innerText;
        const boundTabId =
            String(summary.tabId || resolvedCandidateTabId || lockedTaskTabId || '')
                .trim();
        if (this.currentTask) {
          this.bindTaskTab(
              boundTabId,
              summary.url ?? this.workingTabUrl ?? null,
              summary.title ?? this.currentTask?.goal ?? this.workingTabTitle);
        } else {
          if (summary.tabId) {
            this.currentTabId = summary.tabId;
          }
          this.setWorkingContext(
              summary.tabId ?? this.currentTabId,
              summary.url ?? this.workingTabUrl,
              summary.title ?? this.workingTabTitle);
        }

        const apcStream = context.annotatedPageData?.annotatedPageContent;
        if (apcStream) {
          const bytes = await readStream(apcStream);
          const buffer = bytes.buffer as ArrayBuffer;
          const res = await fetch('/parse-apc', {
            method: 'POST',
            headers: {'Content-Type': 'application/octet-stream'},
            body: buffer,
          });
          if (res.ok) {
            const apcJson = await res.json();
            summary.elements = extractNodesFromApc(apcJson, 40, summary.url);
            summary.documentId = extractDocumentIdFromApc(apcJson);
            const vpCss = extractViewportCssFromApc(apcJson);
            if (vpCss) {
              summary.viewportCss = vpCss;
            }
            const apcTabId = getField(apcJson, ['tabId', 'tab_id']);
            if (typeof apcTabId === 'number' && Number.isFinite(apcTabId)) {
              summary.tabIdInt = apcTabId;
            }
          }
        }
        summary.elements = (summary.elements ?? []).filter(
            (node) => !shouldSuppressContextNodeForPrompt(node, summary.url));
        summary.innerText = sanitizeContextInnerTextForPrompt(
            summary.url, summary.innerText);
        this.lastPageContext = summary;
        return summary;
      } catch (e) {
        this.lastContextError = String(e);
        const recovered = await this.recoverTabContextPermissionIfNeeded(e);
        if (recovered) {
          continue;
        }
        const errText = String(e);
        this.addSystemMessage(
            `Context collection failed: ${errText}` +
            (errText.includes('Failed to fetch')
                 ? ' — cannot reach /parse-apc; restart the glic test-client server (serve.py on port 8000).'
                 : ''));
      }
    }

    return summary;
  }

  private isContextReady(summary: PageContextSummary): boolean {
    const inner = (summary.innerText ?? '').trim();
    if (!summary.url && !inner && summary.elements.length === 0) {
      return false;
    }
    if (!summary.documentId) {
      return false;
    }
    if (!summary.tabId && !summary.tabIdInt) {
      return false;
    }
    if (summary.elements.length === 0 && inner.length < 10) {
      return false;
    }
    const labels = summary.elements.map((el) => (el.label || '').trim().toUpperCase())
        .filter((label) => label);
    const onlyLoading = labels.length > 0 &&
        labels.every((label) => label === 'LOADING...' || label === 'LOADING');
    if (onlyLoading) {
      return false;
    }
    if (inner && inner.toUpperCase() === 'LOADING...') {
      return summary.elements.length > 1;
    }
    if (summary.elements.length < 3 && inner.length < 30) {
      return false;
    }
    return true;
  }

  private async collectContextWithRetry(): Promise<PageContextSummary> {
    let last: PageContextSummary = {elements: []};
    // Avoid hammering APC/screenshot pipelines while the SPA is still booting.
    // Repeated GetContext* during load is a major paint-starvation source.
    let delayMs = 8000;
    let readyStreak = 0;
    let loadNoticeShown = false;
    const start = Date.now();

    while (Date.now() - start < this.maxLoadWaitMs) {
      last = await this.collectContext();
      if (this.isContextReady(last)) {
        readyStreak++;
        if (readyStreak >= 2) {
          return last;
        }
      } else {
        readyStreak = 0;
        if (!loadNoticeShown) {
          this.addSystemMessage('Waiting for page to finish loading before sending context.');
          loadNoticeShown = true;
        }
      }

      if (!this.currentTask) {
        return last;
      }

      if (this.lastContextError &&
          this.lastContextError.includes('tabContext failed: tab not found') &&
          this.currentTask.url &&
          this.tabReopenAttempts < this.maxTabReopenAttempts) {
        this.tabReopenAttempts += 1;
        await this.openTaskUrl(this.currentTask.url, {forceNew: true});
      }

      await sleep(delayMs);
      delayMs = Math.min(Math.floor(delayMs * 1.5), 8000);
    }

    this.addSystemMessage('Timed out waiting for page load; using the latest context.');
    return last;
  }

  private buildManualPromptWithAttachedContext(
      userText: string,
      context: PageContextSummary): string {
    const compactElements = (context.elements ?? [])
                                .slice(0, MANUAL_CONTEXT_ELEMENT_LIMIT)
                                .map((el) => ({
      id: el.id,
      label: el.label,
      role: el.role,
      formType: el.formType,
      fieldName: el.fieldName,
      placeholder: el.placeholder,
      editable: !!el.editable,
      focusable: !!el.focusable,
      clickable: !!el.clickable,
      visible: el.visible !== false,
      bounds: el.bounds,
      locators: (el.locators ?? []).slice(0, 3),
    }));
    const payload = {
      tabId: context.tabId,
      tabIdInt: context.tabIdInt,
      url: context.url,
      title: context.title,
      documentId: context.documentId,
      elementCount: context.elements?.length ?? 0,
      includedElementCount: compactElements.length,
      elements: compactElements,
      innerText: context.innerText?.slice(0, 2000) ?? '',
      contextError: this.lastContextError || undefined,
    };
    return [
      userText,
      '',
      'Attached page context (live snapshot):',
      JSON.stringify(payload, null, 2),
      '',
      this.userLikelyWantsSimulachromeScript(userText)
        ? 'Output format required: return one Simulachrome script JSON object only: {"name":"...","actions":[...]}'
        : 'Output format required: return JSON only (action JSON or script JSON as appropriate).',
      'If context is insufficient, return {"action":"request_context"} to request another snapshot. For helper tabs use list_tabs / peek_tab (reload:true if stale).',
      'Use this context for browser-specific reasoning and troubleshooting.',
    ].join('\n');
  }


  private prependUserGoalPriority(prompt: string): string {
    const goal = String(this.currentTask?.goal || '').trim();
    if (!goal) {
      return prompt;
    }
    return (
      'USER GOAL (highest priority — overrides default habits in system prompts):\n' +
      goal +
      '\n\n' +
      prompt
    );
  }

  private buildContextPrompt(context: PageContextSummary): string {
    if (this.shouldEnforceHeadlessOnlyForCurrentTask()) {
      const lines: string[] = [];
      lines.push('Headless investigation mode is active for this task/profile.');
      if (context.url) {
        lines.push(`Task URL: ${context.url}`);
      }
      if (this.lastHeadlessInvestigationSummary) {
        lines.push(this.lastHeadlessInvestigationSummary);
      } else {
        lines.push('No headless investigation summary has been captured yet.');
      }
      lines.push('Return one JSON action only.');
      lines.push('Allowed actions in this mode:');
      lines.push('{"action":"investigate_page"}');
      lines.push('{"action":"investigate_page","url":"https://example.com/"}');
      lines.push(
          '{"action":"investigate_page","url":"https://example.com/","closeTarget":false}');
      lines.push(
          '{"action":"investigate_page","task_id":"<existing_task_id>","closeTarget":false}');
      lines.push('{"action":"request_console_logs","level":"error","limit":20}');
      lines.push('{"action":"delegate_profile","target_profile":"browser_operator"}');
      lines.push('{"action":"complete"}');
      return lines.join('\n');
    }
    const meta = context.elements.map((el) => {
      const kind = describeNodeKind(el);
      const text = bestContextNodeText(el);
      const pos = positionKey(el.bounds);
      const role = el.role || undefined;
      return {el, kind, text, role, pos};
    });

    const textCounts = countBy(meta, (m) => m.text ? m.text.toLowerCase() : undefined);
    const textKindCounts = countBy(meta, (m) =>
      m.text && m.kind ? `${m.text.toLowerCase()}|${m.kind}` : undefined);
    const textRoleCounts = countBy(meta, (m) =>
      m.text && m.role ? `${m.text.toLowerCase()}|${m.role}` : undefined);
    const kindRoleCounts = countBy(meta, (m) =>
      m.kind && m.role ? `${m.kind}|${m.role}` : undefined);
    const posCounts = countBy(meta, (m) => m.pos?.key);

    const buildBaseDesc = (m: typeof meta[number]): MinimalElementDescriptor => {
      const textKey = m.text ? m.text.toLowerCase() : undefined;
      const desc: MinimalElementDescriptor = {};
      const isButtonLike = Boolean(
          m.el.clickable || (m.kind && /button|clickable/i.test(m.kind)) ||
          /BUTTON/i.test(String(m.el.formType || '')));

      // Always surface button/clickable label text — cheap and high-signal.
      if (m.text && isButtonLike) {
        desc.text = m.text;
        if (m.kind) {
          desc.kind = m.kind;
        }
        return desc;
      }

      if (textKey && (textCounts.get(textKey) ?? 0) === 1) {
        desc.text = m.text;
        return desc;
      }
      if (textKey && m.kind &&
          (textKindCounts.get(`${textKey}|${m.kind}`) ?? 0) === 1) {
        desc.text = m.text;
        desc.kind = m.kind;
        return desc;
      }
      if (textKey && m.role &&
          (textRoleCounts.get(`${textKey}|${m.role}`) ?? 0) === 1) {
        desc.text = m.text;
        desc.role = m.role;
        return desc;
      }
      if (m.kind && m.role &&
          (kindRoleCounts.get(`${m.kind}|${m.role}`) ?? 0) === 1) {
        desc.kind = m.kind;
        desc.role = m.role;
        return desc;
      }

      if (m.text) {
        desc.text = m.text;
        if (m.kind) {
          desc.kind = m.kind;
        }
        return desc;
      }
      if (m.kind) {
        desc.kind = m.kind;
        if (m.role) {
          desc.role = m.role;
        }
        return desc;
      }
      if (m.role) {
        desc.role = m.role;
        return desc;
      }
      return desc;
    };

    const baseDescs = meta.map((m) => buildBaseDesc(m));
    const baseKeys = baseDescs.map((d) => JSON.stringify(d) ?? '{}');
    const baseKeyCounts = countBy(baseKeys, (k) => k);

    const validIds = Array.from(new Set(meta.map((m) => m.el.id)))
        .filter((id) => typeof id === 'number' && Number.isFinite(id) && id > 0)
        .sort((a, b) => a - b);

    const passwordCandidateIds = Array.from(new Set(meta
        .filter((m) => m.el.visible !== false && !m.el.disabled)
        .filter((m) => (m.el.formType?.includes('INPUT_PASSWORD') ?? false) ||
                       /(password|passcode|pin)/i.test(m.text || ''))
        .map((m) => m.el.id)))
        .sort((a, b) => a - b);
    const usernameCandidateIds = Array.from(new Set(meta
        .filter((m) => m.el.visible !== false && !m.el.disabled)
        .filter((m) => (m.el.formType?.includes('INPUT_EMAIL') ?? false) ||
                       (m.el.formType?.includes('INPUT_TEXT') ?? false) ||
                       /(email|e-mail|username|user id|login)/i.test(m.text || '') ||
                       /(email|username|user|login)/i.test(m.el.fieldName || '') ||
                       /(email|username|user|login)/i.test(m.el.placeholder || ''))
        .map((m) => m.el.id)))
        .sort((a, b) => a - b);
    const loginButtonCandidateIds = Array.from(new Set(meta
        .filter((m) => m.el.visible !== false && !m.el.disabled)
        .filter((m) => ((m.kind ?? '').includes('button') || m.el.clickable) &&
                       /(log in|login|sign in|signin)/i.test(m.text || ''))
        .map((m) => m.el.id)))
        .sort((a, b) => a - b);

    const elements = meta.map((m, i) => {
      const entry: Record<string, unknown> = {id: m.el.id};
      const desc: MinimalElementDescriptor = {...baseDescs[i]};

      const key = baseKeys[i] ?? '{}';
      const isDup = (baseKeyCounts.get(key) ?? 0) > 1;
      const posUnique = Boolean(m.pos && (posCounts.get(m.pos.key) ?? 0) === 1);

      // Add coarse position only when it helps disambiguate duplicates (or when
      // we otherwise have no meaningful descriptor).
      if ((isDup || Object.keys(desc).length === 0) && m.pos) {
        desc.pos = m.pos.pos;
      } else if (posUnique && m.pos && !desc.text &&
                 Object.keys(desc).length === 1 && (desc.kind || desc.role)) {
        // If something is still too generic (e.g. "button"), a unique position
        // can help without bloating everything with geometry.
        desc.pos = m.pos.pos;
      }

      if (Object.keys(desc).length > 0) {
        entry['desc'] = desc;
      }
      if (m.el.editable) {
        entry['editable'] = true;
      }
      if (m.el.focusable) {
        entry['focusable'] = true;
      }
      if (m.el.clickable) {
        entry['clickable'] = true;
      }
      if (m.el.required) {
        entry['required'] = true;
      }
      if (m.el.disabled) {
        entry['disabled'] = true;
      }

      return entry;
    });
    const payload = {
      taskTabId: this.currentTask ? this.getAuthoritativeTaskTabId() : undefined,
      taskStep: this.currentTask ? this.currentTaskStepIndex : undefined,
      tabId: context.tabId,
      tabIdInt: context.tabIdInt,
      url: context.url,
      title: context.title,
      documentId: context.documentId,
      element_count: elements.length,
      elements,
      innerText: context.innerText?.slice(0, 2000) ?? '',
      last_action: this.lastActionSummary ?? undefined,
    };
    return (
      `Page context (task step ${this.currentTaskStepIndex || 1}; use elements[].id for actions):\n` +
      JSON.stringify(payload, null, 2) +
      (this.currentTask && this.getAuthoritativeTaskTabId()
          ? `\nAuthoritative task tab id: ${this.getAuthoritativeTaskTabId()}`
          : '') +
      `\nValid element ids (target_id must be one of these integers): ${validIds.join(', ')}` +
      (usernameCandidateIds.length
          ? `\nHint: username/email field ids: ${usernameCandidateIds.join(', ')}. ` +
            `Type a VARIABLE token only: ${
                (this.listIdentityVariableTokens().map((n) => `{${n}}`).join(', ') ||
                 '{Email}')}.`
          : '') +
      (passwordCandidateIds.length
          ? `\nHint: password field ids: ${passwordCandidateIds.join(', ')}. ` +
            `Type a SECRET token only: ${
                (this.listPasswordSecretTokens().map((n) => `{${n}}`).join(', ') ||
                 '{Password}')}.`
          : '') +
      (loginButtonCandidateIds.length
          ? `\nHint: Log In/Sign In button candidate ids: ${loginButtonCandidateIds.join(', ')}`
          : '') +
      '\nProvide the next action JSON only.'
    );
  }

  private buildContinuationPrompt(
      context: PageContextSummary,
      options?: {pageUnchanged?: boolean}): string {
    const lines: string[] = [];
    if (this.shouldEnforceHeadlessOnlyForCurrentTask()) {
      lines.push('Headless investigation mode is active for this task/profile.');
      if (context.url) {
        lines.push(`Task URL: ${context.url}`);
      }
      if (this.lastHeadlessInvestigationSummary) {
        lines.push(this.lastHeadlessInvestigationSummary);
      } else {
        lines.push('No headless investigation summary has been captured yet.');
      }
      lines.push('Return one JSON action only.');
      lines.push('Allowed actions: investigate_page, request_console_logs, delegate_profile(browser_operator), complete.');
      return lines.join('\n');
    }
    const pageUnchanged = !!options?.pageUnchanged;
    lines.push(
        `Continue from prior context on task step ${this.currentTaskStepIndex || 1}; do not assume a fresh element list this turn.`);
    if (pageUnchanged) {
      lines.push(
          'Page-change check: unchanged since last action (same url/document/element ids).');
    }
    const taskTabId = this.getAuthoritativeTaskTabId();
    if (taskTabId) {
      lines.push(`Authoritative task tab id: ${taskTabId}`);
    }
    if (context.url) {
      lines.push(`Current URL: ${context.url}`);
    }
    if (this.lastActionSummary) {
      lines.push(`Last action: ${this.lastActionSummary}`);
    }
    if (this.lastTaskLabeledElements.length) {
      const labeled = this.lastTaskLabeledElements
                          .slice(0, 25)
                          .map((el) => `${el.id}="${el.text}"`)
                          .join(', ');
      lines.push(`Prior labeled targets still valid: ${labeled}`);
    } else if (this.lastTaskElementIds.length) {
      lines.push(
          `Prior valid element ids: ${this.lastTaskElementIds.slice(0, 40).join(', ')}`);
    }
    lines.push('Return the next action JSON only.');
    lines.push(
        'Prefer acting with prior target ids when they still apply. ' +
        'Return {"action":"request_context"} when needed ids are missing or the UI changed ' +
        '(including same-URL modal/dialog/popup/overlay).');
    lines.push('For another open tab (2FA/email), use list_tabs / peek_tab / request_context with tab_id and reload:true if the inbox may be stale.');
    return lines.join('\n');
  }

  private buildTaskContextSignature(context: PageContextSummary): string {
    const ids = (context.elements ?? [])
                    .map((el) => (typeof el?.id === 'number' ? el.id : null))
                    .filter((id): id is number => id !== null)
                    .sort((a, b) => a - b);
    return JSON.stringify({
      tabId: context.tabIdInt ?? context.tabId ?? null,
      url: context.url ?? '',
      documentId: context.documentId ?? '',
      ids,
      innerTextPrefix: (context.innerText ?? '').slice(0, 400),
    });
  }

  private async runTaskLoop() {
    if (this.taskLoopRunning) {
      return;
    }
    this.taskLoopRunning = true;
    this.taskLoopStopRequested = false;
    this.updateLlmUiState();
    let previousUrl = '';
    let forceContextPrompt = true;
    let previousContextSignature = '';
    let repeatedFreshContextCount = 0;
    try {
    for (let step = 0; step < this.maxStepsPerTask; step++) {
      if (this.taskLoopStopRequested || !this.currentTask) {
        break;
      }
      this.currentTaskStepIndex = step + 1;
      this.updateTaskInfo();
      if (this.shouldEnforceHeadlessOnlyForCurrentTask() &&
          (step === 0 || forceContextPrompt) &&
          this.supportsDashboardInvestigation()) {
        await this.requestHeadlessInvestigation({url: this.currentTask.url});
      }
      if (!this.shouldEnforceHeadlessOnlyForCurrentTask() &&
          this.isWebNavigableUrl(this.currentTask.url)) {
        let liveUrl = String(this.workingTabUrl || '').trim();
        if (!liveUrl || this.isNonActuableBrowserUrl(liveUrl) || step === 0) {
          try {
            const probe = await this.collectContext();
            liveUrl = String(probe?.url || liveUrl || '').trim();
          } catch (_) {}
        }
        if (this.isNonActuableBrowserUrl(liveUrl) ||
            (step === 0 && !this.isWebNavigableUrl(liveUrl))) {
          await this.ensureTaskOnDestinationUrl(
              this.currentTask.url,
              this.isNonActuableBrowserUrl(liveUrl) ?
                  'Task tab is not a normal web page' :
                  'Task start');
        }
      }
      const context = this.shouldEnforceHeadlessOnlyForCurrentTask()
          ? {
              elements: [],
              url: this.currentTask.url,
              title: this.currentTask.goal,
            } as PageContextSummary
          : await this.collectContextWithRetry();
      const contextSignature = this.buildTaskContextSignature(context);
      if ((context.elements?.length ?? 0) > 0) {
        this.lastTaskElementIds = context.elements
            .map((el) => el.id)
            .filter((id): id is number =>
                typeof id === 'number' && Number.isFinite(id) && id > 0);
        this.lastTaskLabeledElements = context.elements
            .map((el) => {
              const textLabel = bestContextNodeText(el);
              return (typeof el.id === 'number' && textLabel) ?
                  {id: el.id, text: textLabel.slice(0, 60)} :
                  null;
            })
            .filter((x): x is {id: number, text: string} => !!x)
            .slice(0, 40);
      }
      const currentUrl = (context.url ?? '').trim();
      const urlChanged = Boolean(currentUrl && previousUrl && currentUrl !== previousUrl);
      const pageUnchanged = Boolean(
          previousContextSignature &&
          contextSignature === previousContextSignature && !urlChanged);
      const shouldSendFullContext = forceContextPrompt || step === 0 || urlChanged;
      if (forceContextPrompt) {
        if (contextSignature === previousContextSignature) {
          repeatedFreshContextCount += 1;
        } else {
          repeatedFreshContextCount = 0;
        }
      } else {
        repeatedFreshContextCount = 0;
      }
      let prompt = this.prependUserGoalPriority(
          shouldSendFullContext
              ? this.buildContextPrompt(context)
              : this.buildContinuationPrompt(context, {pageUnchanged}));
      if (shouldSendFullContext && repeatedFreshContextCount > 0) {
        prompt +=
            '\nFresh context snapshot note: signature matches the last one ' +
            `(repeat #${repeatedFreshContextCount}). Context was refreshed — ` +
            'choose the next action from the current ids. Same-URL popups/modals ' +
            'may still warrant another request_context if new controls are missing.';
      }
      if (!shouldSendFullContext && pageUnchanged) {
        prompt +=
            '\nPage-change check: url/document signature looks stable. Prior ' +
            'element ids may still work. If a same-URL popup/modal appeared or ' +
            'needed ids are missing, {"action":"request_context"} is allowed.';
      }
      this.trimMessages();
      this.requestContextNextTurn = false;
      if (this.taskLoopStopRequested || !this.currentTask) {
        break;
      }
      // First full context drop: attach a gridded CSS-pixel screenshot so the
      // model can click overlays immediately without an extra request_context.
      if (step === 0 && shouldSendFullContext &&
          this.supportsScreenshotAction() &&
          !this.shouldEnforceHeadlessOnlyForCurrentTask()) {
        await this.captureScreenshotForNextTurn('with initial context');
      }
      const assistantText = await this.sendUserMessage(
          prompt,
          {context, handleActions: true},
      );
      if (this.taskLoopStopRequested || !this.currentTask) {
        break;
      }
      if (assistantText === MODEL_BUSY_MESSAGE) {
        step -= 1;
        await sleep(500);
        continue;
      }
      const canceled = !assistantText ||
          assistantText === '[Canceled]' ||
          assistantText.startsWith('[Canceled]');
      if (canceled) {
        this.taskLoopStopRequested = true;
        break;
      }
      if (currentUrl) {
        previousUrl = currentUrl;
      }
      previousContextSignature = contextSignature;
      forceContextPrompt = this.requestContextNextTurn;
    }
    if (this.taskLoopStopRequested && this.currentTask) {
      this.addSystemMessage('Agent task loop paused.');
      await this.finishTask('paused', 'Paused by user');
    } else if (this.currentTask) {
      this.addSystemMessage('Stopping task loop (no action or max steps).');
      await this.finishTask('stalled', 'No actionable response from model');
    }
    } finally {
      this.taskLoopRunning = false;
      this.updateLlmUiState();
    }
  }

  private extractActions(responseText: string): any[] {
    const parsed = extractJsonObjects(responseText);
    return parsed.filter((obj) => {
      if (!obj || typeof obj !== 'object') {
        return false;
      }
      if (typeof obj.action === 'string') {
        return true;
      }
      const alt = obj.action_type ?? obj.actionType ?? obj.type ?? obj.name;
      return typeof alt === 'string' && !!alt.trim();
    });
  }

  private normalizeAction(raw: any): any|null {
    if (!raw || typeof raw !== 'object') {
      return null;
    }
    // Models sometimes emit action_type / type instead of action.
    if (typeof raw.action !== 'string') {
      const alt = raw.action_type ?? raw.actionType ?? raw.type ?? raw.name;
      if (typeof alt === 'string' && alt.trim()) {
        raw = {...raw, action: alt};
      }
    }
    if (typeof raw.action !== 'string') {
      return null;
    }
    // Drop alias keys so the strict click/type schema check does not reject
    // otherwise-valid payloads like {"action_type":"click","target_id":1020}.
    if ('action_type' in raw || 'actionType' in raw || 'type' in raw ||
        'name' in raw) {
      const cleaned: any = {...raw};
      delete cleaned.action_type;
      delete cleaned.actionType;
      // Only strip type/name when they duplicate the action verb.
      const actionVerb = String(cleaned.action || '').toLowerCase().trim();
      if (String(cleaned.type || '').toLowerCase().trim() === actionVerb) {
        delete cleaned.type;
      }
      if (String(cleaned.name || '').toLowerCase().trim() === actionVerb) {
        delete cleaned.name;
      }
      raw = cleaned;
    }
    const actionType = raw.action.toLowerCase().trim();
    if (actionType === 'complete' || actionType === 'task_complete' ||
        actionType === 'mark_task_complete' || actionType === 'mark_complete' ||
        actionType === 'task_done' || actionType === 'finish_task' ||
        actionType === 'done') {
      const normalizedComplete: any = {action: 'complete'};
      if ('success' in raw) {
        normalizedComplete.success = raw.success !== false;
      }
      if (typeof raw.reason === 'string' && raw.reason.trim()) {
        normalizedComplete.reason = raw.reason.trim();
      } else if (typeof raw.message === 'string' && raw.message.trim()) {
        normalizedComplete.reason = raw.message.trim();
      }
      return normalizedComplete;
    }
    if (actionType === 'list_tabs' || actionType === 'list_open_tabs') {
      const normalized: any = {action: 'list_tabs'};
      const query = String(raw.query || raw.url_host || raw.host || '').trim();
      if (query) {
        normalized.query = query;
      }
      return normalized;
    }
    if (actionType === 'peek_tab' || actionType === 'inspect_tab' ||
        actionType === 'read_tab') {
      const normalized: any = {action: 'peek_tab'};
      const tabId = String(raw.tab_id || raw.tabId || '').trim();
      if (tabId) {
        normalized.tab_id = tabId;
      }
      const url = String(raw.url || '').trim();
      if (url) {
        normalized.url = url;
      }
      const urlHost = String(raw.url_host || raw.urlHost || raw.host || '').trim();
      if (urlHost) {
        normalized.url_host = urlHost;
      }
      const query = String(raw.query || '').trim();
      if (query) {
        normalized.query = query;
      }
      if ('reload' in raw) {
        normalized.reload = !!raw.reload;
      }
      return normalized;
    }
    if (actionType === 'refresh_tab' || actionType === 'reload_tab') {
      const normalized: any = {action: 'refresh_tab', reload: true};
      const tabId = String(raw.tab_id || raw.tabId || '').trim();
      if (tabId) {
        normalized.tab_id = tabId;
      }
      const url = String(raw.url || '').trim();
      if (url) {
        normalized.url = url;
      }
      const urlHost = String(raw.url_host || raw.urlHost || raw.host || '').trim();
      if (urlHost) {
        normalized.url_host = urlHost;
      }
      const query = String(raw.query || '').trim();
      if (query) {
        normalized.query = query;
      }
      return normalized;
    }
    if (actionType === 'request_screenshot' ||
        actionType === 'screenshot' ||
        actionType === 'take_screenshot' ||
        actionType === 'capture_screenshot') {
      return {action: 'request_screenshot'};
    }
    if (actionType === 'request_context' ||
        actionType === 'refresh_context' ||
        actionType === 'refresh_pagemap' ||
        actionType === 'get_context') {
      const normalized: any = {action: 'request_context'};
      const tabId = String(raw.tab_id || raw.tabId || '').trim();
      if (tabId) {
        normalized.tab_id = tabId;
      }
      const url = String(raw.url || '').trim();
      if (url) {
        normalized.url = url;
      }
      const urlHost = String(raw.url_host || raw.urlHost || raw.host || '').trim();
      if (urlHost) {
        normalized.url_host = urlHost;
      }
      const query = String(raw.query || '').trim();
      if (query) {
        normalized.query = query;
      }
      if ('reload' in raw) {
        normalized.reload = !!raw.reload;
      }
      return normalized;
    }
    if (actionType === 'request_console_logs' ||
        actionType === 'get_console_logs' ||
        actionType === 'console_logs') {
      const level = String(raw.level || 'error').trim().toLowerCase() || 'error';
      const limit = Number.isFinite(Number(raw.limit)) ?
          Math.max(1, Math.min(100, Number(raw.limit))) :
          20;
      return {action: 'request_console_logs', level, limit};
    }
    if (actionType === 'investigate_page' ||
        actionType === 'request_investigation' ||
        actionType === 'headless_investigate') {
      const normalized: any = {action: 'investigate_page'};
      const url = String(raw.url || '').trim();
      if (url) {
        normalized.url = url;
      }
      const taskId = String(raw.taskId || raw.task_id || '').trim();
      if (taskId) {
        normalized.task_id = taskId;
      }
      const actions = Array.isArray(raw.actions) ?
          raw.actions :
          (Array.isArray(raw.timeline) ? raw.timeline : null);
      if (actions) {
        normalized.actions = actions;
      }
      if ('closeTarget' in raw || 'close_target' in raw) {
        normalized.closeTarget = !!raw.closeTarget || !!raw.close_target;
      }
      if ('include_screenshot' in raw || 'includeScreenshot' in raw) {
        normalized.include_screenshot =
            !!raw.include_screenshot || !!raw.includeScreenshot;
      }
      return normalized;
    }
    if (actionType === 'delegate_profile' ||
        actionType === 'switch_profile' ||
        actionType === 'handoff_profile') {
      const targetRaw = String(
          raw.target_profile || raw.targetProfile || raw.profile || raw.target || '')
                            .trim();
      const target = this.normalizeUnifiedProfile(targetRaw);
      if (!target) {
        this.addSystemMessage(
            `Invalid delegate_profile target. Use one of browser_operator|script_builder|script_debugger|auto_heal. Received: ${JSON.stringify(targetRaw)}`);
        return null;
      }
      const normalized: any = {
        action: 'delegate_profile',
        target_profile: target,
      };
      if (typeof raw.reason === 'string' && raw.reason.trim()) {
        normalized.reason = raw.reason.trim();
      }
      return normalized;
    }
    raw.action = this.aliasModelActionName(actionType);

    if (raw.action === 'wait') {
      const rawDelay = Number(raw.timeout ?? raw.duration ?? 1);
      const waitMs = rawDelay > 1000 ? rawDelay : rawDelay * 1000;
      return {action: 'wait', waitMs: Math.min(Math.max(waitMs, 500), 30000)};
    }

    if (raw.action === 'click' || raw.action === 'hover' ||
        raw.action === 'type' || raw.action === 'key_press' ||
        raw.action === 'scroll' || raw.action === 'scroll_to') {
      // Reject extra keys to tighten the model output schema.
      const allowedKeysByAction: Record<string, Set<string>> = {
        click: new Set([
          'action', 'target_id', 'targetId', 'element', 'coordinate',
          'coordinates', 'x', 'y',
        ]),
        hover: new Set([
          'action', 'target_id', 'targetId', 'element', 'coordinate',
          'coordinates', 'x', 'y',
        ]),
        type: new Set([
          'action', 'target_id', 'targetId', 'element', 'text', 'value',
          'coordinate', 'coordinates', 'x', 'y',
        ]),
        key_press: new Set(['action', 'key', 'value', 'count']),
        scroll: new Set(['action', 'target_id', 'targetId', 'element', 'distance']),
        scroll_to: new Set(['action', 'target_id', 'targetId', 'element']),
      };
      const allowed = allowedKeysByAction[raw.action];
      if (allowed) {
        const extras = Object.keys(raw).filter((k) => !allowed.has(k));
        if (extras.length) {
          const ignorableMetaKeys = new Set([
            'success', 'reason', 'status', 'ok', 'error', 'message',
            'action_type', 'actionType', 'description', 'desc', 'comment',
            'thought', 'thinking',
          ]);
          const nonMeta = extras.filter((k) => !ignorableMetaKeys.has(k));
          if (nonMeta.length) {
            this.addSystemMessage(
                `Invalid action keys: ${nonMeta.join(', ')}. Output must match the exact JSON shape for ${raw.action}.`);
            return null;
          }
          if (!this.warnedExtraMetaKeys) {
            this.addSystemMessage(
                `Do not include status keys (${extras.join(', ')}) in action JSON. Provide only the action object.`);
            this.warnedExtraMetaKeys = true;
          }
        }
      }

      if (raw.action === 'key_press') {
        const key = String(raw.key ?? raw.value ?? '').trim();
        if (!key) {
          this.addSystemMessage(`Key press action missing key: ${JSON.stringify(raw)}`);
          return null;
        }
        const count = Math.max(1, Number.parseInt(String(raw.count ?? 1), 10) || 1);
        return {action: 'key_press', key, count};
      }

      const point = this.parseCoordinatePoint(raw);
      const candidate = raw.target_id ??
          raw.targetId ??
          raw.element?.content_node_id ??
          raw.element?.contentNodeId ??
          raw.element?.id;
      const targetId = Number.parseInt(String(candidate ?? ''), 10);
      const hasTarget = Number.isFinite(targetId) && targetId > 0;
      if (!hasTarget &&
          !(point &&
            (raw.action === 'click' || raw.action === 'hover' ||
             raw.action === 'type'))) {
        this.addSystemMessage(
            `Invalid target_id (must be an integer from Page context elements[].id), ` +
            `or provide click/hover/type coordinates: ${JSON.stringify(raw)}`);
        return null;
      }
      if (raw.action === 'type') {
        const originalText = String(raw.text ?? raw.value ?? '');
        let textVal = normalizeTokenPlaceholdersInText(originalText);
        if (!textVal.trim()) {
          this.addSystemMessage(`Type action missing non-empty text: ${JSON.stringify(raw)}`);
          return null;
        }
        if (hasTarget) {
          const enforced = this.enforceCredentialTypeText(
              textVal, targetId, this.lastPageContext);
          if (enforced !== textVal) {
            this.addSystemMessage(
                `Credential policy: "${textVal}" → "${enforced}" ` +
                '(identity fields use variable {tokens}; password fields use secret {tokens}).');
            textVal = enforced;
          } else if (originalText !== textVal) {
            this.addSystemMessage(
                `Normalized type token placeholders to single-brace form: ${textVal}`);
          }
        } else if (originalText !== textVal) {
          this.addSystemMessage(
              `Normalized type token placeholders to single-brace form: ${textVal}`);
        }
        const out: any = {action: 'type', text: textVal};
        if (hasTarget) {
          out.target_id = targetId;
        }
        if (point) {
          out.coordinate = point;
        }
        return out;
      }
      if (raw.action === 'scroll') {
        const distance = Number(raw.distance ?? 600);
        return {action: 'scroll', target_id: targetId, distance};
      }
      if (raw.action === 'scroll_to') {
        return {action: 'scroll_to', target_id: targetId};
      }
      if (raw.action === 'hover') {
        const out: any = {action: 'hover'};
        if (hasTarget) {
          out.target_id = targetId;
        }
        if (point) {
          out.coordinate = point;
        }
        return out;
      }
      {
        const out: any = {action: 'click'};
        if (hasTarget) {
          out.target_id = targetId;
        }
        if (point) {
          out.coordinate = point;
        }
        return out;
      }
    }

    if (raw.action === 'navigate' || raw.action === 'goto' ||
        raw.action === 'open' || raw.action === 'open_url') {
      const rawUrl = String(raw.url || raw.href || raw.target_url || '').trim();
      let url = rawUrl;
      if (!this.isWebNavigableUrl(url)) {
        url = this.inferUrlFromPrompt(rawUrl || String(raw.url || ''));
      }
      if (!this.isWebNavigableUrl(url) && this.currentTask?.url &&
          this.isWebNavigableUrl(this.currentTask.url)) {
        url = String(this.currentTask.url).trim();
      }
      if (!this.isWebNavigableUrl(url)) {
        this.addSystemMessage(
            `navigate requires an http(s) url. Received: ${JSON.stringify(raw)}`);
        return null;
      }
      return {action: 'navigate', url};
    }

    return null;
  }

  private listSecretTokenNames(): string[] {
    const secrets = this.dashboardDataTokenCatalog?.secretTokenNames ?? [];
    if (secrets.length) {
      return secrets.slice();
    }
    return (this.dashboardDataTokenCatalog?.tokenNames ?? [])
        .filter((n) => /password|secret|token|key|pass/i.test(n));
  }

  private listVariableTokenNames(): string[] {
    const vars = this.dashboardDataTokenCatalog?.variableTokenNames ?? [];
    if (vars.length) {
      return vars.slice();
    }
    const secrets = new Set(
        this.listSecretTokenNames().map((n) => n.toLowerCase()));
    return (this.dashboardDataTokenCatalog?.tokenNames ?? [])
        .filter((n) => !secrets.has(n.toLowerCase()));
  }

  /** Variable tokens acceptable for email/username/login fields. */
  private listIdentityVariableTokens(): string[] {
    const vars = this.listVariableTokenNames();
    const matched = vars.filter(
        (n) => /email|e_?mail|user|login|gmail|account|id/i.test(n));
    return matched.length ? matched : vars;
  }

  /** Secret tokens acceptable for password fields. */
  private listPasswordSecretTokens(): string[] {
    const secrets = this.listSecretTokenNames();
    const matched = secrets.filter(
        (n) => /pass|secret|pin|credential/i.test(n));
    return matched.length ? matched : secrets;
  }

  private preferredTokenForGoal(candidates: string[], goal: string): string|null {
    if (!candidates.length) {
      return null;
    }
    const g = String(goal || '').toLowerCase();
    // Prefer an explicit {Token} mention in the goal, then a bare token name hit.
    for (const name of candidates) {
      const lower = name.toLowerCase();
      if (g.includes(`{${lower}}`)) {
        return name;
      }
    }
    for (const name of candidates) {
      const lower = name.toLowerCase();
      // Match LONESTAR against LONESTAR_PW / lonestar casino, etc.
      const base = lower.replace(/_(pw|password|pass|secret|pin)$/i, '');
      if (g.includes(lower) || (base.length >= 4 && g.includes(base))) {
        return name;
      }
    }
    // Task password field often carries the intended {TOKEN}.
    const taskPw = normalizeTokenPlaceholdersInText(
        String(this.currentTask?.password || '').trim());
    const taskToken = tokenNameFromPlaceholder(taskPw);
    if (taskToken) {
      const match = candidates.find(
          (n) => n.toLowerCase() === taskToken.toLowerCase());
      if (match) {
        return match;
      }
    }
    return null;
  }

  private classifyCredentialField(
      context: PageContextSummary|null|undefined,
      targetId: number): 'identity'|'password'|'other' {
    if (!context) {
      return 'other';
    }
    const el = this.getElementById(context, targetId);
    if (!el) {
      return 'other';
    }
    const haystack =
        `${el.label} ${el.placeholder ?? ''} ${el.fieldName ?? ''} ${
            describeNodeKind(el) ?? ''}`.toLowerCase();
    if (el.formType?.includes('INPUT_PASSWORD') ||
        /password|passcode|\bpin\b/.test(haystack)) {
      return 'password';
    }
    if (el.formType?.includes('INPUT_EMAIL') ||
        /e-?mail|username|user[\s_-]?name|user\s*id|\blogin\b/.test(haystack)) {
      return 'identity';
    }
    return 'other';
  }

  /**
   * Scalable credential policy: identity → variable {token}; password → secret {token}.
   * Field labels/placeholders are never valid typed values.
   */
  private enforceCredentialTypeText(
      text: string,
      targetId: number,
      context?: PageContextSummary|null): string {
    const trimmed = normalizeTokenPlaceholdersInText(String(text || '').trim());
    if (!trimmed) {
      return trimmed;
    }
    // Prefer the live page context; fall back to last snapshot so password
    // fields are still classified when normalize runs slightly out of band.
    const ctx = context ?? this.lastPageContext;
    let kind = this.classifyCredentialField(ctx, targetId);
    // Hard fallback: if the model is typing into a password-ish target id from
    // the last action summary / known password targets, treat as password.
    if (kind === 'other' && this.isPasswordTarget(ctx, targetId)) {
      kind = 'password';
    }
    if (kind === 'other') {
      return trimmed;
    }
    const goal = String(this.currentTask?.goal || '');
    const allowed = kind === 'password' ?
        this.listPasswordSecretTokens() :
        this.listIdentityVariableTokens();
    const preferred = this.preferredTokenForGoal(allowed, goal);
    const tokenName = tokenNameFromPlaceholder(trimmed);
    // Goal-named password token always wins over model guess / wrong site secret.
    if (kind === 'password' && preferred) {
      if (!tokenName || tokenName.toLowerCase() !== preferred.toLowerCase()) {
        return `{${preferred}}`;
      }
      return `{${preferred}}`;
    }
    if (tokenName) {
      const match = allowed.find(
          (n) => n.toLowerCase() === tokenName.toLowerCase());
      if (match) {
        return `{${match}}`;
      }
      if (preferred) {
        return `{${preferred}}`;
      }
      return `{${tokenName}}`;
    }
    if (kind === 'identity' && looksLikeLiteralEmail(trimmed)) {
      return trimmed;
    }
    // Password fields: never allow plaintext. Force a secret token when known.
    if (kind === 'password' && allowed.length) {
      return `{${preferred || allowed[0]}}`;
    }
    if (preferred) {
      return `{${preferred}}`;
    }
    return trimmed;
  }

  private isPasswordTarget(
      context: PageContextSummary|null|undefined,
      targetId: number): boolean {
    if (!context) {
      return false;
    }
    const el = this.getElementById(context, targetId);
    if (!el) {
      return false;
    }
    const haystack = `${el.label} ${el.placeholder ?? ''} ${el.fieldName ?? ''}`.toLowerCase();
    if (el.formType?.includes('INPUT_PASSWORD') || haystack.includes('password')) {
      return true;
    }
    return false;
  }

  private isUsernameTarget(context: PageContextSummary, targetId: number): boolean {
    const el = this.getElementById(context, targetId);
    if (!el) {
      return false;
    }
    if (el.formType?.includes('INPUT_PASSWORD')) {
      return false;
    }
    const kind = describeNodeKind(el) ?? '';
    if (!kind.startsWith('input_')) {
      return false;
    }
    const haystack = `${el.label} ${el.placeholder ?? ''} ${el.fieldName ?? ''}`.toLowerCase();
    return haystack.includes('username') || haystack.includes('email') ||
        Boolean(el.formType?.includes('INPUT_EMAIL')) ||
        Boolean(el.formType?.includes('INPUT_TEXT'));
  }

  private findUsernameFieldId(context: PageContextSummary): number|null {
    const candidates = (context.elements ?? []).filter((el) => {
      if (el.disabled) {
        return false;
      }
      if (!el.focusable && !el.clickable) {
        return false;
      }
      if (el.formType?.includes('INPUT_PASSWORD')) {
        return false;
      }
      const kind = describeNodeKind(el) ?? '';
      if (!kind.startsWith('input_')) {
        return false;
      }
      const text = `${el.label} ${el.placeholder ?? ''} ${el.fieldName ?? ''}`.toLowerCase();
      return el.formType?.includes('INPUT_EMAIL') || text.includes('email') || text.includes('username');
    });
    if (!candidates.length) {
      return null;
    }
    candidates.sort((a, b) => scoreContextNode(b) - scoreContextNode(a) || a.id - b.id);
    return candidates[0]?.id ?? null;
  }

  private describeActionResult(code: number): string {
    switch (code) {
      case ACTION_RESULT.OK:
        return 'ok';
      case ACTION_RESULT.TAB_WENT_AWAY:
        return 'tab went away';
      case ACTION_RESULT.INVALID_DOM_NODE_ID:
        return 'invalid dom node id';
      case ACTION_RESULT.ELEMENT_OFFSCREEN:
        return 'element offscreen';
      case ACTION_RESULT.TARGET_OBSCURED:
        return 'element obscured';
      case ACTION_RESULT.COORDINATES_OUT_OF_BOUNDS:
        return 'coordinates out of bounds';
      case ACTION_RESULT.FRAME_LOCATION_CHANGED:
        return 'frame location changed';
      case ACTION_RESULT.OBSERVED_TARGET_DESTROYED:
        return 'target destroyed';
      case ACTION_RESULT.OBSERVED_TARGET_CHANGED:
        return 'target changed';
      default:
        return `error ${code}`;
    }
  }

  private getElementById(context: PageContextSummary, targetId: number): ContextNode|null {
    for (const el of context.elements) {
      if (el.id === targetId) {
        return el;
      }
    }
    return null;
  }

  private getTargetLocators(context: PageContextSummary,
                            targetId: number): LocatorCandidate[] {
    const target = this.getElementById(context, targetId);
    if (!target || !Array.isArray(target.locators)) {
      return [];
    }
    const normalized: LocatorCandidate[] = [];
    const seen = new Set<string>();
    for (const loc of target.locators) {
      if (!loc || typeof loc !== 'object') {
        continue;
      }
      addLocator(
          normalized, seen, String(loc.type || ''), String(loc.value || ''), 12);
    }
    return normalized;
  }

  private buildActionForLogging(action: any,
                                context: PageContextSummary): any {
    const copy = JSON.parse(JSON.stringify(action || {}));
    if (typeof copy?.target_id === 'number') {
      const locators = this.getTargetLocators(context, copy.target_id);
      if (locators.length) {
        copy.locators = locators;
      }
      const target = this.getElementById(context, copy.target_id);
      if (target) {
        if (target.label) {
          copy.target_label = target.label;
        }
        if (target.formType) {
          copy.target_form_type = target.formType;
        }
        if (!copy.coordinate_hint) {
          const point = getActionablePoint(target);
          if (point) {
            copy.coordinate_hint = {
              x: Math.round(point.x),
              y: Math.round(point.y),
            };
          }
        }
      }
    }
    return copy;
  }

  private shouldFollowByEnter(action: any, context: PageContextSummary): boolean {
    if (!action || action.action !== 'type' || typeof action.target_id !== 'number') {
      return false;
    }
    const el = this.getElementById(context, action.target_id);
    if (!el) {
      return false;
    }
    const haystack = `${el.label} ${el.placeholder ?? ''} ${el.fieldName ?? ''}`.toLowerCase();
    if (el.formType?.includes('INPUT_PASSWORD') || haystack.includes('password')) {
      return true;
    }
    if (el.formType?.includes('INPUT_SEARCH') || haystack.includes('search')) {
      return true;
    }
    return false;
  }

  private looksLikeLoginPage(context: PageContextSummary): boolean {
    const elements = context.elements ?? [];
    for (const el of elements) {
      const kind = describeNodeKind(el) ?? '';
      const text = `${el.label} ${el.placeholder ?? ''} ${el.fieldName ?? ''}`.toLowerCase();
      if (kind.startsWith('input_') && (text.includes('password') || text.includes('username') || text.includes('email'))) {
        return true;
      }
      if (/(log in|login|sign in|signin)/.test(text) && (kind.includes('button') || el.clickable)) {
        return true;
      }
    }
    const inner = (context.innerText ?? '').toLowerCase();
    if (/(log in|login|sign in|signin)/.test(inner) && /(password|username|email)/.test(inner)) {
      return true;
    }
    return false;
  }

  /** User goal says creds are autofilled / do not type — honor over model habit. */
  private goalSaysCredentialsAutofilled(): boolean {
    const goal = String(this.currentTask?.goal || '').toLowerCase();
    if (!goal) {
      return false;
    }
    if (/(auto[- ]?fill|already filled|saved cred|password manager)/.test(goal) &&
        /(credential|password|email|username|login|log in)/.test(goal)) {
      return true;
    }
    if (/(don'?t|do not|no need to|shouldn'?t|should not).{0,40}(type|enter|input).{0,40}(credential|password|email|username)/.test(goal)) {
      return true;
    }
    if (/(just|only).{0,20}(click|press).{0,20}(log ?in|sign ?in)/.test(goal)) {
      return true;
    }
    return false;
  }

  private isCredentialFieldTarget(context: PageContextSummary, targetId: number):
      boolean {
    if (this.isPasswordTarget(context, targetId)) {
      return true;
    }
    const el = this.getElementById(context, targetId);
    if (!el) {
      return false;
    }
    const kind = describeNodeKind(el) ?? '';
    const haystack =
        `${el.label} ${el.placeholder ?? ''} ${el.fieldName ?? ''}`.toLowerCase();
    if (kind.includes('input_email') || kind.includes('input_password')) {
      return true;
    }
    return /(email|e-mail|username|user id|password|passcode)/.test(haystack);
  }

  private findLoginButtonId(context: PageContextSummary): number|null {
    const candidates = (context.elements ?? []).filter((el) => {
      const kind = describeNodeKind(el) ?? '';
      if (!kind.includes('button') && !el.clickable) {
        return false;
      }
      const text = `${el.label} ${el.placeholder ?? ''}`.toLowerCase();
      return /(log in|login|sign in|signin)/.test(text);
    });
    if (!candidates.length) {
      return null;
    }
    candidates.sort((a, b) => scoreContextNode(b) - scoreContextNode(a) || a.id - b.id);
    return candidates[0]?.id ?? null;
  }

  private parseCoordinatePoint(raw: any): {x: number, y: number}|null {
    if (!raw || typeof raw !== 'object') {
      return null;
    }
    const fromPair = (value: any): {x: number, y: number}|null => {
      if (Array.isArray(value) && value.length >= 2) {
        const x = Number(value[0]);
        const y = Number(value[1]);
        if (Number.isFinite(x) && Number.isFinite(y)) {
          return {x: Math.round(x), y: Math.round(y)};
        }
        return null;
      }
      if (value && typeof value === 'object') {
        const x = Number(value.x ?? value.left);
        const y = Number(value.y ?? value.top);
        if (Number.isFinite(x) && Number.isFinite(y)) {
          return {x: Math.round(x), y: Math.round(y)};
        }
      }
      return null;
    };
    const fromNamed = fromPair(raw.coordinate ?? raw.coordinates);
    if (fromNamed) {
      return fromNamed;
    }
    if (raw.x != null && raw.y != null) {
      const x = Number(raw.x);
      const y = Number(raw.y);
      if (Number.isFinite(x) && Number.isFinite(y)) {
        return {x: Math.round(x), y: Math.round(y)};
      }
    }
    return null;
  }

  private buildCoordinateFallbackAction(action: any, context: PageContextSummary): any|null {
    if (!action || typeof action.target_id !== 'number') {
      return null;
    }
    const el = this.getElementById(context, action.target_id);
    const point = getActionablePoint(el);
    if (!point) {
      return null;
    }
    const x = Math.round(point.x);
    const y = Math.round(point.y);
    if (!Number.isFinite(x) || !Number.isFinite(y)) {
      return null;
    }
    return {...action, coordinate: {x, y}};
  }

  private buildTimelineActionForRecorder(action: any,
                                         context: PageContextSummary): any {
    const kind = String(action?.action || '').trim() || 'unknown';
    const timelineAction: any = {action: kind, params: {}};
    if (kind === 'hover') {
      // Dashboard actions model hover as click_action with click_count=0.
      timelineAction.action = 'click';
      timelineAction.params.click_count = 0;
      timelineAction.params.clickCount = 0;
    }
    if (typeof action?.target_id === 'number') {
      timelineAction.params.__glic_target_id = action.target_id;
      const target = this.getElementById(context, action.target_id);
      if (target) {
        timelineAction.params.__glic_target_label = target.label || '';
        if (target.formType) {
          timelineAction.params.__glic_target_form_type = target.formType;
        }
        // Click/hover/type all need locators or coordinates for schema
        // preflight. Prefer locators whenever the page context has them;
        // coordinates are a secondary targeting signal.
        const locators = this.getTargetLocators(context, action.target_id);
        if (locators.length) {
          timelineAction.params.locators = locators;
        } else if (kind === 'click' || kind === 'hover' || kind === 'type') {
          // Synthesize a text locator so target_id-only model actions still
          // pass preflight when bounds are missing (common for duplicate
          // labeled controls like "LOG IN").
          const label = String(target.label || '').trim();
          if (label) {
            timelineAction.params.locators = [{
              type: 'TEXT_CONTENT',
              value: label,
              priority: 12,
            }];
          }
        }
        const point = getActionablePoint(target);
        if (point) {
          timelineAction.params.coordinates = {
            x: Math.round(point.x),
            y: Math.round(point.y),
          };
        }
      }
    }
    if (context.documentId) {
      timelineAction.params.__glic_document_id = context.documentId;
    }
    if (typeof action?.text === 'string') {
      timelineAction.params.text = action.text;
    }
    if (typeof action?.key === 'string' && action.key.trim()) {
      timelineAction.params.key = action.key.trim();
    }
    if (typeof action?.count === 'number' && action.count > 0) {
      timelineAction.params.count = action.count;
    }
    if (typeof action?.distance === 'number') {
      timelineAction.params.distance = action.distance;
    }
    if (kind === 'navigate') {
      const navUrl = String(action?.url || action?.params?.url || '').trim();
      if (navUrl) {
        timelineAction.params.url = navUrl;
      }
    }
    if (action?.coordinate && typeof action.coordinate === 'object') {
      timelineAction.params.coordinates = action.coordinate;
    }
    if (action?.action === 'scroll_to' && typeof action?.target_id === 'number') {
      timelineAction.action = 'scroll';
      timelineAction.params = {
        ...timelineAction.params,
        percentY: 50,
        percentX: 50,
      };
    }
    return timelineAction;
  }

  private buildScaffoldBrief(
      action: any, context: PageContextSummary): GlicRecorderScaffoldEvent['brief'] {
    const kind = String(action?.action || '').trim() || 'unknown';
    const brief: NonNullable<GlicRecorderScaffoldEvent['brief']> = {action: kind};
    if (typeof action?.target_id === 'number') {
      brief.target_id = action.target_id;
      const el = this.getElementById(context, action.target_id);
      const desc = el ? (bestContextNodeText(el) || describeNodeKind(el) || '') : '';
      if (desc) {
        brief.desc = desc.slice(0, 80);
      }
    }
    if (typeof action?.text === 'string' && action.text.trim()) {
      const hay = `${brief.desc || ''} ${action.target_id || ''}`.toLowerCase();
      const targetNum = Number(action.target_id);
      const looksSecret = /password|passcode|secret|token/.test(hay) ||
          (Number.isFinite(targetNum) &&
           this.isPasswordTarget(context, targetNum));
      brief.text = looksSecret ? '[redacted]' : action.text.slice(0, 80);
    }
    if (typeof action?.key === 'string' && action.key.trim()) {
      brief.key = action.key.trim();
    }
    if (kind === 'navigate' && typeof action?.url === 'string') {
      brief.url = action.url.trim();
    } else if (context.url) {
      brief.url = context.url;
    }
    return brief;
  }

  private isScaffoldReviewCandidate(ev: GlicRecorderScaffoldEvent): boolean {
    const src = ev?.sourceAction;
    if (!src || typeof src !== 'object') {
      return false;
    }
    // Skip internal pre-type / autofill plumbing — not part of the reusable script.
    if (src.__pre_type_focus || src.__pre_type_select_all || src.__pre_type_clear ||
        src.__login_autofill_bypass) {
      return false;
    }
    const kind = String(src.action || ev.timelineAction?.action || '').toLowerCase();
    return kind === 'click' || kind === 'type' || kind === 'key_press' ||
        kind === 'navigate' || kind === 'scroll' || kind === 'scroll_to' ||
        kind === 'hover';
  }

  private async emitRecorderScaffoldAction(action: any,
                                           context: PageContextSummary,
                                           resultCode?: number): Promise<void> {
    if (!action || typeof action !== 'object') {
      return;
    }
    const sourceAction = JSON.parse(JSON.stringify(action));
    const timelineAction = this.buildTimelineActionForRecorder(sourceAction, context);
    const eventPayload: GlicRecorderScaffoldEvent = {
      taskId: this.currentTask?.id,
      timestampMs: Date.now(),
      status: (typeof resultCode === 'number' && resultCode === ACTION_RESULT.OK)
        ? 'ok'
        : 'unknown',
      url: context.url,
      documentId: context.documentId,
      sourceAction,
      timelineAction,
      brief: this.buildScaffoldBrief(sourceAction, context),
    };
    try {
      if (!Array.isArray(window.__glicRecorderScaffoldBuffer)) {
        window.__glicRecorderScaffoldBuffer = [];
      }
      window.__glicRecorderScaffoldBuffer.push(eventPayload);
      if (window.__glicRecorderScaffoldBuffer.length > 500) {
        window.__glicRecorderScaffoldBuffer.splice(
            0, window.__glicRecorderScaffoldBuffer.length - 500);
      }
    } catch (_) {}
    try {
      window.dispatchEvent(new CustomEvent('glic-agent-action-recorded', {
        detail: eventPayload,
      }));
    } catch (_) {}

    const bridge = window.__dashboardGlicRecorderBridge;
    if (bridge && typeof bridge.recordAction === 'function') {
      try {
        await Promise.resolve(bridge.recordAction({
          action: timelineAction,
          save: false,
          source: 'glic_task_loop',
          metadata: eventPayload,
        }));
      } catch (e) {
        console.warn('[GlicRecorder] bridge recordAction failed', e);
      }
    }
  }

  private async reviewScaffoldStepsWithModel(
      task: TaskItem,
      candidates: GlicRecorderScaffoldEvent[],
  ): Promise<number[]|null> {
    if (!candidates.length) {
      return [];
    }
    const steps = candidates.map((ev, i) => {
      const brief = ev.brief || this.buildScaffoldBrief(ev.sourceAction, {
        elements: [],
        url: ev.url,
      } as PageContextSummary);
      return {
        step: i,
        action: brief?.action || ev.timelineAction?.action || 'unknown',
        target_id: brief?.target_id,
        desc: brief?.desc,
        text: brief?.text,
        key: brief?.key,
        url: brief?.url || ev.url,
      };
    });
    const reviewPrompt =
        'Task succeeded. Review the recorded steps below (same compact id/desc ' +
        'style as page context). Prune retries, mistakes, and dead-ends. Keep the ' +
        'minimal ordered steps that still achieve the USER GOAL.\n' +
        `USER GOAL: ${task.goal}\n` +
        `Steps:\n${JSON.stringify(steps, null, 2)}\n` +
        'Respond with ONE JSON object only, same action field as other tools:\n' +
        '{"action":"complete","keep_steps":[0,2,5]}\n' +
        'keep_steps = 0-based indices into Steps to keep, in final script order. ' +
        'Omit noise. If all steps are needed, return every index.';

    if (this.llmRequestInFlight) {
      this.addSystemMessage(
          'Script prune skipped (model busy); keeping all candidate steps.');
      return null;
    }
    this.llmRequestInFlight = true;
    this.llmAbortController = new AbortController();
    this.updateLlmUiState();
    try {
      const selection = this.getSelectedProviderAndModel();
      const headers = this.buildChatRequestHeaders();
      headers['X-Glic-Task-Id'] = task.id;
      headers['X-Glic-Task'] = encodeTaskHeader(task);
      const messages = [
        {
          role: 'system' as const,
          content:
              'You are pruning an automation script timeline. Prefer fewer robust steps. ' +
              'Return JSON only with action:"complete" and keep_steps:[...].',
        },
        {role: 'user' as const, content: reviewPrompt},
      ];
      const assistantText =
          await this.fetchAssistantText(selection.model, headers, messages);
      this.messages.push({role: 'system', content: 'Script prune/review prompt sent.'});
      this.messages.push({role: 'assistant', content: assistantText});
      this.render();
      const parsed = extractJsonObjects(assistantText);
      for (const obj of parsed) {
        if (!obj || typeof obj !== 'object') {
          continue;
        }
        const keep = obj.keep_steps ?? obj.keepSteps ?? obj.keep ?? obj.steps;
        if (!Array.isArray(keep)) {
          continue;
        }
        const indices = keep
                            .map((v: unknown) => Number.parseInt(String(v), 10))
                            .filter(
                                (n: number) => Number.isFinite(n) && n >= 0 &&
                                    n < candidates.length);
        const seen = new Set<number>();
        const ordered: number[] = [];
        for (const n of indices) {
          if (seen.has(n)) {
            continue;
          }
          seen.add(n);
          ordered.push(n);
        }
        return ordered;
      }
      this.addSystemMessage(
          'Script prune review returned no keep_steps; keeping all candidate steps.');
      return null;
    } catch (e) {
      this.addSystemMessage(`Script prune review failed (${e}); keeping all steps.`);
      return null;
    } finally {
      this.llmRequestInFlight = false;
      this.llmAbortController = null;
      this.updateLlmUiState();
    }
  }

  private async flushRecorderScaffoldForTask(
      status: string, task?: TaskItem|null): Promise<void> {
    if (status !== 'success') {
      return;
    }
    const completed = task || null;
    const buffer = Array.isArray(window.__glicRecorderScaffoldBuffer) ?
        window.__glicRecorderScaffoldBuffer :
        [];
    const taskId = completed?.id || '';
    const relevant = buffer.filter((ev) => {
      if (!ev || typeof ev !== 'object') {
        return false;
      }
      if (!taskId) {
        return true;
      }
      return !ev.taskId || ev.taskId === taskId;
    });
    const candidates = relevant.filter((ev) => this.isScaffoldReviewCandidate(ev));
    let selected = candidates;
    if (completed && candidates.length > 0) {
      this.addSystemMessage(
          `Reviewing ${candidates.length} recorded step(s) for script prune/edit…`);
      const keep = await this.reviewScaffoldStepsWithModel(completed, candidates);
      if (keep && keep.length) {
        selected = keep.map((i) => candidates[i]!).filter(Boolean);
        this.addSystemMessage(
            `Pruned script timeline: kept ${selected.length}/${candidates.length} steps.`);
      } else if (keep && keep.length === 0) {
        this.addSystemMessage(
            'Model kept 0 steps; falling back to full candidate timeline.');
        selected = candidates;
      }
    }

    const actions = selected
                        .map((ev) => ev.timelineAction)
                        .filter((a) => a && typeof a === 'object' &&
                                    String(a.action || '') !== 'complete');
    // Attach compact brief alongside each enriched timeline action for debugging.
    const actionsWithBrief = selected.map((ev) => ({
      ...ev.timelineAction,
      params: {
        ...(ev.timelineAction?.params || {}),
        __glic_brief: ev.brief || null,
      },
    }));
    const toSave = actionsWithBrief.length ? actionsWithBrief : actions;

    if (toSave.length) {
      try {
        const res = await fetch('/task/save_script', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({
            task_id: taskId || `manual-${Date.now()}`,
            goal: completed?.goal || '',
            name: (completed?.goal || 'Agent task').slice(0, 80),
            status,
            actions: toSave,
            brief_steps: selected.map((ev, i) => ({index: i, ...(ev.brief || {})})),
          }),
        });
        if (res.ok) {
          const data = await res.json();
          this.addSystemMessage(
              `Saved agent script (${toSave.length} steps, enriched locators) → ${
                  data?.path || 'scripts/'}`);
        } else {
          this.addSystemMessage(
              `Script save failed: HTTP ${res.status} ${await res.text()}`);
        }
      } catch (e) {
        this.addSystemMessage(`Script save error: ${e}`);
      }
    } else {
      this.addSystemMessage(
          'Task succeeded but no scaffolded actions were buffered to save as a script.');
    }

    const bridge = window.__dashboardGlicRecorderBridge;
    if (bridge && typeof bridge.recordAction === 'function') {
      for (const action of toSave) {
        try {
          await Promise.resolve(bridge.recordAction({
            action,
            save: false,
            source: 'glic_task_complete',
          }));
        } catch (e) {
          console.warn('[GlicRecorder] bridge recordAction failed', e);
        }
      }
    }
    if (bridge && typeof bridge.saveCurrentScript === 'function') {
      try {
        await Promise.resolve(bridge.saveCurrentScript());
        this.addSystemMessage('Also saved via dashboard recorder bridge.');
      } catch (e) {
        console.warn('[GlicRecorder] bridge saveCurrentScript failed', e);
      }
    }
    try {
      if (taskId && Array.isArray(window.__glicRecorderScaffoldBuffer)) {
        window.__glicRecorderScaffoldBuffer =
            window.__glicRecorderScaffoldBuffer.filter(
                (ev) => ev?.taskId && ev.taskId !== taskId);
      } else if (!taskId) {
        window.__glicRecorderScaffoldBuffer = [];
      }
    } catch (_) {
    }
  }

  private shouldTryHybridFallback(action: any, resultCode: number): boolean {
    if (!this.hybridExecutionEnabled) {
      return false;
    }
    if (!action || typeof action !== 'object' || action.__hybrid_fallback) {
      return false;
    }
    const kind = String(action.action || '').toLowerCase();
    if (!(kind === 'click' || kind === 'hover' || kind === 'type')) {
      return false;
    }
    return resultCode === ACTION_RESULT.TARGET_OBSCURED ||
        resultCode === ACTION_RESULT.OBSERVED_TARGET_CHANGED ||
        resultCode === ACTION_RESULT.OBSERVED_TARGET_DESTROYED ||
        resultCode === ACTION_RESULT.CLICK_SUPPRESSED ||
        resultCode === ACTION_RESULT.FRAME_WENT_AWAY;
  }

  private async performDashboardFallbackAction(action: any,
                                               context: PageContextSummary,
                                               actorResultCode: number):
      Promise<boolean> {
    if (!this.hybridExecutionEnabled) {
      return false;
    }
    const bridge = window.__dashboardGlicRecorderBridge;
    if (!bridge || typeof bridge.executeAction !== 'function') {
      return false;
    }

    const sourceAction = {...action, __hybrid_fallback: true};
    const timelineAction = this.buildTimelineActionForRecorder(sourceAction, context);
    if (!timelineAction?.params || typeof timelineAction.params !== 'object') {
      return false;
    }

    if (action?.coordinate && typeof action.coordinate.x === 'number' &&
        typeof action.coordinate.y === 'number') {
      timelineAction.params.coordinates = {
        x: action.coordinate.x,
        y: action.coordinate.y,
      };
    } else if ((!Array.isArray(timelineAction.params.locators) ||
                timelineAction.params.locators.length === 0) &&
               typeof action?.target_id === 'number') {
      const coordinateFallback = this.buildCoordinateFallbackAction(action, context);
      if (coordinateFallback?.coordinate) {
        timelineAction.params.coordinates = coordinateFallback.coordinate;
      }
    }

    this.addSystemMessage(
        `Actor action failed (${this.describeActionResult(actorResultCode)}); ` +
        'trying dashboard locator fallback.');
    try {
      const response = await Promise.resolve(bridge.executeAction({
        action: timelineAction.action,
        params: timelineAction.params,
        wait_for_completion: true,
        tab_id: context.tabIdInt ?? this.getCurrentTabIdInt(),
        completion_timeout_ms: 120000,
      }));
      const ok = !!response?.ok && response?.success !== false;
      await this.logTaskAction('fallback', {
        task_id: this.actorTaskId,
        tab_id: context.tabIdInt ?? this.getCurrentTabIdInt(),
        actor_result_code: actorResultCode,
        fallback_engine: 'dashboard_script_executor',
        fallback_response: response || null,
        action: this.buildActionForLogging(sourceAction, context),
        timeline_action: timelineAction,
      });
      if (!ok) {
        return false;
      }
      this.lastActionSummary = `${action.action} target_id=${action.target_id ?? ''} -> dashboard fallback completed`;
      await this.emitRecorderScaffoldAction(sourceAction, context, ACTION_RESULT.OK);
      await sleep(700);
      return true;
    } catch (e) {
      await this.logTaskAction('fallback_error', {
        task_id: this.actorTaskId,
        tab_id: context.tabIdInt ?? this.getCurrentTabIdInt(),
        actor_result_code: actorResultCode,
        fallback_engine: 'dashboard_script_executor',
        action: this.buildActionForLogging(sourceAction, context),
        error: String(e),
      });
      return false;
    }
  }

  private async performActionOnce(action: any, context: PageContextSummary):
      Promise<{resultCode?: number}|null> {
    if (this.taskLoopStopRequested) {
      return null;
    }
    const actionForLogging = this.buildActionForLogging(action, context);
    const timelineAction = this.buildTimelineActionForRecorder(action, context);
    if (this.supportsUnifiedAgentOrchestrator()) {
      let tabId = this.currentTask ? this.getCurrentTabIdInt() : context.tabIdInt;
      if (!Number.isFinite(tabId)) {
        const parsed = Number.parseInt(
            String(
                (this.currentTask ? this.getAuthoritativeTaskTabId() : null) ??
                context.tabId ?? this.currentTabId ?? ''), 10);
        tabId = Number.isFinite(parsed) ? parsed : undefined;
      }
      if (!Number.isFinite(tabId)) {
        this.addSystemMessage('No valid tab id available for executor action.');
        return null;
      }
      if (this.currentTask) {
        this.bindTaskTab(
            String(tabId),
            context.url ?? this.workingTabUrl ?? null,
            context.title ?? this.workingTabTitle ?? this.currentTask?.goal ?? null);
      } else {
        this.currentTabId = String(context.tabId || tabId);
        this.setWorkingContext(
            this.currentTabId,
            context.url ?? this.workingTabUrl ?? null,
            context.title ?? this.workingTabTitle ?? null);
      }
      if (action?.coordinate && typeof action.coordinate.x === 'number' &&
          typeof action.coordinate.y === 'number') {
        timelineAction.params.coordinates = {
          x: action.coordinate.x,
          y: action.coordinate.y,
        };
      } else if ((!Array.isArray(timelineAction.params?.locators) ||
                  timelineAction.params.locators.length === 0) &&
                 typeof action?.target_id === 'number') {
        const coordinateFallback = this.buildCoordinateFallbackAction(
            action, context);
        if (coordinateFallback?.coordinate) {
          timelineAction.params.coordinates = coordinateFallback.coordinate;
        }
      }
      void this.logTaskAction('requested', {
        engine: 'unified_executor',
        run_id: this.unifiedRunId,
        tab_id: tabId,
        action: actionForLogging,
        timeline_action: timelineAction,
        document_id: context.documentId,
      });
      const result = await this.executeUnifiedExecutorTool('executor.run_action', {
        action: timelineAction,
        tab_id: tabId,
        wait_for_completion: true,
      });
      await this.logTaskAction(result.ok ? 'result' : 'error', {
        engine: 'unified_executor',
        run_id: this.unifiedRunId,
        tab_id: tabId,
        action: actionForLogging,
        timeline_action: timelineAction,
        message: result.message,
      });
      this.lastActionSummary = `${action.action} target_id=${action.target_id ?? ''} -> ${
          result.ok ? 'executor_completed' : result.message}`;
      if (!result.ok) {
        this.addSystemMessage(`Action execution failed: ${result.message}`);
        return null;
      }
      if (action?.action === 'navigate' && typeof action?.url === 'string' &&
          action.url.trim()) {
        this.setWorkingContext(
            this.currentTabId,
            action.url.trim(),
            this.currentTask?.goal ?? this.workingTabTitle);
      }
      await this.emitRecorderScaffoldAction(action, context, ACTION_RESULT.OK);
      return {resultCode: ACTION_RESULT.OK};
    }
    if (!this.browser?.performActions) {
      this.addSystemMessage('performActions unavailable in host.');
      return null;
    }
    if (!this.actorTaskId) {
      this.addSystemMessage('No actor task id; cannot execute actions.');
      return null;
    }
    let tabId = this.currentTask ? this.getCurrentTabIdInt() : context.tabIdInt;
    if (!Number.isFinite(tabId)) {
      const parsed = Number.parseInt(
          String(
              (this.currentTask ? this.getAuthoritativeTaskTabId() : null) ??
              context.tabId ?? this.currentTabId ?? ''), 10);
      tabId = Number.isFinite(parsed) ? parsed : undefined;
    }
    if (!Number.isFinite(tabId)) {
      this.addSystemMessage('No valid tab id available for action.');
      return null;
    }
    if (this.currentTask) {
      this.bindTaskTab(
          String(tabId),
          context.url ?? this.workingTabUrl ?? null,
          context.title ?? this.workingTabTitle ?? this.currentTask?.goal ?? null);
    } else {
      this.currentTabId = String(context.tabId || tabId);
      this.setWorkingContext(
          this.currentTabId,
          context.url ?? this.workingTabUrl ?? null,
          context.title ?? this.workingTabTitle ?? null);
    }
    const payload = {
      task_id: this.actorTaskId,
      tab_id: tabId,
      action: actionForLogging,
      timeline_action: timelineAction,
      document_id: context.documentId,
    };
    void this.logTaskAction('requested', payload);
    try {
      const res = await fetch('/task/build_action', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload),
      });
      if (!res.ok) {
        this.addSystemMessage(`Action build failed: ${res.status} ${res.statusText}`);
        return null;
      }
      const data = await res.json();
      const base64 = data?.base64;
      if (typeof base64 !== 'string') {
        this.addSystemMessage('Action build returned empty payload.');
        return null;
      }
      const bytes = decodeBase64ToBytes(base64);
      const resultBytes = await this.browser.performActions(bytes.buffer as ArrayBuffer);
      const resultArray = new Uint8Array(resultBytes as ArrayBuffer);
      const logged = await this.logTaskAction('result', {
        task_id: this.actorTaskId,
        tab_id: tabId,
        action: actionForLogging,
        timeline_action: timelineAction,
        base64: encodeBytesToBase64(resultArray),
      });
      const code = (typeof logged?.action_result_code === 'number')
        ? logged.action_result_code
        : undefined;
      if (typeof code === 'number') {
        this.lastActionSummary = `${action.action} target_id=${action.target_id ?? ''} -> ${this.describeActionResult(code)}`;
      } else {
        this.lastActionSummary = `${action.action} target_id=${action.target_id ?? ''} -> completed`;
      }
      if (typeof code !== 'number' || code === ACTION_RESULT.OK) {
        if (action?.action === 'navigate' && typeof action?.url === 'string' &&
            action.url.trim()) {
          this.setWorkingContext(
              this.currentTabId,
              action.url.trim(),
              this.currentTask?.goal ?? this.workingTabTitle);
        }
        await this.emitRecorderScaffoldAction(action, context, code);
      }
      return {
        resultCode: code,
      };
    } catch (e) {
      await this.logTaskAction('error', {
        task_id: this.actorTaskId,
        tab_id: tabId,
        action: actionForLogging,
        timeline_action: timelineAction,
        error: String(e),
      });
      this.addSystemMessage(`Action execution failed: ${e}`);
      return null;
    }
  }

  // selectAllBeforeType removed (Meta/Ctrl+A typed literal "a" then Backspace).

  private async performAction(action: any, context: PageContextSummary): Promise<boolean> {
    if (action?.action === 'navigate') {
      const url = String(action.url || '').trim();
      if (!this.isWebNavigableUrl(url)) {
        this.addSystemMessage(`navigate requires an http(s) url, got: ${url || '(empty)'}`);
        return false;
      }
      this.addSystemMessage(`Navigating task tab to ${url}`);
      try {
        await this.openTaskUrl(url);
        this.lastActionSummary = `navigate ${url} -> opened`;
        await this.emitRecorderScaffoldAction(action, context, ACTION_RESULT.OK);
        return true;
      } catch (e) {
        this.lastActionSummary = `navigate ${url} -> ${e}`;
        this.addSystemMessage(`Navigate failed: ${e}`);
        return false;
      }
    }
    // Hard override: when the user said creds are autofilled, never type into
    // login fields — click Log In / Enter instead (model often ignores the goal).
    if (action?.action === 'type' && typeof action.target_id === 'number' &&
        !action.__login_autofill_bypass &&
        this.goalSaysCredentialsAutofilled() &&
        this.looksLikeLoginPage(context) &&
        this.isCredentialFieldTarget(context, action.target_id)) {
      const loginButtonId = this.findLoginButtonId(context);
      this.addSystemMessage(
          'Goal says credentials are autofilled; skipping type and submitting login.');
      if (loginButtonId !== null) {
        return this.performAction(
            {action: 'click', target_id: loginButtonId, __login_autofill_bypass: true},
            context);
      }
      return this.performAction(
          {action: 'key_press', key: 'Enter', __login_autofill_bypass: true},
          context);
    }
    // Pre-type Meta/Ctrl+A + Backspace removed (was typing literal "a").
    if (action?.action === 'type' && typeof action.target_id === 'number') {
      if (this.looksLikeLoginPage(context) &&
          this.currentTask?.username &&
          this.currentTask?.password &&
          this.isPasswordTarget(context, action.target_id) &&
          !this.typedUsername) {
        const usernameFieldId = this.findUsernameFieldId(context);
        if (usernameFieldId && usernameFieldId !== action.target_id) {
          this.addSystemMessage('Typing username/email before password.');
          const forcedUsernameResult = await this.performActionOnce(
              {action: 'type', target_id: usernameFieldId, text: this.currentTask.username, __login_forced: true},
              context,
          );
          if (!forcedUsernameResult ||
              forcedUsernameResult.resultCode !== ACTION_RESULT.OK) {
            return false;
          }
          this.typedUsername = true;
          return true;
        }
      }
    }
    if (this.shouldFollowByEnter(action, context)) {
      action = {...action, follow_by_enter: true};
    }
    const result = await this.performActionOnce(action, context);
    if (!result) {
      const failedSummary = String(this.lastActionSummary || '').toLowerCase();
      if (this.currentTask && this.isWebNavigableUrl(this.currentTask.url) &&
          (failedSummary.includes('tab not eligible') ||
           failedSummary.includes('tab not found') ||
           this.isNonActuableBrowserUrl(String(context?.url || '')))) {
        // Prefer rebinding to an existing site tab; only then open a new one.
        if (!(await this.rebindToExistingDestinationTab(this.currentTask.url))) {
          await this.ensureTaskOnDestinationUrl(
              this.currentTask.url,
              'Recovering from ineligible/chrome tab after failed action');
        } else {
          this.addSystemMessage(
              'Recovering from ineligible tab: rebound to an existing destination tab (no new tab).');
        }
      }
      return false;
    }
    const resultCode = result.resultCode;
    let actionSucceeded = typeof resultCode !== 'number' ||
        resultCode === ACTION_RESULT.OK;
    let fallbackCode = resultCode;
    if (typeof resultCode === 'number' && resultCode !== ACTION_RESULT.OK) {
      this.addSystemMessage(`Action failed: ${this.describeActionResult(resultCode)}.`);
      if ((resultCode === ACTION_RESULT.ELEMENT_OFFSCREEN ||
           resultCode === ACTION_RESULT.TARGET_OBSCURED) &&
          action.action !== 'scroll_to' &&
          !action.__retry) {
        this.addSystemMessage('Attempting scroll-into-view then retrying action.');
        const scrolled = await this.performActionOnce(
            {action: 'scroll_to', target_id: action.target_id},
            context,
        );
        if (scrolled && scrolled.resultCode === ACTION_RESULT.OK) {
          await sleep(1000);
        }
        const retryAction = {...action, __retry: true};
        const retryResult = await this.performActionOnce(retryAction, context);
        if (retryResult && retryResult.resultCode === ACTION_RESULT.OK) {
          actionSucceeded = true;
          fallbackCode = retryResult.resultCode;
        } else {
          if (typeof retryResult?.resultCode === 'number') {
            fallbackCode = retryResult.resultCode;
          }
          const coordinateFallback = this.buildCoordinateFallbackAction(action, context);
          if (coordinateFallback) {
            this.addSystemMessage('Attempting coordinate fallback for action.');
            const coordinateResult = await this.performActionOnce(
                coordinateFallback, context);
            if (coordinateResult && coordinateResult.resultCode === ACTION_RESULT.OK) {
              actionSucceeded = true;
              fallbackCode = coordinateResult.resultCode;
            } else if (typeof coordinateResult?.resultCode === 'number') {
              fallbackCode = coordinateResult.resultCode;
            }
          }
        }
      }
      if (!actionSucceeded && typeof fallbackCode === 'number' &&
          this.shouldTryHybridFallback(action, fallbackCode)) {
        actionSucceeded = await this.performDashboardFallbackAction(
            action, context, fallbackCode);
      }
    }

    if (!actionSucceeded) {
      return false;
    }
    if (action?.action === 'type' && typeof action.target_id === 'number') {
      if (this.isUsernameTarget(context, action.target_id)) {
        this.typedUsername = true;
      }
    }

    if (action.action === 'type' && action.follow_by_enter) {
      const enterResult = await this.performActionOnce(
          {action: 'key_press', key: 'Enter', __after_type_submit: true},
          context);
      if (!enterResult || enterResult.resultCode !== ACTION_RESULT.OK) {
        this.addSystemMessage('Follow-up Enter key press failed after typing.');
        return false;
      }
      // If enter-submit didn't transition away from the login state, try one login button click.
      await sleep(2500);
      const post = await this.collectContext();
      if (this.looksLikeLoginPage(post)) {
        const loginButtonId = this.findLoginButtonId(post);
        if (loginButtonId) {
          this.addSystemMessage('Enter submit may not have logged in; trying a Log In/Sign In button click.');
          await this.performActionOnce({action: 'click', target_id: loginButtonId, __login_fallback: true}, post);
        }
      }
    }
    return actionSucceeded;
  }

  private async handleAssistantActions(responseText: string,
                                       context: PageContextSummary): Promise<ActionHandlingResult> {
    const actions = this.extractActions(responseText);
    if (actions.length === 0) {
      return {handled: false, requestContext: false};
    }
    for (const action of actions) {
      const normalized = this.normalizeAction(action);
      if (!normalized) {
        continue;
      }
      if (normalized.action === 'complete' ||
          normalized.action === 'task_complete') {
        if (!this.currentTask) {
          this.addSystemMessage(
              'Task completion JSON is only meaningful during an active queued task loop.');
          return {handled: true, requestContext: false};
        }
        const success = normalized.success !== false;
        const reason = typeof normalized.reason === 'string'
            ? normalized.reason
            : undefined;
        await this.finishTask(success ? 'success' : 'failure', reason);
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'list_tabs' ||
          normalized.action === 'peek_tab' ||
          normalized.action === 'refresh_tab' ||
          (normalized.action === 'request_context' &&
           (normalized.tab_id || normalized.url || normalized.url_host ||
            normalized.query || normalized.reload))) {
        if (this.shouldEnforceHeadlessOnlyForCurrentTask() &&
            normalized.action !== 'list_tabs') {
          await this.requestHeadlessInvestigation({url: this.currentTask?.url});
          return {handled: true, requestContext: false};
        }
        const cross = await this.handleCrossTabContextAction(normalized);
        if (normalized.action === 'request_context' && cross.requestContext) {
          await this.captureScreenshotForNextTurn('with cross-tab request_context');
        }
        return cross;
      }
      if (normalized.action === 'request_context') {
        if (this.shouldEnforceHeadlessOnlyForCurrentTask()) {
          await this.requestHeadlessInvestigation({url: this.currentTask?.url});
          return {handled: true, requestContext: false};
        }
        if (!this.currentTask) {
          const refreshed = await this.collectContextWithRetry();
          const refreshedCount = refreshed.elements?.length ?? 0;
          const refreshedUrl = String(refreshed.url || '').trim();
          this.addSystemMessage(
              `Refreshed context snapshot (${refreshedCount} candidate elements${refreshedUrl ? `, url=${refreshedUrl}` : ''}).`);
        }
        // Force a screenshot whenever context is requested (Anthropic vision smoke test).
        await this.captureScreenshotForNextTurn('with request_context');
        return {handled: true, requestContext: true};
      }
      if (normalized.action === 'request_screenshot') {
        if (!this.supportsScreenshotAction()) {
          this.addSystemMessage(
              'request_screenshot is only available with Anthropic models.');
          return {handled: true, requestContext: false};
        }
        if (this.shouldEnforceHeadlessOnlyForCurrentTask()) {
          await this.requestHeadlessInvestigation({url: this.currentTask?.url});
          return {handled: true, requestContext: false};
        }
        await this.captureScreenshotForNextTurn('request_screenshot');
        return {handled: true, requestContext: true};
      }
      if (normalized.action === 'delegate_profile') {
        if (!this.supportsUnifiedAgentOrchestrator()) {
          this.addSystemMessage(
              'delegate_profile requires unified orchestrator support in the host.');
          return {handled: true, requestContext: false};
        }
        const target = this.normalizeUnifiedProfile(normalized.target_profile);
        if (!target) {
          this.addSystemMessage(
              'delegate_profile target is invalid.');
          return {handled: true, requestContext: false};
        }
        if (target === this.unifiedProfile) {
          this.addSystemMessage(
              `delegate_profile ignored: already using ${target}.`);
          return {handled: true, requestContext: false};
        }
        if (this.currentTask && this.unifiedProfileHopUsed) {
          this.addSystemMessage(
              `delegate_profile ignored: one profile handoff per task is allowed (already switched to ${this.unifiedProfile}).`);
          return {handled: true, requestContext: false};
        }
        const reason = typeof normalized.reason === 'string' &&
            normalized.reason.trim() ? ` reason=${normalized.reason.trim()}` : '';
        this.setUnifiedProfile(target, 'assistant_delegate',
                               {persist: true, emitMessage: false});
        if (this.currentTask) {
          this.unifiedProfileHopUsed = true;
        }
        this.addSystemMessage(
            `Delegated to unified profile ${target}.${reason}`);
        return {handled: true, requestContext: true};
      }
      if (normalized.action === 'request_console_logs') {
        await this.requestConsoleLogs(normalized.level, normalized.limit);
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'investigate_page') {
        await this.requestHeadlessInvestigation(normalized);
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'wait') {
        await sleep(normalized.waitMs);
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'navigate' || normalized.action === 'click' ||
          normalized.action === 'hover' || normalized.action === 'type' ||
          normalized.action === 'key_press' || normalized.action === 'scroll' ||
          normalized.action === 'scroll_to') {
        if (this.taskLoopStopRequested) {
          this.addSystemMessage('Skipped action because the agent loop is paused.');
          return {handled: true, requestContext: false};
        }
        if (this.shouldEnforceHeadlessOnlyForCurrentTask()) {
          this.addSystemMessage(
              `Profile ${this.unifiedProfile} is headless-only for active tasks. Use investigate_page or delegate_profile to browser_operator for foreground interaction.`);
          return {handled: true, requestContext: false};
        }
        if (!this.currentTask) {
          this.addSystemMessage(
              'Executable tab actions require an active queued task. In manual chat, use request_context / request_console_logs / investigate_page.');
          return {handled: true, requestContext: false};
        }
        if (normalized.action === 'navigate') {
          const ok = await this.performAction(normalized, context);
          return {handled: ok, requestContext: true};
        }
        if (typeof normalized.target_id === 'number' &&
            !this.getElementById(context, normalized.target_id)) {
          this.addSystemMessage(
              `Invalid target_id ${normalized.target_id}: not present in current Page context elements[].id list.`);
          continue;
        }
        const ok = await this.performAction(normalized, context);
        return {handled: ok, requestContext: !ok};
      }
    }
    return {handled: false, requestContext: false};
  }

  private static readonly PROMPT_HISTORY_KEY = 'glic.prompt_history';
  private static readonly PROMPT_HISTORY_MAX = 50;

  private loadPromptHistory(): void {
    try {
      const raw = localStorage.getItem(OllamaWebClient.PROMPT_HISTORY_KEY);
      if (!raw) {
        this.promptHistory = [];
        return;
      }
      const parsed = JSON.parse(raw);
      if (!Array.isArray(parsed)) {
        this.promptHistory = [];
        return;
      }
      this.promptHistory = parsed
                              .map((v) => String(v || '').trim())
                              .filter((v) => !!v)
                              .slice(0, OllamaWebClient.PROMPT_HISTORY_MAX);
    } catch (_) {
      this.promptHistory = [];
    }
    this.promptHistoryIndex = -1;
    this.promptHistoryDraft = '';
  }

  private persistPromptHistory(): void {
    const sliced =
        this.promptHistory.slice(0, OllamaWebClient.PROMPT_HISTORY_MAX);
    try {
      localStorage.setItem(
          OllamaWebClient.PROMPT_HISTORY_KEY, JSON.stringify(sliced));
    } catch (_) {
    }
    void this.persistClientPrefsToServer({prompt_history: sliced});
  }

  private pushPromptHistory(text: string): void {
    const trimmed = String(text || '').trim();
    if (!trimmed) {
      return;
    }
    // Newest-first; drop consecutive duplicates.
    if (this.promptHistory[0] === trimmed) {
      this.promptHistoryIndex = -1;
      this.promptHistoryDraft = '';
      return;
    }
    this.promptHistory = [
      trimmed,
      ...this.promptHistory.filter((entry) => entry !== trimmed),
    ].slice(0, OllamaWebClient.PROMPT_HISTORY_MAX);
    this.persistPromptHistory();
    this.promptHistoryIndex = -1;
    this.promptHistoryDraft = '';
  }

  private browsePromptHistory(direction: 'up'|'down'): void {
    const input = this.promptInputEl ||
        (document.getElementById('prompt') as HTMLInputElement | null);
    if (!input || !this.promptHistory.length) {
      return;
    }
    if (direction === 'up') {
      if (this.promptHistoryIndex < 0) {
        this.promptHistoryDraft = input.value;
        this.promptHistoryIndex = 0;
      } else if (this.promptHistoryIndex < this.promptHistory.length - 1) {
        this.promptHistoryIndex += 1;
      } else {
        return;
      }
    } else {
      if (this.promptHistoryIndex < 0) {
        return;
      }
      if (this.promptHistoryIndex === 0) {
        this.promptHistoryIndex = -1;
        input.value = this.promptHistoryDraft;
        input.setSelectionRange(input.value.length, input.value.length);
        return;
      }
      this.promptHistoryIndex -= 1;
    }
    const next = this.promptHistory[this.promptHistoryIndex] || '';
    input.value = next;
    input.setSelectionRange(next.length, next.length);
  }

  private async submitPromptText(
      text: string,
      options?: {clearInput?: boolean},
  ): Promise<void> {
    const trimmed = String(text || '').trim();
    if (!trimmed) {
      return;
    }
    this.pushPromptHistory(trimmed);
    const promptEl = this.promptInputEl ||
        (document.getElementById('prompt') as HTMLInputElement | null);
    if (options?.clearInput && promptEl) {
      promptEl.value = '';
    }
    const configuredHeadlessModel =
        this.isHeadlessPreferredUnifiedProfile() ?
        this.resolveHeadlessModelOverride() :
        '';
    const selection = this.getSelectedProviderAndModel();
    const selectedModel = configuredHeadlessModel || selection.model;
    const headers = this.buildChatRequestHeaders();

    let outgoingText = trimmed;
    if (this.attachContextForManualPrompts && !this.currentTask) {
      try {
        const context = await this.collectContextWithRetry();
        outgoingText = await this.maybeOptimizePrompt(
            trimmed, selectedModel, headers, context);
        if (await this.maybeStartManualTaskLoop(outgoingText, context)) {
          return;
        }
        await this.maybePrimeManualHeadlessInvestigation(outgoingText, context);
        const promptWithContext =
            this.buildManualPromptWithAttachedContext(outgoingText, context);
        const handleActions =
            !this.userLikelyWantsSimulachromeScript(outgoingText);
        await this.sendUserMessage(
            promptWithContext, {context, handleActions});
        return;
      } catch (e) {
        this.addSystemMessage(
            `Failed to attach live context; sending prompt without it: ${e}`);
      }
    }
    outgoingText = await this.maybeOptimizePrompt(
        outgoingText, selectedModel, headers);
    if (await this.maybeStartManualTaskLoop(outgoingText)) {
      return;
    }
    await this.maybePrimeManualHeadlessInvestigation(outgoingText);
    await this.sendUserMessage(outgoingText);
  }

  private async sendUserMessage(
      content: string,
      options?: {context?: PageContextSummary, handleActions?: boolean},
  ): Promise<string> {
    if (this.llmRequestInFlight) {
      this.addSystemMessage(MODEL_BUSY_MESSAGE);
      return MODEL_BUSY_MESSAGE;
    }
    this.llmRequestInFlight = true;
    this.llmAbortController = new AbortController();
    this.updateLlmUiState();
    this.ensureCoreAssistantContextPrompt();
    this.maybeEmitContextEvidence(options);
    this.messages.push({role: 'user', content});
    this.render();

    const configuredHeadlessModel =
        this.isHeadlessPreferredUnifiedProfile() ?
        this.resolveHeadlessModelOverride() :
        '';
    const selection = this.getSelectedProviderAndModel();
    const model = configuredHeadlessModel || selection.model;
    this.persistProviderModelSelection();

    const headers = this.buildChatRequestHeaders();
    if (this.currentTask) {
      headers['X-Glic-Task-Id'] = this.currentTask.id;
      headers['X-Glic-Task'] = encodeTaskHeader(this.currentTask);
    }

    let assistantText = '';
    try {
      assistantText = await this.fetchAssistantText(
          model, headers, this.messages.map(m => ({role: m.role, content: m.content})));
      assistantText = await this.maybeRepairExternalFrameworkResponse(
          content, assistantText, model, headers);
      assistantText = this.normalizeGeneratedScriptObject(content, assistantText);
    } finally {
      this.llmRequestInFlight = false;
      this.llmAbortController = null;
      this.updateLlmUiState();
    }

    const wasCanceled = assistantText === '[Canceled]' ||
        assistantText.startsWith('[Canceled]');
    if (wasCanceled) {
      this.taskLoopStopRequested = true;
      this.addSystemMessage('Model request canceled.');
      this.render();
      return assistantText;
    }

    this.messages.push({role: 'assistant', content: assistantText});
    this.render();

    if (this.taskLoopStopRequested) {
      return assistantText;
    }

    if (this.currentTask && this.maybeCompleteTask(assistantText)) {
      return assistantText;
    }
    if (options?.handleActions && options.context) {
      if (this.taskLoopStopRequested) {
        return assistantText;
      }
      const actionResult =
          await this.handleAssistantActions(assistantText, options.context);
      this.requestContextNextTurn = actionResult.requestContext;
      if (actionResult.requestContext) {
        this.addSystemMessage('Model requested a fresh page context snapshot.');
      }
      if (!actionResult.handled && this.currentTask) {
        const extracted = this.extractActions(assistantText);
        if (extracted.length > 0) {
          this.addSystemMessage(
              'Action JSON was returned, but execution did not complete successfully.');
        } else {
          const preview = String(assistantText || '').replace(/\s+/g, ' ').trim().slice(0, 180);
          const hadBraces = String(assistantText || '').includes('{');
          this.addSystemMessage(
              hadBraces
                  ? `No actionable JSON found after translation (raw preview: ${preview || '(empty)'}).`
                  : 'No actionable JSON found in model response.');
        }
      }
    }

    return assistantText;
  }

  private async logTaskAction(phase: string, payload: any): Promise<any|null> {
    try {
      const res = await fetch('/task/action', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({phase, ...payload}),
      });
      if (!res.ok) {
        return null;
      }
      return await res.json();
    } catch (_) {
      return null;
    }
  }
}

const client: GlicWebClient = new OllamaWebClient();
// Dev/test hook for CDP and manual inspection.
(window as any).__glicOllamaClient = client;

createGlicHostRegistryOnLoad().then((registry: GlicHostRegistry) => {
  void registry.registerWebClient(client);
});

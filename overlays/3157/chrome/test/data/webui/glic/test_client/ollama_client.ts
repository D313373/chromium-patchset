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
  overlay?: boolean,
  cssClass?: string,
  bounds?: {x: number, y: number, width: number, height: number},
  locators?: LocatorCandidate[],
  autofilled?: boolean,
  nonempty?: boolean,
  /** Live identity-field text. Never sent to the manager model. */
  fieldValue?: string,
};

type PageContextSummary = {
  tabId?: string,
  tabIdInt?: number,
  url?: string,
  title?: string,
  innerText?: string,
  documentId?: string,
  hasOverlay?: boolean,
  /** CSS/DIP viewport size from APC (click coordinate space). */
  viewportCss?: {width: number, height: number},
  elements: ContextNode[],
  /** Browser-side login plan. The credential flags stay off the prompt.
   *  `session` is the one fact the model is told. */
  session?: 'logged_in'|'logged_out',
  login?: {
    onLoginSurface: boolean,
    skipCredentialEntry: boolean,
    identityAutofilled: boolean,
    identityMatches: boolean,
    identityAlreadyPresent: boolean,
    passwordAlreadyPresent: boolean,
    needsIdentity: boolean,
    needsPassword: boolean,
    hasCaptcha: boolean,
    hasOtp: boolean,
    otpFilled: boolean,
    readyToSubmit: boolean,
  },
};


type TimelineStatus = 'SUCCESS'|'FAIL'|'STAGNANT';

interface TimelineEntry {
  timestamp: number;
  action_payload: Record<string, unknown>;
  url_before: string;
  url_after: string;
  dom_hash_before: string;
  dom_hash_after: string;
  status: TimelineStatus;
  agent_rationale: string;
  fingerprint: string;
  summary: string;
  epoch: number;
}

// Deterministic record of what the browser actually did. The model reads the
// last few successes; it does not keep its own plan.
const FLAT_AGENT_ACTION_SCHEMA = {"anyOf":[{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["click"]},"target_id":{"type":"integer","minimum":0},"coordinate":{"type":"object","additionalProperties":false,"required":["x","y"],"properties":{"x":{"type":"integer","minimum":0,"maximum":4000},"y":{"type":"integer","minimum":0,"maximum":4000}}}},"required":["action"]},{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["type"]},"target_id":{"type":"integer","minimum":0},"text":{"type":"string"}},"required":["action","target_id","text"]},{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["navigate"]},"url":{"type":"string"}},"required":["action","url"]},{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["scroll"]},"direction":{"type":"string","enum":["up","down"]},"distance":{"type":"integer","minimum":0,"maximum":4000},"scope":{"type":"string","enum":["page","iframe"]},"target_id":{"type":"integer","minimum":0}},"required":["action","direction","distance"]},{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["key_press"]},"key":{"type":"string"}},"required":["action","key"]},{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["wait"]},"timeout":{"type":"integer","minimum":0}},"required":["action","timeout"]},{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["request_screenshot"]}},"required":["action"]},{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["complete"]},"status":{"type":"string","enum":["solved","blocked"]},"summary":{"type":"string"}},"required":["action","status","summary"]}]};


const OPENAI_STRICT_STRIPPED = new Set([
  'minimum', 'maximum', 'minLength', 'maxLength', 'minItems', 'maxItems',
  'pattern', 'format',
]);

function lowerSchemaForOpenAIStrict(node: any): any {
  if (Array.isArray(node)) {
    return node.map(lowerSchemaForOpenAIStrict);
  }
  if (!node || typeof node !== 'object') {
    return node;
  }
  const required = new Set(Array.isArray(node.required) ? node.required : []);
  const out: any = {};
  for (const [key, value] of Object.entries(node)) {
    if (OPENAI_STRICT_STRIPPED.has(key) || key === 'properties' ||
        key === 'anyOf' || key === 'oneOf' || key === 'items' ||
        key === 'required' || key === 'additionalProperties') {
      continue;
    }
    out[key] = lowerSchemaForOpenAIStrict(value);
  }
  for (const key of ['anyOf', 'oneOf'] as const) {
    if (Array.isArray(node[key])) {
      out[key] = node[key].map(lowerSchemaForOpenAIStrict);
    }
  }
  if (node.items) {
    out.items = lowerSchemaForOpenAIStrict(node.items);
  }
  if (node.properties && typeof node.properties === 'object') {
    const properties: any = {};
    const nextRequired: string[] = [];
    for (const [name, property] of Object.entries(node.properties)) {
      const lowered = lowerSchemaForOpenAIStrict(property);
      properties[name] = required.has(name) ? lowered :
          {anyOf: [lowered, {type: 'null'}]};
      nextRequired.push(name);
    }
    out.properties = properties;
    out.required = nextRequired;
    out.additionalProperties = false;
  } else if ('additionalProperties' in node) {
    out.additionalProperties = false;
  }
  return out;
}

/** Dev-proxy /openai-chat does not pass through AgentModelContract. */
function compileOpenAICompatibleResponseFormat(provider: string) {
  const strict = provider === 'openai';
  const schema = strict ? lowerSchemaForOpenAIStrict(FLAT_AGENT_ACTION_SCHEMA) :
                          FLAT_AGENT_ACTION_SCHEMA;
  return {
    type: 'json_schema',
    json_schema: {
      name: 'simulachrome_agent_action',
      strict,
      schema: {
        type: 'object',
        additionalProperties: false,
        required: ['result'],
        properties: {result: schema},
      },
    },
  };
}

class ExecutionTimeline {
  readonly entries: TimelineEntry[] = [];
  private pending: TimelineEntry|null = null;
  private notes: string[] = [];
  private breakerEpoch = 0;
  private breakerNudges = 0;
  static readonly STAGNANT_BREAKER_LIMIT = 3;

  reset(): void {
    this.entries.length = 0;
    this.pending = null;
    this.notes = [];
    this.breakerEpoch = 0;
    this.breakerNudges = 0;
  }

  addNote(note: string): void {
    const text = String(note || '').trim();
    if (text) this.notes.push(text);
  }

  static hashDom(context: PageContextSummary): string {
    const rows = (context.elements || []).map((el) => [
      el.id,
      el.role || '',
      el.formType || '',
      String(el.label || '').replace(/\s+/g, ' ').trim().slice(0, 40),
      (el.locators && el.locators[0] ? el.locators[0].value : ''),
      el.disabled ? 1 : 0,
      el.overlay ? 1 : 0,
      el.nonempty ? 1 : 0,
      el.clickable ? 1 : 0,
    ].join('|'));
    rows.sort();
    const layout = (context.elements || []).slice(0, 16).map((el) => {
      const bounds = el.bounds;
      const x = bounds ? Math.round(bounds.x / 32) : 0;
      const y = bounds ? Math.round(bounds.y / 32) : 0;
      return `${el.id}@${x},${y},${el.visible === false ? 0 : 1}`;
    }).join(';');
    const body = `${rows.join('\n')}\n#${rows.length}|${context.hasOverlay ? 1 : 0}|${layout}`;
    let hash = 5381;
    for (let i = 0; i < body.length; i++) {
      hash = ((hash << 5) + hash + body.charCodeAt(i)) >>> 0;
    }
    return hash.toString(16);
  }

  static fingerprint(payload: any, context?: PageContextSummary): string {
    const action = String(payload?.action || payload?.op || '').toLowerCase();
    if (action === 'batch' || action === 'plan') {
      const steps = Array.isArray(payload.steps) ? payload.steps :
          (Array.isArray(payload.batch) ? payload.batch : []);
      const sig = steps.map((step: any) => {
        const row = step as any;
        return `${row?.op || row?.action || ''}:${row?.target || row?.from || ''}`;
      }).join(';');
      return `batch|${sig}`;
    }
    const id = Number(payload?.target_id ?? payload?.targetId);
    let elementKey = '';
    if (context && Number.isFinite(id)) {
      const el = (context.elements || []).find((node) => node.id === id);
      if (el) {
        const loc = el.locators && el.locators[0] ? el.locators[0].value : '';
        const label = String(el.label || el.fieldName || '')
                          .replace(/\s+/g, ' ')
                          .trim()
                          .slice(0, 40);
        elementKey = [el.role || '', el.formType || '', loc, label].join(':');
      }
    }
    const target = elementKey ||
        (Number.isFinite(id) ? String(id) :
                               String(payload?.target ?? payload?.target_id ?? ''));
    const coordinate = payload?.coordinate;
    let coord = '';
    if (Array.isArray(coordinate) && coordinate.length >= 2) {
      const x = Math.round(Number(coordinate[0]));
      const y = Math.round(Number(coordinate[1]));
      if (Number.isFinite(x) && Number.isFinite(y)) coord = `${x},${y}`;
    } else if (coordinate && typeof coordinate === 'object') {
      const x = Math.round(Number(coordinate.x));
      const y = Math.round(Number(coordinate.y));
      if (Number.isFinite(x) && Number.isFinite(y)) coord = `${x},${y}`;
    }
    const detail = action === 'type' ? String(payload?.text || '').slice(0, 32) :
        action === 'navigate' ? String(payload?.url || '').slice(0, 120) :
        action === 'key_press' ? String(payload?.key || '') :
        action === 'scroll' ?
            `${payload?.direction || ''}:${payload?.distance || ''}:${payload?.scope || ''}` :
            '';
    return [action, target, coord, detail].filter((part) => part !== '').join('|');
  }

  static describe(payload: any, context: PageContextSummary): string {
    const action = String(payload.action || payload.op || 'act').toLowerCase();
    const id = Number(payload.target_id ?? payload.targetId);
    const el = Number.isFinite(id) ?
        (context.elements || []).find((node) => node.id === id) :
        undefined;
    const locator = el?.locators?.[0]?.value || '';
    const label = String(el?.label || el?.fieldName || locator || '')
                      .replace(/\s+/g, ' ')
                      .trim()
                      .slice(0, 48);
    const coordinate = payload.coordinate;
    const coord = Array.isArray(coordinate) && coordinate.length >= 2 ?
        `(${coordinate[0]},${coordinate[1]})` :
        (coordinate && typeof coordinate === 'object' &&
                 coordinate.x != null && coordinate.y != null ?
             `(${coordinate.x},${coordinate.y})` :
             '');
    const where = label || (Number.isFinite(id) && id > 0 ? `id ${id}` : '') || coord ||
        'the page';
    if (action === 'batch' || action === 'plan') {
      const steps = Array.isArray(payload.steps) ? payload.steps : [];
      const first = (steps[0] || {}) as any;
      const op = String(first.op || first.action || 'step');
      const target = String(first.target || first.from || '').slice(0, 40);
      const extra = steps.length > 1 ? ` (+${steps.length - 1} more)` : '';
      return `${op} ${target}${extra}`.trim();
    }
    if (action === 'type') {
      const typed = String(payload.text || '');
      const secret = /password|secret|passcode/i.test(
          `${el?.formType || ''} ${el?.fieldName || ''} ${el?.label || ''}`);
      const shown = /^\{[A-Za-z0-9_]+\}$/.test(typed) ? typed :
          (!secret && typed ? `"${typed.slice(0, 24)}"` : 'a stored value');
      return `Typed ${shown} into ${where}`;
    }
    if (action === 'navigate') {
      return `Navigated to ${String(payload.url || '').slice(0, 80)}`;
    }
    if (action === 'scroll' || action === 'scroll_to') return `Scrolled ${where}`;
    if (action === 'key_press') return `Pressed ${String(payload.key || 'key')}`;
    if (action === 'hover') return `Hovered ${where}`;
    const verb = action === 'click' ? 'Clicked' :
        action === 'double_click' ? 'Double-clicked' :
        action === 'right_click' ? 'Right-clicked' :
        action;
    return `${verb} ${where}`;
  }

  private static rationaleOf(payload: any): string {
    const raw = payload.reason ?? payload.rationale ?? payload.agent_rationale;
    return typeof raw === 'string' ? raw.trim().slice(0, 240) : '';
  }

  private static isActuating(payload: any): boolean {
    const action = String(payload.action || payload.op || '').toLowerCase();
    return action === 'click' || action === 'double_click' ||
        action === 'right_click' || action === 'hover' || action === 'type' ||
        action === 'scroll' || action === 'scroll_to' || action === 'key_press' ||
        action === 'navigate' || action === 'drag' || action === 'batch' ||
        action === 'plan';
  }

  begin(payload: any, context: PageContextSummary): void {
    if (!ExecutionTimeline.isActuating(payload)) return;
    const copy = {...payload};
    this.pending = {
      timestamp: Date.now(),
      action_payload: copy,
      url_before: String(context.url || ''),
      url_after: '',
      dom_hash_before: ExecutionTimeline.hashDom(context),
      dom_hash_after: '',
      status: 'SUCCESS',
      agent_rationale: ExecutionTimeline.rationaleOf(copy),
      fingerprint: ExecutionTimeline.fingerprint(copy, context),
      epoch: this.breakerEpoch,
      summary: ExecutionTimeline.describe(copy, context),
    };
  }

  consecutiveStagnant(fingerprint?: string): number {
    let count = 0;
    let expected = fingerprint || '';
    for (let i = this.entries.length - 1; i >= 0; i--) {
      const entry = this.entries[i];
      if (!entry || entry.epoch !== this.breakerEpoch) break;
      if (entry.status !== 'STAGNANT' && entry.status !== 'FAIL') break;
      if (!expected) expected = entry.fingerprint;
      if (!entry.fingerprint || entry.fingerprint !== expected) break;
      count += 1;
    }
    return count;
  }

  static fallbackAction(nudge: number, context?: PageContextSummary): any {
    const slot = ((nudge % 3) + 3) % 3;
    if (slot === 1) return {action: 'wait', waitMs: 1200};
    if (slot === 2) {
      const url = String(context?.url || '');
      if (/^https?:/i.test(url)) return {action: 'reload', url};
    }
    return {action: 'scroll', distance: slot === 2 ? -480 : 640, scope: 'page'};
  }

  circuitOpen(payload: any, context?: PageContextSummary):
      {reason: string, fallback: any}|null {
    if (!ExecutionTimeline.isActuating(payload)) return null;
    const fingerprint = ExecutionTimeline.fingerprint(payload, context);
    const streak = this.consecutiveStagnant(fingerprint);
    if (!fingerprint || streak < ExecutionTimeline.STAGNANT_BREAKER_LIMIT) {
      return null;
    }
    const fallback = ExecutionTimeline.fallbackAction(this.breakerNudges, context);
    this.breakerNudges += 1;
    this.breakerEpoch += 1;
    const reason =
        `Stagnation circuit breaker: ${streak} consecutive no-effect actions ` +
        `on ${fingerprint}. Forcing ${fallback.action}.`;
    this.notes.push(reason);
    return {reason, fallback};
  }

  // Same control again after the hard stagnant limit. Under the limit the
  // action still runs so the streak can be recorded.
  rejects(payload: any, context?: PageContextSummary): string {
    if (!ExecutionTimeline.isActuating(payload)) return '';
    const fingerprint = ExecutionTimeline.fingerprint(payload, context);
    if (!fingerprint ||
        this.consecutiveStagnant(fingerprint) <
            ExecutionTimeline.STAGNANT_BREAKER_LIMIT) {
      return '';
    }
    this.notes.push('Your last action had no effect on the page. Do not repeat it.');
    return 'Skipped a repeat of an action that did not change the page.';
  }

  close(context: PageContextSummary, goal: string): string {
    const url = String(context.url || '');
    const hash = ExecutionTimeline.hashDom(context);
    let rollback = '';
    if (this.pending) {
      const changed = this.pending.url_before !== url ||
          this.pending.dom_hash_before !== hash;
      const dead = url.startsWith('chrome-error:');
      const status: TimelineStatus = dead ? 'FAIL' : (changed ? 'SUCCESS' : 'STAGNANT');
      this.entries.push({
        ...this.pending,
        url_after: url,
        dom_hash_after: hash,
        status,
      });
      this.pending = null;
      if (status === 'FAIL') {
        this.notes.push('That action failed. Do not repeat it.');
        if (dead) rollback = this.popFailed(url);
      } else if (status === 'STAGNANT') {
        const streak = this.consecutiveStagnant();
        if (streak >= ExecutionTimeline.STAGNANT_BREAKER_LIMIT) {
          this.notes.push(
              'Stagnation circuit breaker: the same action left the page unchanged ' +
              `${streak} times. The next repeat will scroll, wait, or reload.`);
        } else if (streak >= 2) {
          this.notes.push('Your last action had no effect on the page. Do not repeat it.');
        }
      }
    }
    if (this.entries.length > 48) {
      this.entries.splice(0, this.entries.length - 48);
    }
    const back = this.backtrackWarning(url, goal);
    if (back) this.notes.push(back);
    return rollback;
  }

  fail(context: PageContextSummary): string {
    const url = String(context.url || this.pending?.url_before || '');
    const hash = ExecutionTimeline.hashDom(context);
    if (this.pending) {
      this.entries.push({
        ...this.pending,
        url_after: url,
        dom_hash_after: hash,
        status: 'FAIL',
      });
      this.pending = null;
    }
    this.notes.push('That action failed. Do not repeat it.');
    return this.popFailed(url);
  }

  promptBlock(): string {
    const parts: string[] = [];
    const recent = this.recentSuccessText();
    if (recent) parts.push(recent);
    if (this.notes.length) {
      parts.push(this.notes.join(' '));
      this.notes = [];
    }
    return parts.length ? '\n' + parts.join('\n') : '';
  }

  private recentSuccessText(): string {
    const successes = this.entries.filter((entry) => entry.status === 'SUCCESS').slice(-5);
    if (!successes.length) return '';
    const body = successes.map((entry, index) => `${index + 1}. ${entry.summary}`).join(' -> ');
    return `Recent History: ${body}.`;
  }

  private popFailed(currentUrl: string): string {
    const tail = this.entries[this.entries.length - 1];
    const restore = tail && tail.status === 'FAIL' ? tail.url_before : '';
    return this.restoreUrl(currentUrl, restore);
  }

  private restoreUrl(currentUrl: string, fallback: string): string {
    const good = [...this.entries].reverse().find((entry) => entry.status === 'SUCCESS');
    const target = good?.url_after || fallback;
    if (target && target !== currentUrl) return target;
    return '';
  }

  private backtrackWarning(currentUrl: string, goal: string): string {
    if (this.entries.length < 2 || !currentUrl) return '';
    if (/\b(paginat\w*|next page|previous page|prev page|wizard|go back|step back)\b/i
            .test(goal)) {
      return '';
    }
    const earlier = this.entries[this.entries.length - 2];
    const latest = this.entries[this.entries.length - 1];
    if (earlier && latest && currentUrl === earlier.url_before &&
        latest.url_before !== currentUrl) {
      return 'You have navigated back to a previous page. Ensure you are moving forward toward the goal.';
    }
    return '';
  }
}


const MEMENTO_STORAGE_KEY = 'glic.agent_mementos';
const GLOBAL_MEMENTO_TASK_ID = '*';
const DEFAULT_TASK_MEMENTO_TTL_MS = 30 * 60 * 1000;
const MAX_MEMENTOS = 64;

interface AgentMemento {
  content: string;
  timestamp: number;
  taskId: string;
  ttl: number;
}

function looksLikeGlobalPreference(content: string): boolean {
  return /^(?:pref|preference|global)\s*:|^(?:always|never)\b/i.test(
      String(content || '').trim());
}

function mementoExpired(item: AgentMemento, now: number): boolean {
  if (!item.ttl || item.ttl <= 0) return false;
  return now - item.timestamp >= item.ttl;
}

function normalizeMementoEntry(entry: any, now: number): AgentMemento|null {
  if (typeof entry === 'string') {
    const content = entry.trim();
    if (!content) return null;
    const global = looksLikeGlobalPreference(content);
    return {
      content: content.slice(0, 2000),
      timestamp: now,
      taskId: global ? GLOBAL_MEMENTO_TASK_ID : '',
      ttl: global ? 0 : DEFAULT_TASK_MEMENTO_TTL_MS,
    };
  }
  if (!entry || typeof entry !== 'object') return null;
  const content = String(entry.content ?? entry.text ?? '').trim();
  if (!content) return null;
  const explicitGlobal = entry.global === true || entry.scope === 'global' ||
      entry.taskId === GLOBAL_MEMENTO_TASK_ID || looksLikeGlobalPreference(content);
  const taskId = explicitGlobal ? GLOBAL_MEMENTO_TASK_ID :
      String(entry.taskId || entry.task_id || '').trim();
  let ttl = Number(entry.ttl);
  if (!Number.isFinite(ttl) || ttl < 0) {
    ttl = explicitGlobal ? 0 : DEFAULT_TASK_MEMENTO_TTL_MS;
  }
  const timestamp = Number(entry.timestamp);
  return {
    content: content.slice(0, 2000),
    timestamp: Number.isFinite(timestamp) && timestamp > 0 ? timestamp : now,
    taskId,
    ttl,
  };
}

class MementoStore {
  private items: AgentMemento[] = [];

  constructor() {
    this.load();
  }

  private storage(): Storage|null {
    try {
      return localStorage;
    } catch (_) {
      return null;
    }
  }

  load(): void {
    const raw = this.storage()?.getItem(MEMENTO_STORAGE_KEY) || '';
    if (!raw) {
      this.items = [];
      return;
    }
    const now = Date.now();
    try {
      const parsed = JSON.parse(raw);
      const list = Array.isArray(parsed) ? parsed :
          (typeof parsed === 'string' ? [parsed] : []);
      this.items = list
                       .map((entry) => normalizeMementoEntry(entry, now))
                       .filter((entry): entry is AgentMemento => !!entry);
    } catch (_) {
      const one = normalizeMementoEntry(raw, now);
      this.items = one ? [one] : [];
    }
    this.dropExpired(now);
    this.persist();
  }

  write(content: string, options?: {taskId?: string, ttl?: number, global?: boolean}): boolean {
    const text = String(content || '').trim();
    if (!text) return false;
    const now = Date.now();
    this.dropExpired(now);
    const global = options?.global === true || looksLikeGlobalPreference(text);
    const taskId = global ? GLOBAL_MEMENTO_TASK_ID :
        String(options?.taskId || '').trim();
    let ttl = Number(options?.ttl);
    if (!Number.isFinite(ttl) || ttl < 0) {
      ttl = global ? 0 : DEFAULT_TASK_MEMENTO_TTL_MS;
    }
    this.items.push({
      content: text.slice(0, 2000),
      timestamp: now,
      taskId,
      ttl,
    });
    if (this.items.length > MAX_MEMENTOS) {
      const removable: number[] = [];
      this.items.forEach((item, index) => {
        if (item.taskId !== GLOBAL_MEMENTO_TASK_ID) removable.push(index);
      });
      const drop = new Set(removable.slice(0, this.items.length - MAX_MEMENTOS));
      this.items = this.items.filter((_, index) => !drop.has(index));
      if (this.items.length > MAX_MEMENTOS) {
        this.items = this.items.slice(this.items.length - MAX_MEMENTOS);
      }
    }
    this.persist();
    return true;
  }

  readText(taskId?: string): string {
    return this.live(taskId).map((item) => item.content).join('\n');
  }

  promptBlock(taskId?: string): string {
    const rows = this.live(taskId);
    if (!rows.length) return '';
    const lines: string[] = [];
    const prefs = rows.filter((item) => item.taskId === GLOBAL_MEMENTO_TASK_ID);
    const notes = rows.filter((item) => item.taskId !== GLOBAL_MEMENTO_TASK_ID);
    if (prefs.length) {
      lines.push('Global preferences:');
      for (const item of prefs) lines.push(`- ${item.content}`);
    }
    if (notes.length) {
      lines.push('Task notes:');
      for (const item of notes) lines.push(`- ${item.content}`);
    }
    return lines.join('\n');
  }

  /** Drop this task's notes. Global preferences and other tasks stay. */
  pruneCompletedTask(taskId?: string): number {
    const id = String(taskId || '').trim();
    const now = Date.now();
    const before = this.items.length;
    this.items = this.items.filter((item) => {
      if (mementoExpired(item, now)) return false;
      if (item.taskId === GLOBAL_MEMENTO_TASK_ID) return true;
      if (!item.taskId) return false;
      if (id && item.taskId === id) return false;
      return true;
    });
    const removed = before - this.items.length;
    if (removed) this.persist();
    return removed;
  }

  private live(taskId?: string): AgentMemento[] {
    const now = Date.now();
    const id = String(taskId || '').trim();
    return this.items.filter((item) => {
      if (mementoExpired(item, now)) return false;
      if (item.taskId === GLOBAL_MEMENTO_TASK_ID) return true;
      return !!id && item.taskId === id;
    });
  }

  private dropExpired(now: number): void {
    this.items = this.items.filter((item) => !mementoExpired(item, now));
  }

  private persist(): void {
    try {
      this.storage()?.setItem(MEMENTO_STORAGE_KEY, JSON.stringify(this.items));
    } catch (_) {}
  }
}

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


type AgentProvider = 'local'|'cerebras'|'gemini'|'openai'|'anthropic';

type ModelOption = {
  value: string,
  endpoint?: string,
  label?: string,
};

const AGENT_PROVIDERS: AgentProvider[] = [
  'local',
  'cerebras',
  'gemini',
  'openai',
  'anthropic',
];

const DEFAULT_MODELS_BY_PROVIDER: Record<AgentProvider, string> = {
  local: 'local-model',
  cerebras: 'qwen-3-235b-a22b-instruct-2507',
  gemini: 'gemini-2.5-pro',
  openai: 'gpt-4.1',
  anthropic: 'claude-haiku-4-5',
};

const DEFAULT_ENDPOINTS_BY_PROVIDER: Record<AgentProvider, string> = {
  local: 'http://127.0.0.1:11434/v1/chat/completions',
  cerebras: 'https://api.cerebras.ai/v1/chat/completions',
  gemini: 'https://generativelanguage.googleapis.com/v1beta',
  openai: 'https://api.openai.com/v1/chat/completions',
  anthropic: 'https://api.anthropic.com/v1/messages',
};

/** Weights dir for utility-process smolvlm2 (Manager eyes/hands). */
const DEFAULT_EMBEDDED_MODEL_DIR = '/Users/matcauthon/simulachrome_models';

const PROVIDER_MODEL_OPTIONS: Record<AgentProvider, ModelOption[]> = {
  local: [
    {value: 'ahmadwaqar/smolvlm2-agentic-gui:latest', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'qwen3-coder-next', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'qwen3.5:9b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'qwen3-vl:2b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'gemma4:e2b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'qwen3.5:0.8b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'functiongemma:latest', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'qwen2.5vl:7b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'qwen2.5vl:3b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'simula-vision-q8:latest', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'qwen3-coder-next:latest', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'qwen3:0.6b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'deepseek-r1:1.5b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'llama3:latest', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
    {value: 'gemma4:e4b', endpoint: 'http://127.0.0.1:11434/v1/chat/completions'},
  ],
  cerebras: [
    {value: 'qwen-3-235b-a22b-instruct-2507'},
    {value: 'llama3.1-8b'},
  ],
  gemini: [
    {value: 'gemini-flash-latest', label: 'gemini-flash-latest'},
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
  let provider = String(raw || '').trim().toLowerCase();
  if (provider === 'local') {
    provider = 'local';
  }
  return (AGENT_PROVIDERS as string[]).includes(provider) ?
      provider as AgentProvider :
      'anthropic';
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
  // Local OpenAI-compatible servers use arbitrary model ids (must mirror Settings).
  if (provider === 'local') {
    return candidate || DEFAULT_MODELS_BY_PROVIDER.local;
  }
  const options = PROVIDER_MODEL_OPTIONS[provider] || [];
  if (options.some((opt) => opt.value === candidate)) {
    return candidate;
  }
  return DEFAULT_MODELS_BY_PROVIDER[provider] || DEFAULT_MODELS_BY_PROVIDER.local;
}

function sanitizeModelForProvider(provider: AgentProvider, model: string): string {
  return normalizeModelForProvider(provider, model);
}

function modelOptionLabel(provider: AgentProvider, option: ModelOption): string {
  if (option.label) {
    return option.label;
  }
  if (provider === 'local' && option.endpoint === 'http://127.0.0.1:11434/v1/chat/completions') {
    return `${option.value} via 11434`;
  }
  return option.value;
}


// Task-loop prompts (goal + page context) and review prompts are model input;
// they stay in `messages` but are not rendered in the chat.
function isLoopContextMessage(m: ChatMessage): boolean {
  const text = String(m.content || '');
  if (m.role === 'system') {
    return text.startsWith('[Simulachrome Context]');
  }
  if (m.role !== 'user') {
    return false;
  }
  return /^\s*USER GOAL \(highest priority/.test(text) ||
      /(Return|Provide) the next action JSON only\./.test(text) ||
      text.startsWith('Task succeeded. Review the recorded steps');
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

    // Tie-breakers only. A label must not outrank a different control.
  const keywordBoosts: Array<[RegExp, number]> = [
    [/\b(LOG IN|LOGIN|SIGN IN|SIGN-IN|SIGNIN)\b/i, 12],
    [/\b(SIGN UP|REGISTER|CREATE ACCOUNT)\b/i, 6],
    [/\b(USERNAME|EMAIL|E-MAIL)\b/i, 8],
    [/\b(PASSWORD|PASSCODE|PIN)\b/i, 8],
    [/\b(SUBMIT|CONTINUE|NEXT)\b/i, 6],
    [/\b(IMAGES|IMAGE)\b/i, 4],
    [/\b(CLAIM|BONUS|REWARD|SPIN|WHEEL)\b/i, 8],
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
    const ft = node.formType.toLowerCase();
    if (ft === 'password' || ft.includes('input_password')) {
      return 'input_password';
    }
    if (ft === 'email' || ft.includes('input_email')) {
      return 'input_email';
    }
    if (ft === 'text' || ft === 'tel' || ft.includes('input_text')) {
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
  cls?: string,
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


/** Visual tokens: one per 28x28 patch. Matches Anthropic vision-coordinates. */
function countImageTokens(width: number, height: number): number {
  return Math.ceil(width / 28) * Math.ceil(height / 28);
}

function roundTiesToEven(value: number): number {
  const floor = Math.floor(value);
  if (value - floor !== 0.5) {
    return Math.round(value);
  }
  return floor % 2 === 0 ? floor : floor + 1;
}

/**
 * Size Claude actually sees before padding. Standard tier = 1568 edge / 1568
 * tokens (Sonnet 4.5). Sending a larger screenshot makes Claude downscale and
 * return coordinates in that smaller space — which we were clicking as DIP.
 */
function claudeResizedSize(
    width: number, height: number, maxEdge = 1568, maxTokens = 1568):
    [number, number] {
  const fits = (w: number, h: number): boolean =>
      Math.ceil(w / 28) * 28 <= maxEdge &&
      Math.ceil(h / 28) * 28 <= maxEdge &&
      countImageTokens(w, h) <= maxTokens;
  if (fits(width, height)) {
    return [width, height];
  }
  if (height > width) {
    const [resizedH, resizedW] =
        claudeResizedSize(height, width, maxEdge, maxTokens);
    return [resizedW, resizedH];
  }
  const aspectRatio = width / height;
  let lo = 1;
  let hi = width;
  while (lo + 1 < hi) {
    const mid = Math.floor((lo + hi) / 2);
    if (fits(mid, Math.max(roundTiesToEven(mid / aspectRatio), 1))) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return [lo, Math.max(roundTiesToEven(lo / aspectRatio), 1)];
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


function describeCoordinateStructureError(raw: any): string {
  if (!raw || typeof raw !== 'object') return '';
  const named = raw.coordinate ?? raw.coordinates;
  if (typeof named === 'string' || typeof raw.cell === 'string') {
    return 'coordinate must be an object {"x": integer, "y": integer}, not a string.';
  }
  const range = (x: number, y: number): string => {
    if (!Number.isFinite(x) || !Number.isFinite(y)) {
      return 'coordinate x and y must be numbers.';
    }
    if (x < 0 || y < 0) return 'coordinate x and y must be >= 0.';
    return '';
  };
  if (Array.isArray(named)) {
    if (named.length < 2) return 'coordinate array must be [x, y].';
    return range(Number(named[0]), Number(named[1]));
  }
  if (named != null) {
    if (typeof named !== 'object') {
      return 'coordinate must be {"x": integer, "y": integer}.';
    }
    if (!('x' in named) || !('y' in named)) return 'coordinate is missing x or y.';
    return range(Number(named.x), Number(named.y));
  }
  if (raw.x != null || raw.y != null) {
    if (raw.x == null || raw.y == null) return 'coordinate is missing x or y.';
    return range(Number(raw.x), Number(raw.y));
  }
  return '';
}

function extractJsonObjects(text: string): any[] {
  const cleaned = stripModelNoise(text);
  const parsed: any[] = [];
  const seen = new Set<string>();

  const unwrapStructuredAction = (obj: any) => {
    if (!obj || typeof obj !== 'object' || Array.isArray(obj)) {
      return obj;
    }
    if (typeof obj.action === 'string' && obj.action) {
      return obj;
    }
    const result = obj.result;
    if (result && typeof result === 'object' && !Array.isArray(result) &&
        typeof result.action === 'string' && result.action) {
      return result;
    }
    return obj;
  };

  const push = (obj: any) => {
    obj = unwrapStructuredAction(obj);
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


/**
 * serve.py owns /glic-prefs. A blob: or chrome: document has no HTTP base, so
 * a root-relative fetch cannot be parsed. Provider and model in those guests
 * go through dashboardGetEffectiveSettings instead.
 */
export function resolveGlicClientPrefsUrl(locationLike: {
  protocol?: string,
  origin?: string,
}|null|undefined): string|null {
  const protocol = String(locationLike?.protocol || '');
  if (protocol !== 'http:' && protocol !== 'https:') {
    return null;
  }
  const origin = String(locationLike?.origin || '').replace(/\/$/, '');
  if (!origin || origin === 'null') {
    return null;
  }
  return `${origin}/glic-prefs`;
}

/**
 * The packaged guest initializes with an empty host until glic-bootstrap
 * installs GlicBrowserHostImpl. Missing the method is not success. A host that
 * has other browser methods but not this one is a failed permission call.
 */
export async function requestTabContextPermission(
    browser: {
      setTabContextPermissionState?: (enabled: boolean) => Promise<void>,
      getFocusedTabStateV2?: unknown,
      closePanel?: unknown,
    }|null|undefined,
    enabled: boolean): Promise<'enabled'|'unavailable'> {
  const setState = browser?.setTabContextPermissionState;
  const hostPresent = typeof browser?.getFocusedTabStateV2 === 'function' ||
      typeof browser?.closePanel === 'function';
  if (typeof setState !== 'function') {
    if (hostPresent) {
      throw new TypeError(
          'setTabContextPermissionState is unavailable on this host');
    }
    return 'unavailable';
  }
  await setState.call(browser, enabled);
  return 'enabled';
}

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
  private queueStatusEl: HTMLElement|null = null;
  private pauseQueueBtnEl: HTMLButtonElement|null = null;
  private taskInfoEl: HTMLElement|null = null;
  private maxStepsInputEl: HTMLInputElement|null = null;
  private actorTaskId: number|null = null;
  private unifiedRunId: string|null = null;
  private warnedNoActorTask = false;
  private lastContextError: string|null = null;
  private lastScreenshotError: string|null = null;
  private lastPageContext: PageContextSummary|null = null;
  private lastActionSummary: string|null = null;
  private maxStepsPerTask = 40;
  private currentTabId: string|null = null;
  private focusedTabId: string|null = null;
  private candidateTabId: string|null = null;
  private tabReopenAttempts = 0;
  private maxTabReopenAttempts = 1;
  private maxLoadWaitMs = 120000;
  private taskStartDelayMs = 2500;
  private pendingTaskStartTimer: number|null = null;
  private warnedExtraMetaKeys = false;
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
  /** When set, Send appends to the running task instead of starting a new one. */
  private guideCurrentTaskEnabled = false;
  /** When set, a manual prompt that starts a task may open a new tab. */
  private openPromptInNewTabEnabled = false;
  /** User notes waiting to merge into the next runTaskLoop iteration. */
  private pendingLoopGuidance: string[] = [];
  /** Extra guidance currently attached to loop prompts (not part of the original goal). */
  private activeLoopGuidance = '';
  /** How many loop prompts have already included activeLoopGuidance. */
  private loopGuidanceAppliedTurns = 0;
  private captchaChallengeActive = false;
  private captchaClicksThisChallenge = 0;
  private captchaSeenThisTask = false;
  /** At least one captcha checkbox/coordinate click this task. */
  private captchaActuatedThisTask = false;
  /** Extra puzzle/tiles were seen after the checkbox. */
  private sawCaptchaFollowUpThisTask = false;
  /** Captcha is finished; login/submit is allowed. */
  private captchaClearedThisTask = false;
  /** Loop yielded so the user can solve a hard captcha; task stays active. */
  private humanCaptchaHold = false;
  private promptOptimizerEnabled = false;
  private promptHistory: string[] = [];
  /** -1 = live draft; 0 = most recent history entry. */
  private promptHistoryIndex = -1;
  private promptHistoryDraft = '';
  private promptInputEl: HTMLInputElement|null = null;
  private contextLockMode: ContextLockMode = 'locked';
  private taskTabId: string|null = null;
  private oauthTabId: string|null = null;
  private oauthTabUrl: string|null = null;
  private oauthWindowId: number|null = null;
  private oauthFlowStartedAt = 0;
  private oauthAnnounced = false;
  private oauthPopupExpectedUntil = 0;
  private knownBrowserTabIds: Set<string> = new Set();
  private workingTabId: string|null = null;
  private workingTabUrl: string|null = null;
  private workingTabTitle: string|null = null;
  private currentTaskStepIndex = 0;
  private pausedResumeFromStep = 0;
  private scriptSegmentStartedAtMs = 0;
  private scriptSegmentIndex = 0;
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
  private guideCurrentTaskToggleEl: HTMLInputElement|null = null;
  private openPromptInNewTabToggleEl: HTMLInputElement|null = null;
  private promptOptimizerToggleEl: HTMLInputElement|null = null;
  private modeSelectEl: HTMLSelectElement|null = null;
  private providerSelectEl: HTMLSelectElement|null = null;
  private modelSelectEl: HTMLSelectElement|null = null;
  private selectedProvider: AgentProvider = 'anthropic';
  private selectedModel = DEFAULT_MODELS_BY_PROVIDER.anthropic;
  private selectedOllamaEndpoint = DEFAULT_ENDPOINTS_BY_PROVIDER.local;
  private selectedEmbeddedModelDir = DEFAULT_EMBEDDED_MODEL_DIR;
  private embeddedModelLoadedDir: string|null = null;
  private lastEmbeddedStepSummary = '';
  private managerActionHistory: Array<{
    step: number,
    instruction: string,
    status: string,
    clicked?: {x: number, y: number, element: string},
    // Batch steps: what the browser reported for the step.
    hit_element?: string,
    grade?: string,
    note?: string,
  }> = [];
  // Outcome outline for the active task. stage is 1-based. The vision model
  // does not plan; the manager names the stage on the action it already returns.
  private taskPlanStages: string[] = [];
  private taskPlanStage = 0;
  private managerDidWork = false;
  private managerIdleTurns = 0;
  private lastManagerWorkKey = '';
  private managerPageUnchanged = false;
  private executionTimeline = new ExecutionTimeline();
  private mementos = new MementoStore();
  private lastProviderFailure = '';
  private previousManagerAccessibilityLines: string[] = [];
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
    imageWidth?: number,
    imageHeight?: number,
    note?: string,
  }|null = null;
  private lastScreenshotSource = '';
  private lastScreenshotClickSpace: {clickW: number, clickH: number, imageW: number, imageH: number}|null = null;
  /** Consecutive turns that only captured/requested screenshots (no click/type). */
  private screenshotOnlyStreak = 0;
  private capturedScreenshotThisTurn = false;
  /** Extra request_screenshot calls after a live shot was already attached. */
  private screenshotSpamStrikes = 0;



  private forcedGeminiApiMode(): boolean {
    return String((window as any).__SIMULACHROME_FORCE_PROVIDER || '')
               .trim() === 'gemini';
  }

  private applyForcedGeminiProvider(): void {
    const model = String((window as any).__SIMULACHROME_FORCE_MODEL || '')
                      .trim() ||
        'gemini-flash-latest';
    this.applyProviderModelSelection('gemini', model, false);
  }

  private resolveConfiguredProvider(): AgentProvider {
    try {
      const fromStorage = String(localStorage.getItem('glic.provider') || '').trim();
      if (fromStorage) {
        return normalizeAgentProvider(fromStorage);
      }
    } catch (_) {}
    return normalizeAgentProvider(window.__DASH_PROVIDER || 'anthropic');
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
    const options = PROVIDER_MODEL_OPTIONS.local;
    const match = options.find((option) => option.value === model);
    if (match?.endpoint) {
      return match.endpoint;
    }
    const fromWindow = String(window.__DASH_OLLAMA_ENDPOINT || '').trim();
    return fromWindow || DEFAULT_ENDPOINTS_BY_PROVIDER.local;
  }

  private persistProviderModelSelection(): void {
    try {
      localStorage.setItem('glic.provider', this.selectedProvider);
      localStorage.setItem('glic.model', this.selectedModel);
      if (this.selectedProvider === 'local') {
        localStorage.setItem('glic.ollama_endpoint', this.selectedOllamaEndpoint);
      }
    } catch (_) {}
    window.__DASH_PROVIDER = this.selectedProvider;
    window.__DASH_MODEL = this.selectedModel;
    if (this.selectedProvider === 'local') {
      window.__DASH_OLLAMA_ENDPOINT = this.selectedOllamaEndpoint;
    }
    // Disk-backed via serve.py so provider/model survive DevProfile browser resets.
    void this.persistClientPrefsToServer({
      provider: this.selectedProvider,
      model: this.selectedModel,
      ollama_endpoint: this.selectedProvider === 'local' ?
          this.selectedOllamaEndpoint :
          undefined,
    });
    // Mirror into Dashboard Settings (source of truth for chrome://dashboard).
    void this.persistProviderModelToDashboardSettings();
  }

  private async persistProviderModelToDashboardSettings(): Promise<void> {
    if (!this.canUseChromeSend()) {
      return;
    }
    try {
      const res = await this.sendWithPromise('dashboardGetEffectiveSettings');
      const settings = (res?.settings && typeof res.settings === 'object') ?
          {...res.settings} :
          {};
      const agent = (settings.agent && typeof settings.agent === 'object') ?
          {...settings.agent} :
          {};
      agent.provider = this.selectedProvider;
      agent.model = this.selectedModel;
      if (this.selectedProvider === 'local') {
        agent.localEndpoint = this.selectedOllamaEndpoint;
        agent.ollamaEndpoint = this.selectedOllamaEndpoint;
        agent.endpoint = this.selectedOllamaEndpoint;
      } else {
        const endpoints =
            (agent.providerEndpoints && typeof agent.providerEndpoints === 'object') ?
            {...agent.providerEndpoints} :
            {};
        const fallback = DEFAULT_ENDPOINTS_BY_PROVIDER[this.selectedProvider];
        endpoints[this.selectedProvider] =
            String(endpoints[this.selectedProvider] || agent.endpoint || fallback);
        agent.providerEndpoints = endpoints;
        agent.endpoint = endpoints[this.selectedProvider];
      }
      settings.agent = agent;
      await this.sendWithPromise('dashboardSetGlobalSettings', settings);
    } catch (e) {
      this.addSystemMessage(
          `Model saved in panel only; Settings sync failed (${e}).`);
    }
  }


  private async hydrateProviderModelFromDashboardSettings(): Promise<void> {
    if (!this.canUseChromeSend()) {
      return;
    }
    try {
      const res = await this.sendWithPromise('dashboardGetEffectiveSettings');
      const agent = res?.settings?.agent || {};
      const providerRaw = String(agent.provider || '').trim();
      const modelRaw = String(agent.model || '').trim();
      if (!providerRaw && !modelRaw) {
        return;
      }
      const provider = normalizeAgentProvider(providerRaw || this.selectedProvider);
      const model = sanitizeModelForProvider(
          provider, modelRaw || this.selectedModel);
      if (provider === 'local') {
        const ep = String(
            agent.localEndpoint || agent.ollamaEndpoint ||
            agent.endpoint || '').trim();
        if (ep) {
          this.selectedOllamaEndpoint = ep;
          window.__DASH_OLLAMA_ENDPOINT = ep;
        }
      }
      // Don't re-persist to Settings (avoid loop); update panel UI + local cache.
      this.applyProviderModelSelection(provider, model, false);
      try {
        localStorage.setItem('glic.provider', this.selectedProvider);
        localStorage.setItem('glic.model', this.selectedModel);
      } catch (_) {}
      window.__DASH_PROVIDER = this.selectedProvider;
      window.__DASH_MODEL = this.selectedModel;
    } catch (_) {
    }
  }

  private mapUnifiedProfileToLoopProfile(
      profile: UnifiedAgentProfile): string {
    if (profile === 'manager') {
      return 'manager';
    }
    if (profile === 'script_builder') {
      return 'dashboard_assistant';
    }
    if (profile === 'script_debugger' || profile === 'auto_heal') {
      return 'investigator';
    }
    return 'glic_task';
  }

  private persistUnifiedProfileLocally(profile: UnifiedAgentProfile) {
    try {
      localStorage.setItem('glic.unified_profile', profile);
      localStorage.setItem(
          'glic.loop_profile', this.mapUnifiedProfileToLoopProfile(profile));
    } catch (_) {
    }
    window.__DASH_LOOP_PROFILE = this.mapUnifiedProfileToLoopProfile(profile);
  }

  private async persistLoopProfileToDashboardSettings(
      profile: UnifiedAgentProfile): Promise<void> {
    if (!this.canUseChromeSend()) {
      return;
    }
    try {
      const res = await this.sendWithPromise('dashboardGetEffectiveSettings');
      const settings = (res?.settings && typeof res.settings === 'object') ?
          {...res.settings} :
          {};
      const agent = (settings.agent && typeof settings.agent === 'object') ?
          {...settings.agent} :
          {};
      agent.loopProfile = this.mapUnifiedProfileToLoopProfile(profile);
      settings.agent = agent;
      await this.sendWithPromise('dashboardSetGlobalSettings', settings);
    } catch (e) {
      this.addSystemMessage(
          `Loop profile saved in panel only; Settings sync failed (${e}).`);
    }
  }

  /** Dashboard Settings agent.loopProfile is the shared source of truth. */
  private async hydrateLoopProfileFromDashboardSettings(
      options?: {emitMessage?: boolean}): Promise<boolean> {
    if (!this.canUseChromeSend()) {
      return false;
    }
    try {
      const res = await this.sendWithPromise('dashboardGetEffectiveSettings');
      const loopRaw = String(res?.settings?.agent?.loopProfile || '').trim();
      if (!loopRaw) {
        return false;
      }
      const profile = this.mapLoopProfileToUnifiedProfile(loopRaw);
      const changed = profile !== this.unifiedProfile;
      this.setUnifiedProfile(
          profile, 'dashboard_settings',
          {persist: true, emitMessage: options?.emitMessage ?? changed});
      this.persistUnifiedProfileLocally(profile);
      return true;
    } catch (_) {
      return false;
    }
  }

  private clientPrefsUrl(): string|null {
    try {
      return resolveGlicClientPrefsUrl(window.location);
    } catch (_) {
      return null;
    }
  }

  private async persistClientPrefsToServer(partial: Record<string, unknown>):
      Promise<void> {
    const url = this.clientPrefsUrl();
    if (!url) {
      return;
    }
    try {
      await fetch(url, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(partial),
      });
    } catch (_) {
    }
  }

  private async restoreClientPrefsFromServer(): Promise<void> {
    const url = this.clientPrefsUrl();
    if (!url) {
      return;
    }
    try {
      const res = await fetch(url);
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
        if (!this.forcedGeminiApiMode()) {
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
          ollama_endpoint: this.selectedProvider === 'local' ?
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
    let options = [...(PROVIDER_MODEL_OPTIONS[provider] || [])];
    const desired = sanitizeModelForProvider(
        provider, preferredModel || this.selectedModel || this.modelSelectEl.value);
    this.modelSelectEl.replaceChildren();
    const optionValues = new Set(options.map((o) => o.value));
    if (desired && !optionValues.has(desired)) {
      options = [{value: desired, label: `${desired} (from Settings)`}, ...options];
    }
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
    if (provider === 'local') {
      const selectedOption =
          this.modelSelectEl.options[this.modelSelectEl.selectedIndex] || null;
      this.selectedOllamaEndpoint =
          selectedOption?.dataset?.['endpoint'] ||
          this.resolveConfiguredOllamaEndpoint(this.selectedModel);
      this.modelSelectEl.title =
          'Local models / OpenAI-compatible HTTP endpoint.';
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
    if (this.selectedProvider === 'local') {
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
    if (provider === 'local') {
      const selectedOption = this.modelSelectEl?.options
          ?.[this.modelSelectEl.selectedIndex] ||
          null;
      endpoint = selectedOption?.dataset?.['endpoint'] ||
          this.selectedOllamaEndpoint || endpoint;
    }
    this.selectedProvider = provider;
    this.selectedModel = model;
    if (provider === 'local') {
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
    if (provider === 'local') {
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
    if (value === 'manager' || value === 'manager_vlm' ||
        value === 'embedded_manager') {
      return 'manager';
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
    if (normalized === 'manager') {
      return 'manager';
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
    if (raw === 'manager' || raw === 'manager_vlm' ||
        raw === 'embedded_manager') {
      return 'manager';
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
      // Prefer loop_profile (Dashboard Settings mirror) over sticky unified_profile
      // so changing Manager in Settings wins after hydrate/persist keeps them aligned.
      const loopFromStorage =
          String(localStorage.getItem('glic.loop_profile') || '').trim();
      if (loopFromStorage) {
        return {
          profile: this.mapLoopProfileToUnifiedProfile(loopFromStorage),
          source: 'local_storage_loopProfile',
        };
      }
      const fromStorage = this.normalizeUnifiedProfile(
          localStorage.getItem('glic.unified_profile'));
      if (fromStorage) {
        return {profile: fromStorage, source: 'local_storage'};
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
    // Manager vs operator use different host contracts — re-inject on switch.
    if (previous !== profile &&
        (previous === 'manager' || profile === 'manager')) {
      this.hasCoreAssistantContextPrompt = false;
      this.lastEmbeddedStepSummary = '';
      this.managerActionHistory = [];
      this.clearTaskPlan();
      this.previousManagerAccessibilityLines = [];
    }
    if (options?.persist) {
      this.persistUnifiedProfileLocally(profile);
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
    // Dashboard Settings wins over sticky guest localStorage.
    await this.hydrateLoopProfileFromDashboardSettings({emitMessage: false});
    this.hybridExecutionEnabled = this.resolveHybridExecutionSetting();
    this.nonVisualContextMode = this.resolveNonVisualContextMode();
    this.attachContextForManualPrompts = this.resolveAttachContextMode();
    this.guideCurrentTaskEnabled = this.resolveGuideCurrentTaskEnabled();
    this.openPromptInNewTabEnabled = this.resolveOpenPromptInNewTabEnabled();
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
    const resetPanel = document.getElementById('resetPanel') as HTMLButtonElement | null;
    resetPanel?.addEventListener('click', () => {
      this.resetPanel();
    });
    this.panelModeEl = document.getElementById('panelMode');
    this.panelAttachToggleEl =
        document.getElementById('panelAttachToggle') as HTMLButtonElement | null;
    if (!this.browser.attachPanel && !this.browser.detachPanel &&
        !this.canUseChromeSend()) {
      this.panelAttachToggleEl?.setAttribute('hidden', 'hidden');
      if (this.panelModeEl) {
        this.panelModeEl.textContent = 'Attached';
      }
    }
    this.panelAttachToggleEl?.addEventListener('click', () => {
      void this.togglePanelDockFromChrome();
    });
    if (this.canUseChromeSend()) {
      void this.refreshPanelChromeFromHost();
    }
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
    const prompt = document.getElementById('prompt') as HTMLInputElement | null;
    const modeSelect =
        document.getElementById('modeSelect') as HTMLSelectElement | null;
    const providerSelect =
        document.getElementById('provider') as HTMLSelectElement | null;
    const modelSelect =
        document.getElementById('model') as HTMLSelectElement | null;
    const attachContextToggle =
        document.getElementById('attachContextToggle') as HTMLInputElement | null;
    const guideCurrentTaskToggle =
        document.getElementById('guideCurrentTaskToggle') as HTMLInputElement | null;
    const openPromptInNewTabToggle =
        document.getElementById('openPromptInNewTabToggle') as HTMLInputElement | null;
    const promptOptimizerToggle =
        document.getElementById('promptOptimizerToggle') as HTMLInputElement | null;
    this.sendBtnEl = send;
    this.stopBtnEl = null;  // Stop removed; use Pause in the taskbar.
    this.modeSelectEl = modeSelect;
    this.providerSelectEl = providerSelect;
    this.modelSelectEl = modelSelect;
    this.attachContextToggleEl = attachContextToggle;
    this.guideCurrentTaskToggleEl = guideCurrentTaskToggle;
    this.openPromptInNewTabToggleEl = openPromptInNewTabToggle;
    this.promptOptimizerToggleEl = promptOptimizerToggle;
    {
      if (this.forcedGeminiApiMode()) {
        try {
          this.applyForcedGeminiProvider();
        } catch (e) {
          console.warn('applyForcedGeminiProvider failed', e);
        }
        this.addSystemMessage(
            'Gemini API mode. Calls generativelanguage.googleapis.com with GEMINI_API_KEY from Data > Secrets.');
      } else {
        const initialProvider = this.resolveConfiguredProvider();
        const initialModel = this.resolveConfiguredModel(initialProvider);
        // localStorage / window only — do not POST yet or a fresh DevProfile
        // would overwrite disk prefs with the Ollama default before restore.
        try {
          this.applyProviderModelSelection(initialProvider, initialModel, false);
        } catch (e) {
          console.warn('applyProviderModelSelection failed', e);
        }
        window.__DASH_PROVIDER = this.selectedProvider;
        window.__DASH_MODEL = this.selectedModel;
        if (this.selectedProvider === 'local') {
          window.__DASH_OLLAMA_ENDPOINT = this.selectedOllamaEndpoint;
        }
        // Prefer Dashboard Settings as the shared source of truth with Settings tab.
        void this.hydrateProviderModelFromDashboardSettings();
      }
      void this.hydrateLoopProfileFromDashboardSettings({emitMessage: false});
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
        void this.persistLoopProfileToDashboardSettings(next);
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
    if (this.guideCurrentTaskToggleEl) {
      this.guideCurrentTaskToggleEl.checked = this.guideCurrentTaskEnabled;
    }
    this.guideCurrentTaskToggleEl?.addEventListener('change', () => {
      this.guideCurrentTaskEnabled = !!this.guideCurrentTaskToggleEl?.checked;
      try {
        localStorage.setItem(
            'glic.guide_current_task',
            this.guideCurrentTaskEnabled ? '1' : '0');
      } catch (_) {}
      this.syncPromptPlaceholder();
    });
    if (this.openPromptInNewTabToggleEl) {
      this.openPromptInNewTabToggleEl.checked = this.openPromptInNewTabEnabled;
    }
    this.openPromptInNewTabToggleEl?.addEventListener('change', () => {
      this.openPromptInNewTabEnabled = !!this.openPromptInNewTabToggleEl?.checked;
      try {
        localStorage.setItem(
            'glic.open_prompt_new_tab',
            this.openPromptInNewTabEnabled ? '1' : '0');
      } catch (_) {}
    });
    this.syncPromptPlaceholder();
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

    const startQueue = document.getElementById('startQueue') as HTMLButtonElement | null;
    this.maxStepsInputEl = document.getElementById('maxSteps') as HTMLInputElement | null;
    const pauseQueue = document.getElementById('pauseQueue') as HTMLButtonElement | null;
    this.pauseQueueBtnEl = pauseQueue;
    const completeTask = document.getElementById('completeTask') as HTMLButtonElement | null;
    const solvedCaptcha = document.getElementById('solvedCaptcha') as HTMLButtonElement | null;
    const nextTask = document.getElementById('nextTask') as HTMLButtonElement | null;
    const queueWidgetClose =
        document.getElementById('queueWidgetClose') as HTMLButtonElement | null;
    const queueResume =
        document.getElementById('queueResume') as HTMLButtonElement | null;
    const queuePause =
        document.getElementById('queuePause') as HTMLButtonElement | null;
    const queueComplete =
        document.getElementById('queueComplete') as HTMLButtonElement | null;
    const queueSolvedCaptcha =
        document.getElementById('queueSolvedCaptcha') as HTMLButtonElement | null;
    const queueNext = document.getElementById('queueNext') as HTMLButtonElement | null;
    const queueClear = document.getElementById('queueClear') as HTMLButtonElement | null;

    startQueue?.addEventListener('click', () => void this.startQueue());
    this.maxStepsInputEl?.addEventListener('blur', () => {
      void this.applyMaxStepsFromInput({persist: true});
    });
    this.maxStepsInputEl?.addEventListener('change', () => {
      void this.applyMaxStepsFromInput({persist: true});
    });
    this.maxStepsInputEl?.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        (e.currentTarget as HTMLInputElement).blur();
      }
    });
    pauseQueue?.addEventListener('click', () => this.togglePauseResume());
    completeTask?.addEventListener('click', () => void this.markTaskCompleteByUser());
    solvedCaptcha?.addEventListener('click', () => void this.startQueue());
    nextTask?.addEventListener('click', () => void this.skipToNextTask());
    this.queueWidgetToggleEl?.addEventListener('click', () => {
      this.toggleQueueWidget();
    });
    queueWidgetClose?.addEventListener('click', () => this.toggleQueueWidget(false));
    queueResume?.addEventListener('click', () => this.resumePausedTask());
    queuePause?.addEventListener('click', () => this.pauseQueue());
    queueComplete?.addEventListener('click', () => void this.markTaskCompleteByUser());
    queueSolvedCaptcha?.addEventListener('click', () => void this.startQueue());
    queueNext?.addEventListener('click', () => void this.skipToNextTask());
    queueClear?.addEventListener('click', () => this.clearQueuedTasks());

    this.render();
    this.updateQueueStatus();
    this.updateTaskInfo();
    this.renderQueueWidget();
    void this.syncMaxStepsInputFromSettings();
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


  private async togglePanelDockFromChrome() {
    const label = (this.panelAttachToggleEl?.dataset['mode'] || '').toLowerCase();
    const wantDock = label === 'detached';
    // Packaged guest still has Glic host attachPanel, but AlwaysDetached
    // used to ReportBadMessage. Prefer chrome.send so Dock always hits
    // the dashboard attach path.
    if (this.canUseChromeSend()) {
      const res = await this.sendWithPromise(
          wantDock ? 'dashboardAgentAttachPanel' : 'dashboardAgentDetachPanel');
      if (res && res.ok === false) {
        this.addSystemMessage(
            `Panel ${wantDock ? 'dock' : 'pop out'} failed: ${
                res.error || 'unknown'}`);
      }
    } else if (wantDock) {
      this.browser?.attachPanel?.();
    } else {
      this.browser?.detachPanel?.();
    }
    await this.refreshPanelChromeFromHost();
  }

  private async refreshPanelChromeFromHost() {
    if (!this.canUseChromeSend()) {
      return;
    }
    const res = await this.sendWithPromise('dashboardAgentGetPanelState');
    if (!res || res.ok === false) {
      return;
    }
    this.updatePanelChrome({
      kind: res.detached ? PanelStateKind.DETACHED : PanelStateKind.ATTACHED,
    } as any);
  }

  private updatePanelAttachability(canAttach: boolean) {
    this.canAttachPanelNow = canAttach;
    if (!this.panelAttachToggleEl) {
      return;
    }
    const mode = this.panelAttachToggleEl.dataset['mode'] || 'attached';
    // chrome.send attach works even when the Glic host reports canAttach=false
    // (AlwaysDetached). Never grey out Dock in the packaged guest.
    if (this.canUseChromeSend()) {
      this.panelAttachToggleEl.disabled = false;
      return;
    }
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

  private resolveGuideCurrentTaskEnabled(): boolean {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = params.get('guideCurrentTask');
      if (fromQuery !== null) {
        const v = fromQuery.trim().toLowerCase();
        return v === '1' || v === 'true' || v === 'on';
      }
    } catch (_) {}
    try {
      const fromStorage = localStorage.getItem('glic.guide_current_task');
      if (typeof fromStorage === 'string' && fromStorage.trim()) {
        const v = fromStorage.trim().toLowerCase();
        return v === '1' || v === 'true' || v === 'on';
      }
    } catch (_) {}
    return false;
  }

  private resolveOpenPromptInNewTabEnabled(): boolean {
    try {
      const params = new URLSearchParams(window.location.search);
      const fromQuery = params.get('openPromptInNewTab');
      if (fromQuery !== null) {
        const v = fromQuery.trim().toLowerCase();
        return v === '1' || v === 'true' || v === 'on';
      }
    } catch (_) {}
    try {
      const fromStorage = localStorage.getItem('glic.open_prompt_new_tab');
      if (typeof fromStorage === 'string' && fromStorage.trim()) {
        const v = fromStorage.trim().toLowerCase();
        return v === '1' || v === 'true' || v === 'on';
      }
    } catch (_) {}
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
      const outcome = await requestTabContextPermission(this.browser, true);
      if (outcome === 'enabled') {
        this.addSystemMessage('Enabled tab context permission.');
      }
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

  // Local vision ids (qwen2.5vl:7b and the same family) get the mailbox
  // frame. Text models stay text-only. Anthropic keeps its own screenshot path.
  private modelUsesMailboxVision(): boolean {
    const {provider, model} = this.getSelectedProviderAndModel();
    if (provider !== 'local') {
      return false;
    }
    return /(?:^|[^a-z])vl\b|vl:|vision|smolvlm|llava|minicpm-v/i.test(
        String(model || ''));
  }

  private supportsScreenshotAction(): boolean {
    const provider = this.getSelectedProviderAndModel().provider;
    return provider === 'anthropic' || this.modelUsesMailboxVision();
  }

  private hasLiveScreenshot(): boolean {
    return !!(this.pendingScreenshot && this.supportsScreenshotAction());
  }

  private liveScreenshotMustClickHint(): string {
    const spam = this.screenshotSpamStrikes > 0 ?
        ' Extra request_screenshot calls are ignored while this image is attached.' :
        '';
    return (
      'A 1:1 viewport screenshot is already attached. request_screenshot is disabled. ' +
      'You may still scroll, type, wait, use target_id, or click ' +
      '{"coordinate":{"x":120,"y":80}} on this picture. After a scroll/navigation the shot is stale — ' +
      'then request_screenshot is allowed again (or the host will recapture).' + spam
    );
  }

  /** Anthropic: capture viewport for the next model turn (used by request_screenshot
   *  and temporarily also by request_context to verify the vision path). */
  private async captureScreenshotForNextTurn(reason: string): Promise<boolean> {
    if (!this.supportsScreenshotAction()) {
      return false;
    }
    if (this.pendingScreenshot && reason !== 'captcha multi-step vision') {
      this.addSystemMessage(
          `Skipped extra screenshot (${reason}): already have a fresh 1:1 viewport image. Click {"coordinate":{"x":120,"y":80}} on that picture.`);
      return true;
    }
    if (this.screenshotOnlyStreak >= 2 && reason !== 'captcha multi-step vision') {
      this.addSystemMessage(
          `Skipped screenshot (${reason}): ${this.screenshotOnlyStreak} screenshot-only turns already. ` +
          'Click {"coordinate":{"x":120,"y":80}} on the existing image, or use target_id.');
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
            (this.lastScreenshotError ? ` (${this.lastScreenshotError})` : '') +
            (hasTabHint ? '.' : ' (no task tab bound in this Glic guest).'));
      }
      return false;
    }
    const prepared = await this.prepareOneToOneScreenshot(shot);
    this.pendingScreenshot = prepared || shot;
    this.screenshotSpamStrikes = 0;
    const ready = this.pendingScreenshot;
    const clickW = Number(ready.cssWidth || 0);
    const clickH = Number(ready.cssHeight || 0);
    const imageW = Number((ready as any).imageWidth || clickW);
    const imageH = Number((ready as any).imageHeight || clickH);
    if (clickW > 0 && clickH > 0) {
      this.lastScreenshotClickSpace = {
        clickW,
        clickH,
        imageW: imageW || clickW,
        imageH: imageH || clickH,
      };
    }
    const kb = Math.max(1, Math.round(ready.base64.length / 1024));
    this.capturedScreenshotThisTurn = true;
    this.screenshotOnlyStreak += 1;
    this.addSystemMessage(
        `Screenshot ${imageW}x${imageH} (viewport ${clickW}x${clickH}) ` +
        `(~${kb}KB)${this.lastScreenshotSource === 'mailbox' ? ' via mailbox' : ''}. ` +
        `Click {"coordinate":{"x":120,"y":80}} on the picture (${reason}).`);
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
      manager: 'Manager',
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
    // Keep GLiC provider/model/profile aligned with Dashboard Settings.
    // Gemini API mode is a panel toggle and must not be overwritten by the
    // local-agent settings used on the AGENT side.
    if (this.forcedGeminiApiMode()) {
      this.applyForcedGeminiProvider();
    } else {
      void this.hydrateProviderModelFromDashboardSettings();
    }
    void this.hydrateLoopProfileFromDashboardSettings();
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

  async notifyPanelWasClosed(): Promise<void> {
    if (this.currentTask && !this.isTaskPaused()) {
      this.pauseQueue('glic panel closed');
    }
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
    const showContextStream =
        localStorage.getItem('glic.show_context_stream') === '1';
    for (const m of this.messages) {
      if (!showContextStream && isLoopContextMessage(m)) {
        continue;
      }
      const div = document.createElement('div');
      div.className = `msg ${m.role}`;
      div.textContent = m.content;
      messagesEl.appendChild(div);
    }
    messagesEl.scrollTop = messagesEl.scrollHeight;
    this.persistLastRunTranscript();
  }

  private persistLastRunTranscript() {
    try {
      const compact = this.messages.slice(-60).map((m) => {
        const raw = String(m.content || '');
        return {
          role: m.role,
          content: raw.length > 800 ? raw.slice(0, 800) + '…' : raw,
        };
      });
      localStorage.setItem('glic.last_run_transcript', JSON.stringify({
        savedAt: Date.now(),
        screenshotOnlyStreak: this.screenshotOnlyStreak,
        captchaChallengeActive: this.captchaChallengeActive,
        lastAction: this.lastActionSummary,
        url: this.workingTabUrl || this.lastPageContext?.url || '',
        messages: compact,
      }));
    } catch (_) {
    }
  }

  private updateQueueStatus() {
    if (this.queueStatusEl) {
      const running = this.queueRunning ? 'running' : 'paused';
      this.queueStatusEl.textContent = `Queue: ${this.taskQueue.length} (${running})`;
    }
    this.syncPauseResumeButton();
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


  // One prompt, one output contract. The operation list is generated from the
  // same vocabulary the C++ validator uses (action_vocabulary.json), so the
  // manager can never be offered an op the browser would reject.
  private buildManagerContract(): string {
    const lines: string[] = [];
    lines.push('You are the Simulachrome Manager. You plan and supervise a browser task.');
    lines.push(
        'A small vision model only finds controls on the screen. Scaffolding performs every action. ' +
        'YOU decide the operation and every argument; the vision model never does.');
    lines.push('Reply with ONE flat JSON action and nothing else (no markdown, no commentary).');
    lines.push('{"action":"click","target_id":1}');
    lines.push('{"action":"click","coordinate":{"x":120,"y":80}}');
    lines.push('{"action":"type","target_id":2,"text":"{Email}"}');
    lines.push('{"action":"navigate","url":"https://example.com/"}');
    lines.push('{"action":"scroll","direction":"down","distance":600}');
    lines.push('{"action":"key_press","key":"Enter"}');
    lines.push('{"action":"wait","timeout":2}');
    lines.push('{"action":"request_screenshot"}');
    lines.push('{"action":"complete","status":"solved","summary":"done"}');
    lines.push('');
    lines.push('Each reply is one of those objects and nothing else. A click may include target_id, coordinate, or both. It does not include text, url, or distance.');
    lines.push('Each reply is the next action for the page in front of you. Recent History lists what already worked.');
    lines.push('');
    lines.push('Rules:');
    lines.push(
        '1. Act only on a control that is visible on the CURRENT page. Anything that appears after this action ' +
        '(a menu, a dialog, a new page) waits for your next turn.');
    lines.push(
        '2. Stored values: type the {TokenName} from the token list. Never invent a literal for a stored value.');
    lines.push('3. One action per turn. The next turn shows what that action did.');
    lines.push(
        '4. If the click missed, pick a different target_id or coordinate, or scroll. ' +
        'Do not repeat an action that failed the same way twice.');
    lines.push(
        '5. Use {"action":"complete"} only when the page shows the goal is done. ' +
        'Set "status" to "blocked" and "summary" to why when the goal cannot be finished.');
    return lines.join('\n');
  }

  private ensureManagerCoreAssistantContextPrompt() {
    this.messages.push({role: 'system', content: this.buildManagerContract()});
    this.hasCoreAssistantContextPrompt = true;
  }

  private addManagerTaskSystemPrompt(task: TaskItem) {
    this.managerActionHistory = [];
    this.clearTaskPlan();
    this.previousManagerAccessibilityLines = [];
    this.lastEmbeddedStepSummary = '';
    this.externalCredentialPlaceholderKeys.clear();
    this.loginScaffoldKey = '';
    this.loginScaffoldRepeats = 0;
    this.loginCaptchaOwned = false;
    this.loginSubmitArmed = false;
    this.clearLoginAutofillLatch();
    const lines: string[] = [];
    lines.push(`USER GOAL: ${task.goal}`);
    lines.push(`URL: ${task.url}`);
    lines.push(
        'The goal names outcomes, not the UI path. The page in front of you decides the steps, ' +
        'and anything blocking the next control (cookie banner, dialog, popup) is its own step.');
    lines.push(
        'If the live URL is about:blank, chrome:// or the wrong site, navigate to the destination first.');
    const identityTokens = this.listIdentityVariableTokens();
    const passwordTokens = this.listPasswordSecretTokens();
    if (identityTokens.length) {
      lines.push(
          `Variable tokens: ${identityTokens.map((n) => `{${n}}`).join(', ')}`);
    }
    if (passwordTokens.length) {
      lines.push(
          `Secret tokens: ${passwordTokens.map((n) => `{${n}}`).join(', ')}`);
    }
    this.messages.push({role: 'system', content: lines.join('\n')});
  }

  private buildManagerAccessibilitySummary(
      context: PageContextSummary, limit = 36): string[] {
    const elements = Array.isArray(context.elements) ? context.elements : [];
    const scored = elements
        .filter((el) => el && el.visible !== false && !el.disabled)
        .map((el) => {
          const kind = describeNodeKind(el) || 'control';
          const text = bestContextNodeText(el);
          const value = String(
              (el as any).value || el.fieldName || el.placeholder || '').trim();
          const role = String(el.role || '').trim();
          const tag = role || kind;
          let label = text || value || role || kind || `id=${el.id}`;
          label = label.replace(/\s+/g, ' ').trim().slice(0, 80);
          const flags: string[] = [];
          if (el.clickable) flags.push('clickable');
          if (el.editable) flags.push('editable');
          if (el.focusable) flags.push('focusable');
          if (/password/i.test(String(el.formType || ''))) flags.push('password');
          if (/email/i.test(String(el.formType || ''))) flags.push('email');
          const score = scoreContextNode(el);
          return {el, tag, label, flags, score};
        })
        .sort((a, b) => b.score - a.score || (a.el.id - b.el.id));
    const out: string[] = [];
    const seen = new Set<string>();
    for (const row of scored) {
      const key = `${row.tag}|${row.label}`.toLowerCase();
      if (seen.has(key)) continue;
      seen.add(key);
      const flagTxt = row.flags.length ? ` (${row.flags.join(',')})` : '';
      out.push(`[${row.tag}] ${row.label}${flagTxt}`);
      if (out.length >= limit) break;
    }
    return out;
  }

  private clearTaskPlan(): void {
    this.taskPlanStages = [];
    this.taskPlanStage = 0;
    this.managerDidWork = false;
    this.managerIdleTurns = 0;
    this.lastManagerWorkKey = '';
    this.managerPageUnchanged = false;
    this.executionTimeline.reset();
    this.lastProviderFailure = '';
  }

  private rememberPlanStage(action: any): number {
    const stage = Number(action?.stage);
    if (Number.isFinite(stage) && stage >= 1) {
      const rounded = Math.round(stage);
      const cap = this.taskPlanStages.length || rounded;
      this.taskPlanStage = Math.min(rounded, cap);
    }
    return this.taskPlanStage;
  }

  private noteManagerWork(key: string): void {
    const normalized = String(key || '').trim().slice(0, 180);
    if (this.managerPageUnchanged && normalized &&
        normalized === this.lastManagerWorkKey) {
      this.managerIdleTurns += 1;
    } else {
      this.managerIdleTurns = 0;
      this.lastManagerWorkKey = normalized;
    }
    this.managerDidWork = true;
  }

  private noteManagerIdle(): void {
    if (!this.managerDidWork) {
      return;
    }
    this.managerIdleTurns += 1;
  }

  private planPromptLine(): string {
    return this.executionTimeline.promptBlock();
  }

  private async noteTaskCompleterProgress(raw: any, normalized: any): Promise<boolean> {
    this.rememberPlanStage(raw);
    const workKey = [
      String(normalized?.action || ''),
      String(normalized?.target_id || ''),
      String(normalized?.url || ''),
      String(normalized?.text || normalized?.key || '').slice(0, 40),
    ].join(':');
    this.noteManagerWork(workKey);
    return false;
  }

  private buildManagerContextPrompt(context: PageContextSummary): string {
    const goal = String(this.currentTask?.goal || '').trim();
    const a11y = this.buildManagerAccessibilitySummary(context);
    const prev = this.previousManagerAccessibilityLines;
    const a11yChanged =
        a11y.length !== prev.length ||
        a11y.some((line, i) => line !== prev[i]);
    const history = this.managerActionHistory.slice(-8);

    // Lean Manager feed: goal + page + history + pruned A11y (diff when stable).
    const payload: Record<string, unknown> = {
      user_goal: goal || undefined,
      current_page: {
        title: context.title || '',
        url: context.url || '',
      },
      action_history: history,
    };
    if (a11yChanged || !prev.length) {
      payload['accessibility_summary'] = a11y;
      payload['accessibility_mode'] = 'full';
    } else {
      payload['accessibility_summary'] =
          [`(unchanged — ${a11y.length} controls; last summary still valid)`];
      payload['accessibility_mode'] = 'diff_unchanged';
    }
    if (this.lastEmbeddedStepSummary) {
      payload['last_embedded_result'] = this.lastEmbeddedStepSummary;
    }
    const lastAction = history.length ? history[history.length - 1] : undefined;
    if (lastAction?.clicked) {
      payload['last_click'] = lastAction.clicked;
    }
    if (context.hasOverlay) {
      payload['has_overlay'] = true;
    }
    this.previousManagerAccessibilityLines = a11y;

    const lines: string[] = [];
    lines.push(
        'Manager turn. Choose the next action from what current_page and ' +
        'accessibility_summary show NOW. user_goal is a summary: its clauses are not single steps. ' +
        'Recent History lists what already worked.');
    const timeline = this.executionTimeline.promptBlock().trim();
    if (timeline) {
      lines.push(timeline);
    }
    const sessionLine = this.sessionPromptLine(context);
    if (sessionLine) {
      lines.push(sessionLine);
    }
    if (history.length) {
      lines.push(
          'action_history lists earlier steps. A step with a grade of C, or a hit_element ' +
          'that is not the control you named, was a miss.');
    }
    lines.push(JSON.stringify(payload));
    lines.push('Reply with ONE JSON object: batch | navigate | wait | complete.');
    return lines.join('\n');
  }

  private async ensureEmbeddedModelLoaded(): Promise<{ok: boolean, error?: string}> {
    if (!this.canUseChromeSend()) {
      return {ok: false, error: 'chrome.send unavailable'};
    }
    const dir =
        String(this.selectedEmbeddedModelDir || DEFAULT_EMBEDDED_MODEL_DIR)
            .trim() ||
        DEFAULT_EMBEDDED_MODEL_DIR;
    if (this.embeddedModelLoadedDir === dir) {
      return {ok: true};
    }
    try {
      const res = await this.sendWithPromise('load_simulachrome_model', dir);
      if (!res || res.ok === false) {
        return {ok: false, error: String(res?.error || 'load_failed')};
      }
      this.embeddedModelLoadedDir = dir;
      this.addSystemMessage(`Embedded VLM loaded from ${dir}.`);
      return {ok: true};
    } catch (e) {
      return {ok: false, error: String(e)};
    }
  }

  private embeddedActionCall(payload: unknown): string {
    const raw = String(payload || '').trim();
    const at = raw.lastIndexOf('Action:');
    const call = (at >= 0 ? raw.slice(at + 'Action:'.length) : raw).trim();
    return call.slice(0, 180);
  }

  private summarizeEmbeddedResult(res: any, instruction: string): string {
    const action = res?.action || {};
    const typeMap: Record<number, string> = {
      0: 'none',
      1: 'click',
      2: 'type',
      3: 'scroll',
      4: 'wait',
      5: 'done',
      6: 'fail',
      7: 'element_not_found',
      8: 'key_press',
      9: 'double_click',
      10: 'right_click',
      11: 'hover',
      12: 'drag',
    };
    const typeName = typeMap[Number(action.type)] || `type_${action.type}`;
    const ok = !!res?.ok;
    const err = String(res?.error || '').trim();
    const hit = String(res?.hit_element || '').trim();
    const bits = [
      ok ? 'ok' : 'fail',
      typeName,
      err ? `err=${err}` : '',
      Number.isFinite(action.x) ? `x=${Number(action.x).toFixed(3)}` : '',
      Number.isFinite(action.y) ? `y=${Number(action.y).toFixed(3)}` : '',
      hit ? `hit=${hit.slice(0, 160)}` : '',
      action.payload ? `action=${this.embeddedActionCall(action.payload)}` : '',
      `instruction=${String(instruction || '').slice(0, 120)}`,
    ].filter(Boolean);
    return bits.join(' | ');
  }

  // One short line per step so the manager can see what each step did without
  // reading raw JSON. Typed text is shown as the manager wrote it ({Token}s
  // stay tokens; values are only ever expanded inside the browser process).
  private describePlannedStep(step: any): string {
    const op = String(step?.op || '?');
    const target = String(
        step?.target || step?.from || '').trim();
    const to = String(step?.to || '').trim();
    const args = step?.args || {};
    const bits: string[] = [op];
    if (target) bits.push(`"${target.slice(0, 60)}"`);
    if (to) bits.push(`-> "${to.slice(0, 60)}"`);
    if (typeof args.text === 'string' && args.text) {
      bits.push(`text=${args.text.slice(0, 40)}`);
    }
    if (typeof args.key === 'string' && args.key) bits.push(`key=${args.key}`);
    return bits.join(' ');
  }

  private summarizeBatchResult(steps: any[], res: any): string {
    const results: any[] = Array.isArray(res?.results) ? res.results : [];
    const parts = results.map((r) => {
      const bits = [
        `${r.id || `#${r.index}`}`,
        String(r.status || (r.ok ? 'ok' : 'failed')),
        this.describePlannedStep(steps[Number(r.index)]),
        r.error ? `err=${r.error}` : '',
        r.hit_element ? `hit=${String(r.hit_element).slice(0, 80)}` : '',
        r.grade ? `grade=${r.grade}` : '',
        r.snapped ? 'snapped' : '',
        r.raw_fallback ? 'raw_fallback' : '',
        r.warning ? `warn=${r.warning}` : '',
      ].filter(Boolean);
      return bits.join(' ');
    });
    return parts.join(' | ');
  }

  // Submit a manager-composed batch. The browser validates the whole plan first
  // (nothing runs if any step is malformed), asks the embedded model to locate
  // one control at a time, and fires each action the moment its targets
  // resolve. We get one reply when the batch is done.
  private async runEmbeddedBatch(
      steps: any[], stopOnFailure: boolean): Promise<boolean> {
    const loaded = await this.ensureEmbeddedModelLoaded();
    if (!loaded.ok) {
      this.lastEmbeddedStepSummary = `load_failed: ${loaded.error}`;
      this.addSystemMessage(`Embedded LoadModel failed (${loaded.error}).`);
      return false;
    }
    try {
      const res = await this.sendWithPromise(
          'submit_simulachrome_batch',
          {actions: steps, stop_on_failure: stopOnFailure});
      if (res && res.ok === false && res.error && !Array.isArray(res.results)) {
        // Rejected before anything ran (malformed plan, no tab, ...).
        const detail = res.detail ? `: ${res.detail}` : '';
        this.lastEmbeddedStepSummary = `plan_rejected ${res.error}${detail}`;
        this.managerActionHistory.push({
          step: this.managerActionHistory.length + 1,
          instruction: `batch of ${steps.length}`,
          status: 'REJECTED',
          note: `${res.error}${detail}`.slice(0, 200),
        });
        this.addSystemMessage(`Batch rejected: ${res.error}${detail}`);
        return false;
      }
      const results: any[] = Array.isArray(res?.results) ? res.results : [];
      for (const r of results) {
        const idx = Number(r.index);
        const entry: {
          step: number,
          instruction: string,
          status: string,
          clicked?: {x: number, y: number, element: string},
          hit_element?: string,
          grade?: string,
          note?: string,
        } = {
          step: this.managerActionHistory.length + 1,
          instruction: this.describePlannedStep(steps[idx]).slice(0, 160),
          status: String(r.status || (r.ok ? 'ok' : 'failed')).toUpperCase(),
        };
        if (r.hit_element) entry.hit_element = String(r.hit_element).slice(0, 160);
        if (r.grade) entry.grade = String(r.grade);
        const note = [r.error, r.warning, r.raw_fallback ? 'raw_fallback' : '']
                         .filter(Boolean).join(' ');
        if (note) entry.note = note.slice(0, 160);
        const pt = Array.isArray(r.points) && r.points.length ? r.points[0] : null;
        if (pt && (r.hit_element || r.grade)) {
          entry.clicked = {
            x: Number(Number(pt.x).toFixed(3)),
            y: Number(Number(pt.y).toFixed(3)),
            element: String(r.hit_element || '(no element reported)'),
          };
        }
        this.managerActionHistory.push(entry);
      }
      if (this.managerActionHistory.length > 24) {
        this.managerActionHistory = this.managerActionHistory.slice(-24);
      }
      this.lastEmbeddedStepSummary = this.summarizeBatchResult(steps, res);
      this.addSystemMessage(`Batch: ${this.lastEmbeddedStepSummary}`);
      return !!res?.ok;
    } catch (e) {
      this.lastEmbeddedStepSummary = `batch_failed: ${e}`;
      this.addSystemMessage(`Embedded batch failed (${e}).`);
      return false;
    }
  }

  private async runEmbeddedUtilityStep(instruction: string): Promise<boolean> {
    const loaded = await this.ensureEmbeddedModelLoaded();
    if (!loaded.ok) {
      this.lastEmbeddedStepSummary = `load_failed: ${loaded.error}`;
      this.addSystemMessage(
          `Embedded LoadModel failed (${loaded.error}).`);
      return false;
    }
    try {
      const res = await this.sendWithPromise(
          'execute_simulachrome_action', instruction);
      this.lastEmbeddedStepSummary =
          this.summarizeEmbeddedResult(res, instruction);
      const ok = !!res?.ok;
      const action = res?.action || {};
      const hit = String(res?.hit_element || '').trim();
      const isClick = Number(action.type) === 1;
      const entry: {
        step: number,
        instruction: string,
        status: string,
        clicked?: {x: number, y: number, element: string},
      } = {
        step: this.managerActionHistory.length + 1,
        instruction: String(instruction || '').slice(0, 160),
        status: ok ? 'SUCCESS' : 'FAILED',
      };
      if (isClick || hit) {
        entry.clicked = {
          x: Number.isFinite(action.x) ? Number(Number(action.x).toFixed(3)) : 0,
          y: Number.isFinite(action.y) ? Number(Number(action.y).toFixed(3)) : 0,
          element: hit || '(no element reported)',
        };
      }
      this.managerActionHistory.push(entry);
      if (this.managerActionHistory.length > 20) {
        this.managerActionHistory = this.managerActionHistory.slice(-20);
      }
      this.addSystemMessage(`Embedded step: ${this.lastEmbeddedStepSummary}`);
      return ok;
    } catch (e) {
      this.lastEmbeddedStepSummary = `execute_failed: ${e}`;
      this.addSystemMessage(`Embedded ExecuteAction failed (${e}).`);
      return false;
    }
  }

  private ensureCoreAssistantContextPrompt() {
    if (this.hasCoreAssistantContextPrompt) {
      return;
    }
    if (this.isManagerProfile()) {
      this.ensureManagerCoreAssistantContextPrompt();
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
    lines.push('Reply with one flat JSON action and nothing else.');
    lines.push('{"action":"click","target_id":1}');
    lines.push('{"action":"click","coordinate":{"x":120,"y":80}}');
    lines.push('{"action":"type","target_id":2,"text":"{Email}"}');
    lines.push('{"action":"navigate","url":"https://example.com/"}');
    lines.push('{"action":"scroll","direction":"down","distance":600}');
    lines.push('{"action":"key_press","key":"Enter"}');
    lines.push('{"action":"wait","timeout":2}');
    lines.push('{"action":"request_screenshot"}');
    lines.push('{"action":"complete","status":"solved","summary":"done"}');
    lines.push('');
    lines.push('For clickable/editable actions, prefer target_id from current context elements[].id.');
    lines.push(
        'Use screenshot coordinates only for a canvas or challenge widget after {"action":"request_screenshot"}. Form fields and buttons use target_id. coordinate is {"x":integer,"y":integer}.');
    lines.push(
        '{"action":"request_screenshot"} at most once, then {"action":"click","coordinate":{"x":120,"y":80}}. Do not request another screenshot.');
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
    return profile === 'browser_operator' || profile === 'manager' ||
        profile === 'script_debugger' || profile === 'auto_heal';
  }

  private isManagerProfile(
      profile: UnifiedAgentProfile = this.unifiedProfile): boolean {
    return profile === 'manager';
  }

  // Task completer is the browser-operator loop. The outcome outline and the
  // stop-when-the-last-stage-lands check apply there, not only to the manager.
  private usesOutcomePlan(
      profile: UnifiedAgentProfile = this.unifiedProfile): boolean {
    return profile === 'browser_operator' || profile === 'manager';
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
    // Re-read Dashboard loop profile so Settings → Manager takes effect without
    // closing the GLiC panel (guest localStorage is a separate origin).
    await this.hydrateLoopProfileFromDashboardSettings({emitMessage: false});
    if (!this.profileSupportsManualTaskLoop()) {
      return false;
    }
    if (!this.userLikelyWantsImmediateExecution(userText, this.unifiedProfile)) {
      return false;
    }

    const live = await this.resolveLivePageTab(context);
    const destFromPrompt = this.promptDestinationUrl(userText);
    const url = destFromPrompt ||
        (live && this.isWebNavigableUrl(live.url) ? live.url : '') ||
        this.resolveManualTaskExecutionUrl(userText, context);
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
    this.requestContextNextTurn = false;
    this.lastTaskElementIds = [];
    this.lastTaskLabeledElements = [];
    this.clearLoopGuidanceState();
    this.clearCaptchaChallengeState();
    this.currentTaskStepIndex = 0;
    this.scriptSegmentStartedAtMs = Date.now();
    this.scriptSegmentIndex = 0;
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
    if (!this.shouldEnforceHeadlessOnlyForCurrentTask()) {
      if (this.openPromptInNewTabEnabled) {
        const openUrl = destFromPrompt ||
            (live && this.isWebNavigableUrl(live.url) ? live.url : '') ||
            (this.isWebNavigableUrl(url) ? url : '');
        if (this.isWebNavigableUrl(openUrl)) {
          this.addSystemMessage(`Opening a new tab for ${openUrl}`);
          await this.openTaskUrl(openUrl, {forceNew: true});
          temporaryTask.url = openUrl;
        } else {
          this.addSystemMessage(
              'Open prompt in new tab is on, but the prompt has no URL to open.');
        }
      } else if (live?.tabId) {
        this.bindTaskTab(live.tabId, live.url || url, temporaryTask.goal);
        if (destFromPrompt &&
            (!live.url || !this.sameSite(live.url, destFromPrompt) ||
             String(live.url).replace(/\/$/, '') !==
                 destFromPrompt.replace(/\/$/, ''))) {
          this.addSystemMessage(
              `Using the current tab; navigating it to ${destFromPrompt}`);
          const ok = await this.navigateCurrentTab(destFromPrompt);
          if (ok) {
            temporaryTask.url = destFromPrompt;
          } else {
            this.addSystemMessage(
                `Current-tab navigate failed; staying on ${live.url || 'this tab'}.`);
          }
        } else {
          this.addSystemMessage(
              `Using the current tab ${live.tabId}` +
              (live.url ? ` (${live.url})` : '') + '.');
        }
      } else if (this.isWebNavigableUrl(destFromPrompt)) {
        this.addSystemMessage(
            `No live tab bound; opening ${destFromPrompt} so the task can run.`);
        await this.openTaskUrl(destFromPrompt, {forceNew: true});
        temporaryTask.url = destFromPrompt;
      } else {
        this.addSystemMessage(
            'No page tab is bound and the prompt has no URL; the model has nothing to act on.');
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
    const dataUrl = `data:${pending.mimeType};base64,${pending.base64}`;
    const note = pending.note ||
        ('[Attached 1:1 viewport screenshot. Click {"action":"click","coordinate":{"x":120,"y":80}} ' +
         'using pixels on this image. (0,0)=top-left.]');
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

  private sendWithPromise(message: string, ...args: unknown[]): Promise<any> {
    return new Promise((resolve, reject) => {
      const cb = `__glic_cb_${Date.now()}_${Math.random().toString(36).slice(2)}`;
      try {
        (window as any)[cb] = (result: any) => {
          try {
            if (Array.isArray(result) && result.length === 1) {
              result = result[0];
            }
          } catch (_) {}
          try {
            delete (window as any)[cb];
          } catch (_) {}
          resolve(result ?? null);
        };
        if ((window as any).chrome &&
            typeof (window as any).chrome.send === 'function') {
          (window as any).chrome.send(message, [cb, ...args]);
        } else {
          delete (window as any)[cb];
          reject(new Error('chrome.send is not available'));
        }
      } catch (e) {
        try {
          delete (window as any)[cb];
        } catch (_) {}
        reject(e);
      }
    });
  }

  private canUseChromeSend(): boolean {
    return !!(window as any).chrome &&
        typeof (window as any).chrome.send === 'function';
  }

  private async runDashboardExecutorAction(
      action: string, params: any,
      options?: {waitForCompletion?: boolean, tabId?: number}):
      Promise<any> {
    const opts: Record<string, unknown> = {
      wait_for_completion: options?.waitForCompletion !== false,
      is_agent_mode: true,
    };
    if (Number.isFinite(options?.tabId as number)) {
      opts['tab_id'] = options!.tabId;
    }
    return this.sendWithPromise(
        'dashboardAgentCallAction', action, params || {}, opts);
  }

  private extractAssistantTextFromProviderBody(
      provider: AgentProvider, bodyText: string): string {
    try {
      const json = JSON.parse(bodyText);
      if (provider === 'anthropic') {
        const blocks = Array.isArray(json?.content) ? json.content : [];
        const texts = blocks
                          .filter((b: any) => b && b.type === 'text')
                          .map((b: any) => String(b.text || ''));
        if (texts.length) {
          return texts.join('\n');
        }
      }
      if (provider === 'gemini') {
        const parts = json?.candidates?.[0]?.content?.parts;
        if (Array.isArray(parts)) {
          return parts.map((p: any) => String(p?.text || '')).join('');
        }
      }
      const choice = json?.choices?.[0]?.message?.content;
      if (typeof choice === 'string') {
        return choice;
      }
      if (typeof json?.message?.content === 'string') {
        return json.message.content;
      }
      return bodyText;
    } catch (_) {
      return bodyText;
    }
  }

  private async fetchAssistantText(
      model: string,
      headers: Record<string, string>,
      messagesPayload: Array<{role: 'user'|'assistant'|'system', content: string}>,
  ): Promise<string> {
    const {provider, endpoint} = this.getSelectedProviderAndModel();
    const wireMessages = this.buildWireMessages(messagesPayload);
    const canUseChromeSend =
        !!(window as any).chrome &&
        typeof (window as any).chrome.send === 'function';

    if (canUseChromeSend) {
      try {
        const res = await this.sendWithPromise('dashboardAgentOllamaChat', {
          provider,
          endpoint,
          model,
          messages: wireMessages,
          stream: false,
          max_tokens: 1024,
          response_format: {
            type: 'json_schema',
            json_schema: {
              name: 'simulachrome_agent_action',
              strict: true,
              schema: FLAT_AGENT_ACTION_SCHEMA,
            },
          },
        });
        if (!res || res.ok === false) {
          return `Error: ${res?.error || 'dashboardAgentOllamaChat failed'}` +
              (res?.body ? `\n${res.body}` : '');
        }
        const body = String(res.body || '');
        return this.extractAssistantTextFromProviderBody(provider, body) || body;
      } catch (e) {
        const msg = String(e);
        if ((e instanceof DOMException && e.name === 'AbortError') ||
            msg.includes('AbortError')) {
          return '[Canceled]';
        }
        // Fall through to localhost proxy for --glic-dev serve.py workflows.
      }
    }

    const body = {
      model,
      stream: false,
      messages: wireMessages,
      max_tokens: 1024,
      response_format: compileOpenAICompatibleResponseFormat(provider),
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
        const {url, goal} = this.currentTask;
        const stepText =
            this.currentTaskStepIndex > 0 ?
                ` | step ${this.currentTaskStepIndex}/${this.maxStepsPerTask}` :
                ` | max ${this.maxStepsPerTask} steps`;
        const tabText = this.getAuthoritativeTaskTabId() ?
            ` | tab ${this.getAuthoritativeTaskTabId()}` :
            '';
        const guidanceText = this.activeLoopGuidance ?
            (this.loopGuidanceAppliedTurns > 0 ?
                 ' | +guidance (may be stale)' :
                 ' | +guidance') :
            (this.pendingLoopGuidance.length ? ' | +guidance (queued)' : '');
        const captchaText = this.humanCaptchaHold ?
            ' | waiting for captcha solve' :
            (this.captchaChallengeActive ? ' | captcha challenge' : '');
        const pauseText = this.isTaskPaused() ? ' | paused' : '';
        this.taskInfoEl.textContent =
            `Task: ${goal} | ${url}${stepText}${tabText}${guidanceText}${captchaText}${pauseText}`;
      }
    }
    this.renderQueueWidget();
  }

  private clampMaxSteps(value: number): number {
    if (!Number.isFinite(value)) {
      return this.maxStepsPerTask;
    }
    return Math.max(1, Math.min(500, Math.round(value)));
  }

  private syncMaxStepsInput() {
    if (this.maxStepsInputEl) {
      this.maxStepsInputEl.value = String(this.maxStepsPerTask);
    }
  }

  private async syncMaxStepsInputFromSettings() {
    if (this.canUseChromeSend()) {
      try {
        const res = await this.sendWithPromise('dashboardGetEffectiveSettings');
        const raw = Number(res?.settings?.agent?.maxAutonomousSteps);
        if (Number.isFinite(raw) && raw > 0) {
          this.maxStepsPerTask = this.clampMaxSteps(raw);
        }
      } catch (_) {
      }
    }
    this.syncMaxStepsInput();
    this.updateTaskInfo();
  }

  private async applyMaxStepsFromInput(options?: {persist?: boolean}):
      Promise<void> {
    const raw = Number.parseInt(this.maxStepsInputEl?.value ?? '', 10);
    const next = this.clampMaxSteps(raw);
    const changed = next !== this.maxStepsPerTask;
    this.maxStepsPerTask = next;
    this.syncMaxStepsInput();
    this.updateTaskInfo();
    if (!changed || options?.persist === false || !this.canUseChromeSend()) {
      return;
    }
    try {
      const res = await this.sendWithPromise('dashboardGetEffectiveSettings');
      const settings = (res?.settings && typeof res.settings === 'object') ?
          {...res.settings} :
          {};
      const agent = (settings.agent && typeof settings.agent === 'object') ?
          {...settings.agent} :
          {};
      agent.maxAutonomousSteps = next;
      settings.agent = agent;
      await this.sendWithPromise('dashboardSetGlobalSettings', settings);
    } catch (e) {
      this.addSystemMessage(`Max steps saved locally only (${e}).`);
    }
  }

  private async startQueue() {
    await this.applyMaxStepsFromInput({persist: false});
    this.taskLoopStopRequested = false;
    this.queueRunning = true;
    this.currentTaskStepIndex = 0;
    this.pausedResumeFromStep = 0;
    this.updateQueueStatus();
    this.updateTaskInfo();
    // Resume a paused or max-steps-exhausted task with a fresh step budget.
    if (this.currentTask && !this.taskLoopRunning) {
      if (this.humanCaptchaHold) {
        this.humanCaptchaHold = false;
        this.captchaChallengeActive = false;
        this.captchaClicksThisChallenge = 0;
        this.syncSolvedCaptchaButtons();
        this.addSystemMessage(
            'Captcha marked solved. Resuming with a fresh screenshot, then the next step.');
      } else {
        this.addSystemMessage(
            `Resuming agent task loop with a fresh ${this.maxStepsPerTask}-step budget.`);
      }
      void this.runTaskLoop();
      return;
    }
    if (!this.currentTask) {
      this.scheduleNextTaskStart();
    }
  }

  private async markTaskCompleteByUser(): Promise<void> {
    if (!this.currentTask) {
      this.addSystemMessage('No active task to mark complete.');
      return;
    }
    this.taskLoopStopRequested = true;
    this.cancelModelRequest('task marked complete');
    void this.abortActiveTooling('task marked complete');
    this.clearPendingTaskStart();
    this.addSystemMessage(
        'Task marked complete. Reviewing the recorded script, then saving.');
    await this.finishTask('success', 'Marked complete by user');
  }

  private isTaskPaused(): boolean {
    return !!(this.currentTask &&
        (this.taskLoopStopRequested || !this.queueRunning || this.humanCaptchaHold));
  }

  private syncPauseResumeButton(): void {
    if (!this.pauseQueueBtnEl) {
      return;
    }
    const showResume = !!(this.currentTask &&
        (this.taskLoopStopRequested || !this.queueRunning || this.humanCaptchaHold));
    this.pauseQueueBtnEl.textContent = showResume ? 'Resume' : 'Pause';
    this.pauseQueueBtnEl.title = showResume ?
        'Resume the paused task from this step' :
        'Pause the current task. Guided prompts still apply. Resume to continue.';
  }

  private togglePauseResume(): void {
    const showResume = !!(this.currentTask &&
        (this.taskLoopStopRequested || !this.queueRunning || this.humanCaptchaHold));
    if (showResume) {
      this.resumePausedTask();
      return;
    }
    this.pauseQueue();
  }

  private resumePausedTask(): void {
    if (!this.currentTask) {
      this.addSystemMessage('No paused task to resume.');
      return;
    }
    this.taskLoopStopRequested = false;
    this.queueRunning = true;
    if (this.humanCaptchaHold) {
      this.humanCaptchaHold = false;
      this.syncSolvedCaptchaButtons();
    }
    this.updateQueueStatus();
    this.updateTaskInfo();
    if (this.taskLoopRunning) {
      this.addSystemMessage(
          'Resumed. The current step will finish, then the loop continues.');
      return;
    }
    this.addSystemMessage('Resumed the paused task from the current step.');
    void this.runTaskLoop();
  }

  private pauseQueue(reason: string = 'queue paused') {
    if (!this.currentTask && !this.taskLoopRunning && !this.queueRunning) {
      this.addSystemMessage('Nothing to pause.');
      this.syncPauseResumeButton();
      return;
    }
    // Hold the task. Do not finish/clear it — guided prompts still apply.
    this.taskLoopStopRequested = true;
    this.queueRunning = false;
    this.pausedResumeFromStep = this.currentTaskStepIndex;
    this.clearPendingTaskStart();
    this.updateQueueStatus();
    this.updateTaskInfo();
    this.addSystemMessage(
        this.taskLoopRunning ?
            `Paused. This step can finish; no further loop turns until Resume (${reason}).` :
            `Paused. Task is still active — guide it, then press Resume (${reason}).`);
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
      this.clearLoopGuidanceState();
    this.clearCaptchaChallengeState();
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
    const hydrated =
        await this.hydrateLoopProfileFromDashboardSettings({emitMessage: false});
    if (!hydrated) {
      const configuredProfile = this.resolveUnifiedProfileFromConfiguration();
      this.setUnifiedProfile(configuredProfile.profile,
                             configuredProfile.source,
                             {emitMessage: false});
    }
    this.actorTaskId = null;
    this.unifiedRunId = null;
    this.tabReopenAttempts = 0;
    this.warnedExtraMetaKeys = false;
    this.requestContextNextTurn = false;
    this.clearLoopGuidanceState();
    this.clearCaptchaChallengeState();
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
        String(this.getActuationTabId() ?? this.currentTabId ?? ''), 10);
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
    if (this.canUseChromeSend()) {
      const timelineAction = action?.action === 'navigate' ? {
        action: 'navigate',
        params: {url: action.url},
      } : {
        action: String(action?.action || 'unknown'),
        params: action?.params || {},
      };
      const result = await this.runDashboardExecutorAction(
          String(timelineAction.action), timelineAction.params,
          {waitForCompletion: true, tabId});
      if (!result || result.ok === false || result.success === false ||
          result.error) {
        this.addSystemMessage(
            `Tab action execution failed: ${result?.error || result?.message || 'script executor'}`);
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
    if (!this.supportsUnifiedAgentOrchestrator() && !this.canUseChromeSend()) {
      if (!this.actorTaskId) {
        await this.ensureActorTask();
      }
      if (!this.actorTaskId) {
        return false;
      }
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

  private async resolveLivePageTab(context?: PageContextSummary):
      Promise<{tabId: string, url: string}|null> {
    const fromContextId = String(context?.tabId || '').trim();
    const fromContextUrl = String(context?.url || '').trim();
    if (fromContextId) {
      return {tabId: fromContextId, url: fromContextUrl};
    }
    const hinted = String(
        this.focusedTabId || this.candidateTabId || this.currentTabId ||
        this.workingTabId || '').trim();
    let hintedUrl = String(this.workingTabUrl || '');
    if (hinted) {
      try {
        const tab = this.browser?.getTabById?.(hinted)?.getCurrentValue?.();
        if (tab?.url) {
          hintedUrl = String(tab.url);
        }
      } catch (_) {}
      return {tabId: hinted, url: hintedUrl};
    }
    try {
      const tabs = await this.listBrowserTabs() || [];
      const active = (tabs || []).find(
          (t) => t?.isActiveInWindow || t?.isWindowActive) || (tabs || [])[0];
      const tabId = String(active?.tabId ?? '').trim();
      if (tabId) {
        return {tabId, url: String(active?.url || '')};
      }
    } catch (_) {}
    return null;
  }

  private promptDestinationUrl(userText: string): string {
    const fromPrompt = this.inferUrlFromPrompt(userText);
    return this.isWebNavigableUrl(fromPrompt) ? fromPrompt : '';
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
    if (this.canUseChromeSend()) {
      const res = await this.sendWithPromise('dashboardAgentOpenTab', {url});
      if (res && res.ok !== false && (res.tab_id || res.tabId)) {
        const tabId = String(res.tab_id ?? res.tabId);
        this.bindTaskTab(tabId, String(res.url || url), this.currentTask?.goal ?? null);
        this.addSystemMessage(`Opened task tab ${tabId} for ${url}`);
        await new Promise((resolve) => setTimeout(resolve, 1500));
        return;
      }
      throw new Error(String(res?.error || 'dashboardAgentOpenTab failed'));
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
    this.clearLoginAutofillLatch();
    this.clearTaskPlan();
    if (this.isManagerProfile()) {
      this.addManagerTaskSystemPrompt(task);
      return;
    }
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
        'Do not click Sign in, Log in, or Continue with Apple, Google, or Facebook unless the USER GOAL names that provider. ' +
        'If an OAuth popup is already open, page context includes its tab id and window id and the elements from that window. Act only in that window until it closes.');
    lines.push(
        'If the live task context URL is about:blank, chrome://, a New Tab page, or any host that is not the task destination, your FIRST action must be navigate to the task URL (or the goal destination).');
    lines.push(
        'Do not click New Tab shortcuts or chrome:// UI to reach a site — use {"action":"navigate","url":"https://..."}.');
    lines.push(
        'After you are already on the correct site, do not navigate again just because the original task URL is known. Only navigate when the live context shows the wrong page or the task explicitly requires a new destination.');
    const identityTokens = this.listIdentityVariableTokens();
    const passwordTokens = this.listPasswordSecretTokens();
    lines.push(
        'STORED VALUES (only when the current page asks for them):');
    lines.push(
        '1) Identity fields: text MUST be a Dashboard VARIABLE token in {TokenName} form. ' +
        'Never type field labels/placeholders or bare names.');
    lines.push(
        '2) Secret fields: text MUST be a Dashboard SECRET token in {TokenName} form. ' +
        'Never type a field label or a plaintext secret.');
    lines.push(
        '3) Exact format: {TokenName} with ONE pair of braces. Never {{TokenName}}. ' +
        'Executor resolves tokens at run time — do not invent values.');
    lines.push(
        '4) Choose from the token lists below (prefer any token named in the USER GOAL). ' +
        'Labels only identify which field to target.');
    lines.push(
        identityTokens.length ?
            `Variable tokens: ${
                identityTokens.map((n) => `{${n}}`).join(', ')}` :
            'Variable tokens: (none loaded from Data > Variables).');
    lines.push(
        passwordTokens.length ?
            `Secret tokens: ${
                passwordTokens.map((n) => `{${n}}`).join(', ')}` :
            'Secret tokens: (none loaded from Data > Secrets).');
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
        '- One control per action, in the order the current page requires. Do not assume a login, a search, or a claim.');
    lines.push(
        '- Prefer acting with current element ids; request_context when UI changed (incl. same-URL popup/modal) or needed ids are missing.');
    lines.push(
        '- Form fields and buttons: always target_id. Do not click them with screenshot coordinates.');
    lines.push(
        '- If a challenge widget is blocking the next control, {"action":"request_screenshot"} then {"action":"click","coordinate":{"x":120,"y":80}} on it before continuing. Do not skip it, and do not treat it as the whole task.');
    lines.push('Do NOT include action results (e.g., success=true/false, status, reason) in click, type, scroll, or key_press JSON.');
    lines.push(
        'Recent History on each turn lists the actions that already worked. Use it, and do not repeat a step the turn says had no effect.');
    lines.push('Respond with a single flat JSON action and nothing else:');
    lines.push('{"action":"navigate","url":"https://example.com/"}');
    lines.push('{"action":"click","target_id":123}');
    lines.push('{"action":"click","coordinate":{"x":120,"y":80}}');
    lines.push('{"action":"type","target_id":123,"text":"{Email}"}');
    lines.push(
        'Do not use element kind names as actions (input_text/input_email). Typing is always {"action":"type","target_id":123,"text":"{Email}"}.');
    lines.push('{"action":"scroll","direction":"down","distance":600,"scope":"page"}');
    lines.push('{"action":"scroll","direction":"down","distance":600,"scope":"iframe","target_id":123}');
    lines.push('{"action":"key_press","key":"Enter"}');
    lines.push('{"action":"wait","timeout":2}');
    lines.push('{"action":"request_screenshot"}');
    lines.push('{"action":"complete","status":"solved","summary":"done"}');
    lines.push('{"action":"complete","status":"blocked","summary":"why"}');
    if (this.hasLiveScreenshot()) {
      lines.push(this.liveScreenshotMustClickHint());
    } else if (this.supportsScreenshotAction()) {
      lines.push(
          'request_screenshot at most once per challenge, then click {"action":"click","coordinate":{"x":120,"y":80}} on that picture. Do not spam screenshots.');
      lines.push(
          'Screenshot coordinates are only for an active captcha/canvas. Normal fields use target_id.');
    }
    lines.push(
        'Page scroll: {"action":"scroll","direction":"down","distance":600,"scope":"page"} moves the MAIN document. ' +
        'Iframe scroll: {"action":"scroll","direction":"down","distance":600,"scope":"iframe","target_id":123}.');
    lines.push(
        'When the page shows the goal is done, {"action":"complete","status":"solved","summary":"..."}. ' +
        'When it cannot be finished, {"action":"complete","status":"blocked","summary":"..."}. ' +
        'One task is one site.');
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

  private getActuationTabId(): string|null {
    const oauth = String(this.oauthTabId || '').trim();
    if (oauth) {
      return oauth;
    }
    return this.getAuthoritativeTaskTabId();
  }

  private isOauthLoginUrl(url?: string|null): boolean {
    const raw = String(url || '').trim().toLowerCase();
    if (!raw) {
      return false;
    }
    try {
      const parsed = new URL(raw);
      const host = parsed.hostname;
      const path = parsed.pathname || '';
      if (host === 'accounts.google.com' || host === 'accounts.youtube.com' ||
          host === 'appleid.apple.com' || host === 'idmsa.apple.com' ||
          host === 'login.microsoftonline.com' || host === 'login.live.com' ||
          host === 'login.yahoo.com' || host === 'facebook.com' ||
          host === 'www.facebook.com' || host === 'm.facebook.com' ||
          host === 'twitter.com' || host === 'api.twitter.com' ||
          host === 'x.com') {
        return true;
      }
      if (host.endsWith('.google.com') &&
          (host.includes('accounts') || host.includes('oauth'))) {
        return true;
      }
      return path.includes('/o/oauth2') || path.includes('/oauth2') ||
          path.includes('/signin/oauth');
    } catch (_) {
      return /accounts\.google|appleid\.apple|idmsa\.apple|login\.microsoft|oauth2/i.test(raw);
    }
  }

  private clearOauthFlow(): void {
    this.oauthTabId = null;
    this.oauthTabUrl = null;
    this.oauthWindowId = null;
    this.oauthFlowStartedAt = 0;
    this.oauthAnnounced = false;
    this.oauthPopupExpectedUntil = 0;
  }

  private async activateBrowserTab(tabId: string|null): Promise<boolean> {
    const parsed = Number.parseInt(String(tabId || ''), 10);
    if (!Number.isFinite(parsed) || parsed <= 0 || !this.canUseChromeSend()) {
      return false;
    }
    try {
      const res = await this.sendWithPromise(
          'dashboardAgentActivateTab', {tab_id: parsed});
      return !!res && res.ok !== false;
    } catch (_) {
      return false;
    }
  }

  private async snapshotKnownBrowserTabs(): Promise<void> {
    try {
      const tabs = await this.listBrowserTabs();
      this.knownBrowserTabIds = new Set(
          tabs.map((tab) => String(tab.tabId || '').trim()).filter(Boolean));
    } catch (_) {
    }
  }

  private looksLikeOauthProviderClick(action: any, context: PageContextSummary):
      boolean {
    if (String(action?.action || '') !== 'click') {
      return false;
    }
    if (this.isOauthLoginUrl(context.url)) {
      return false;
    }
    const target = (context.elements ?? []).find(
        (el) => typeof action?.target_id === 'number' && el.id === action.target_id);
    const blob = [
      target?.cssClass,
      target?.label,
      target?.fieldName,
      target?.placeholder,
      target ? bestContextNodeText(target) : '',
    ].map((part) => String(part || '')).join(' ').toLowerCase();
    return /google-btn|apple-btn|continue with google|continue with apple|sign in with (google|apple|facebook|microsoft)|oauth/.test(blob);
  }

  private async syncOauthFlow(options?: {expectPopup?: boolean}):
      Promise<boolean> {
    const expectPopup = !!options?.expectPopup ||
        (this.oauthPopupExpectedUntil > 0 && Date.now() < this.oauthPopupExpectedUntil);
    const taskId = String(this.getAuthoritativeTaskTabId() || '').trim();
    let tabs: Array<{
      tabId: string,
      title: string,
      url: string,
      isPopup?: boolean,
      isOauth?: boolean,
      windowId?: number,
    }> = [];
    try {
      tabs = await this.listBrowserTabs();
    } catch (_) {
      tabs = [];
    }
    if (this.oauthTabId) {
      const stillOpen = tabs.find((tab) => tab.tabId === this.oauthTabId);
      if (!stillOpen) {
        this.addSystemMessage(
            'OAuth login window closed — resuming the original task tab.');
        this.clearOauthFlow();
        await this.snapshotKnownBrowserTabs();
        return false;
      }
      this.oauthTabUrl = stillOpen.url || this.oauthTabUrl;
      if (typeof stillOpen.windowId === 'number') {
        this.oauthWindowId = stillOpen.windowId;
      }
      await this.activateBrowserTab(this.oauthTabId);
      return true;
    }
    const unknown = tabs.filter(
        (tab) => tab.tabId && tab.tabId !== taskId &&
            !this.knownBrowserTabIds.has(tab.tabId));
    const oauthish = tabs.filter(
        (tab) => tab.tabId && tab.tabId !== taskId &&
            (tab.isOauth || this.isOauthLoginUrl(tab.url)));
    let chosen = oauthish[0] ||
        (expectPopup ? unknown.find((tab) => tab.isPopup) : undefined) ||
        (expectPopup ? unknown[0] : undefined) ||
        null;
    if (!chosen && expectPopup) {
      await sleep(700);
      try {
        tabs = await this.listBrowserTabs();
      } catch (_) {
        tabs = [];
      }
      const retryUnknown = tabs.filter(
          (tab) => tab.tabId && tab.tabId !== taskId &&
              !this.knownBrowserTabIds.has(tab.tabId));
      chosen = tabs.find(
          (tab) => tab.tabId && tab.tabId !== taskId &&
              (tab.isOauth || this.isOauthLoginUrl(tab.url))) ||
          retryUnknown.find((tab) => tab.isPopup) || retryUnknown[0] || null;
    }
    if (!chosen) {
      return false;
    }
    this.oauthTabId = chosen.tabId;
    this.oauthTabUrl = chosen.url || null;
    this.oauthWindowId =
        typeof chosen.windowId === 'number' ? chosen.windowId : null;
    this.oauthFlowStartedAt = Date.now();
    this.oauthPopupExpectedUntil = 0;
    if (!this.oauthAnnounced) {
      this.oauthAnnounced = true;
      const windowText = typeof chosen.windowId === 'number' ?
          `, window ${chosen.windowId}` : '';
      this.addSystemMessage(
          `OAuth login window is open (tab ${chosen.tabId}${windowText}` +
          (chosen.url ? `, ${chosen.url}` : '') +
          '). Context and actions now use this window until it closes.');
    }
    await sleep(400);
    await this.activateBrowserTab(chosen.tabId);
    return true;
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
    if (this.oauthTabId && trimmed && trimmed === String(this.oauthTabId)) {
      return;
    }
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
    this.clearOauthFlow();
    this.knownBrowserTabIds = new Set();
  }

  private getContextTabCandidates(): string[] {
    if (this.oauthTabId) {
      return [this.oauthTabId];
    }
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

  private async maybeCompleteTask(responseText: string): Promise<boolean> {
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
        await this.finishTask(success ? 'success' : 'failure', reason);
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
    this.mementos.pruneCompletedTask(completed.id);
    this.currentTask = null;
    this.currentTaskStepIndex = 0;
    this.clearLoopGuidanceState();
    this.clearCaptchaChallengeState();
    this.clearTaskTabBinding();
    this.clearWorkingContext();
    this.updateTaskInfo();
    await this.logTaskEnd(completed, status, reason);
    if (status === 'success') {
      this.addSystemMessage(
          reason ? `Task complete: ${reason}. Reviewing the recorded script.` :
                   'Task marked complete. Reviewing the recorded script.');
    }
    await this.flushRecorderScaffoldForTask(status, completed);
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

  /** Drop every task and the chat without tearing down the panel WebUI. */
  private resetPanel(): void {
    this.taskLoopStopRequested = true;
    this.queueRunning = false;
    this.clearPendingTaskStart();
    if (this.llmAbortController) {
      this.llmAbortController.abort();
      this.llmAbortController = null;
    }
    this.llmRequestInFlight = false;
    this.updateLlmUiState();
    void this.abortActiveTooling('panel reset');
    if (this.canUseChromeSend()) {
      try {
        (window as any).chrome.send('cancel_simulachrome_batch', []);
      } catch (_) {}
    }
    this.taskQueue = [];
    this.currentTask = null;
    this.currentTaskStepIndex = 0;
    this.pausedResumeFromStep = 0;
    this.clearLoopGuidanceState();
    this.clearCaptchaChallengeState();
    this.clearTaskTabBinding();
    this.clearWorkingContext();
    this.loginScaffoldKey = '';
    this.loginScaffoldRepeats = 0;
    this.loginCaptchaOwned = false;
    this.loginSubmitArmed = false;
    this.clearLoginAutofillLatch();
    this.externalCredentialPlaceholderKeys.clear();
    this.managerActionHistory = [];
    this.clearTaskPlan();
    this.previousManagerAccessibilityLines = [];
    this.lastEmbeddedStepSummary = '';
    this.lastPageContext = null;
    this.lastActionSummary = null;
    this.messages = [];
    this.hasCoreAssistantContextPrompt = false;
    this.promptHistoryIndex = -1;
    this.promptHistoryDraft = '';
    if (this.promptInputEl) {
      this.promptInputEl.value = '';
    }
    try {
      localStorage.removeItem('glic.last_run_transcript');
    } catch (_) {}
    this.render();
    this.updateQueueStatus();
    this.updateTaskInfo();
    this.addSystemMessage(
        'Panel reset. Tasks, queue, and chat cleared.');
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
        this.queueActiveTaskEl.textContent =
            `${this.currentTask.goal} · ${this.currentTask.url}`;
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
    isPopup?: boolean,
    isOauth?: boolean,
    windowId?: number,
    source: string,
  }>> {
    const q = String(query || '').trim().toLowerCase();
    const byId = new Map<string, {
      tabId: string,
      title: string,
      url: string,
      isActiveInWindow?: boolean,
      isWindowActive?: boolean,
      isPopup?: boolean,
      isOauth?: boolean,
      windowId?: number,
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

    if (this.canUseChromeSend()) {
      try {
        const res = await this.sendWithPromise('dashboardAgentListTabs', {});
        const listed = Array.isArray(res?.tabs) ? res.tabs : [];
        for (const raw of listed) {
          const tabId = String(raw?.tabId ?? raw?.tab_id ?? '').trim();
          if (!tabId) {
            continue;
          }
          const url = String(raw?.url || '');
          const title = String(raw?.title || '');
          if (q) {
            const hay = `${title} ${url}`.toLowerCase();
            if (!hay.includes(q)) {
              continue;
            }
          }
          byId.set(tabId, {
            tabId,
            title,
            url,
            isActiveInWindow: !!raw?.isActiveInWindow,
            isWindowActive: !!raw?.isWindowActive,
            isPopup: !!raw?.isPopup,
            isOauth: !!(raw?.isOauth || this.isOauthLoginUrl(url)),
            windowId: Number.isFinite(Number(raw?.windowId)) ? Number(raw.windowId) :
                                                             undefined,
            source: 'dashboard',
          });
        }
      } catch (_) {
      }
    }

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
      const aOauth = (a.isOauth || a.tabId === this.oauthTabId) ? 0 : 1;
      const bOauth = (b.isOauth || b.tabId === this.oauthTabId) ? 0 : 1;
      if (aOauth !== bOauth) {
        return aOauth - bOauth;
      }
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
            oauthTabId: this.oauthTabId || undefined,
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

  private resolveScreenshotClickSpace(
      imageWidth: number,
      imageHeight: number,
      captureCss?: {width?: number, height?: number},
  ): {clickW: number, clickH: number} {
    // Size of the photographed viewport only. Not page context or elements.
    const capW = Number(captureCss?.width || 0);
    const capH = Number(captureCss?.height || 0);
    const clickW = (Number.isFinite(capW) && capW > 0) ? Math.round(capW) : imageWidth;
    const clickH = (Number.isFinite(capH) && capH > 0) ? Math.round(capH) : imageHeight;
    return {clickW, clickH};
  }

  /**
   * Make the picture the model sees have exactly one pixel per viewport DIP.
   * No grid. If the capture is already that size, send the raw PNG.
   */
  private async prepareOneToOneScreenshot(shot: {
    base64: string,
    mimeType: string,
    widthPixels?: number,
    heightPixels?: number,
    cssWidth?: number,
    cssHeight?: number,
    note?: string,
  }): Promise<{
    base64: string,
    mimeType: string,
    cssWidth: number,
    cssHeight: number,
    imageWidth: number,
    imageHeight: number,
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
      const map = this.resolveScreenshotClickSpace(imageWidth, imageHeight, {
        width: shot.cssWidth,
        height: shot.cssHeight,
      });
      const {clickW, clickH} = map;
      // Pre-resize to the pixels Claude will see. Otherwise it downscales
      // (token budget) and [x,y] is in the small image, which lands left/up.
      const [sentW, sentH] = claudeResizedSize(clickW, clickH);
      const note =
          `[Screenshot ${sentW}×${sentH} px. (0,0)=top-left. ` +
          `Send {"coordinate":{"x":120,"y":80}} in THESE image pixels — not the browser window.]`;
      const alreadyClaudeSize = imageWidth === sentW && imageHeight === sentH;
      if (alreadyClaudeSize) {
        return {
          base64: shot.base64,
          mimeType: shot.mimeType || 'image/png',
          cssWidth: clickW,
          cssHeight: clickH,
          imageWidth: sentW,
          imageHeight: sentH,
          note,
        };
      }
      const canvas = document.createElement('canvas');
      canvas.width = sentW;
      canvas.height = sentH;
      const ctx = canvas.getContext('2d');
      if (!ctx) {
        return null;
      }
      ctx.imageSmoothingEnabled = sentW !== imageWidth || sentH !== imageHeight;
      ctx.imageSmoothingQuality = 'high';
      ctx.drawImage(img, 0, 0, sentW, sentH);
      const mimeType = 'image/png';
      const dataUrl = canvas.toDataURL(mimeType);
      const comma = dataUrl.indexOf(',');
      if (comma < 0) {
        return null;
      }
      return {
        base64: dataUrl.slice(comma + 1),
        mimeType,
        cssWidth: clickW,
        cssHeight: clickH,
        imageWidth: sentW,
        imageHeight: sentH,
        note,
      };
    } catch (e) {
      console.warn('[glic] prepareOneToOneScreenshot failed', e);
      return null;
    }
  }

  private async captureTaskViewportScreenshot():
      Promise<{
        base64: string,
        mimeType: string,
        widthPixels?: number,
        heightPixels?: number,
        cssWidth?: number,
        cssHeight?: number,
      }|null> {
    if (this.canUseChromeSend()) {
      const parsed = Number.parseInt(
          String(this.getAuthoritativeTaskTabId() || this.currentTabId || ''), 10);
      const payload: Record<string, unknown> = {};
      if (Number.isFinite(parsed)) {
        payload['tab_id'] = parsed;
      }
      if (this.modelUsesMailboxVision()) {
        try {
          // Unregistered chrome.send never replies. Time out so CopyFromSurface
          // still runs on an old framework or a stuck GPU attach.
          const mailbox = await Promise.race([
            this.sendWithPromise('capture_simulachrome_frame', payload),
            new Promise((_, reject) => {
              setTimeout(() => reject(new Error('mailbox_timeout')), 15000);
            }),
          ]);
          const mailboxB64 = String(mailbox?.base64 || '');
          if (mailbox && mailbox.ok !== false && mailboxB64) {
            this.lastScreenshotError = null;
            this.lastScreenshotSource = String(mailbox.source || 'mailbox');
            return {
              base64: mailboxB64,
              mimeType: String(mailbox.mimeType || 'image/png'),
              widthPixels: Number(mailbox.widthPixels || 0) || undefined,
              heightPixels: Number(mailbox.heightPixels || 0) || undefined,
              cssWidth: Number(mailbox.cssWidth || 0) || undefined,
              cssHeight: Number(mailbox.cssHeight || 0) || undefined,
            };
          }
          this.lastScreenshotError = String(
              mailbox?.error || 'capture_simulachrome_frame failed');
        } catch (e) {
          this.lastScreenshotError = String(e || 'capture_simulachrome_frame failed');
        }
      }
      const res = await this.sendWithPromise('dashboardAgentScreenshot', payload);
      const dataUrl = String(res?.image || '');
      const base64 = String(res?.base64 ||
          (dataUrl.includes(',') ? dataUrl.split(',')[1] : ''));
      if (res && res.ok !== false && base64) {
        this.lastScreenshotError = null;
        this.lastScreenshotSource = 'cpu';
        return {
          base64,
          mimeType: String(res.mimeType || 'image/png'),
          widthPixels: Number(res.widthPixels || 0) || undefined,
          heightPixels: Number(res.heightPixels || 0) || undefined,
          cssWidth: Number(res.cssWidth || 0) || undefined,
          cssHeight: Number(res.cssHeight || 0) || undefined,
        };
      }
      this.lastScreenshotError = String(res?.error || 'dashboardAgentScreenshot failed');
      return null;
    }
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

  private async collectContextFromDashboard(tabId?: string):
      Promise<PageContextSummary|null> {
    if (!this.canUseChromeSend()) {
      return null;
    }
    const parsed = Number.parseInt(
        String(tabId || this.getActuationTabId() || ''), 10);
    if (this.oauthTabId && Number.isFinite(parsed) &&
        String(parsed) === this.oauthTabId) {
      await this.activateBrowserTab(this.oauthTabId);
    }
    const payload: Record<string, unknown> = {};
    if (Number.isFinite(parsed)) {
      payload['tab_id'] = parsed;
    }
    let res: any = null;
    try {
      res = await this.sendWithPromise('dashboardAgentGetPageContext', payload);
    } catch (e) {
      this.lastContextError = String(e || 'dashboardAgentGetPageContext failed');
      return null;
    }
    if (!res || res.ok === false) {
      this.lastContextError = String(res?.error || 'dashboardAgentGetPageContext failed');
      return null;
    }
    const elements = Array.isArray(res.elements) ? res.elements : [];
    const summary: PageContextSummary = {
      elements,
      tabId: String(res.tabId ?? res.tab_id ?? parsed ?? ''),
      tabIdInt: Number(res.tabId ?? res.tab_id ?? parsed),
      url: String(res.url || ''),
      title: String(res.title || ''),
      innerText: String(res.innerText || ''),
      documentId: String(res.documentId || ''),
      hasOverlay: !!(res.hasOverlay ||
          elements.some((el: any) => el && el.overlay)),
    };
    await this.annotateLoginState(summary);
    const vp = res.viewportCss || {};
    const cssW = Number(vp.width || res.viewportWidth || 0);
    const cssH = Number(vp.height || res.viewportHeight || 0);
    if (Number.isFinite(cssW) && cssW > 0 && Number.isFinite(cssH) && cssH > 0) {
      summary.viewportCss = {width: Math.round(cssW), height: Math.round(cssH)};
    }
    this.lastContextError = null;
    if (summary.tabId &&
        !(this.oauthTabId && summary.tabId === this.oauthTabId)) {
      this.bindTaskTab(summary.tabId, summary.url, summary.title);
    }
    this.lastPageContext = summary;
    return summary;
  }

  private async collectContext(): Promise<PageContextSummary> {
    await this.syncOauthFlow();
    const dashboardContext = await this.collectContextFromDashboard();
    if (dashboardContext) {
      return dashboardContext;
    }
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
    if (!summary.tabId && !summary.tabIdInt) {
      return false;
    }
    const url = String(summary.url || '');
    const httpUrl = /^https?:/i.test(url);
    if (!httpUrl || /about:blank/i.test(url)) {
      return false;
    }
    const inner = (summary.innerText ?? '').trim();
    const title = String(summary.title || '').trim();
    const labels = summary.elements.map((el) => (el.label || '').trim().toUpperCase())
        .filter((label) => label);
    const onlyLoading = labels.length > 0 &&
        labels.every((label) => label === 'LOADING...' || label === 'LOADING');
    if (onlyLoading && summary.elements.length <= 1 && inner.length < 10 && !title) {
      return false;
    }
    if (inner && inner.toUpperCase() === 'LOADING...' &&
        summary.elements.length <= 1 && !title) {
      return false;
    }
    // A committed http(s) task tab is enough to send the model a snapshot.
    // Casino/SPA pages may keep a spinner forever; waiting for 3+ APC nodes
    // never completes.
    return true;
  }

  private async collectContextWithRetry(): Promise<PageContextSummary> {
    let last: PageContextSummary = {elements: []};
    // Avoid hammering APC/screenshot pipelines while the SPA is still booting.
    // Repeated GetContext* during load is a major paint-starvation source.
    let delayMs = 800;
    let readyStreak = 0;
    let loadNoticeShown = false;
    const start = Date.now();

    while (Date.now() - start < this.maxLoadWaitMs) {
      last = await this.collectContext();
      if (this.isContextReady(last)) {
        readyStreak++;
        if (readyStreak >= 1) {
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
      delayMs = Math.min(Math.floor(delayMs * 1.5), 2500);
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
      lines.push('Return one flat JSON action only.');
      lines.push('{"action":"click","target_id":1}');
      lines.push('{"action":"type","target_id":2,"text":"{Email}"}');
      lines.push('{"action":"navigate","url":"https://example.com/"}');
      lines.push('{"action":"scroll","direction":"down","distance":600}');
      lines.push('{"action":"request_screenshot"}');
      lines.push('{"action":"complete","status":"solved","summary":"..."}');
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
      const cls = String(m.el.cssClass || '').trim();
      if (m.text && isButtonLike) {
        desc.text = m.text;
        if (m.kind) {
          desc.kind = m.kind;
        }
        if (cls) {
          desc.cls = cls;
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
    const overlayIds = Array.from(new Set(meta
        .filter((m) => m.el.overlay)
        .map((m) => m.el.id)))
        .sort((a, b) => a - b);
    const goalText = String(this.currentTask?.goal || '').toLowerCase();
    const wantsGoogle = /google-btn|continue with google|google oauth|google sign[- ]?in/.test(goalText);
    const wantsApple = /apple-btn|continue with apple|apple (id|oauth|sign[- ]?in)/.test(goalText) &&
        !/not apple|not use apple|don't use apple|do not use apple/.test(goalText);
    const googleIds = meta
        .filter((m) => /google/i.test(String(m.el.cssClass || '')) ||
                       /continue with google/i.test(m.text || ''))
        .map((m) => m.el.id);
    const appleIds = meta
        .filter((m) => /apple/i.test(String(m.el.cssClass || '')) ||
                       /continue with apple/i.test(m.text || ''))
        .map((m) => m.el.id);
    let oauthHint = '';
    if (this.oauthTabId) {
      oauthHint =
          `\nPRIORITY: OAuth login window is open. tab_id=${this.oauthTabId}` +
          (typeof this.oauthWindowId === 'number' ? `, window_id=${this.oauthWindowId}` : '') +
          (this.oauthTabUrl ? `, url=${this.oauthTabUrl}` : '') +
          `. The elements below are from THIS window. ` +
          `click and type use these element ids and are executed in this window, ` +
          `not on the original site (task tab ${this.getAuthoritativeTaskTabId() || '?'}). ` +
          `Finish account selection, email, and consent here. ` +
          `Resume the original site only after this window closes.`;
    } else if (this.isOauthLoginUrl(context.url)) {
      oauthHint =
          '\nPRIORITY: This page is the OAuth login flow. Complete account selection / email / consent now before any other task steps.';
    } else if (wantsGoogle && googleIds.length) {
      oauthHint =
          `\nHint: USER GOAL names Google OAuth / .google-btn — click id ${
              googleIds.join(', ')}. ` +
          `Do not click Apple${appleIds.length ? ` (id ${appleIds.join(', ')})` : ''} ` +
          `or the email/code path unless that Google control is missing. ` +
          `If a Google click reports success but the page does not change, retry that id or ` +
          `request_screenshot and click the Google button by coordinate.`;
    } else if (wantsApple && appleIds.length) {
      oauthHint =
          `\nHint: USER GOAL names Apple sign-in — click id ${appleIds.join(', ')}.`;
    }

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
      if (m.el.overlay) {
        entry['overlay'] = true;
      }
      if (m.el.cssClass && desc) {
        desc.cls = m.el.cssClass;
        entry['desc'] = desc;
      }

      return entry;
    });
    const payload = {
      taskTabId: this.currentTask ? this.getAuthoritativeTaskTabId() : undefined,
      oauthTabId: this.oauthTabId || undefined,
      oauthWindowId: this.oauthWindowId || undefined,
      oauthUrl: this.oauthTabUrl || undefined,
      oauthAgeSec: this.oauthFlowStartedAt ?
          Math.max(0, Math.round((Date.now() - this.oauthFlowStartedAt) / 1000)) :
          undefined,
      actuationTabId: this.getActuationTabId() || undefined,
      taskStep: this.currentTask ? this.currentTaskStepIndex : undefined,
      tabId: context.tabId,
      tabIdInt: context.tabIdInt,
      url: context.url,
      title: context.title,
      documentId: context.documentId,
      element_count: elements.length,
      elements,
      innerText: context.innerText?.slice(0, 2000) ?? '',
      hasOverlay: !!(context.hasOverlay ||
          (context.elements ?? []).some((el) => el.overlay)),
      session: context.session || undefined,
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
      (overlayIds.length
          ? `\nHint: a popup/overlay is open (ids ${overlayIds.join(', ')}). ` +
            `These block the page — dismiss or use them before background controls. ` +
            `Zero-size, aria-hidden, pointer-events, and disabled flags are often decoys; prefer overlay:true targets.`
          : '') +
      (oauthHint || '') +
      (this.sessionPromptLine(context) ? '\n' + this.sessionPromptLine(context) : '') +
      (this.mustSolveCaptchaBeforeSubmit(context)
          ? '\n' + this.captchaMustSolveBeforeSubmitHint()
          : '') +
      (this.captchaChallengeActive ? '\n' + this.captchaSolverHint() : '') +
      (this.hasLiveScreenshot() ? '\n' + this.liveScreenshotMustClickHint() : '') +
      '\nProvide the next action JSON only.' +
      this.planPromptLine()
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
    if (this.oauthTabId) {
      lines.push(
          `PRIORITY: OAuth login window is still open. tab_id=${this.oauthTabId}` +
          (typeof this.oauthWindowId === 'number' ? `, window_id=${this.oauthWindowId}` : '') +
          (this.oauthTabUrl ? `, url=${this.oauthTabUrl}` : '') +
          '. The element ids you have are from this window. click and type run in this window until it closes.');
    }
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
    const sessionLine = this.sessionPromptLine(context);
    if (sessionLine) {
      lines.push(sessionLine);
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
    const planLine = this.planPromptLine().trim();
    if (planLine) {
      lines.push(planLine);
    }
    lines.push(
        'Prefer acting with prior target ids when they still apply. ' +
        'Return {"action":"request_context"} when needed ids are missing or the UI changed ' +
        '(including same-URL modal/dialog/popup/overlay).');
    lines.push('For another open tab (2FA/email), use list_tabs / peek_tab / request_context with tab_id and reload:true if the inbox may be stale.');
    if (this.mustSolveCaptchaBeforeSubmit(context)) {
      lines.push(this.captchaMustSolveBeforeSubmitHint());
    }
    if (this.captchaChallengeActive) {
      lines.push(this.captchaSolverHint());
    }
    if (this.hasLiveScreenshot()) {
      lines.push(this.liveScreenshotMustClickHint());
    }
    const goalText = String(this.currentTask?.goal || '').toLowerCase();
    if (/google-btn|continue with google|google oauth/.test(goalText)) {
      const labeledGoogle = this.lastTaskLabeledElements
                                .filter((el) => /google/i.test(el.text || ''))
                                .map((el) => el.id);
      if (labeledGoogle.length) {
        lines.push(
            `USER GOAL requires Continue with Google (.google-btn). Use id ${
                labeledGoogle.join(', ')}. Do not use Apple or email login as a substitute.`);
      }
      if (pageUnchanged && /click target_id=/.test(this.lastActionSummary || '')) {
        lines.push(
            'Last click did not change the page. If that click was not the Google OAuth button, click the Google id. ' +
            'If it was, retry it or request_screenshot and click the Google button by coordinate — do not switch to Apple/email.');
      }
    }
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
    await this.snapshotKnownBrowserTabs();
    let previousUrl = '';
    let forceContextPrompt = true;
    let previousContextSignature = '';
    let repeatedFreshContextCount = 0;
    try {
    const startStep = Math.max(
        0,
        Math.min(
            this.pausedResumeFromStep > 0 ?
                this.pausedResumeFromStep - 1 :
                0,
            Math.max(0, this.maxStepsPerTask - 1)));
    this.pausedResumeFromStep = 0;
    for (let step = startStep; step < this.maxStepsPerTask; step++) {
      if (this.taskLoopStopRequested || !this.currentTask) {
        break;
      }
      this.currentTaskStepIndex = step + 1;
      this.updateTaskInfo();
      if (await this.syncOauthFlow()) {
        forceContextPrompt = true;
      }
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
      const shouldSendFullContext =
          forceContextPrompt || step === 0 || urlChanged || !!this.oauthTabId;
      if (forceContextPrompt) {
        if (contextSignature === previousContextSignature) {
          repeatedFreshContextCount += 1;
        } else {
          repeatedFreshContextCount = 0;
        }
      } else {
        repeatedFreshContextCount = 0;
      }
      if (this.isManagerProfile() &&
          await this.advanceLoginScaffold(context)) {
        previousUrl = currentUrl;
        previousContextSignature = contextSignature;
        forceContextPrompt = true;
        continue;
      }
      const rollbackUrl = this.executionTimeline.close(
          context, String(this.currentTask?.goal || ''));
      if (rollbackUrl) {
        this.addSystemMessage(`Reverting to the last good page after a failed step.`);
        await this.navigateCurrentTab(rollbackUrl);
        forceContextPrompt = true;
        continue;
      }
      this.applyPendingLoopGuidance();
      let prompt = this.prependUserGoalPriority(
          this.isManagerProfile() ?
              this.buildManagerContextPrompt(context) :
              (shouldSendFullContext ?
                   this.buildContextPrompt(context) :
                   this.buildContinuationPrompt(context, {pageUnchanged})));
      const memoryBlock = this.mementos.promptBlock(this.currentTask?.id);
      if (memoryBlock) {
        prompt = memoryBlock + '\n\n' + prompt;
      }
      const guidanceBlock = this.formatActiveLoopGuidanceForPrompt();
      if (guidanceBlock) {
        prompt = guidanceBlock + prompt;
      }
      if (this.mustSolveCaptchaBeforeSubmit(context)) {
        prompt += '\n' + this.captchaMustSolveBeforeSubmitHint();
      }
      if (this.captchaChallengeActive) {
        prompt += '\n' + this.captchaSolverHint();
      }
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
      if (this.hasLiveScreenshot()) {
        prompt += '\n' + this.liveScreenshotMustClickHint();
      }
      if (this.taskLoopStopRequested || !this.currentTask) {
        break;
      }
      this.managerPageUnchanged = pageUnchanged;
      this.capturedScreenshotThisTurn = false;
      // One screenshot is enough. Do not reshoot every turn — that burns the
      // step budget (20 shots, no clicks). Only capture when we do not already
      // have a pending/recent shot and the model has not been screenshot-spinning.
      if (this.captchaChallengeActive && this.supportsScreenshotAction() &&
          !this.shouldEnforceHeadlessOnlyForCurrentTask() &&
          !this.pendingScreenshot && this.screenshotOnlyStreak === 0) {
        await this.captureScreenshotForNextTurn('captcha visual challenge');
      } else if (this.modelUsesMailboxVision() &&
                 !this.shouldEnforceHeadlessOnlyForCurrentTask() &&
                 !this.pendingScreenshot && this.screenshotOnlyStreak === 0 &&
                 (shouldSendFullContext || !pageUnchanged)) {
        await this.captureScreenshotForNextTurn('mailbox vision');
      }
      const assistantText = await this.sendUserMessage(
          prompt,
          {context, handleActions: true},
      );
      if (!this.currentTask) {
        break;
      }
      if (assistantText === MODEL_BUSY_MESSAGE) {
        step -= 1;
        await sleep(500);
        continue;
      }
      if (this.taskLoopStopRequested) {
        break;
      }
      if (guidanceBlock) {
        this.loopGuidanceAppliedTurns += 1;
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
    if (this.humanCaptchaHold && this.currentTask) {
      this.queueRunning = false;
      this.addSystemMessage(
          'Waiting for you to solve the captcha. Press "I solved it" or Resume when done.');
      this.updateQueueStatus();
      this.updateTaskInfo();
      this.syncSolvedCaptchaButtons();
    } else if (this.taskLoopStopRequested && this.currentTask) {
      this.queueRunning = false;
      this.addSystemMessage(
          this.lastProviderFailure ||
          'Task paused. Guide current task still works. Press Resume to continue from here.');
      this.lastProviderFailure = '';
      this.updateQueueStatus();
      this.updateTaskInfo();
    } else if (this.currentTask) {
      // Keep the task so Start Queue can reset the step budget and continue.
      this.queueRunning = false;
      this.addSystemMessage(
          `Max steps (${this.maxStepsPerTask}) reached. Increase Max steps if needed, then press Start Queue to continue.`);
      this.updateQueueStatus();
      this.updateTaskInfo();
    }
    } finally {
      this.taskLoopRunning = false;
      this.updateLlmUiState();
    }
  }

  /** "[a] LOG IN (clickable,focusable)" is an accessibility row, not a locate phrase. */
  private visibleTargetText(raw: unknown): string {
    let text = String(raw ?? '').trim();
    if (!text || /^\d+$/.test(text)) {
      return '';
    }
    const tagged = text.match(/^\[[^\]]{1,24}\]\s+(.+)$/);
    if (tagged) {
      text = tagged[1]!.trim();
    }
    text = text.replace(
        /\s*\((?:clickable|focusable|editable|password|email)(?:,\s*(?:clickable|focusable|editable|password|email))*\)\s*$/i,
        '').trim();
    return text;
  }

  /** Manager steps use op or action, plus a visual target. */
  private coercePlannedStep(step: any): any|null {
    if (!step || typeof step !== 'object' || Array.isArray(step)) {
      return null;
    }
    const op = String(step.op || step.action || '').trim();
    if (!op || /^(batch|plan)$/i.test(op)) {
      return null;
    }
    const out: any = {op: op.toLowerCase()};
    for (const key of ['target', 'from', 'to'] as const) {
      const text = this.visibleTargetText(step[key]);
      if (text) {
        out[key] = text;
      }
    }
    if (step.args && typeof step.args === 'object' && !Array.isArray(step.args)) {
      out.args = step.args;
    } else {
      const args: any = {};
      if (typeof step.text === 'string' && step.text.trim()) {
        args.text = step.text;
      } else if (typeof step.value === 'string' && step.value.trim()) {
        args.text = step.value;
      }
      if (typeof step.key === 'string' && step.key.trim()) {
        args.key = step.key;
      }
      if (typeof step.direction === 'string' && step.direction.trim()) {
        args.direction = step.direction;
      }
      if (step.amount_viewport != null) {
        args.amount_viewport = step.amount_viewport;
      }
      if (typeof step.submit === 'boolean') {
        args.submit = step.submit;
      }
      if (Object.keys(args).length) {
        out.args = args;
      }
    }
    if (step.expect && typeof step.expect === 'object') {
      out.expect = step.expect;
    }
    if (typeof step.id === 'string' && step.id.trim()) {
      out.id = step.id.trim();
    }
    if (typeof step.strategy === 'string' && step.strategy.trim()) {
      out.strategy = step.strategy;
    }
    return out;
  }

  /** {"batch":[{action,target}]} is the same plan as {"action":"batch","steps":[...]}. */
  private asManagerBatch(obj: any): any|null {
    if (!obj || typeof obj !== 'object') {
      return null;
    }
    const named = String(obj.action || '').trim().toLowerCase();
    let stepsRaw: any[]|null = Array.isArray(obj.batch) ? obj.batch :
        (Array.isArray(obj.steps) ? obj.steps : null);
    if (!stepsRaw && (named === 'batch' || named === 'plan') &&
        Array.isArray(obj.actions)) {
      stepsRaw = obj.actions;
    }
    if (!stepsRaw) {
      return null;
    }
    const steps = stepsRaw
        .map((step) => this.coercePlannedStep(step))
        .filter((step) => !!step);
    if (!steps.length) {
      return null;
    }
    return {
      action: 'batch',
      steps,
      stop_on_failure: obj.stop_on_failure !== false,
    };
  }

  private extractActions(responseText: string): any[] {
    const parsed = extractJsonObjects(responseText);
    const out: any[] = [];
    const seen = new Set<string>();
    const push = (obj: any) => {
      let key = '';
      try {
        key = JSON.stringify(obj);
      } catch (_) {
        key = '';
      }
      if (key && seen.has(key)) {
        return;
      }
      if (key) {
        seen.add(key);
      }
      out.push(obj);
    };
    for (const obj of parsed) {
      if (!obj || typeof obj !== 'object') {
        continue;
      }
      const actionName = typeof obj.action === 'string' ?
          obj.action.trim().toLowerCase() : '';
      if (actionName === 'batch' || actionName === 'plan' ||
          Array.isArray(obj.batch) || Array.isArray(obj.steps)) {
        const batch = this.asManagerBatch(obj);
        if (batch) {
          push(batch);
          continue;
        }
      }
      if (actionName) {
        push(obj);
        continue;
      }
      const alt = obj.action_type ?? obj.actionType ?? obj.type ?? obj.name;
      if (typeof alt === 'string' && alt.trim()) {
        push(obj);
      }
    }
    return out;
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
    if (actionType === 'save_script' || actionType === 'complete_site' ||
        actionType === 'complete_script' || actionType === 'site_complete' ||
        actionType === 'clip_script' || actionType === 'save_site_script') {
      const out: any = {action: 'save_script'};
      if (typeof raw.name === 'string' && raw.name.trim()) {
        out.name = raw.name.trim();
      }
      if (typeof raw.reason === 'string' && raw.reason.trim()) {
        out.reason = raw.reason.trim();
      } else if (typeof raw.message === 'string' && raw.message.trim()) {
        out.reason = raw.message.trim();
      }
      return out;
    }
    if (actionType === 'batch' || actionType === 'plan') {
      const steps = Array.isArray(raw.steps) ? raw.steps :
          (Array.isArray(raw.actions) ? raw.actions : []);
      return {
        action: 'batch',
        steps,
        stop_on_failure: raw.stop_on_failure !== false,
      };
    }
    if (actionType === 'direct' || actionType === 'observe' ||
        actionType === 'embedded_step' || actionType === 'vlm_step') {
      const instruction = String(
          raw.instruction || raw.text || raw.prompt || raw.payload || '').trim();
      return {
        action: actionType === 'observe' ? 'observe' : 'direct',
        instruction,
      };
    }
    if (actionType === 'store_memento' || actionType === 'note' ||
        actionType === 'remember') {
      const content = String(raw.text || raw.content || raw.note || '').trim();
      const out: any = {action: 'store_memento', text: content};
      if (raw.global === true || raw.scope === 'global') out.global = true;
      if (Number.isFinite(Number(raw.ttl))) out.ttl = Number(raw.ttl);
      return out;
    }
    if (actionType === 'read_memento' || actionType === 'read_note') {
      return {action: 'read_memento'};
    }
    if (actionType === 'complete' || actionType === 'task_complete' ||
        actionType === 'mark_task_complete' || actionType === 'mark_complete' ||
        actionType === 'task_done' || actionType === 'finish_task' ||
        actionType === 'done') {
      const normalizedComplete: any = {action: 'complete'};
      if (raw.status === 'blocked') {
        normalizedComplete.success = false;
      } else if (raw.status === 'solved') {
        normalizedComplete.success = true;
      } else if ('success' in raw) {
        normalizedComplete.success = raw.success !== false;
      }
      const summary = typeof raw.summary === 'string' ? raw.summary.trim() : '';
      if (summary) {
        normalizedComplete.reason = summary;
      } else if (typeof raw.reason === 'string' && raw.reason.trim()) {
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
    if (actionType === 'ignore_guidance' ||
        actionType === 'drop_guidance' ||
        actionType === 'guidance_stale') {
      return {action: 'ignore_guidance'};
    }
    if (actionType === 'human_solve_captcha' ||
        actionType === 'pause_for_human' ||
        actionType === 'solve_captcha') {
      return {action: 'human_solve_captcha'};
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
          'coordinates', 'cell', 'x', 'y', 'stage', 'stage_status', 'stageStatus',
          'ignore_guidance', 'ignoreGuidance',
          'guidance_stale', 'guidanceStale', 'drop_guidance', 'dropGuidance',
        ]),
        hover: new Set([
          'action', 'target_id', 'targetId', 'element', 'coordinate',
          'coordinates', 'cell', 'x', 'y', 'stage', 'stage_status', 'stageStatus',
          'ignore_guidance', 'ignoreGuidance',
          'guidance_stale', 'guidanceStale', 'drop_guidance', 'dropGuidance',
        ]),
        type: new Set([
          'action', 'target_id', 'targetId', 'element', 'text', 'value',
          'coordinate', 'coordinates', 'cell', 'x', 'y', 'stage', 'stage_status',
          'stageStatus', 'ignore_guidance',
          'ignoreGuidance', 'guidance_stale', 'guidanceStale', 'drop_guidance',
          'dropGuidance',
        ]),
        key_press: new Set([
          'action', 'key', 'value', 'count', 'stage', 'stage_status', 'stageStatus',
          'ignore_guidance', 'ignoreGuidance',
          'guidance_stale', 'guidanceStale', 'drop_guidance', 'dropGuidance',
        ]),
        scroll: new Set([
          'action', 'target_id', 'targetId', 'element', 'direction',
          'distance', 'scope',
          'stage', 'stage_status', 'stageStatus',
          'ignore_guidance', 'ignoreGuidance', 'guidance_stale', 'guidanceStale',
          'drop_guidance', 'dropGuidance',
        ]),
        scroll_to: new Set([
          'action', 'target_id', 'targetId', 'element', 'stage', 'stage_status',
          'stageStatus', 'ignore_guidance',
          'ignoreGuidance', 'guidance_stale', 'guidanceStale', 'drop_guidance',
          'dropGuidance',
        ]),
      };
      const allowed = allowedKeysByAction[raw.action];
      if (allowed) {
        const extras = Object.keys(raw).filter((k) => !allowed.has(k));
        if (extras.length) {
          const ignorableMetaKeys = new Set([
            'success', 'reason', 'status', 'ok', 'error', 'message',
            'action_type', 'actionType', 'description', 'desc', 'comment',
            'thought', 'thinking',
            'ignore_guidance', 'ignoreGuidance', 'guidance_stale',
            'guidanceStale', 'drop_guidance', 'dropGuidance',
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
      let hasTarget = Number.isFinite(targetId) && targetId > 0;
      if (point && hasTarget) {
        this.addSystemMessage(
            `Keeping target_id ${targetId}; ignoring screenshot coordinate.`);
        // Form fields must stay on locators. Coordinates are captcha-only.
      }
      // Page scroll has no element. Iframe scroll is checked once scope is known.
      if (raw.action !== 'scroll' && !hasTarget &&
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
        if (point && !hasTarget) {
          out.coordinate = point;
        }
        return out;
      }
      if (raw.action === 'scroll') {
        const magnitude = Math.abs(Number(raw.distance ?? 600)) || 600;
        const direction = String(raw.direction || '').trim().toLowerCase();
        if (direction && direction !== 'up' && direction !== 'down') {
          this.addSystemMessage(
              `Invalid scroll direction ${JSON.stringify(raw.direction)}. Use "up" or "down".`);
          return null;
        }
        const distance = direction === 'up' ? -magnitude :
            (direction === 'down' ? magnitude : Number(raw.distance ?? 600));
        const scopeRaw = String(raw.scope || raw.element || '').toLowerCase();
        const scope =
            (scopeRaw === 'iframe' || scopeRaw === 'frame' ||
             scopeRaw === 'element') ?
                'iframe' :
                ((scopeRaw === 'page' || scopeRaw === 'window' ||
                  scopeRaw === 'document' || !hasTarget) ?
                     'page' :
                     'iframe');
        if (scope === 'iframe' && !hasTarget) {
          this.addSystemMessage(
              'Iframe scroll needs an integer target_id from Page context elements[].id.');
          return null;
        }
        const out: any = {action: 'scroll', distance, scope};
        if (hasTarget && scope === 'iframe') {
          out.target_id = targetId;
        }
        return out;
      }
      if (raw.action === 'scroll_to') {
        return {action: 'scroll_to', target_id: targetId};
      }
      if (raw.action === 'hover') {
        const out: any = {action: 'hover'};
        if (hasTarget) {
          out.target_id = targetId;
        }
        if (point && !hasTarget) {
          out.coordinate = point;
        }
        return out;
      }
      {
        const out: any = {action: 'click'};
        if (hasTarget) {
          out.target_id = targetId;
        }
        if (point && !hasTarget) {
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
  /** Keys the user named in the goal, even when the catalog has not loaded. */
  private goalNamedCredentialTokens(): {identity: string|null, password: string|null} {
    const goal = String(this.currentTask?.goal || '');
    let identity: string|null = null;
    let password: string|null = null;
    const consider = (name: string) => {
      const trimmed = String(name || '').trim();
      if (!trimmed) {
        return;
      }
      if (/pass|secret|pin/i.test(trimmed)) {
        password = password || trimmed;
      } else if (/email|e-?mail|user|login|account/i.test(trimmed)) {
        identity = identity || trimmed;
      }
    };
    const pair = goal.match(
        /\b([A-Za-z][\w.-]*)\s+and\s+([A-Za-z][\w.-]*)\s+keys?\b/);
    if (pair) {
      consider(pair[1] || '');
      consider(pair[2] || '');
    }
    for (const match of goal.matchAll(/\{([A-Za-z][\w.#-]*)\}/g)) {
      consider(match[1] || '');
    }
    return {identity, password};
  }

  private prependGoalToken(names: string[], goalName: string|null): string[] {
    if (!goalName) {
      return names;
    }
    if (names.some((n) => n.toLowerCase() === goalName.toLowerCase())) {
      return names;
    }
    return [goalName, ...names];
  }

  private listIdentityVariableTokens(): string[] {
    const vars = this.listVariableTokenNames();
    const matched = vars.filter(
        (n) => /email|e_?mail|user|login|gmail|account|id/i.test(n));
    return this.prependGoalToken(
        matched.length ? matched : vars,
        this.goalNamedCredentialTokens().identity);
  }

  /** Secret tokens acceptable for password fields. */
  private listPasswordSecretTokens(): string[] {
    const secrets = this.listSecretTokenNames();
    const matched = secrets.filter(
        (n) => /pass|secret|pin|credential/i.test(n));
    return this.prependGoalToken(
        matched.length ? matched : secrets,
        this.goalNamedCredentialTokens().password);
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
      if (this.mustSolveCaptchaBeforeSubmit(context)) {
        return false;
      }
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

  private externalCredentialPlaceholderKeys = new Set<string>();
  private loginScaffoldKey = '';
  private loginScaffoldRepeats = 0;
  private loginCaptchaOwned = false;
  private loginSubmitArmed = false;
  /** One autofill look per document. Not recomputed on later turns. */
  private loginAutofillPageKey = '';
  private loginAutofillChecked = false;
  private loginAutofillIdentityFilled = false;
  private loginAutofillPasswordFilled = false;

  private clearLoginAutofillLatch(): void {
    this.loginAutofillPageKey = '';
    this.loginAutofillChecked = false;
    this.loginAutofillIdentityFilled = false;
    this.loginAutofillPasswordFilled = false;
  }

  private isPasswordElement(el: ContextNode|null|undefined): boolean {
    if (!el) {
      return false;
    }
    const t = String(el.formType || '').toLowerCase();
    const hay = `${el.label} ${el.fieldName ?? ''} ${el.placeholder ?? ''}`.toLowerCase();
    return t === 'password' || t.includes('input_password') || hay.includes('password');
  }

  private findPasswordElement(context: PageContextSummary): ContextNode|null {
    const matches = (context.elements ?? []).filter((el) => this.isPasswordElement(el));
    if (!matches.length) {
      return null;
    }
    matches.sort((a, b) => {
      const filled = (el: ContextNode) =>
          (el.autofilled ? 2 : 0) + (el.nonempty ? 1 : 0);
      return (filled(b) - filled(a)) ||
          (scoreContextNode(b) - scoreContextNode(a)) || a.id - b.id;
    });
    return matches[0] ?? null;
  }

  private findIdentityElement(context: PageContextSummary): ContextNode|null {
    const elements = context.elements ?? [];
    const password = this.findPasswordElement(context);
    const ranked = elements.filter((el) => {
      if (!el || this.isPasswordElement(el)) {
        return false;
      }
      const kind = describeNodeKind(el) ?? '';
      const t = String(el.formType || '').toLowerCase();
      const editable = !!el.editable || kind.startsWith('input') ||
          t === 'email' || t === 'text' || t === 'tel';
      if (!editable) {
        return false;
      }
      const hay = `${el.label} ${el.fieldName ?? ''} ${el.placeholder ?? ''} ${t}`.toLowerCase();
      if (/email|e-mail|username|user[\s_-]?name|user\s*id|login/.test(hay)) {
        return true;
      }
      return !!password && (t === 'text' || t === 'email' || t === '' || kind.startsWith('input'));
    });
    if (!ranked.length) {
      return null;
    }
    ranked.sort((a, b) => {
      const filled = (el: ContextNode) =>
          (el.autofilled ? 2 : 0) +
          ((el.nonempty || String(el.fieldValue || '').trim()) ? 1 : 0);
      return (filled(b) - filled(a)) ||
          (scoreContextNode(b) - scoreContextNode(a)) || a.id - b.id;
    });
    return ranked[0] ?? null;
  }

  private sessionVerdict(summary: PageContextSummary): 'logged_in'|'logged_out'|'' {
    const url = String(summary.url || '').toLowerCase();
    const urlLogin = /login|log-in|signin|sign-in|logon|authenticate|session[-_]expired|logged[-_]out/.test(url);
    let password = false;
    let identity = false;
    let loginButton = false;
    let session = false;
    for (const el of summary.elements || []) {
      if (!el || el.disabled || el.visible === false) {
        continue;
      }
      const bounds = el.bounds;
      if (bounds && (Number(bounds.width) < 1 || Number(bounds.height) < 1)) {
        continue;
      }
      const hay = `${el.label || ''} ${el.fieldName || ''} ${el.placeholder || ''} ${
          el.formType || ''} ${el.role || ''}`.toLowerCase();
      const clickable = !!(el.clickable || el.focusable);
      const editable = !!el.editable || /input|textarea/.test(String(el.formType || '')) ||
          /\binput\b|textarea/.test(hay);
      if (!password && (editable || /password/.test(String(el.formType || ''))) &&
          /password|passcode|passwd/.test(hay)) {
        password = true;
      }
      if (!identity && editable &&
          /username|user name|userid|user id|\bemail\b|e-mail|account/.test(hay)) {
        identity = true;
      }
      if (!loginButton && clickable &&
          /\b(log ?in|login|sign ?in|signin|sign-in|logon)\b/.test(hay) &&
          !/log ?out|sign ?out/.test(hay)) {
        loginButton = true;
      }
      if (!session && clickable && /log ?out|sign ?out|avatar/.test(hay)) {
        session = true;
      }
    }
    // Same veto as DetectAuthStateSignals: a visible log out / avatar wins
    // over a menu "Sign in", unless a password field is still on the page.
    if (session && !password) {
      return 'logged_in';
    }
    const authRequired = !session && password && (identity || loginButton || urlLogin);
    const score = (urlLogin ? 3 : 0) + (password ? 4 : 0) + (identity ? 2 : 0) +
        (loginButton ? 3 : 0) + (session ? -6 : 0);
    if (authRequired || (!session && score >= 6 && (loginButton || urlLogin))) {
      return 'logged_out';
    }
    if (session) {
      return 'logged_in';
    }
    return '';
  }

  private sessionPromptLine(context: PageContextSummary): string {
    if (context.session === 'logged_in') {
      const passwordStillVisible = (context.elements || []).some((el) => {
        if (!el || el.disabled || el.visible === false) {
          return false;
        }
        const hay = `${el.label || ''} ${el.fieldName || ''} ${el.formType || ''}`.toLowerCase();
        return /password|passcode|passwd/.test(hay);
      });
      // Change-password / account pages have a session control AND a password
      // field. Do not tell the model to ignore that form.
      if (passwordStillVisible) {
        return 'Session: logged in. A log out or account control is on this page.';
      }
      return 'Session: logged in. A log out or account control is on this page. Do not look for a login form or type credentials.';
    }
    if (context.session === 'logged_out') {
      return 'Session: logged out. A login form is on this page.';
    }
    return '';
  }

  private async annotateLoginState(summary: PageContextSummary): Promise<void> {
    const identity = this.findIdentityElement(summary);
    const password = this.findPasswordElement(summary);
    const loginButtonId = this.findLoginButtonId(summary);
    const pageKey = `${summary.documentId || ''}|${summary.url || ''}`;
    if (pageKey !== this.loginAutofillPageKey) {
      this.loginAutofillPageKey = pageKey;
      this.loginAutofillChecked = false;
      this.loginAutofillIdentityFilled = false;
      this.loginAutofillPasswordFilled = false;
    }
    // Once, when both login boxes are on the page. Later turns reuse the result.
    if (!this.loginAutofillChecked && identity && password) {
      this.loginAutofillChecked = true;
      this.loginAutofillIdentityFilled = !!(
          identity.autofilled || String(identity.fieldValue || '').trim());
      this.loginAutofillPasswordFilled = !!(
          password.autofilled || password.nonempty);
      if (this.loginAutofillIdentityFilled && this.loginAutofillPasswordFilled &&
          this.currentTask) {
        this.addSystemMessage(
            'Login form is already filled. Not typing credentials.');
      }
    }
    const identityAlreadyPresent = this.loginAutofillIdentityFilled;
    const passwordAlreadyPresent = this.loginAutofillPasswordFilled;
    const bothFilled = !!identity && !!password &&
        identityAlreadyPresent && passwordAlreadyPresent;
    const goalSaysFilled = this.goalSaysCredentialsAutofilled() &&
        (!!identity || !!password);
    const externalFill = bothFilled || goalSaysFilled;
    const identityOk = !identity || identityAlreadyPresent || externalFill;
    const passwordOk = !password || passwordAlreadyPresent || externalFill;
    const otp = this.findOtpElement(summary);
    const hasCaptcha = this.pageHasCaptchaWidget(summary) ||
        this.captchaStillOpen(summary);
    const session = (summary.elements ?? []).some((el) => {
      const hay = `${el.label} ${el.fieldName ?? ''}`.toLowerCase();
      return (el.clickable || el.focusable) &&
          /log ?out|sign ?out|avatar/.test(hay);
    });
    const onLoginSurface = (!!password || !!identity || !!otp || hasCaptcha) &&
        !(session && !password);
    const needsIdentity = !!identity && !identityOk;
    const needsPassword = !!password && !passwordOk;
    const otpFilled = !!(otp && (otp.nonempty || otp.autofilled));
    const hasOtp = !!otp;
    if (onLoginSurface || hasCaptcha || hasOtp) {
      summary.login = {
        onLoginSurface,
        skipCredentialEntry: externalFill,
        identityAutofilled: !!identity?.autofilled,
        identityMatches: identityAlreadyPresent,
        identityAlreadyPresent,
        passwordAlreadyPresent,
        needsIdentity,
        needsPassword,
        hasCaptcha,
        hasOtp,
        otpFilled,
        readyToSubmit: onLoginSurface && !needsIdentity && !needsPassword &&
            !hasCaptcha && !(hasOtp && !otpFilled) && loginButtonId != null,
      };
    }
    const sessionState = this.sessionVerdict(summary);
    if (sessionState) {
      summary.session = sessionState;
    }
    for (const el of summary.elements || []) {
      delete el.fieldValue;
    }

    if (summary.login?.skipCredentialEntry && this.currentTask) {
      await this.recordExternalCredentialPlaceholders(summary);
    }
  }

  private async recordExternalCredentialPlaceholders(
      context: PageContextSummary): Promise<void> {
    const key = `${context.documentId || ''}|${context.url || ''}`;
    if (this.externalCredentialPlaceholderKeys.has(key)) {
      return;
    }
    this.externalCredentialPlaceholderKeys.add(key);
    const identity = this.findIdentityElement(context);
    const password = this.findPasswordElement(context);
    const steps: Array<{target_id?: number, text: string}> = [
      {target_id: identity?.id, text: '{Username}'},
      {target_id: password?.id, text: '{Password}'},
    ];
    for (const step of steps) {
      const action: any = {
        action: 'type',
        text: step.text,
        __external_credential: true,
      };
      if (typeof step.target_id === 'number') {
        action.target_id = step.target_id;
      }
      await this.emitRecorderScaffoldAction(action, context, ACTION_RESULT.OK);
    }
  }


  private findOtpElement(context: PageContextSummary): ContextNode|null {
    const ranked = (context.elements ?? []).filter((el) => {
      if (!el || this.isPasswordElement(el)) {
        return false;
      }
      const kind = describeNodeKind(el) ?? '';
      const editable = !!el.editable || kind.startsWith('input');
      if (!editable) {
        return false;
      }
      const hay = `${el.label} ${el.fieldName ?? ''} ${el.placeholder ?? ''} ${
          el.formType ?? ''}`.toLowerCase();
      return /one[- ]?time|\botp\b|\btotp\b|verification code|authenticator|two[- ]factor|\b2fa\b|\bmfa\b/.test(hay);
    });
    if (!ranked.length) {
      return null;
    }
    ranked.sort((a, b) => scoreContextNode(b) - scoreContextNode(a) || a.id - b.id);
    return ranked[0] ?? null;
  }

  private nextLoginScaffoldStep(context: PageContextSummary):
      'identity'|'password'|'captcha'|'otp'|'submit'|null {
    const login = context.login;
    if (!login) {
      return null;
    }
    if (login.onLoginSurface && login.hasCaptcha) {
      this.loginCaptchaOwned = true;
    }
    if (login.needsIdentity) {
      return 'identity';
    }
    if (login.needsPassword) {
      return 'password';
    }
    if ((login.onLoginSurface || this.loginCaptchaOwned) &&
        this.mustSolveCaptchaBeforeSubmit(context)) {
      return 'captcha';
    }
    if (login.hasOtp && !login.otpFilled &&
        (login.onLoginSurface || this.loginSubmitArmed)) {
      return 'otp';
    }
    if (login.readyToSubmit) {
      return 'submit';
    }
    if (!login.onLoginSurface && !this.mustSolveCaptchaBeforeSubmit(context)) {
      this.loginCaptchaOwned = false;
    }
    return null;
  }

  /** One login step chosen from the page, with no planner turn. */
  private async advanceLoginScaffold(context: PageContextSummary): Promise<boolean> {
    const step = this.nextLoginScaffoldStep(context);
    if (!step) {
      this.loginScaffoldKey = '';
      this.loginScaffoldRepeats = 0;
      return false;
    }
    const key = `${step}|${context.documentId || ''}|${context.url || ''}`;
    if (key === this.loginScaffoldKey) {
      this.loginScaffoldRepeats += 1;
    } else {
      this.loginScaffoldKey = key;
      this.loginScaffoldRepeats = 0;
    }
    if (this.loginScaffoldRepeats >= 2) {
      this.addSystemMessage(
          `Login scaffold stuck on ${step}; leaving this page to the task planner.`);
      return false;
    }
    const goal = String(this.currentTask?.goal || '');
    if (step === 'identity') {
      const names = this.listIdentityVariableTokens();
      const token = this.preferredTokenForGoal(names, goal) || names[0];
      const target = this.findIdentityElement(context);
      if (!token || target?.id == null) {
        return false;
      }
      this.addSystemMessage('Login scaffold: type username.');
      return this.performAction(
          {action: 'type', target_id: target.id, text: `{${token}}`}, context);
    }
    if (step === 'password') {
      const names = this.listPasswordSecretTokens();
      const token = this.preferredTokenForGoal(names, goal) || names[0];
      const target = this.findPasswordElement(context);
      if (!token || target?.id == null) {
        return false;
      }
      this.addSystemMessage('Login scaffold: type password.');
      return this.performAction(
          {action: 'type', target_id: target.id, text: `{${token}}`}, context);
    }
    if (step === 'captcha') {
      this.loginCaptchaOwned = true;
      this.addSystemMessage('Login scaffold: captcha.');
      await this.runEmbeddedUtilityStep(
          'Click the captcha checkbox or the next captcha image. Do not click Log In or any other control.');
      return true;
    }
    if (step === 'otp') {
      const names = this.listSecretTokenNames().filter(
          (n) => /otp|totp|2fa|mfa|code/i.test(n));
      const target = this.findOtpElement(context);
      if (names[0] && target?.id != null) {
        this.addSystemMessage('Login scaffold: type one-time code.');
        return this.performAction(
            {action: 'type', target_id: target.id, text: `{${names[0]}}`}, context);
      }
      this.addSystemMessage('Login scaffold: one-time code.');
      await this.runEmbeddedUtilityStep(
          'Type the one-time code into the verification field. Do not click Log In.');
      return true;
    }
    const loginButtonId = this.findLoginButtonId(context);
    if (loginButtonId == null) {
      return false;
    }
    this.loginSubmitArmed = true;
    this.addSystemMessage('Login scaffold: click login.');
    return this.performAction(
        {action: 'click', target_id: loginButtonId}, context);
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

  private isSocialLoginControl(el: ContextNode|null|undefined): boolean {
    if (!el) {
      return false;
    }
    const blob = [
      el.cssClass,
      el.label,
      el.fieldName,
      el.placeholder,
      bestContextNodeText(el),
    ].map((part) => String(part || '')).join(' ').toLowerCase();
    return /google-btn|apple-btn|facebook-btn|continue with (google|apple|facebook|microsoft)|sign in with (google|apple|facebook|microsoft)|log in with (google|apple|facebook|microsoft)|\boauth\b/.test(blob);
  }

  private goalWantsSocialProvider(): boolean {
    const goal = String(this.currentTask?.goal || '').toLowerCase();
    return /sign in with (google|apple|facebook|microsoft)|continue with (google|apple|facebook|microsoft)|log in with (google|apple|facebook|microsoft)|apple id|google oauth|facebook login/.test(goal);
  }

  private findLoginButtonId(context: PageContextSummary): number|null {
    const candidates = (context.elements ?? []).filter((el) => {
      const kind = describeNodeKind(el) ?? '';
      if (!kind.includes('button') && !el.clickable) {
        return false;
      }
      if (this.isSocialLoginControl(el)) {
        return false;
      }
      const text = `${el.label} ${el.placeholder ?? ''} ${bestContextNodeText(el)}`.toLowerCase();
      if (/\b(apple|google|facebook|microsoft)\b/.test(text) &&
          /(log in|login|sign in|signin|continue)/.test(text)) {
        return false;
      }
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
    const cellToken = raw.cell ?? (
        typeof raw.coordinate === 'string' ? raw.coordinate :
        (typeof raw.coordinates === 'string' ? raw.coordinates : ''));
    if (typeof cellToken === 'string' && cellToken.trim()) {
      this.addSystemMessage(
          `Coordinates must be {"coordinate":{"x":120,"y":80}} image pixels, not "${cellToken.trim()}".`);
      return null;
    }
    const fromPair = (value: any): {x: number, y: number}|null => {
      if (Array.isArray(value) && value.length >= 2) {
        const x = Number(value[0]);
        const y = Number(value[1]);
        if (Number.isFinite(x) && Number.isFinite(y)) {
          // Keep fractions so normalized [0,1] coords survive.
          return {x, y};
        }
        return null;
      }
      if (value && typeof value === 'object') {
        const x = Number(value.x ?? value.left);
        const y = Number(value.y ?? value.top);
        if (Number.isFinite(x) && Number.isFinite(y)) {
          return {x, y};
        }
      }
      return null;
    };
    const fromNamed = fromPair(raw.coordinate ?? raw.coordinates);
    const fromXY = (!fromNamed && raw.x != null && raw.y != null) ? (() => {
      const x = Number(raw.x);
      const y = Number(raw.y);
      if (Number.isFinite(x) && Number.isFinite(y)) {
        return {x, y};
      }
      return null;
    })() : null;
    const pixel = fromNamed || fromXY;
    if (pixel) {
      const space = this.lastScreenshotClickSpace;
      let x = Number(pixel.x);
      let y = Number(pixel.y);
      // Local VLMs often emit normalized [0,1] into the image-pixel path.
      const looksNormalized =
          x >= 0 && y >= 0 && x <= 1 && y <= 1 && (x > 0 || y > 0) &&
          (x <= 1 && y <= 1) &&
          !(Number.isInteger(x) && Number.isInteger(y) &&
            (x === 0 || x === 1) && (y === 0 || y === 1) &&
            space && space.imageW <= 2);
      if (looksNormalized && space && space.clickW > 0 && space.clickH > 0) {
        const rawX = x;
        const rawY = y;
        x = Math.round(rawX * (space.clickW - 1));
        y = Math.round(rawY * (space.clickH - 1));
        this.addSystemMessage(
            `Click normalized (${rawX},${rawY}) → viewport (${x},${y}) of ` +
            `${space.clickW}x${space.clickH}.`);
        return {x, y};
      }
      x = Math.round(x);
      y = Math.round(y);
      if (space && space.clickW > 0 && space.clickH > 0 &&
          space.imageW > 0 && space.imageH > 0) {
        const rawX = x;
        const rawY = y;
        // If css metadata is missing, clickW==imageW; on retina that maps
        // physical px as DIPs and clicks land ~DSF× too far.
        if (space.clickW === space.imageW && space.clickH === space.imageH &&
            space.imageW > 1400) {
          this.addSystemMessage(
              'Screenshot click space may be missing cssWidth (retina risk). Prefer dashboardAgentScreenshot captures.');
        }
        x = Math.round(rawX * space.clickW / space.imageW);
        y = Math.round(rawY * space.clickH / space.imageH);
        x = Math.max(0, Math.min(space.clickW - 1, x));
        y = Math.max(0, Math.min(space.clickH - 1, y));
        this.addSystemMessage(
            `Click image (${rawX},${rawY}) on ${space.imageW}x${space.imageH} ` +
            `→ viewport (${x},${y}) of ${space.clickW}x${space.clickH}.`);
      } else {
        x = Math.max(0, x);
        y = Math.max(0, y);
        this.addSystemMessage(`Click (${x},${y}).`);
      }
      return {x, y};
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
        // Screenshot pixels are the click. Do not attach wrapper
        // locators — #captcha_container / `.class button` retargets to Claim.
        const hasScreenshotCell = !!(action?.coordinate &&
            typeof action.coordinate.x === 'number' &&
            typeof action.coordinate.y === 'number');
        const locators = hasScreenshotCell ?
            [] :
            this.getTargetLocators(context, action.target_id);
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
        if (!hasScreenshotCell) {
          const point = getActionablePoint(target);
          if (point) {
            timelineAction.params.coordinates = {
              x: Math.round(point.x),
              y: Math.round(point.y),
            };
          }
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
    if (action?.coordinate && typeof action.coordinate === 'object' &&
        typeof action.coordinate.x === 'number' &&
        typeof action.coordinate.y === 'number') {
      delete timelineAction.params.locators;
      timelineAction.params.coordinates = {
        x: action.coordinate.x,
        y: action.coordinate.y,
      };
      timelineAction.params.humanOffset = false;
    }
    if (kind === 'scroll' || kind === 'scroll_to') {
      const scope = String(action?.scope || '').toLowerCase();
      const pageScroll = scope === 'page' || scope === 'window' ||
          (kind === 'scroll' && !action?.target_id && scope !== 'iframe');
      if (pageScroll) {
        timelineAction.params.element = 'window';
        delete timelineAction.params.locators;
        delete timelineAction.params.coordinates;
      } else {
        timelineAction.params.element = 'element';
      }
    }
    if (action?.action === 'scroll_to' && typeof action?.target_id === 'number') {
      timelineAction.action = 'scroll';
      timelineAction.params = {
        ...timelineAction.params,
        percentY: 50,
        percentX: 50,
        element: timelineAction.params.element || 'element',
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
      if (action.__external_credential) {
        brief.text = action.text.trim().slice(0, 80);
      } else {
        const hay = `${brief.desc || ''} ${action.target_id || ''}`.toLowerCase();
        const targetNum = Number(action.target_id);
        const looksSecret = /password|passcode|secret|token/.test(hay) ||
            (Number.isFinite(targetNum) &&
             this.isPasswordTarget(context, targetNum));
        brief.text = looksSecret ? '[redacted]' : action.text.slice(0, 80);
      }
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
        src.__login_autofill_bypass || src.__glic_captcha) {
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

    if (sourceAction?.__glic_captcha) {
      return;
    }
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
        'minimal ordered steps that still achieve the USER GOAL. ' +
        'Omit captcha/visual-challenge clicks; those are one-shot and replaced by a glic_prompt on this tab.\n' +
        `USER GOAL: ${task.goal}\n` +
        `Steps:\n${JSON.stringify(steps, null, 2)}\n` +
        'Respond with ONE JSON object only:\n' +
        '{"action":"review_script","keep_steps":[0,2,5]}\n' +
        'keep_steps = 0-based indices into Steps to keep, in final script order. ' +
        'Omit noise. If all steps are needed, return every index. ' +
        'Do not return {"action":"complete"}.';

    for (let i = 0; i < 40 && this.llmRequestInFlight; i++) {
      await sleep(50);
    }
    if (this.llmRequestInFlight) {
      this.addSystemMessage(
          'Script review waited for the in-flight model call and is running anyway.');
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
              'Return JSON only: {"action":"review_script","keep_steps":[...]}. ' +
              'Do not use action complete.',
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
        const verb = String(obj.action || obj.do || '').toLowerCase();
        if (verb === 'done' || verb === 'keep_draft') {
          return null;
        }
        const keep = obj.keep_steps ?? obj.keepSteps ?? obj.indexes ??
            obj.indices ?? obj.keep ?? obj.steps;
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

  private async loadAgentScriptSaveSettings():
      Promise<{autoSave: boolean, review: boolean}> {
    const defaults = {autoSave: true, review: true};
    if (!this.canUseChromeSend()) {
      return defaults;
    }
    try {
      const res = await this.sendWithPromise('dashboardGetEffectiveSettings');
      const agent = res?.settings?.agent || res?.agent || {};
      return {
        autoSave: agent.autoSaveScriptOnComplete !== false,
        review: agent.reviewScriptAfterComplete !== false,
      };
    } catch (_) {
      return defaults;
    }
  }

  private async saveAgentScriptToDashboard(payload: {
    script_id?: string,
    name: string,
    goal?: string,
    startUrl?: string,
    source?: string,
    actions: any[],
  }): Promise<{ok: boolean, id?: string, name?: string, error?: string,
               action_count?: number}> {
    if (this.canUseChromeSend()) {
      const res = await this.sendWithPromise('dashboardAgentSaveScript', {
        script_id: payload.script_id,
        name: payload.name,
        goal: payload.goal || '',
        startUrl: payload.startUrl || '',
        source: payload.source || 'glic_panel_autosave',
        actions: payload.actions,
      });
      if (res && res.ok !== false) {
        return {
          ok: true,
          id: String(res.id || payload.script_id || ''),
          name: String(res.name || payload.name),
          action_count: Number(res.action_count || payload.actions.length),
        };
      }
      return {
        ok: false,
        error: String(res?.error || 'dashboardAgentSaveScript failed'),
      };
    }
    try {
      const res = await fetch('/task/save_script', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          task_id: payload.script_id || `manual-${Date.now()}`,
          goal: payload.goal || '',
          name: payload.name,
          status: 'success',
          actions: payload.actions,
        }),
      });
      if (!res.ok) {
        return {ok: false, error: `HTTP ${res.status} ${await res.text()}`};
      }
      const data = await res.json();
      return {
        ok: true,
        id: String(data?.id || payload.script_id || ''),
        name: payload.name,
        action_count: payload.actions.length,
      };
    } catch (e) {
      return {ok: false, error: String(e)};
    }
  }

  private hostnameFromUrl(url: string|null|undefined): string {
    const raw = String(url || '').trim();
    if (!raw) {
      return '';
    }
    try {
      return new URL(raw).hostname.replace(/^www\./, '');
    } catch (_) {
      return raw.replace(/^https?:\/\//, '').split('/')[0] || '';
    }
  }

  private async saveCurrentSiteScriptAndContinue(action: any): Promise<void> {
    if (!this.currentTask) {
      this.addSystemMessage('No active task — cannot save a site script.');
      return;
    }
    this.scriptSegmentIndex += 1;
    const host = this.hostnameFromUrl(this.workingTabUrl || this.currentTask.url);
    const nameHint = String(action?.name || '').trim() ||
        (`Agent: ${host || 'site'} ${this.scriptSegmentIndex}`).slice(0, 80);
    const saved = await this.saveRecorderSegment({
      task: this.currentTask,
      sinceMs: this.scriptSegmentStartedAtMs,
      nameHint,
      source: 'glic_panel_site_segment',
      clip: true,
      requireAutoSave: false,
      review: true,
      reason: String(action?.reason || '').trim(),
    });
    this.scriptSegmentStartedAtMs = Date.now();
    this.clearCaptchaChallengeState();
    this.pendingScreenshot = null;
    this.screenshotSpamStrikes = 0;
    this.requestContextNextTurn = true;
    this.addSystemMessage(
        saved ?
            `Saved and clipped site script "${nameHint}". ` +
                'The overall task is still running — go to the next website.' :
            `No recorded steps to save for this site (${nameHint}). ` +
                'Continuing the same task.');
  }

  private async saveRecorderSegment(options: {
    task?: TaskItem|null,
    sinceMs?: number,
    nameHint?: string,
    source?: string,
    clip: boolean,
    requireAutoSave: boolean,
    review: boolean,
    reason?: string,
    quietIfEmpty?: boolean,
  }): Promise<boolean> {
    const completed = options.task || this.currentTask || null;
    const buffer = Array.isArray(window.__glicRecorderScaffoldBuffer) ?
        window.__glicRecorderScaffoldBuffer :
        [];
    const taskId = completed?.id || '';
    const sinceMs = Number(options.sinceMs || 0);
    const relevant = buffer.filter((ev) => {
      if (!ev || typeof ev !== 'object') {
        return false;
      }
      if (taskId && ev.taskId && ev.taskId !== taskId) {
        return false;
      }
      if (sinceMs && Number(ev.timestampMs || 0) < sinceMs) {
        return false;
      }
      return true;
    });
    const saveOk = await this.persistRecorderEventsAsScript(
        relevant, completed, options);
    if (options.clip) {
      try {
        if (Array.isArray(window.__glicRecorderScaffoldBuffer)) {
          const drop = new Set(relevant);
          window.__glicRecorderScaffoldBuffer =
              window.__glicRecorderScaffoldBuffer.filter((ev) => !drop.has(ev));
        }
      } catch (_) {
      }
    }
    return saveOk;
  }

  private async persistRecorderEventsAsScript(
      relevant: GlicRecorderScaffoldEvent[],
      completed: TaskItem|null,
      options: {
        nameHint?: string,
        source?: string,
        requireAutoSave: boolean,
        review: boolean,
        reason?: string,
        quietIfEmpty?: boolean,
      }): Promise<boolean> {
    const captchaEvents = relevant.filter((ev) => this.isCaptchaScaffoldEvent(ev));
    const candidates = relevant.filter((ev) => this.isScaffoldReviewCandidate(ev));
    const captchaTs = captchaEvents[0]?.timestampMs;
    const shouldInsertCaptchaPause =
        this.captchaSeenThisTask || captchaEvents.length > 0;
    const settings = await this.loadAgentScriptSaveSettings();
    if (options.requireAutoSave && !settings.autoSave) {
      this.addSystemMessage(
          'autoSaveScriptOnComplete is off; not saving a dashboard script.');
      return false;
    }

    const toTimeline = (events: GlicRecorderScaffoldEvent[]) => events
        .map((ev) => ({
          ...ev.timelineAction,
          params: {
            ...(ev.timelineAction?.params || {}),
            __glic_brief: ev.brief || null,
          },
        }))
        .filter((a) => a && typeof a === 'object' &&
                    String(a.action || '') !== 'complete' &&
                    String(a.action || '') !== 'save_script');
    let draftActions = toTimeline(candidates);
    if (shouldInsertCaptchaPause) {
      draftActions = this.withHumanCaptchaPause(
          draftActions, this.captchaInsertIndex(candidates, captchaTs));
    }
    if (!draftActions.length) {
      this.addSystemMessage(
          options.quietIfEmpty ?
              'No new recorded steps at completion. Earlier site saves already covered this task, so there is nothing further to review.' :
              'Script review did not run: no successful actions were recorded.');
      return false;
    }

    const host = this.hostnameFromUrl(
        this.workingTabUrl || relevant[0]?.url || completed?.url || '');
    const draftName = (options.nameHint ||
        (`Agent: ${host || completed?.goal || 'Agent task'}`)).slice(0, 80);
    const startUrl = relevant[0]?.url || completed?.url || this.workingTabUrl || '';
    const draft = await this.saveAgentScriptToDashboard({
      name: draftName,
      goal: completed?.goal || '',
      startUrl,
      source: options.source || 'glic_panel_autosave',
      actions: draftActions,
    });
    if (!draft.ok) {
      this.addSystemMessage(
          `Script save failed.${draft.error ? ' ' + draft.error : ''}`);
      return false;
    }
    this.addSystemMessage(
        `Saved draft script: ${draft.name || draftName} (${draftActions.length} steps).`);

    if (options.review && !settings.review) {
      this.addSystemMessage(
          'reviewScriptAfterComplete is off; saved the draft without a prune pass.');
    }
    if (options.review && settings.review && !completed) {
      this.addSystemMessage(
          'Script review skipped: the completed task was missing.');
    }
    if (options.review && settings.review && completed && candidates.length > 0) {
      this.addSystemMessage(
          `Reviewing ${candidates.length} recorded step(s) for script prune/edit…`);
      const keep = await this.reviewScaffoldStepsWithModel(completed, candidates);
      let selected = candidates;
      if (keep && keep.length) {
        selected = keep.map((i) => candidates[i]!).filter(Boolean);
        this.addSystemMessage(
            `Pruned script timeline: kept ${selected.length}/${candidates.length} steps.`);
      } else if (keep && keep.length === 0) {
        this.addSystemMessage(
            'Model kept 0 steps; falling back to full candidate timeline.');
        selected = candidates;
      } else {
        this.addSystemMessage('Review kept the draft as-is.');
      }
      let reviewedActions = toTimeline(selected);
      if (shouldInsertCaptchaPause) {
        reviewedActions = this.withHumanCaptchaPause(
            reviewedActions, this.captchaInsertIndex(selected, captchaTs));
      }
      const sameOrder = !!keep && keep.length === candidates.length &&
          keep.every((n, i) => n === i);
      if (keep && keep.length && reviewedActions.length && !sameOrder) {
        const pruned = await this.saveAgentScriptToDashboard({
          script_id: draft.id,
          name: draft.name || draftName,
          goal: completed?.goal || '',
          startUrl,
          source: 'glic_panel_review',
          actions: reviewedActions,
        });
        if (pruned.ok) {
          this.addSystemMessage(
              `Reviewed script: ${pruned.name || draft.name} (${
                  reviewedActions.length} steps).`);
        } else {
          this.addSystemMessage(
              `Review rewrite failed; kept draft.${
                  pruned.error ? ' ' + pruned.error : ''}`);
        }
      }
    }

    return true;
  }

  private async flushRecorderScaffoldForTask(
      status: string, task?: TaskItem|null): Promise<void> {
    if (status !== 'success') {
      return;
    }
    await this.saveRecorderSegment({
      task: task || null,
      sinceMs: 0,
      nameHint: '',
      source: 'glic_panel_autosave',
      clip: true,
      requireAutoSave: true,
      review: true,
      // save_script already clipped every step into per-site scripts.
      quietIfEmpty: this.scriptSegmentIndex > 0,
    });
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

    if (action?.coordinate &&
        typeof action.coordinate.x === 'number' &&
        typeof action.coordinate.y === 'number') {
      delete timelineAction.params.locators;
      timelineAction.params.coordinates = {
        x: action.coordinate.x,
        y: action.coordinate.y,
      };
      timelineAction.params.humanOffset = false;
    } else if (typeof action?.target_id === 'number' &&
               !(Array.isArray(timelineAction.params.locators) &&
                 timelineAction.params.locators.length)) {
      const coordinateFallback = this.buildCoordinateFallbackAction(action, context);
      if (coordinateFallback?.coordinate) {
        timelineAction.params.coordinates = coordinateFallback.coordinate;
        timelineAction.params.humanOffset = false;
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
    if (!this.currentTask) {
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
      if (action?.coordinate &&
          typeof action.coordinate.x === 'number' &&
          typeof action.coordinate.y === 'number') {
        delete timelineAction.params.locators;
        timelineAction.params.coordinates = {
          x: action.coordinate.x,
          y: action.coordinate.y,
        };
        timelineAction.params.humanOffset = false;
      } else if (typeof action?.target_id === 'number' &&
                 !(Array.isArray(timelineAction.params?.locators) &&
                   timelineAction.params.locators.length)) {
        const coordinateFallback = this.buildCoordinateFallbackAction(
            action, context);
        if (coordinateFallback?.coordinate) {
          timelineAction.params.coordinates = coordinateFallback.coordinate;
          timelineAction.params.humanOffset = false;
        timelineAction.params.humanOffset = false;
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
    if (this.canUseChromeSend()) {
      let tabId = this.currentTask ? this.getCurrentTabIdInt() : context.tabIdInt;
      if (!Number.isFinite(tabId)) {
        const parsed = Number.parseInt(
            String(
                (this.currentTask ? this.getAuthoritativeTaskTabId() : null) ??
                context.tabId ?? this.currentTabId ?? ''), 10);
        tabId = Number.isFinite(parsed) ? parsed : undefined;
      }
      if (this.currentTask && Number.isFinite(tabId)) {
        this.bindTaskTab(
            String(tabId),
            context.url ?? this.workingTabUrl ?? null,
            context.title ?? this.workingTabTitle ?? this.currentTask?.goal ?? null);
      }
      void this.logTaskAction('requested', {
        engine: 'dashboard_script_executor',
        tab_id: tabId,
        action: actionForLogging,
        timeline_action: timelineAction,
      });
      try {
        const result = await this.runDashboardExecutorAction(
            String(timelineAction?.action || action?.action || ''),
            timelineAction?.params || {},
            {waitForCompletion: true, tabId: Number.isFinite(tabId) ? tabId : undefined});
        const ok = !!result && result.ok !== false && result.success !== false &&
            !result.error;
        await this.logTaskAction(ok ? 'result' : 'error', {
          engine: 'dashboard_script_executor',
          tab_id: tabId,
          action: actionForLogging,
          timeline_action: timelineAction,
          response: result || null,
        });
        if (!ok) {
          this.addSystemMessage(
              `Action execution failed: ${result?.error || result?.message || 'script executor'}`);
          return null;
        }
        if (action?.action === 'navigate' && typeof action?.url === 'string' &&
            action.url.trim()) {
          this.setWorkingContext(
              this.currentTabId,
              action.url.trim(),
              this.currentTask?.goal ?? this.workingTabTitle);
        }
        this.lastActionSummary =
            `${action.action} target_id=${action.target_id ?? ''} -> executor_completed`;
        await this.emitRecorderScaffoldAction(action, context, ACTION_RESULT.OK);
        return {resultCode: ACTION_RESULT.OK};
      } catch (e) {
        this.addSystemMessage(`Action execution failed: ${e}`);
        return null;
      }
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
    if (action?.action === 'click' && typeof action.target_id === 'number' &&
        !action.__login_fallback && !this.goalWantsSocialProvider() &&
        !this.oauthTabId && !this.isOauthLoginUrl(context.url)) {
      const clicked = this.getElementById(context, action.target_id);
      if (this.isSocialLoginControl(clicked)) {
        this.addSystemMessage(
            'Ignoring social login. The goal did not ask for it.');
        return true;
      }
    }
    // A filled box is left alone. Typing would append. This does not start a
    // new credential step and does not recheck autofill.
    const loginState = context.login;
    const targetKind = (action?.action === 'type' &&
                        typeof action.target_id === 'number') ?
        this.classifyCredentialField(context, action.target_id) :
        'other';
    if (action?.action === 'type' && typeof action.target_id === 'number' &&
        !action.__external_credential &&
        this.looksLikeLoginPage(context) &&
        this.isCredentialFieldTarget(context, action.target_id)) {
      const bothFilled = !!loginState?.skipCredentialEntry ||
          this.goalSaysCredentialsAutofilled();
      const thisFieldFilled = bothFilled ||
          (targetKind === 'identity' && !!loginState?.identityAlreadyPresent) ||
          (targetKind === 'password' && !!loginState?.passwordAlreadyPresent);
      if (thisFieldFilled) {
        return true;
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
    if (this.looksLikeOauthProviderClick(action, context)) {
      this.oauthPopupExpectedUntil = Date.now() + 8000;
      await sleep(800);
      await this.syncOauthFlow({expectPopup: true});
    }
    if (action?.action === 'type' && typeof action.target_id === 'number') {
      if (this.isUsernameTarget(context, action.target_id)) {
      }
      if (this.isPasswordTarget(context, action.target_id) &&
          this.mustSolveCaptchaBeforeSubmit(context)) {
        this.addSystemMessage(
            'Credentials entered. Solve the captcha before Log In: ' +
            '{"action":"request_screenshot"} then {"action":"click","coordinate":{"x":120,"y":80}} on the checkbox.');
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
        if (this.mustSolveCaptchaBeforeSubmit(post)) {
          this.addSystemMessage(
              'Password typed. Captcha is still unchecked — do not click Log In. ' +
              (this.hasLiveScreenshot() ?
                  'Next: click the checkbox {"coordinate":{"x":120,"y":80}} on the attached 1:1 screenshot.' :
                  'Next: {"action":"request_screenshot"} then click the checkbox {"coordinate":{"x":120,"y":80}}.'));
        } else {
          const loginButtonId = this.findLoginButtonId(post);
          if (loginButtonId) {
            this.addSystemMessage('Enter submit may not have logged in; trying a Log In/Sign In button click.');
            await this.performActionOnce({action: 'click', target_id: loginButtonId, __login_fallback: true}, post);
          }
        }
      }
    }
    return actionSucceeded;
  }


  private async maybeForceStagnationFallback(
      action: any, context: PageContextSummary): Promise<boolean> {
    const open = this.executionTimeline.circuitOpen(action, context);
    if (!open) return false;
    this.addSystemMessage(open.reason);
    const fallback = open.fallback;
    if (fallback.action === 'wait') {
      await sleep(fallback.waitMs || 1200);
      return true;
    }
    if (fallback.action === 'reload') {
      const tabId = this.getAuthoritativeTaskTabId();
      const reloaded = tabId ? await this.reloadBrowserTab(tabId) : false;
      if (!reloaded && /^https?:/i.test(String(context.url || ''))) {
        await this.navigateCurrentTab(String(context.url));
      }
      return true;
    }
    this.executionTimeline.begin(fallback, context);
    const ok = await this.performAction(fallback, context);
    if (!ok) this.executionTimeline.fail(context);
    return true;
  }

  private preflightActionSchema(text: string): string {
    const cleaned = String(text || '').trim();
    if (!cleaned || cleaned.startsWith('Error:') || cleaned.startsWith('[Canceled]')) {
      return '';
    }
    const objects = extractJsonObjects(text);
    if (!objects.length) return 'the reply was not valid JSON.';
    const actions = this.extractActions(text);
    if (!actions.length) return 'the JSON object had no action field.';
    for (const action of actions) {
      const coord = describeCoordinateStructureError(action);
      if (coord) return coord;
    }
    return '';
  }

  private async repromptForStructuredAction(
      assistantText: string,
      reason: string,
      model: string,
      headers: Record<string, string>,
  ): Promise<string> {
    const correction =
        'The previous reply was rejected: ' + reason +
        ' Reply with exactly one JSON object and nothing else. ' +
        'coordinate, when present, is {"x": <integer 0-4000>, "y": <integer 0-4000>}. ' +
        'Do not wrap it in markdown.';
    const retryMessages = [
      ...this.messages.map((m) => ({role: m.role, content: m.content})),
      {role: 'user' as const, content: correction},
    ];
    const repaired = await this.fetchAssistantText(model, headers, retryMessages);
    if (!repaired || repaired === '[Canceled]' || repaired.startsWith('Error:') ||
        repaired.startsWith('[Canceled]')) {
      return assistantText;
    }
    this.addSystemMessage('Re-prompted for a schema-valid action JSON object.');
    return repaired;
  }

  private async handleAssistantActions(responseText: string,
                                       context: PageContextSummary): Promise<ActionHandlingResult> {
    const actions = this.extractActions(responseText);
    if (actions.length === 0) {
      return {handled: false, requestContext: false};
    }
    let handledScreenshot = false;
    for (const action of actions) {
      const rawName = String(action?.action || action?.type || '').trim().toLowerCase();
      if (rawName === 'outline' || rawName === 'task_plan' ||
          ((rawName === 'plan' || rawName === 'batch') &&
           Array.isArray(action.stages) && !Array.isArray(action.steps) &&
           !Array.isArray(action.actions) && !Array.isArray(action.batch))) {
        this.executionTimeline.addNote(
            'Do not send an outline. Recent History is tracked for you. Reply with the single next action.');
        return {handled: true, requestContext: false};
      }
      // Manager → embedded VLM worker (eyes/hands). Keep ahead of target_id actuation.
      if (this.isManagerProfile()) {
        if (rawName === 'batch' || rawName === 'plan') {
          const rawSteps: any[] = Array.isArray(action.steps) ? action.steps :
              (Array.isArray(action.batch) ? action.batch :
               (Array.isArray(action.actions) ? action.actions : []));
          const steps = rawSteps
              .map((step) => this.coercePlannedStep(step))
              .filter((step) => !!step);
          if (!steps.length) {
            this.noteManagerIdle();
            this.addSystemMessage(
                'Manager batch requires a non-empty "steps" array.');
            return {handled: true, requestContext: false};
          }
          this.rememberPlanStage(action);
          const workKey = steps.map((step) => this.describePlannedStep(step)).join(' ; ');
          this.noteManagerWork(workKey);
          const batchPayload = {action: 'batch', steps};
          if (await this.maybeForceStagnationFallback(batchPayload, context)) {
            return {handled: true, requestContext: true};
          }
          const batchRepeat = this.executionTimeline.rejects(batchPayload, context);
          if (batchRepeat) {
            this.addSystemMessage(batchRepeat);
            return {handled: true, requestContext: false};
          }
          this.executionTimeline.begin(batchPayload, context);
          const ok = await this.runEmbeddedBatch(
              steps, action.stop_on_failure !== false);
          if (!ok) {
                    const revert = this.executionTimeline.fail(context);
            if (revert) {
              await this.navigateCurrentTab(revert);
            }
          }
          return {handled: true, requestContext: true};
        }
        const hasTargetId = Number(action.target_id ?? action.targetId);
        const described = (!Number.isFinite(hasTargetId) || hasTargetId <= 0) &&
            !this.parseCoordinatePoint(action) ?
            this.coercePlannedStep(action) : null;
        if (described && (described.target || described.from || described.args)) {
          const one = {action: 'batch', steps: [described]};
          if (await this.maybeForceStagnationFallback(one, context)) {
            return {handled: true, requestContext: true};
          }
          const repeat = this.executionTimeline.rejects(one, context);
          if (repeat) {
            this.addSystemMessage(repeat);
            return {handled: true, requestContext: false};
          }
          this.executionTimeline.begin(one, context);
          const ok = await this.runEmbeddedBatch([described], true);
          if (!ok) {
            const revert = this.executionTimeline.fail(context);
            if (revert) {
              await this.navigateCurrentTab(revert);
            }
          }
          return {handled: true, requestContext: true};
        }
        if (rawName === 'direct' || rawName === 'observe' ||
            rawName === 'embedded_step' || rawName === 'vlm_step') {
          const instruction = String(
              action.instruction || action.text || action.prompt ||
              action.payload || '').trim();
          if (!instruction) {
            this.addSystemMessage(
                'Manager direct/observe requires a non-empty instruction string.');
            return {handled: true, requestContext: false};
          }
          await this.runEmbeddedUtilityStep(instruction);
          return {handled: true, requestContext: true};
        }
      }
      if (this.actionRequestsIgnoreGuidance(action)) {
        this.clearActiveLoopGuidance('model flagged ignore_guidance');
      }
      const normalized = this.normalizeAction(action);
      if (!normalized) {
        continue;
      }
      if (normalized.action === 'store_memento') {
        const ok = this.mementos.write(String(normalized.text || ''), {
          taskId: this.currentTask?.id,
          global: normalized.global === true,
          ttl: normalized.ttl,
        });
        this.addSystemMessage(ok ? 'Stored memento.' : 'Memento was empty.');
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'read_memento') {
        const text = this.mementos.readText(this.currentTask?.id);
        this.addSystemMessage(text ? `Mementos:\n${text}` : 'No mementos stored.');
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'ignore_guidance') {
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'human_solve_captcha') {
        this.captchaSeenThisTask = true;
        this.pauseForHumanCaptcha('Model requested a human captcha solve');
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'save_script') {
        await this.saveCurrentSiteScriptAndContinue(normalized);
        return {handled: true, requestContext: true};
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
        let probe = context;
        try {
          probe = await this.collectContextWithRetry();
        } catch (_) {
        }
        if (this.captchaStillOpen(probe)) {
          if (this.screenshotOnlyStreak === 0 && !this.pendingScreenshot) {
            await this.captureScreenshotForNextTurn('captcha still present');
          }
          const humanHint = this.captchaClicksThisChallenge >= 3 ?
              ' If several coordinate attempts have already failed, {"action":"human_solve_captcha"} is an optional last resort.' :
              '';
          this.addSystemMessage(
              'Captcha still open (checkbox and/or follow-up steps). Use the 1:1 screenshot. ' +
              'Click {"coordinate":{"x":120,"y":80}} on the next target; do not request_context or Log In yet.' + humanHint);
          return {handled: true, requestContext: false};
        }
        if (this.captchaChallengeActive && !this.captchaStillOpen(probe)) {
          this.markCaptchaCleared('request_context saw no remaining challenge');
        }
        if (!this.currentTask) {
          const refreshedCount = probe.elements?.length ?? 0;
          const refreshedUrl = String(probe.url || '').trim();
          this.addSystemMessage(
              `Refreshed context snapshot (${refreshedCount} candidate elements${refreshedUrl ? `, url=${refreshedUrl}` : ''}).`);
        }
        if (this.usesOutcomePlan() && this.managerPageUnchanged &&
            !this.captchaChallengeActive) {
          this.noteManagerIdle();
        }
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
        if (this.hasLiveScreenshot() || this.screenshotOnlyStreak >= 1 ||
            this.capturedScreenshotThisTurn) {
          this.screenshotSpamStrikes += 1;
          handledScreenshot = true;
          this.addSystemMessage(
              'Ignored request_screenshot — the same 1:1 image is still attached. ' +
              'Scroll, type, wait, use target_id, or click {"coordinate":{"x":120,"y":80}} on that picture. ' +
              'request_screenshot is disabled until the viewport changes.');
          continue;
        }
        this.captchaChallengeActive = true;
        handledScreenshot = true;
        await this.captureScreenshotForNextTurn('request_screenshot');
        continue;
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
        if (this.usesOutcomePlan() && this.managerPageUnchanged) {
          this.noteManagerIdle();
        }
        await sleep(normalized.waitMs);
        return {handled: true, requestContext: false};
      }
      if (normalized.action === 'navigate' || normalized.action === 'click' ||
          normalized.action === 'hover' || normalized.action === 'type' ||
          normalized.action === 'key_press' || normalized.action === 'scroll' ||
          normalized.action === 'scroll_to') {
        if (!this.currentTask) {
          this.addSystemMessage(
              'Executable tab actions require an active queued task. In manual chat, use request_context / request_console_logs / investigate_page.');
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
          if (await this.maybeForceStagnationFallback(normalized, context)) {
            return {handled: true, requestContext: true};
          }
          const repeat = this.executionTimeline.rejects(normalized, context);
          if (repeat) {
            this.addSystemMessage(repeat);
            return {handled: true, requestContext: false};
          }
          this.executionTimeline.begin(normalized, context);
          const ok = await this.performAction(normalized, context);
          if (!ok) {
            const revert = this.executionTimeline.fail(context);
            if (revert) {
              await this.navigateCurrentTab(revert);
            }
          }
          return {handled: ok, requestContext: true};
        }
        if (this.shouldBlockSubmitUntilCaptcha(normalized, context)) {
          this.addSystemMessage(
              'Blocked Log In/submit: captcha is still unchecked. ' +
              (this.hasLiveScreenshot() ?
                  'Click {"coordinate":{"x":120,"y":80}} on the attached 1:1 screenshot checkbox. Do not request_screenshot.' :
                  '{"action":"request_screenshot"} then {"action":"click","coordinate":{"x":120,"y":80}} on the checkbox first.'));
          return {handled: true, requestContext: false};
        }
        if (typeof normalized.target_id === 'number' &&
            !this.getElementById(context, normalized.target_id)) {
          this.executionTimeline.begin(normalized, context);
          this.executionTimeline.fail(context);
          this.addSystemMessage(
              `Invalid target_id ${normalized.target_id}: not present in current Page context elements[].id list.`);
          return {handled: true, requestContext: false};
        }
        if (this.actionLooksLikeCaptcha(normalized, context)) {
          normalized.__glic_captcha = true;
        }
        if (await this.maybeForceStagnationFallback(normalized, context)) {
          return {handled: true, requestContext: true};
        }
        const repeat = this.executionTimeline.rejects(normalized, context);
        if (repeat) {
          this.addSystemMessage(repeat);
          return {handled: true, requestContext: false};
        }
        this.executionTimeline.begin(action, context);
        const ok = await this.performAction(normalized, context);
        if (!ok) {
          const revert = this.executionTimeline.fail(context);
          if (revert) {
            await this.navigateCurrentTab(revert);
          }
          return {handled: true, requestContext: true};
        }
        if (ok) {
          this.screenshotOnlyStreak = 0;
          this.capturedScreenshotThisTurn = false;
          if (this.usesOutcomePlan() &&
              await this.noteTaskCompleterProgress(action, normalized)) {
            return {handled: true, requestContext: false};
          }
        }
        if (ok && normalized.__glic_captcha) {
          return await this.afterCaptchaActuation(context);
        }
        if (ok && (normalized.action === 'scroll' ||
                   normalized.action === 'scroll_to' ||
                   normalized.action === 'navigate')) {
          this.pendingScreenshot = null;
          this.screenshotSpamStrikes = 0;
          this.lastScreenshotClickSpace = null;
          if (this.captchaChallengeActive && this.supportsScreenshotAction()) {
            await this.captureScreenshotForNextTurn('captcha multi-step vision');
          }
        }
        return {handled: ok, requestContext: !ok};
      }
    }
    return {handled: handledScreenshot, requestContext: false};
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

  private syncPromptPlaceholder(): void {
    const promptEl = this.promptInputEl ||
        (document.getElementById('prompt') as HTMLInputElement | null);
    if (!promptEl) {
      return;
    }
    promptEl.placeholder = this.guideCurrentTaskEnabled ?
        'Add guidance for the next step of the current task...' :
        'Ask...';
  }

  private queueGuidanceForCurrentTask(text: string): void {
    const trimmed = String(text || '').trim();
    if (!trimmed || !this.currentTask) {
      return;
    }
    this.pendingLoopGuidance.push(trimmed);
    const when = this.taskLoopRunning && !this.taskLoopStopRequested ?
        'It will be added on the next step.' :
        'It will be added when the task loop continues.';
    this.addSystemMessage(
        `Queued guidance for the current task (will not start a new task). ${when}\n${trimmed}`);
  }

  private clearLoopGuidanceState(): void {
    this.pendingLoopGuidance = [];
    this.activeLoopGuidance = '';
    this.loopGuidanceAppliedTurns = 0;
  }

  private clearCaptchaChallengeState(): void {
    this.captchaChallengeActive = false;
    this.captchaClicksThisChallenge = 0;
    this.captchaSeenThisTask = false;
    this.captchaActuatedThisTask = false;
    this.sawCaptchaFollowUpThisTask = false;
    this.captchaClearedThisTask = false;
    this.humanCaptchaHold = false;
    this.screenshotOnlyStreak = 0;
    this.capturedScreenshotThisTurn = false;
    this.screenshotSpamStrikes = 0;
    this.pendingScreenshot = null;
    this.syncSolvedCaptchaButtons();
  }

  private applyPendingLoopGuidance(): void {
    if (!this.pendingLoopGuidance.length || !this.currentTask) {
      return;
    }
    const extra = this.pendingLoopGuidance.join('\n\n');
    this.pendingLoopGuidance = [];
    this.activeLoopGuidance = this.activeLoopGuidance ?
        `${this.activeLoopGuidance}\n\n${extra}` :
        extra;
    // Newly queued text is fresh even if older guidance had already been used.
    this.loopGuidanceAppliedTurns = 0;
    this.updateTaskInfo();
  }

  private formatActiveLoopGuidanceForPrompt(): string {
    if (!this.activeLoopGuidance) {
      return '';
    }
    if (this.loopGuidanceAppliedTurns <= 0) {
      return (
        'USER GUIDANCE (just added this step — treat as part of the original goal and follow it now):\n' +
        this.activeLoopGuidance +
        '\n\n'
      );
    }
    return (
      'USER GUIDANCE (may be stale — the user added this earlier; it may no longer be needed or relevant):\n' +
      this.activeLoopGuidance +
      '\nIf this extra guidance is no longer needed, ignore it and set "ignore_guidance":true on your action JSON ' +
      '(you may still include the real next action in the same object), or return {"action":"ignore_guidance"}. ' +
      'We will then stop attaching this extra guidance on later steps.\n\n'
    );
  }

  private actionRequestsIgnoreGuidance(raw: any): boolean {
    if (!raw || typeof raw !== 'object') {
      return false;
    }
    const verb = String(
                     raw.action ?? raw.action_type ?? raw.actionType ??
                     raw.type ?? raw.name ?? '')
                     .toLowerCase()
                     .trim();
    if (verb === 'ignore_guidance' || verb === 'drop_guidance' ||
        verb === 'guidance_stale') {
      return true;
    }
    const flag = raw.ignore_guidance ?? raw.ignoreGuidance ??
        raw.guidance_stale ?? raw.guidanceStale ?? raw.drop_guidance ??
        raw.dropGuidance;
    return flag === true || flag === 1 ||
        String(flag || '').trim().toLowerCase() === 'true';
  }

  private clearActiveLoopGuidance(reason: string): void {
    if (!this.activeLoopGuidance && !this.pendingLoopGuidance.length) {
      return;
    }
    this.activeLoopGuidance = '';
    this.pendingLoopGuidance = [];
    this.loopGuidanceAppliedTurns = 0;
    this.addSystemMessage(
        `Dropped extra task guidance (${reason}). Later steps use the original goal only.`);
    this.updateTaskInfo();
  }

  private captchaPageBlob(context?: PageContextSummary): string {
    if (!context) {
      return '';
    }
    const bits: string[] = [
      String(context.url || ''),
      String(context.title || ''),
      String(context.innerText || '').slice(0, 2500),
    ];
    for (const el of (context.elements ?? []).slice(0, 50)) {
      bits.push(
          String(el.label || ''), String(el.cssClass || ''),
          String(el.role || ''), String(el.fieldName || ''),
          String(el.placeholder || ''));
    }
    return bits.join(' ').toLowerCase();
  }

  /** Widget is on the form (IconCaptcha heading, recaptcha, …). Not the visual loop. */
  private pageHasCaptchaWidget(context?: PageContextSummary): boolean {
    const blob = this.captchaPageBlob(context);
    if (!blob) {
      return false;
    }
    return /iconcaptcha|#captcha|captcha_container|captcha-container|\brecaptcha\b|\bh-?captcha\b|\bturnstile\b|cf-challenge|challenges\.cloudflare|i['’]?m not a robot|verify (that )?you are human/.test(blob);
  }

  /** Active visual challenge (Cloudflare / recaptcha interstitial), not a login-form checkbox. */
  private looksLikeCaptchaChallenge(context?: PageContextSummary): boolean {
    const blob = this.captchaPageBlob(context);
    if (!blob) {
      return false;
    }
    return /\brecaptcha\b|\bh-?captcha\b|\bturnstile\b|cf-challenge|challenges\.cloudflare|i['’]?m not a robot|unusual traffic from your computer/.test(blob);
  }

  /** Puzzle / second-step UI after the first checkbox click. */
  private looksLikeCaptchaFollowUp(context?: PageContextSummary): boolean {
    const blob = this.captchaPageBlob(context);
    if (!blob) {
      return false;
    }
    if (context?.hasOverlay && this.captchaActuatedThisTask) {
      return true;
    }
    // Do not match the static IconCaptcha heading — only extra puzzle copy.
    return /select the|click all|click each|which of these|odd one out|matching icons?|similar images?|next image|next challenge|tiles? remaining|choose the|pick the/.test(blob);
  }

  private looksLikeCaptchaSolved(context?: PageContextSummary): boolean {
    if (!context) {
      return false;
    }
    if (this.looksLikeLoginPage(context) === false && this.captchaActuatedThisTask) {
      return true;
    }
    const blob = this.captchaPageBlob(context);
    return /captcha (solved|verified|complete)|verification complete|you(?: are|['’]re) verified/.test(blob);
  }

  private captchaStillOpen(context?: PageContextSummary): boolean {
    if (this.captchaClearedThisTask) {
      return false;
    }
    if (this.looksLikeCaptchaSolved(context)) {
      return false;
    }
    if (this.looksLikeCaptchaChallenge(context) || this.looksLikeCaptchaFollowUp(context)) {
      return true;
    }
    // Visual loop stays armed so more [x,y] clicks are allowed, but a static
    // "verify you are human" heading alone is not a remaining puzzle.
    return this.looksLikeCaptchaChallenge(context) ||
        this.looksLikeCaptchaFollowUp(context);
  }

  private markCaptchaCleared(reason: string): void {
    this.captchaClearedThisTask = true;
    this.captchaChallengeActive = false;
    this.screenshotSpamStrikes = 0;
    this.pendingScreenshot = null;
    this.addSystemMessage(`Captcha cleared (${reason}). Login/submit is allowed now.`);
  }

  private mustSolveCaptchaBeforeSubmit(context?: PageContextSummary): boolean {
    if (this.humanCaptchaHold || this.captchaClearedThisTask) {
      return false;
    }
    if (this.looksLikeCaptchaSolved(context)) {
      return false;
    }
    if (this.captchaStillOpen(context)) {
      return true;
    }
    // Checkbox not clicked yet: block submit. After a click, only block if
    // a follow-up puzzle/challenge is actually showing.
    return this.pageHasCaptchaWidget(context) && !this.captchaActuatedThisTask;
  }

  private captchaMustSolveBeforeSubmitHint(): string {
    if (this.captchaActuatedThisTask) {
      return (
        'CAPTCHA IS NOT FINISHED. The checkbox click may open more steps (icons, tiles, another image). ' +
        'Use the CURRENT 1:1 screenshot and click {"coordinate":{"x":120,"y":80}} on the next target. ' +
        'Do NOT press Enter or click Log In / Sign In / Claim / Collect until the extra challenge is gone. ' +
        'Do not request another screenshot.'
      );
    }
    if (this.hasLiveScreenshot()) {
      return (
        'CAPTCHA IS ON THIS FORM and is not done yet. A 1:1 screenshot is already attached. ' +
        'Scroll if the checkbox is off-screen, then click {"coordinate":{"x":120,"y":80}} on it. ' +
        'Do not request_screenshot unless you just scrolled. ' +
        'Do NOT press Enter or click Log In / Sign In / Claim / Collect until the captcha is fully done.'
      );
    }
    return (
      'CAPTCHA IS ON THIS FORM and is not done yet. Order: ' +
      'type email/password with target_id, then {"action":"request_screenshot"}, ' +
      'then {"action":"click","coordinate":{"x":120,"y":80}} on the checkbox circle. ' +
      'If more images/tasks appear after that, keep clicking them on a fresh screenshot. ' +
      'Do NOT press Enter or click Log In / Sign In / Claim / Collect until the captcha is fully done. ' +
      'Login fields still use target_id. The captcha uses screenshot pixels only.'
    );
  }

  private isLoginSubmitTarget(action: any, context: PageContextSummary): boolean {
    if (action?.__login_fallback || action?.__login_autofill_bypass ||
        action?.__after_type_submit) {
      return true;
    }
    const kind = String(action?.action || '').toLowerCase();
    if (kind === 'key_press' &&
        /^(enter|return)$/i.test(String(action.key || ''))) {
      return this.looksLikeLoginPage(context) || this.pageHasCaptchaWidget(context);
    }
    if (kind !== 'click') {
      return false;
    }
    const loginId = this.findLoginButtonId(context);
    if (typeof action?.target_id === 'number' && loginId !== null &&
        action.target_id === loginId) {
      return true;
    }
    if (typeof action?.target_id === 'number') {
      const el = this.getElementById(context, action.target_id);
      const text = `${el?.label || ''} ${el?.placeholder || ''}`.toLowerCase();
      if (/(log ?in|sign ?in|submit|claim|collect)\b/.test(text)) {
        return true;
      }
    }
    return false;
  }

  private shouldBlockSubmitUntilCaptcha(action: any, context: PageContextSummary): boolean {
    if (!this.mustSolveCaptchaBeforeSubmit(context)) {
      return false;
    }
    return this.isLoginSubmitTarget(action, context);
  }

  private captchaSolverHint(): string {
    const grid = this.supportsScreenshotAction() ?
        'Look at the CURRENT 1:1 viewport screenshot and click the checkbox circle with {"coordinate":{"x":120,"y":80}}. Never reuse an earlier shot. ' :
        'No screenshot available — keep using target_id from the last context if you have one. ';
    let text = (
      'CAPTCHA / visual challenge: click the checkbox, then any follow-up tiles/icons, not Claim/Collect. ' +
      '{"action":"click","coordinate":{"x":120,"y":80}} from the CURRENT 1:1 screenshot. ' +
      'Do not use elements[], locators, or iframe boxes. ' +
      grid +
      'After a captcha click the host waits and reshoots; do not request_context until the challenge is gone.'
    );
    if (this.captchaClicksThisChallenge >= 3) {
      text +=
          ' Soft suggestion only: several attempts have not cleared it. ' +
          'You may {"action":"human_solve_captcha"} as a last resort — do not prefer that over another screenshot+coordinate try.';
    }
    return text;
  }

  private actionLooksLikeCaptcha(action: any, context: PageContextSummary): boolean {
    if (action?.__glic_captcha) {
      return true;
    }
    const kind = String(action?.action || '').toLowerCase();
    if (!(kind === 'click' || kind === 'hover' || kind === 'type')) {
      return false;
    }
    const hasTarget = typeof action?.target_id === 'number' && action.target_id > 0;
    const hasCoord = !!(action?.coordinate &&
        typeof action.coordinate.x === 'number' &&
        typeof action.coordinate.y === 'number');
    if (hasTarget && !hasCoord) {
      return false;
    }
    return !!(hasCoord &&
        (this.captchaChallengeActive || this.pageHasCaptchaWidget(context)));
  }

  private syncSolvedCaptchaButtons(): void {
    const show = !!(this.humanCaptchaHold && this.currentTask);
    for (const id of ['solvedCaptcha', 'queueSolvedCaptcha']) {
      const el = document.getElementById(id);
      if (!el) {
        continue;
      }
      if (show) {
        el.removeAttribute('hidden');
      } else {
        el.setAttribute('hidden', 'hidden');
      }
    }
  }

  private pauseForHumanCaptcha(reason: string): void {
    this.humanCaptchaHold = true;
    this.taskLoopStopRequested = true;
    this.queueRunning = false;
    this.cancelModelRequest('waiting for captcha solve');
    void this.abortActiveTooling('waiting for captcha solve');
    this.updateQueueStatus();
    this.updateTaskInfo();
    this.syncSolvedCaptchaButtons();
    this.addSystemMessage(
        `${reason}. Solve it in the page, then press "I solved it" or Resume. ` +
        'Captcha clicks are not saved into the reusable script.');
  }

  private async afterCaptchaActuation(
      context: PageContextSummary): Promise<ActionHandlingResult> {
    this.captchaChallengeActive = true;
    this.captchaSeenThisTask = true;
    this.captchaActuatedThisTask = true;
    this.captchaClearedThisTask = false;
    this.captchaClicksThisChallenge += 1;
    this.screenshotOnlyStreak = 0;
    this.addSystemMessage(
        'Captcha click sent. Waiting in case more tiles/icons appear, then taking a fresh 1:1 screenshot.');
    await sleep(2200);
    if (this.taskLoopStopRequested || !this.currentTask) {
      return {handled: true, requestContext: false};
    }
    let post = context;
    try {
      post = await this.collectContext();
    } catch (_) {
    }
    if (this.looksLikeCaptchaFollowUp(post)) {
      this.sawCaptchaFollowUpThisTask = true;
    }
    if (this.looksLikeCaptchaSolved(post) ||
        (this.sawCaptchaFollowUpThisTask && !this.looksLikeCaptchaFollowUp(post) &&
         !this.looksLikeCaptchaChallenge(post))) {
      this.markCaptchaCleared('follow-up challenge is gone');
      return {handled: true, requestContext: true};
    }
    await this.captureScreenshotForNextTurn('captcha multi-step vision');
    this.addSystemMessage(
        'Look at the new screenshot. If more captcha steps appeared, click the next {"coordinate":{"x":120,"y":80}}. ' +
        'Do not Log In yet. Do not reuse the previous pixel.');
    return {handled: true, requestContext: false};
  }

  private isCaptchaScaffoldEvent(ev: GlicRecorderScaffoldEvent): boolean {
    const src = ev?.sourceAction;
    if (src?.__glic_captcha) {
      return true;
    }
    return !!(ev?.timelineAction?.params?.__glic_human_captcha);
  }

  private buildHumanCaptchaPauseAction(): any {
    return {
      action: 'glic_prompt',
      params: {
        prompt:
            'Complete the captcha on this page. Stay on this tab — do not navigate away, ' +
            'do not open a new tab, and do not reload into a different URL. ' +
            'Use the current 1:1 viewport screenshot and click/type with {"coordinate":{"x":120,"y":80}}. ' +
            'After each click, wait for the widget to settle and use a ' +
            'fresh screenshot; do not reuse old pixels. When the challenge is gone, finish. ' +
            'Keep trying with fresh screenshots. Only if many attempts fail, finish unsuccessful.',
        stayOnCurrentTab: true,
        skipAutoSave: true,
        __glic_human_captcha: true,
        humanCaptcha: true,
      },
    };
  }

  private withHumanCaptchaPause(actions: any[], insertAt: number): any[] {
    const i = Math.max(0, Math.min(insertAt, actions.length));
    const next = actions.slice();
    next.splice(i, 0, this.buildHumanCaptchaPauseAction());
    return next;
  }

  private captchaInsertIndex(
      events: GlicRecorderScaffoldEvent[], captchaTs?: number): number {
    if (!Number.isFinite(captchaTs as number)) {
      return events.length;
    }
    const i = events.findIndex((ev) => (ev.timestampMs || 0) >= (captchaTs as number));
    return i < 0 ? events.length : i;
  }

  private async submitPromptText(
      text: string,
      options?: {clearInput?: boolean},
  ): Promise<void> {
    const trimmed = String(text || '').trim();
    if (!trimmed) {
      return;
    }
    const promptEl = this.promptInputEl ||
        (document.getElementById('prompt') as HTMLInputElement | null);
    if (this.guideCurrentTaskEnabled) {
      if (!this.currentTask) {
        this.addSystemMessage(
            'No active task to guide. Uncheck "Guide current task" to start a new prompt or task.');
        return;
      }
      this.pushPromptHistory(trimmed);
      if (options?.clearInput && promptEl) {
        promptEl.value = '';
      }
      this.queueGuidanceForCurrentTask(trimmed);
      return;
    }
    this.pushPromptHistory(trimmed);
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


  // A provider error is not a model reply. Counting it as an empty turn
  // burns the step budget as fast as the HTTP failure returns.
  private providerFailureReason(text: string): string {
    const raw = String(text || '').trim();
    if (!raw.startsWith('Error:')) {
      return '';
    }
    const missing = raw.match(/model '([^']+)' not found/i);
    if (missing && missing[1]) {
      return `Stopped. Model '${missing[1]}' is not available, so the request never reached a model.`;
    }
    if (/not_found_error|dashboardAgentOllamaChat failed|ECONNREFUSED|Failed to fetch|fetch failed/i.test(raw) ||
        /^Error:\s*\d{3}\b/.test(raw)) {
      return 'Stopped. The model request failed before a reply.';
    }
    return '';
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

    const providerFailure =
        this.currentTask ? this.providerFailureReason(assistantText) : '';
    if (providerFailure) {
      this.lastProviderFailure = providerFailure;
      this.taskLoopStopRequested = true;
      this.messages.push({role: 'assistant', content: assistantText});
      this.render();
      return assistantText;
    }

    this.messages.push({role: 'assistant', content: assistantText});
    this.render();

    let workingText = assistantText;
    if (options?.handleActions && options.context && this.currentTask) {
      const schemaError = this.preflightActionSchema(workingText);
      if (schemaError) {
        const repaired = await this.repromptForStructuredAction(
            workingText, schemaError, model, headers);
        if (repaired && repaired !== workingText) {
          workingText = repaired;
          this.messages[this.messages.length - 1] = {role: 'assistant', content: workingText};
          this.render();
        }
      }
    }
    if (this.currentTask && await this.maybeCompleteTask(workingText)) {
      return workingText;
    }
    if (options?.handleActions && options.context) {
      const actionResult =
          await this.handleAssistantActions(workingText, options.context);
      this.requestContextNextTurn = actionResult.requestContext;
      if (actionResult.requestContext) {
        this.addSystemMessage('Model requested a fresh page context snapshot.');
      }
      if (!actionResult.handled && this.currentTask) {
        const extracted = this.extractActions(workingText);
        if (extracted.length > 0) {
          this.addSystemMessage(
              'Action JSON was returned, but execution did not complete successfully.');
        } else {
          const preview = String(workingText || '').replace(/\s+/g, ' ').trim().slice(0, 180);
          const hadBraces = String(workingText || '').includes('{');
          if (this.usesOutcomePlan()) {
            this.noteManagerIdle();
          }
          this.addSystemMessage(
              hadBraces
                  ? `No actionable JSON found after translation (raw preview: ${preview || '(empty)'}).`
                  : 'No actionable JSON found in model response.');
        }
      }
    }

    return workingText;
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

// Packaged chrome://glic/agent guest never receives Gemini's glic-bootstrap
// (or CSP blocks injecting it). Wire buttons immediately; a later registry
// registerWebClient upgrades this.browser without re-binding listeners.
if (typeof (window as any).chrome?.send === 'function' ||
    String(window.location?.protocol || '') === 'chrome:' ||
    String(window.location?.protocol || '') === 'blob:' ||
    String(window.location?.protocol || '') === 'about:') {
  void client.initialize({} as GlicBrowserHost);
}

createGlicHostRegistryOnLoad().then((registry: GlicHostRegistry) => {
  void registry.registerWebClient(client);
});

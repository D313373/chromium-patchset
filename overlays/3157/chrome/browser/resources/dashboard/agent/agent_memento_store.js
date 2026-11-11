// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Cross-turn notes for the local agent.
 *
 * Each memento is {content, timestamp, taskId, ttl}. Task-scoped rows die
 * when that task ends solved or blocked. Rows with taskId '*' are global
 * preferences and survive the prune. ttl is milliseconds; 0 means no expiry.
 */

const STORAGE_KEY = 'glic.agent_mementos';
const GLOBAL_TASK_ID = '*';
const DEFAULT_TASK_TTL_MS = 30 * 60 * 1000;
const MAX_MEMENTOS = 64;

function looksLikeGlobalPreference(content) {
  return /^(?:pref|preference|global)\s*:|^(?:always|never)\b/i.test(
      String(content || '').trim());
}

function isExpired(item, now) {
  const ttl = Number(item && item.ttl) || 0;
  if (ttl <= 0) {
    return false;
  }
  return now - Number(item.timestamp || 0) >= ttl;
}

function normalizeEntry(entry, now) {
  if (typeof entry === 'string') {
    const content = entry.trim();
    if (!content) {
      return null;
    }
    const global = looksLikeGlobalPreference(content);
    return {
      content: content.slice(0, 2000),
      timestamp: now,
      taskId: global ? GLOBAL_TASK_ID : '',
      ttl: global ? 0 : DEFAULT_TASK_TTL_MS,
    };
  }
  if (!entry || typeof entry !== 'object') {
    return null;
  }
  const content = String(entry.content ?? entry.text ?? '').trim();
  if (!content) {
    return null;
  }
  const explicitGlobal = entry.global === true || entry.scope === 'global' ||
      entry.taskId === GLOBAL_TASK_ID || looksLikeGlobalPreference(content);
  const taskId = explicitGlobal ? GLOBAL_TASK_ID :
      String(entry.taskId || entry.task_id || '').trim();
  let ttl = Number(entry.ttl);
  if (!Number.isFinite(ttl) || ttl < 0) {
    ttl = explicitGlobal ? 0 : DEFAULT_TASK_TTL_MS;
  }
  const timestamp = Number(entry.timestamp);
  return {
    content: content.slice(0, 2000),
    timestamp: Number.isFinite(timestamp) && timestamp > 0 ? timestamp : now,
    taskId,
    ttl,
  };
}

export function createAgentMementoStore(options = {}) {
  const storage = options.storage ||
      (typeof localStorage !== 'undefined' ? localStorage : null);
  const nowFn = typeof options.now === 'function' ? options.now : () => Date.now();
  let items = [];
  let activeTaskId = '';

  function persist() {
    if (!storage) {
      return;
    }
    try {
      storage.setItem(STORAGE_KEY, JSON.stringify(items));
    } catch (_) {}
  }

  function dropExpired() {
    const now = nowFn();
    items = items.filter((item) => !isExpired(item, now));
  }

  function load() {
    items = [];
    if (!storage) {
      return;
    }
    let raw = '';
    try {
      raw = storage.getItem(STORAGE_KEY) || '';
    } catch (_) {
      return;
    }
    if (!raw) {
      return;
    }
    const now = nowFn();
    try {
      const parsed = JSON.parse(raw);
      const list = Array.isArray(parsed) ? parsed :
          (typeof parsed === 'string' ? [parsed] : []);
      items = list.map((entry) => normalizeEntry(entry, now)).filter(Boolean);
    } catch (_) {
      const one = normalizeEntry(raw, now);
      items = one ? [one] : [];
    }
    dropExpired();
    persist();
  }

  function setActiveTask(taskId) {
    activeTaskId = String(taskId || '').trim();
  }

  function write(content, meta = {}) {
    const text = String(content || '').trim();
    if (!text) {
      return false;
    }
    const now = nowFn();
    dropExpired();
    const global = !!(meta && (meta.global === true || meta.scope === 'global' ||
        looksLikeGlobalPreference(text)));
    const taskId = global ? GLOBAL_TASK_ID :
        String((meta && (meta.taskId || meta.task_id)) || activeTaskId || '').trim();
    let ttl = Number(meta && meta.ttl);
    if (!Number.isFinite(ttl) || ttl < 0) {
      ttl = global ? 0 : DEFAULT_TASK_TTL_MS;
    }
    items.push({
      content: text.slice(0, 2000),
      timestamp: now,
      taskId,
      ttl,
    });
    if (items.length > MAX_MEMENTOS) {
      const removable = [];
      items.forEach((item, index) => {
        if (item.taskId !== GLOBAL_TASK_ID) {
          removable.push(index);
        }
      });
      const overflow = items.length - MAX_MEMENTOS;
      const drop = new Set(removable.slice(0, overflow));
      items = items.filter((_, index) => !drop.has(index));
      if (items.length > MAX_MEMENTOS) {
        items = items.slice(items.length - MAX_MEMENTOS);
      }
    }
    persist();
    return true;
  }

  function liveItems(taskId) {
    const now = nowFn();
    const id = String(taskId || activeTaskId || '').trim();
    return items.filter((item) => {
      if (isExpired(item, now)) {
        return false;
      }
      if (item.taskId === GLOBAL_TASK_ID) {
        return true;
      }
      return !!id && item.taskId === id;
    });
  }

  function readText(taskId) {
    return liveItems(taskId).map((item) => item.content).join('\n');
  }

  function promptBlock(taskId) {
    const rows = liveItems(taskId);
    if (!rows.length) {
      return '';
    }
    const lines = [];
    const prefs = rows.filter((item) => item.taskId === GLOBAL_TASK_ID);
    const notes = rows.filter((item) => item.taskId !== GLOBAL_TASK_ID);
    if (prefs.length) {
      lines.push('Global preferences:');
      for (const item of prefs) {
        lines.push(`- ${item.content}`);
      }
    }
    if (notes.length) {
      lines.push('Task notes:');
      for (const item of notes) {
        lines.push(`- ${item.content}`);
      }
    }
    return lines.join('\n');
  }

  /**
   * Task boundary. Drops this task's notes and anything past its ttl.
   * Global preferences (taskId '*') stay.
   */
  function pruneCompletedTask(taskId) {
    const id = String(taskId || activeTaskId || '').trim();
    const now = nowFn();
    const before = items.length;
    items = items.filter((item) => {
      if (isExpired(item, now)) {
        return false;
      }
      if (item.taskId === GLOBAL_TASK_ID) {
        return true;
      }
      if (!item.taskId) {
        return false;
      }
      if (id && item.taskId === id) {
        return false;
      }
      return true;
    });
    const removed = before - items.length;
    if (removed) {
      persist();
    }
    if (id && activeTaskId === id) {
      activeTaskId = '';
    }
    return removed;
  }

  load();
  return {
    load,
    setActiveTask,
    write,
    readText,
    promptBlock,
    pruneCompletedTask,
  };
}

let sharedStore = null;

export function getAgentMementoStore() {
  if (!sharedStore) {
    sharedStore = createAgentMementoStore();
  }
  return sharedStore;
}

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Shared investigate_page result trimmer for model prompts / heal / dispatcher.
 * Keeps the useful text snapshot; drops screenshots and page-map echoes.
 */

function slimLocatorProbe(probe) {
  if (!probe || typeof probe !== 'object') {
    return undefined;
  }
  const per = Array.isArray(probe.per_locator) ?
    probe.per_locator :
    (Array.isArray(probe.perLocator) ? probe.perLocator : null);
  const result = probe.result && typeof probe.result === 'object' ?
    {
      found: !!probe.result.found,
      match_count: probe.result.match_count ?? probe.result.matchCount,
      tag: probe.result.tag,
      css_selector: probe.result.css_selector || probe.result.cssSelector,
      text: probe.result.text ?
        String(probe.result.text).slice(0, 80) :
        undefined,
    } :
    undefined;
  const slimPer = per ?
    per.slice(0, 8).map((entry) => {
      if (!entry || typeof entry !== 'object') {
        return entry;
      }
      const sample = entry.sample && typeof entry.sample === 'object' ?
        {
          tag: entry.sample.tag,
          css_selector:
              entry.sample.css_selector || entry.sample.cssSelector,
          id_attr: entry.sample.id_attr || entry.sample.idAttr,
          visible: entry.sample.visible,
          text: entry.sample.text ?
            String(entry.sample.text).slice(0, 60) :
            undefined,
        } :
        undefined;
      return {
        type: entry.type,
        value: entry.value,
        found: !!entry.found,
        match_count: entry.match_count ?? entry.matchCount,
        matched_via: entry.matched_via || entry.matchedVia,
        sample,
      };
    }) :
    undefined;
  return {
    condition: probe.condition,
    locators: Array.isArray(probe.locators) ?
      probe.locators.slice(0, 8) :
      undefined,
    per_locator: slimPer,
    result,
  };
}

function slimTables(tables) {
  if (!Array.isArray(tables) || !tables.length) {
    return undefined;
  }
  return tables.slice(0, 4).map((t) => {
    if (!t || typeof t !== 'object') {
      return t;
    }
    return {
      id: t.id,
      className: t.className,
      trCount: t.trCount ?? t.tr_count,
      tbodyChildTags: t.tbodyChildTags || t.tbody_child_tags,
      tbodyChildCount: t.tbodyChildCount ?? t.tbody_child_count,
      suggestedRowSelector:
          t.suggestedRowSelector || t.suggested_row_selector,
      sampleRows: Array.isArray(t.sampleRows) ?
        t.sampleRows.slice(0, 3) :
        (Array.isArray(t.sample_rows) ? t.sample_rows.slice(0, 3) : undefined),
    };
  });
}

function slimControls(list, limit) {
  if (!Array.isArray(list) || !list.length) {
    return undefined;
  }
  return list.slice(0, limit);
}

/**
 * @param {Object} raw investigate_page result
 * @return {Object|null}
 */
export function slimInvestigateResult(raw) {
  if (!raw || typeof raw !== 'object') {
    return null;
  }
  const out = {
    ok: raw.ok !== false,
    error: raw.error ? String(raw.error).slice(0, 240) : undefined,
    transport: raw.transport,
    url: raw.url,
    title: raw.title,
    ready_state: raw.ready_state || raw.readyState,
    element_count: raw.element_count ?? raw.elementCount,
    target_id: raw.target_id || raw.targetId,
    close_target: raw.close_target ?? raw.closeTarget,
    locator_probe: slimLocatorProbe(
        raw.locator_probe || raw.locatorProbe),
    tables: slimTables(raw.tables),
    inputs: slimControls(raw.inputs, 6),
    buttons: slimControls(raw.buttons, 6),
    links: slimControls(raw.links, 4),
    inner_text_excerpt: String(
        raw.inner_text_excerpt || raw.innerTextExcerpt || '')
        .slice(0, 400) ||
        undefined,
  };
  // Drop undefined keys so prompts stay small.
  for (const key of Object.keys(out)) {
    if (out[key] === undefined) {
      delete out[key];
    }
  }
  return out;
}

/**
 * FULL / DELTA / UNCHANGED for successive investigate-style payloads on the
 * same target (login click/type storms). Mutates module-level last digest.
 */
let _lastInvestigateContextDigest = '';
let _lastInvestigateContextUrl = '';

function digestInvestigateContext(raw) {
  if (!raw || typeof raw !== 'object') {
    return '';
  }
  const url = String(raw.url || '');
  const title = String(raw.title || '');
  const inputs = Array.isArray(raw.inputs) ? raw.inputs.length : 0;
  const buttons = Array.isArray(raw.buttons) ? raw.buttons.length : 0;
  const excerpt = String(raw.inner_text_excerpt || raw.innerTextExcerpt || '')
                      .slice(0, 120);
  return `${url}|${title}|${inputs}|${buttons}|${excerpt}`;
}

/**
 * @param {Object} raw
 * @return {Object}
 */
export function applyInvestigateContextDiff(raw) {
  if (!raw || typeof raw !== 'object') {
    return raw;
  }
  const slim = slimInvestigateResult(raw) || {...raw};
  const url = String(slim.url || '');
  const digest = digestInvestigateContext(slim);
  if (url && url !== _lastInvestigateContextUrl) {
    slim.context_status = 'FULL';
    _lastInvestigateContextUrl = url;
    _lastInvestigateContextDigest = digest;
    return slim;
  }
  if (digest && digest === _lastInvestigateContextDigest) {
    return {
      ok: slim.ok !== false,
      error: slim.error,
      target_id: slim.target_id,
      url: slim.url,
      title: slim.title,
      context_status: 'UNCHANGED',
      actions_ok: raw.actions_ok,
      script_id: raw.script_id || raw.scriptId,
      failed_step: raw.failed_step ?? raw.failedStep,
    };
  }
  slim.context_status = 'DELTA';
  _lastInvestigateContextDigest = digest;
  if (url) {
    _lastInvestigateContextUrl = url;
  }
  return slim;
}

/**
 * Normalize outbound investigate payload: no screenshots by default, no
 * redundant navigate+wait when url is set (C++ synthesizes those).
 * @param {Object} payload
 * @return {Object}
 */
export function normalizeInvestigateRequestPayload(payload = {}) {
  const out = {...(payload || {})};
  const url = String(out.url || out.start_url || out.startUrl || '').trim();
  if (url) {
    out.url = url;
  }
  const tid = out.target_id || out.targetId;
  if (tid) {
    out.target_id = tid;
    delete out.targetId;
  }
  // Heal / investigator reuse tabs by default.
  if (out.close_target == null && out.closeTarget == null) {
    out.closeTarget = false;
  } else if (out.closeTarget == null && out.close_target != null) {
    out.closeTarget = !!out.close_target;
  }
  delete out.close_target;

  out.include_screenshot = false;
  out.includeScreenshot = false;
  out.include_image_base64 = false;
  delete out.includeImageBase64;

  // C++ InvestigateSession synthesizes navigate+wait from url; sending them
  // again is wasted IPC and can confuse "actions only nav+wait" short-circuit.
  if (url && Array.isArray(out.actions) && out.actions.length) {
    const onlyNavWait = out.actions.every((a) => {
      const name = String(a?.action || a?.type || '').toLowerCase();
      return name === 'navigate' || name === 'wait' || name === 'goto';
    });
    if (onlyNavWait) {
      delete out.actions;
    }
  }
  return out;
}

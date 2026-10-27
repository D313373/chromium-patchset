#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"

#include <algorithm>
#include <optional>
#include <sstream>

#include "base/logging.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/string_escape.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.mojom.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/isolated_world_ids.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"

namespace {

constexpr int kMinLocatorResolveTimeoutMs = 10000;
constexpr int kMaxLocatorResolveTimeoutMs = 120000;
constexpr int kMinLocatorRetryDelayMs = 5000;
constexpr int kMaxLocatorRetryDelayMs = 10000;
constexpr int kMaxLocatorResolveRetries = 1;

bool HasUsableResolvedGeometry(std::optional<int> cx,
                               std::optional<int> cy,
                               std::optional<int> left,
                               std::optional<int> top,
                               std::optional<int> width,
                               std::optional<int> height) {
  if (!cx || !cy) {
    return false;
  }
  if ((width && *width <= 1) || (height && *height <= 1)) {
    return false;
  }
  return !(*cx == 0 && *cy == 0 && left.value_or(0) == 0 &&
           top.value_or(0) == 0 && width.value_or(0) == 0 &&
           height.value_or(0) == 0);
}

bool HasResolvableElementIdentity(bool focusable,
                                  bool clickable,
                                  const std::string& id_attr,
                                  const std::string& name_attr) {
  return focusable || clickable || !id_attr.empty() || !name_attr.empty();
}

bool AcceptResolvedCandidateGeometry(std::optional<int> cx,
                                     std::optional<int> cy,
                                     std::optional<int> left,
                                     std::optional<int> top,
                                     std::optional<int> width,
                                     std::optional<int> height,
                                     bool has_identity) {
  if (HasUsableResolvedGeometry(cx, cy, left, top, width, height)) {
    return true;
  }
  // Layout may be deferred on background tabs, or the matched node may be a
  // wrapper whose meaningful geometry lives on a parent/child element.
  return has_identity;
}

std::vector<dashboard::mojom::LocatorPtr> ToMojomLocators(
    const std::vector<dashboard::Locator>& locators) {
  std::vector<dashboard::mojom::LocatorPtr> out;
  out.reserve(locators.size());
  for (const auto& locator : locators) {
    out.push_back(dashboard::mojom::Locator::New(
        dashboard::LocatorTypeToString(locator.type), locator.value));
  }
  return out;
}

dashboard::mojom::SearchScope ToMojomSearchScope(
    dashboard::SearchScope scope) {
  return scope == dashboard::SearchScope::kInteractiveOnly
             ? dashboard::mojom::SearchScope::kInteractiveOnly
             : dashboard::mojom::SearchScope::kAll;
}

// (Deprecated) legacy multi-result probe kept for reference.
#if 0
const char kBatchProbeJs[] = R"JS(
(function(locators, scope){
  const norm = (s) => String(s||'').replace(/\s+/g,' ').trim();
  function isInteractive(el){
    if (!el) return false;
    const tag = (el.tagName||'').toLowerCase();
    if (tag === 'a' || tag === 'button' || tag === 'input' || tag === 'textarea' || tag === 'select' || tag === 'label' || tag === 'summary') return true;
    if (el.hasAttribute('onclick')) return true;
    const role = el.getAttribute && el.getAttribute('role');
    if (role === 'button' || role === 'link') return true;
    return false;
  }
  function getCenter(el){
    const r = el.getBoundingClientRect();
    return {x: (r.left + r.right)/2, y: (r.top + r.bottom)/2};
  }
  function byXPath(xpath, root){
    try {
      const doc = root || document;
      const snap = doc.evaluate(xpath, doc, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
      const arr = [];
      for (let i=0;i<snap.snapshotLength;i++){ arr.push(snap.snapshotItem(i)); }
      return arr;
    } catch(e){ return []; }
  }
  function ariaMatchAll(name){
    const out = [];
    const all = document.getElementsByTagName('*');
    for (let i=0;i<all.length;i++){
      const el = all[i];
      const label = el.getAttribute && el.getAttribute('aria-label');
      if (label && label.trim() === name) out.push(el);
      const id = el.getAttribute && el.getAttribute('aria-labelledby');
      if (id){
        const lab = document.getElementById(id);
        if (lab && (lab.textContent||'').trim() === name) out.push(el);
      }
    }
    return out;
  }
  function byTextContent(text, allScope){
    const out = [];
    const all = document.getElementsByTagName('*');
    for (let i=0;i<all.length;i++){
      const el = all[i];
      if (!allScope && !isInteractive(el)) continue;
      if ((el.innerText||'').trim() === text) out.push(el);
    }
    return out;
  }
  function byPartialLinkText(substr){
    const out = [];
    const anchors = document.getElementsByTagName('a');
    for (let i=0;i<anchors.length;i++){
      const t = (anchors[i].innerText||'').trim();
      if (t.includes(substr)) out.push(anchors[i]);
    }
    return out;
  }
  function byLinkText(txt){
    const out = [];
    const anchors = document.getElementsByTagName('a');
    for (let i=0;i<anchors.length;i++){
      const t = (anchors[i].innerText||'').trim();
      if (t === txt) out.push(anchors[i]);
    }
    return out;
  }
  function byShadowChain(chain){
    // very simple: 'host_css >> inner_css'
    const parts = chain.split('>>').map(s=>s.trim());
    if (parts.length < 2) return [];
    let host = document.querySelector(parts[0]);
    if (!host || !host.shadowRoot) return [];
    let el = host.shadowRoot.querySelector(parts[1]);
    return el ? [el] : [];
  }
  function jsEval(expr){
    try {
      const el = eval(expr);
      return el ? [el] : [];
    } catch(e){ return []; }
  }

  const results = [];
  const pageUrl = (location && location.href) ? location.href : '';
  for (const loc of locators){
    let matches = [];
    switch(loc.type){
      case 'ID': matches = [document.getElementById(loc.value)].filter(Boolean); break;
      case 'CSS_SELECTOR': matches = Array.from(document.querySelectorAll(loc.value)); break;
      case 'NAME': matches = Array.from(document.querySelectorAll(`[name="${loc.value}"]`)); break;
      case 'CLASS_NAME': matches = Array.from(document.getElementsByClassName(loc.value)); break;
      case 'TAG_NAME': matches = Array.from(document.getElementsByTagName(loc.value)); break;
      case 'XPATH': matches = byXPath(loc.value); break;
      case 'LINK_TEXT': matches = byLinkText(loc.value); break;
      case 'PARTIAL_LINK_TEXT': matches = byPartialLinkText(loc.value); break;
      case 'ACCESSIBILITY_NAME': matches = ariaMatchAll(loc.value); break;
      case 'TEXT_CONTENT': matches = byTextContent(loc.value, scope === 'all'); break;
      case 'SHADOW_DOM': matches = byShadowChain(loc.value); break;
      case 'JS_EVAL': matches = jsEval(loc.value); break;
      case 'VISUAL_COORDS': results.push({type: loc.type, value: loc.value, matchCount: 1, centerX: 0, centerY: 0}); continue;
      default: matches = []; break;
    }
    if (scope === 'interactive') matches = matches.filter(isInteractive);
    const mc = matches.length;
    if (mc >= 1){
      const c = getCenter(matches[0]);
      results.push({type: loc.type, value: loc.value, matchCount: mc, centerX: c.x, centerY: c.y, pageUrl});
    } else {
      results.push({type: loc.type, value: loc.value, matchCount: 0, pageUrl});
    }
  }
  return results;
})
)JS";
#endif

// Simpler single-candidate selector used by the new one-pass flow.
[[maybe_unused]] const char kSelectByPriorityJs[] = R"JS(
(function(locators, scope){
  function isInteractive(el){
    if (!el) return false;
    const tag = (el.tagName||'').toLowerCase();
    if (tag === 'a' || tag === 'button' || tag === 'input' || tag === 'textarea' || tag === 'select' || tag === 'label' || tag === 'summary') return true;
    if (el.hasAttribute && el.hasAttribute('onclick')) return true;
    const role = el.getAttribute && el.getAttribute('role');
    if (role === 'button' || role === 'link') return true;
    return false;
  }
  function norm(s){
    return String(s||'').replace(/\s+/g,' ').trim();
  }
  function parseVisualCoords(v){
    const s = String(v||'').trim();
    if (!s) return null;
    try {
      const obj = JSON.parse(s);
      if (obj && Number.isFinite(Number(obj.x)) && Number.isFinite(Number(obj.y)))
        return {x: Math.round(Number(obj.x)), y: Math.round(Number(obj.y))};
    } catch (e) {}
    const m = s.match(/^(-?\d+)\s*,\s*(-?\d+)$/);
    if (m) return {x: parseInt(m[1],10), y: parseInt(m[2],10)};
    return null;
  }
  function byXPath(xpath){ try{ const snap=document.evaluate(xpath, document, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null); const arr=[]; for(let i=0;i<snap.snapshotLength;i++){arr.push(snap.snapshotItem(i));} return arr; }catch(e){ return []; } }
  function interactiveAncestor(el){
    let cur = el;
    for (let i = 0; i < 6 && cur; ++i) {
      if (isInteractive(cur)) return cur;
      cur = cur.parentElement;
    }
    return el;
  }
  function matchesFor(loc){
    switch(loc.type){
      case 'ID': return [document.getElementById(loc.value)].filter(Boolean);
      case 'CSS_SELECTOR': return Array.from(document.querySelectorAll(loc.value));
      case 'NAME': return Array.from(document.querySelectorAll(`[name=\"${loc.value}\"]`));
      case 'CLASS_NAME': return Array.from(document.getElementsByClassName(loc.value));
      case 'TAG_NAME': return Array.from(document.getElementsByTagName(loc.value));
      case 'XPATH': return byXPath(loc.value);
      case 'LINK_TEXT': {
        const want = norm(loc.value);
        const out=[]; const a=document.getElementsByTagName('a');
        for(let i=0;i<a.length;i++){ const t=norm((a[i].innerText||a[i].textContent)||''); if(t===want) out.push(a[i]); }
        return out;
      }
      case 'PARTIAL_LINK_TEXT': {
        const want = norm(loc.value);
        const out=[]; const a=document.getElementsByTagName('a');
        for(let i=0;i<a.length;i++){ const t=norm((a[i].innerText||a[i].textContent)||''); if(t.includes(want)) out.push(a[i]); }
        return out;
      }
      case 'TEXT_CONTENT': {
        const want = norm(loc.value);
        const out=[]; const all=document.getElementsByTagName('*');
        for(let i=0;i<all.length;i++){ const el=all[i]; const t=norm((el.innerText||el.textContent)||''); if(t===want) out.push(el); }
        return out;
      }
      case 'ACCESSIBILITY_NAME': {
        // Best-effort accessible-ish name heuristic. True accessible name
        // computation is complex; this covers common sources.
        const out=[]; const all=document.getElementsByTagName('*');
        const want = String(loc.value||'').replace(/\\s+/g,' ').trim();
        const norm = (s) => String(s||'').replace(/\\s+/g,' ').trim();
        for(let i=0;i<all.length;i++){
          const el=all[i];
          let name='';
          try {
            const aria = el.getAttribute && el.getAttribute('aria-label');
            if (aria) name = norm(aria);
            if (!name) {
              const id = el.getAttribute && el.getAttribute('aria-labelledby');
              if (id) {
                const ref = document.getElementById(String(id));
                if (ref) name = norm(ref.innerText || ref.textContent || '');
              }
            }
            if (!name) {
              const title = el.getAttribute && el.getAttribute('title');
              if (title) name = norm(title);
            }
            if (!name) {
              const alt = el.getAttribute && el.getAttribute('alt');
              if (alt) name = norm(alt);
            }
            if (!name && isInteractive(el)) {
              name = norm(el.innerText || el.textContent || '');
            }
          } catch(e) {}
          if (name && name === want) out.push(el);
        }
        return out;
      }
      case 'SHADOW_DOM': {
        // Resolve open-shadow-root chains encoded as: "host_css >> inner_css >> leaf_css".
        try {
          const parts = String(loc.value || '').split('>>')
            .map(s => s.trim()).filter(Boolean);
          if (parts.length < 2) return [];
          let ctx = document;
          let el = null;
          for (let i = 0; i < parts.length; i++) {
            el = ctx.querySelector(parts[i]);
            if (!el) return [];
            if (i < parts.length - 1) {
              if (!el.shadowRoot) return [];
              ctx = el.shadowRoot;
            }
          }
          return el ? [el] : [];
        } catch (e) { return []; }
      }
      case 'JS_EVAL': {
        try { const fn = new Function('return ('+String(loc.value)+')'); const res = fn(); if (res instanceof Element) return [res]; if (Array.isArray(res)) return res.filter(e=>e && e.nodeType===1); } catch (e) {}
        return [];
      }
      case 'VISUAL_AI': return [];
      case 'FRAME_ID': return [];
      case 'RESOURCE_TREE': return [];
      default: return [];
    }
  }
  function baseScore(type){
    switch(String(type||'')){
      // Visual coords are useful as a fallback, but should not override strong
      // stable locators (id/css/xpath/link text).
      case 'VISUAL_COORDS': return 550;
      case 'JS_EVAL': return 900;
      case 'ID': return 850;
      case 'XPATH': return 845;
      case 'CSS_SELECTOR': return 840;
      case 'ACCESSIBILITY_NAME': return 830;
      case 'LINK_TEXT': return 825;
      case 'PARTIAL_LINK_TEXT': return 800;
      case 'NAME': return 780;
      case 'CLASS_NAME': return 760;
      case 'TEXT_CONTENT': return 720;
      case 'TAG_NAME': return 680;
      default: return 0;
    }
  }
  function tagBonus(el){
    try {
      const tag = String(el.tagName||'').toLowerCase();
      if (tag === 'a') return 20;
      if (tag === 'button') return 18;
      if (tag === 'input') return 10;
      if (tag === 'textarea') return 4;
    } catch(e) {}
    return 0;
  }

  function rectFor(el){
    try { return el.getBoundingClientRect(); } catch(e) { return null; }
  }
  function isVisible(el, r){
    if (!el) return false;
    try {
      const cs = window.getComputedStyle ? window.getComputedStyle(el) : null;
      if (cs) {
        if (cs.display === 'none') return false;
        if (cs.visibility === 'hidden' || cs.visibility === 'collapse') return false;
        const op = parseFloat(cs.opacity || '1');
        if (!Number.isNaN(op) && op <= 0.01) return false;
      }
    } catch(e) {}
    if (!r) r = rectFor(el);
    if (!r) return false;
    if (!Number.isFinite(r.width) || !Number.isFinite(r.height)) return false;
    if (r.width <= 0 || r.height <= 0) return false;
    try { if (el.getClientRects && el.getClientRects().length === 0) return false; } catch(e) {}
    return true;
  }
  function isInViewport(r){
    if (!r) return false;
    const vw = Math.max(0, window.innerWidth || 0);
    const vh = Math.max(0, window.innerHeight || 0);
    if (vw === 0 || vh === 0) return true;
    return r.bottom > 0 && r.right > 0 && r.top < vh && r.left < vw;
  }
  function isDeceptive(el){
    if (!el || el.nodeType !== 1) return true;
    try {
      const cls = (typeof el.className === 'string') ? el.className : '';
      const blob = ((el.id || '') + ' ' + cls + ' ' +
          ((el.getAttribute && el.getAttribute('name')) || '')).toLowerCase();
      if (/honeypot|hp[-_]?field|bot[-_]?trap|decoy|fake[-_]?input|trap[-_]?field|leave[-_]?blank|hideme/.test(blob))
        return true;
      if (el.getAttribute && el.getAttribute('aria-hidden') === 'true') return true;
      const cs = window.getComputedStyle ? window.getComputedStyle(el) : null;
      if (cs) {
        if (cs.display === 'none' || cs.visibility === 'hidden' || cs.visibility === 'collapse')
          return true;
        const op = parseFloat(cs.opacity || '1');
        if (!isNaN(op) && op <= 0.02) return true;
        if (cs.pointerEvents === 'none') return true;
      }
    } catch (e) {}
    return false;
  }
  function hitByPoint(el, r){
    if (!el || !r || r.width < 2 || r.height < 2) return false;
    const x = r.left + r.width / 2;
    const y = r.top + r.height / 2;
    const vw = window.innerWidth || 0;
    const vh = window.innerHeight || 0;
    if (x < 0 || y < 0 || x >= vw || y >= vh) return false;
    let hit = null;
    try { hit = document.elementFromPoint(x, y); } catch (e) { return false; }
    return !!(hit && (hit === el || el.contains(hit)));
  }

  function pickBestCandidate(arr){
    let best = null;
    let bestRect = null;
    let bestScore = -1e18;
    let visibleCount = 0;
    if (!Array.isArray(arr)) return {el:null, rect:null, visibleCount:0};

    const limit = Math.min(arr.length, 80);
    for (let j = 0; j < limit; j++) {
      let e = arr[j];
      if (!e || e.nodeType !== 1) continue;
      try {
        const walked = interactiveAncestor(e);
        if (walked && !isDeceptive(walked)) {
          const wr = rectFor(walked);
          const vw = window.innerWidth || 0;
          const vh = window.innerHeight || 0;
          const huge = wr && vw && vh && (wr.width * wr.height) > vw * vh * 0.6;
          if (!huge) e = walked;
        }
      } catch(e2) {}
      if (isDeceptive(e)) continue;
      const r = rectFor(e);
      const vis = isVisible(e, r);
      if (vis) visibleCount++;
      let s = tagBonus(e);
      if (vis) s += 100;
      else s -= 200;
      // Prefer the node a click would actually land on. Small on purpose:
      // it must not outrank a unique visible match of a better locator.
      if (hitByPoint(e, r)) s += 24;
      if (r) {
        if (isInViewport(r)) s += 40;
        const area = Math.max(0, Math.min(200000, (r.width || 0) * (r.height || 0)));
        s += Math.min(30, area / 5000);
        // Penalize "giant container" hits (e.g. <div> wrapping most of the page).
        const vw = Math.max(0, window.innerWidth || 0);
        const vh = Math.max(0, window.innerHeight || 0);
        const vpa = vw * vh;
        const ra = (r.width || 0) * (r.height || 0);
        if (vpa > 0 && ra > vpa * 0.60) s -= 120;
        if (vw > 0 && vh > 0 && (r.width || 0) > vw * 0.95 && (r.height || 0) > vh * 0.95) s -= 200;
      }
      s -= j * 0.25;
      if (s > bestScore) {
        bestScore = s;
        best = e;
        bestRect = r;
      }
    }

    return {el: best, rect: bestRect, visibleCount: visibleCount};
  }

  let bestEl = null;
  let bestMeta = null;
  let bestScoreVal = -1e9;

  if (Array.isArray(locators)) {
    for (let i = 0; i < locators.length; i++) {
      const loc = locators[i];
      if (!loc || !loc.type) continue;

      let arr = null;
      if (loc.type === 'VISUAL_COORDS') {
        const xy = parseVisualCoords(loc.value);
        if (!xy) continue;
        try {
          const el = document.elementFromPoint(xy.x, xy.y);
          if (el && el.nodeType === 1)
            arr = [el];
        } catch (e) {}
        if (!arr || !arr.length) continue;
      } else {
        arr = matchesFor(loc);
      }
      if (scope === 'interactive') {
        const mapped = [];
        const seen = new WeakSet();
        for (let j = 0; j < arr.length; j++) {
          let e = arr[j];
          try { e = interactiveAncestor(e); } catch (e2) {}
          if (e && isInteractive(e) && !seen.has(e)) { seen.add(e); mapped.push(e); }
        }
        arr = mapped;
      }
      if (!arr || !arr.length) continue;

      const mc = arr.length;
      const picked = pickBestCandidate(arr);
      let el = picked.el;
      let r0 = picked.rect;
      const visibleCount = picked.visibleCount;
      if (!el) continue;

      let score = baseScore(loc.type) + tagBonus(el);
      if (isVisible(el, r0)) score += 60;
      else score -= 120;
      if (r0 && isInViewport(r0)) score += 30;

      const uniq_basis = (visibleCount > 0) ? visibleCount : mc;
      if (uniq_basis === 1) score += 140;
      else score -= Math.min(180, (uniq_basis - 1) * 18);

      // Penalize clearly broad/ambiguous locators when other options exist.
      if (String(loc.type) === 'TAG_NAME') score -= 60;
      if (String(loc.type) === 'TEXT_CONTENT') score -= 20;
      if (String(loc.type) === 'PARTIAL_LINK_TEXT') score -= 10;
      // Stronger penalty for a TAG_NAME that still matches many visible nodes.
      if (String(loc.type) === 'TAG_NAME' && uniq_basis > 10) score -= 180;

      // Penalize giant container hits again at the per-locator stage (after
      // scrollIntoView can change the rect).
      if (r0) {
        const vw = Math.max(0, window.innerWidth || 0);
        const vh = Math.max(0, window.innerHeight || 0);
        const vpa = vw * vh;
        const ra = (r0.width || 0) * (r0.height || 0);
        if (vpa > 0 && ra > vpa * 0.60) score -= 120;
        if (vw > 0 && vh > 0 && (r0.width || 0) > vw * 0.95 && (r0.height || 0) > vh * 0.95) score -= 200;
      }

      if (score > bestScoreVal) {
        bestScoreVal = score;
        bestEl = el;
        bestMeta = {type:String(loc.type||''), value:String(loc.value||''), matchCount:mc, visibleCount:visibleCount, score:score, index:i};
      }
    }
  }

  if (!bestEl) return {ok:false};
  try {
    const tag = String(bestEl.tagName||'').toLowerCase();
    if (tag === 'body' || tag === 'html') return {ok:false, reason:'root'};
  } catch (e) {}
  const r = bestEl.getBoundingClientRect();
  return {ok:true,
          cx:Math.round((r.left+r.right)/2),
          cy:Math.round((r.top+r.bottom)/2),
          left:Math.round(r.left), top:Math.round(r.top), width:Math.round(r.width), height:Math.round(r.height),
          chosen: bestMeta || null};
})
)JS";

}  // namespace

namespace dashboard {

ResolvedElement::ResolvedElement() = default;
ResolvedElement::~ResolvedElement() = default;
ResolvedElement::ResolvedElement(const ResolvedElement&) = default;
ResolvedElement& ResolvedElement::operator=(const ResolvedElement&) = default;
ResolvedElement::ResolvedElement(ResolvedElement&&) noexcept = default;
ResolvedElement& ResolvedElement::operator=(ResolvedElement&&) noexcept =
    default;

LocatorResolver::LocatorResolver(content::WebContents* web_contents)
    : web_contents_(web_contents) {}
LocatorResolver::~LocatorResolver() = default;

namespace {

constexpr size_t kFrameBatchSize = 12;
constexpr size_t kMaxFramesToQueryWithHint = 32;
constexpr size_t kMaxFramesToQueryNoHint = 64;

size_t FrameDepth(content::RenderFrameHost* rfh) {
  size_t d = 0;
  for (content::RenderFrameHost* p = rfh ? rfh->GetParent() : nullptr; p;
       p = p->GetParent()) {
    ++d;
    if (d > 50)
      break;
  }
  return d;
}

std::optional<std::pair<int, int>> ParseGlobalIdLike(const std::string& s) {
  // Accept "child_id,frame_routing_id".
  size_t comma = s.find(',');
  if (comma == std::string::npos)
    return std::nullopt;
  int a = 0;
  int b = 0;
  if (!base::StringToInt(s.substr(0, comma), &a))
    return std::nullopt;
  if (!base::StringToInt(s.substr(comma + 1), &b))
    return std::nullopt;
  return std::make_pair(a, b);
}

}  // namespace

// Removed synchronous resolution path to avoid UI thread blocking.

// ============================================================================
// ASYNC IMPLEMENTATION
// ============================================================================

void LocatorResolver::ResolveUniqueAsync(const std::vector<Locator>& locators,
                                         SearchScope scope,
                                         int timeout_ms,
                                         ResolveCallback callback) {
  async_callback_ = std::move(callback);
  async_start_time_ = base::TimeTicks::Now();
  if (timeout_ms < kMinLocatorResolveTimeoutMs)
    timeout_ms = kMinLocatorResolveTimeoutMs;
  if (timeout_ms > kMaxLocatorResolveTimeoutMs)
    timeout_ms = kMaxLocatorResolveTimeoutMs;
  async_timeout_ = base::Milliseconds(timeout_ms);
  async_retry_delay_ = base::Milliseconds(
      std::clamp(timeout_ms / 2, kMinLocatorRetryDelayMs,
                 kMaxLocatorRetryDelayMs));
  async_locators_ = locators;
  async_scope_ = scope;
  cancelled_ = false;
  async_retry_count_ = 0;
  found_candidate_ = false;
  best_candidate_ = ResolvedElement();
  best_rank_ = 0;
  best_frame_index_ = std::numeric_limits<size_t>::max();

  std::ostringstream loc_stream;
  loc_stream << "[LocatorResolver] Resolve start locators=";
  for (size_t i = 0; i < locators.size(); ++i) {
    if (i) loc_stream << ", ";
    loc_stream << LocatorTypeToString(locators[i].type) << "='" << locators[i].value << "'";
  }
  LOG(INFO) << loc_stream.str();

  // Enforce an overall timeout even if some frames never answer.
  if (timeout_ms > 0) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&LocatorResolver::OnHardTimeout,
                       weak_ptr_factory_.GetWeakPtr()),
        base::Milliseconds(timeout_ms));
  }

  TryResolveOnceAsync(locators, scope);
}

void LocatorResolver::Cancel() {
  cancelled_ = true;
  if (async_callback_) {
    std::move(async_callback_).Run(false, ResolvedElement(), "Cancelled");
  }
}

void LocatorResolver::Finish(bool success, const std::string& error) {
  if (cancelled_)
    return;
  cancelled_ = true;
  pending_frame_queries_ = 0;
  if (!async_callback_)
    return;
  if (success) {
    dashboard::ResolvedElement out = best_candidate_;
    std::move(async_callback_).Run(true, out, "");
    return;
  }
  std::string msg = error;
  if (!frame_query_note_.empty()) {
    msg = msg.empty() ? frame_query_note_ : (msg + " (" + frame_query_note_ + ")");
  }
  std::move(async_callback_).Run(false, ResolvedElement(), msg);
}

void LocatorResolver::TryResolveOnceAsync(const std::vector<Locator>& locators,
                                          SearchScope scope) {
  if (cancelled_) {
    if (async_callback_) {
      std::move(async_callback_).Run(false, ResolvedElement(), "Cancelled");
    }
    return;
  }

  if (!web_contents_) {
    if (async_callback_) {
      std::move(async_callback_).Run(false, ResolvedElement(), "No web contents");
    }
    return;
  }

  // Proceed with the full native multi-frame query.
  StartFullQuery(locators, scope);
}

void LocatorResolver::OnQuickIdResult(base::Value v) {
  bool ok = false;
  std::optional<int> cx;
  std::optional<int> cy;
  std::optional<int> left;
  std::optional<int> top;
  std::optional<int> width;
  std::optional<int> height;
  if (v.is_dict()) {
    const base::Value::Dict& d = v.GetDict();
    ok = d.FindBool("ok").value_or(false);
    if (ok) {
      cx = d.FindInt("cx");
      cy = d.FindInt("cy");
      left = d.FindInt("left");
      top = d.FindInt("top");
      width = d.FindInt("width");
      height = d.FindInt("height");
      ok = HasUsableResolvedGeometry(cx, cy, left, top, width, height);
    }
  }
  if (ok && async_callback_) {
    // Stop further work; we'll accept this fast-path result.
    cancelled_ = true;
    pending_frame_queries_ = 0;

    // Log quick-ID result with frame URL if possible (primary frame)
    if (web_contents_) {
      if (auto* rfh = web_contents_->GetPrimaryMainFrame()) {
        GURL u = rfh->GetLastCommittedURL();
        LOG(INFO) << "[LocatorResolver] quick-id result cx=" << cx.value_or(0)
                  << " cy=" << cy.value_or(0) << " in frame=" << u.spec();
        if (left || top || width || height) {
          LOG(INFO) << "[LocatorResolver] element rect L=" << (left ? *left : 0)
                    << " T=" << (top ? *top : 0)
                    << " W=" << (width ? *width : 0)
                    << " H=" << (height ? *height : 0);
        }
      }
    }
    found_candidate_ = true; best_rank_ = 100;
    dashboard::ResolvedElement out;
    out.center_x = cx.value_or(0);
    out.center_y = cy.value_or(0);
    out.left = left.value_or(0);
    out.top = top.value_or(0);
    out.width = width.value_or(0);
    out.height = height.value_or(0);
    if (web_contents_) {
      if (auto* rfh = web_contents_->GetPrimaryMainFrame()) out.frame_id = rfh->GetGlobalId();
    }
    auto cb = std::move(async_callback_);
    std::move(cb).Run(true, out, "");
  }
}

void LocatorResolver::StartFullQuery(const std::vector<Locator>& locators,
                                     SearchScope scope) {
  // Prepare JSON list for JS
  base::Value::List locators_list;
  for (const auto& loc : locators)
    locators_list.Append(LocatorToValue(loc));

  locators_json_.clear();
  base::JSONWriter::Write(locators_list, &locators_json_);

  // Collect all frames to query
  frames_to_query_.clear();
  content::RenderFrameHost* primary = web_contents_->GetPrimaryMainFrame();
  content::Page* primary_page = nullptr;
  if (primary && primary->IsActive()) {
    primary_page = &primary->GetPage();
  }
  if (primary)
    frames_to_query_.push_back(primary);

  web_contents_->ForEachRenderFrameHost(
      [this, primary, primary_page](content::RenderFrameHost* rfh){
        if (!rfh) return;
        if (primary && rfh == primary) return;
        // Only query frames that are part of the currently active page. This
        // avoids selecting stale RenderFrameHosts in BFCache / pending deletion.
        if (!rfh->IsActive())
          return;
        if (primary_page && &rfh->GetPage() != primary_page)
          return;
        frames_to_query_.push_back(rfh);
      });

  if (frames_to_query_.empty()) {
    if (async_callback_) {
      std::move(async_callback_).Run(false, ResolvedElement(), "No frames available");
    }
    return;
  }

  frame_query_note_.clear();
  next_frame_to_query_ = 0;
  pending_frame_queries_ = 0;
  found_candidate_ = false;
  best_candidate_ = ResolvedElement();
  best_rank_ = 0;

  // If a frame hint exists, restrict frame traversal aggressively.
  std::vector<std::string> frame_hints;
  for (const auto& loc : locators) {
    if (loc.type == LocatorType::FRAME_ID && !loc.value.empty())
      frame_hints.push_back(loc.value);
  }

  if (!frame_hints.empty()) {
    std::vector<content::RenderFrameHost*> preferred;
    preferred.reserve(frames_to_query_.size());
    for (content::RenderFrameHost* rfh : frames_to_query_) {
      if (!rfh)
        continue;
      const std::string token = rfh->GetFrameToken().ToString();
      const content::GlobalRenderFrameHostId gid = rfh->GetGlobalId();
      for (const std::string& hint : frame_hints) {
        if (hint == token) {
          preferred.push_back(rfh);
          break;
        }
        if (auto parsed = ParseGlobalIdLike(hint)) {
          if (parsed->first == gid.child_id && parsed->second == gid.frame_routing_id) {
            preferred.push_back(rfh);
            break;
          }
        }
      }
    }
    if (!preferred.empty()) {
      // Keep primary frame first if it matches; otherwise keep it as a fallback.
      std::vector<content::RenderFrameHost*> filtered;
      filtered.reserve(preferred.size() + 1);
      if (primary) {
        const bool primary_in_preferred =
            std::find(preferred.begin(), preferred.end(), primary) != preferred.end();
        if (primary_in_preferred)
          filtered.push_back(primary);
        for (content::RenderFrameHost* rfh : preferred) {
          if (rfh && rfh != primary)
            filtered.push_back(rfh);
        }
        if (!primary_in_preferred)
          filtered.push_back(primary);
      } else {
        filtered = std::move(preferred);
      }
      if (filtered.size() > kMaxFramesToQueryWithHint) {
        filtered.resize(kMaxFramesToQueryWithHint);
        frame_query_note_ = "frame-hinted query truncated";
      }
      frames_to_query_ = std::move(filtered);
    } else {
      frame_query_note_ = "frame hint did not match any live frame";
      // Fall through to heuristic truncation.
    }
  }

  // Tarpit defense: cap the total number of frames we will attempt when there
  // is no reliable frame hint. Prefer shallow frames.
  if (frames_to_query_.size() > kMaxFramesToQueryNoHint && frame_hints.empty()) {
    std::stable_sort(frames_to_query_.begin(), frames_to_query_.end(),
                     [](content::RenderFrameHost* a, content::RenderFrameHost* b) {
                       if (!a || !b)
                         return !!a;
                       return FrameDepth(a) < FrameDepth(b);
                     });
    frames_to_query_.resize(kMaxFramesToQueryNoHint);
    frame_query_note_ = "frame traversal capped";
  }

  // Debug: log the frames being queried (first few for brevity)
  size_t log_count = std::min<size_t>(frames_to_query_.size(), 5);
  for (size_t li = 0; li < log_count; ++li) {
    content::RenderFrameHost* fr = frames_to_query_[li];
    if (!fr) continue;
    GURL u = fr->GetLastCommittedURL();
    LOG(INFO) << "[LocatorResolver] Query frame[" << li << "] url=" << u.spec();
  }

  DispatchNextFrameBatch();
}

void LocatorResolver::DispatchNextFrameBatch() {
  if (cancelled_)
    return;

  size_t started = 0;
  while (next_frame_to_query_ < frames_to_query_.size() &&
         started < kFrameBatchSize) {
    const size_t i = next_frame_to_query_++;
    content::RenderFrameHost* rfh = frames_to_query_[i];
    if (!rfh)
      continue;

    ++pending_frame_queries_;
    started++;

    auto stealth = std::make_unique<
        mojo::AssociatedRemote<dashboard::mojom::StealthyElementResolver>>();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(stealth.get());
    if (stealth->is_bound()) {
      (*stealth)->ResolveElementStealthily(
          ToMojomLocators(async_locators_), ToMojomSearchScope(async_scope_),
          base::BindOnce(
              [](base::WeakPtr<LocatorResolver> weak, size_t frame_index,
                 std::unique_ptr<mojo::AssociatedRemote<
                     dashboard::mojom::StealthyElementResolver>> /*keep_alive*/,
                 dashboard::mojom::ResolvedElementInfoPtr info) {
                if (!weak) {
                  return;
                }
                weak->OnFrameResolveComplete(frame_index, std::move(info));
              },
              weak_ptr_factory_.GetWeakPtr(), i, std::move(stealth)));
      continue;
    }

    auto resolver = std::make_unique<
        mojo::AssociatedRemote<dashboard::mojom::ElementResolver>>();
    rfh->GetRemoteAssociatedInterfaces()->GetInterface(resolver.get());
    if (resolver->is_bound()) {
      (*resolver)->ResolveElementStealthily(
          ToMojomLocators(async_locators_), ToMojomSearchScope(async_scope_),
          base::BindOnce(
              [](base::WeakPtr<LocatorResolver> weak, size_t frame_index,
                 std::unique_ptr<mojo::AssociatedRemote<
                     dashboard::mojom::ElementResolver>> /*keep_alive*/,
                 dashboard::mojom::ResolvedElementInfoPtr info) {
                if (!weak) {
                  return;
                }
                weak->OnFrameResolveComplete(frame_index, std::move(info));
              },
              weak_ptr_factory_.GetWeakPtr(), i, std::move(resolver)));
      continue;
    }

    --pending_frame_queries_;
    frame_query_note_ = "renderer resolver unavailable";
  }
}

void LocatorResolver::OnFrameResolveComplete(
    size_t frame_index,
    dashboard::mojom::ResolvedElementInfoPtr info) {
  if (cancelled_) {
    return;
  }

  if (frame_index < frames_to_query_.size()) {
    content::RenderFrameHost* frame = frames_to_query_[frame_index];
    if (frame && frame->IsRenderFrameLive() && info && info->ok) {
      std::optional<int> cx = info->center_x;
      std::optional<int> cy = info->center_y;
      std::optional<int> left = info->x;
      std::optional<int> top = info->y;
      std::optional<int> width = info->width;
      std::optional<int> height = info->height;
      const bool has_identity = HasResolvableElementIdentity(
          info->focusable, info->clickable, info->id_attr, info->name_attr);
      if (AcceptResolvedCandidateGeometry(cx, cy, left, top, width, height,
                                          has_identity)) {
        if (!HasUsableResolvedGeometry(cx, cy, left, top, width, height)) {
          LOG(INFO) << "[LocatorResolver] accepting weak-geometry candidate"
                    << " id=" << info->id_attr << " name=" << info->name_attr
                    << " frame=" << frame->GetLastCommittedURL();
        }
        int rank = 0;
        if (info->visible)
          rank += 100;
        if (info->clickable)
          rank += 60;
        if (info->focusable)
          rank += 40;
        if (web_contents_ && frame == web_contents_->GetPrimaryMainFrame()) {
          rank += 50;
        }
        rank -= static_cast<int>(FrameDepth(frame)) * 5;

        if (!found_candidate_ || rank > best_rank_) {
          found_candidate_ = true;
          best_rank_ = rank;
          best_frame_index_ = frame_index;
          best_candidate_.center_x = cx.value_or(0);
          best_candidate_.center_y = cy.value_or(0);
          best_candidate_.left = left.value_or(0);
          best_candidate_.top = top.value_or(0);
          best_candidate_.width = width.value_or(0);
          best_candidate_.height = height.value_or(0);
          best_candidate_.frame_id = frame->GetGlobalId();
          best_candidate_.text = info->text;
          best_candidate_.accessibility_name = info->accessibility_name;
          best_candidate_.role = info->role;
          best_candidate_.id_attr = info->id_attr;
          best_candidate_.class_attr = info->class_attr;
          best_candidate_.name_attr = info->name_attr;
          best_candidate_.css_selector = info->css;
          best_candidate_.xpath = info->xpath;
          best_candidate_.shadow_chain_selector = info->shadow_chain_selector;
          best_candidate_.resource_path = info->resource_path;
          best_candidate_.inner_html = info->inner_html;
          best_candidate_.href = info->href;
          best_candidate_.src = info->src;
          best_candidate_.value = info->value;
          best_candidate_.visible = info->visible;
          best_candidate_.clickable = info->clickable;
          best_candidate_.focusable = info->focusable;
          best_candidate_.locators.clear();
          best_candidate_.locators.reserve(info->locators.size());
          for (const auto& locator : info->locators) {
            Locator promoted_locator;
            promoted_locator.type = StringToLocatorType(locator->type);
            promoted_locator.value = locator->value;
            best_candidate_.locators.push_back(std::move(promoted_locator));
          }
          LOG(INFO) << "[LocatorResolver] native candidate rank=" << rank
                    << " cx=" << cx.value_or(0) << " cy=" << cy.value_or(0)
                    << " rect=(" << left.value_or(0) << ","
                    << top.value_or(0) << "," << width.value_or(0) << "x"
                    << height.value_or(0) << ") frame="
                    << frame->GetLastCommittedURL();
        }
      }
    }
  }

  --pending_frame_queries_;
  CheckTimeout();
}

void LocatorResolver::OnFrameSelectComplete(size_t frame_index,
                                            const std::vector<Locator>& /*locators*/,
                                            SearchScope /*scope*/,
                                            base::Value result) {
  if (cancelled_) {
    return;
  }

  if (frame_index < frames_to_query_.size()) {
    content::RenderFrameHost* frame = frames_to_query_[frame_index];
    if (frame && frame->IsRenderFrameLive() && result.is_dict()) {
      const base::Value::Dict& d = result.GetDict();
      bool ok = d.FindBool("ok").value_or(false);
      if (ok) {
        std::optional<int> cx = d.FindInt("cx");
        std::optional<int> cy = d.FindInt("cy");
        std::optional<int> left = d.FindInt("left");
        std::optional<int> top = d.FindInt("top");
        std::optional<int> width = d.FindInt("width");
        std::optional<int> height = d.FindInt("height");
        int chosen_score = 0;
        std::string chosen_type;
        std::string chosen_value;
        int chosen_index = -1;
        int chosen_match_count = -1;
        int chosen_visible_count = -1;
        if (const base::Value::Dict* chosen = d.FindDict("chosen")) {
          chosen_score = chosen->FindInt("score").value_or(0);
          if (const std::string* t = chosen->FindString("type"))
            chosen_type = *t;
          if (const std::string* v = chosen->FindString("value"))
            chosen_value = *v;
          chosen_index = chosen->FindInt("index").value_or(-1);
          chosen_match_count = chosen->FindInt("matchCount").value_or(-1);
          chosen_visible_count = chosen->FindInt("visibleCount").value_or(-1);
        }
        // Log select result for diagnosis
        GURL u = frame->GetLastCommittedURL();
        LOG(INFO) << "[LocatorResolver] select result cx=" << (cx ? *cx : 0)
                  << " cy=" << (cy ? *cy : 0)
                  << " chosenScore=" << chosen_score
                  << (chosen_type.empty()
                          ? ""
                          : (" chosenType=" + chosen_type +
                             " chosenIndex=" + std::to_string(chosen_index) +
                             " matches=" + std::to_string(chosen_match_count) +
                             " visible=" + std::to_string(chosen_visible_count)))
                  << " in frame=" << u.spec();
        if (!chosen_value.empty()) {
          std::string v = chosen_value;
          if (v.size() > 120)
            v.resize(120);
          LOG(INFO) << "[LocatorResolver] chosenValue='" << v << "'";
        }
        if (left || top || width || height) {
          LOG(INFO) << "[LocatorResolver] element rect L=" << (left ? *left : 0)
                    << " T=" << (top ? *top : 0)
                    << " W=" << (width ? *width : 0)
                    << " H=" << (height ? *height : 0);
        }
        if (HasUsableResolvedGeometry(cx, cy, left, top, width, height)) {
          // Prefer higher-scoring matches and shallower/primary frames. This
          // reduces brittleness on pages where broad locators match hidden
          // duplicates in earlier iframes.
          int rank = chosen_score;
          if (web_contents_ && frame == web_contents_->GetPrimaryMainFrame()) {
            rank += 50;
          }
          rank -= static_cast<int>(FrameDepth(frame)) * 5;

          if (!found_candidate_ || rank > best_rank_) {
            found_candidate_ = true;
            best_rank_ = rank;
            best_frame_index_ = frame_index;
            best_candidate_.center_x = *cx;
            best_candidate_.center_y = *cy;
            best_candidate_.left = left.value_or(0);
            best_candidate_.top = top.value_or(0);
            best_candidate_.width = width.value_or(0);
            best_candidate_.height = height.value_or(0);
            best_candidate_.frame_id = frame->GetGlobalId();
            LOG(INFO) << "[LocatorResolver] accept candidate rank=" << rank
                      << " cx=" << *cx << " cy=" << *cy
                      << " rect=(" << (left.value_or(0)) << ","
                      << (top.value_or(0)) << ","
                      << (width.value_or(0)) << "x"
                      << (height.value_or(0)) << ") in frame="
                      << frame->GetLastCommittedURL();

            // If we found a strong match in the primary frame, return quickly.
            if (web_contents_ && frame == web_contents_->GetPrimaryMainFrame() &&
                rank >= 900) {
              Finish(/*success=*/true, /*error=*/std::string());
              return;
            }
          }
        }
      }
    }
  }

  --pending_frame_queries_;
  CheckTimeout();
}

void LocatorResolver::CheckTimeout() {
  if (cancelled_) {
    return;
  }

  if (pending_frame_queries_ != 0)
    return;

  // If we've already found a candidate and there are no more outstanding
  // queries, finish once we've exhausted our planned frame set.
  if (found_candidate_ && next_frame_to_query_ >= frames_to_query_.size()) {
    Finish(/*success=*/true, /*error=*/std::string());
    return;
  }

  // If we have more frames to query, continue in batches unless we've timed out.
  if (next_frame_to_query_ < frames_to_query_.size()) {
    base::TimeDelta elapsed = base::TimeTicks::Now() - async_start_time_;
    if (!async_timeout_.is_zero() && elapsed >= async_timeout_) {
      if (found_candidate_) {
        Finish(/*success=*/true, /*error=*/std::string());
      } else {
        Finish(/*success=*/false, "No matching element found within timeout");
      }
      return;
    }
    DispatchNextFrameBatch();
    return;
  }

  // Exhausted.
  base::TimeDelta elapsed = base::TimeTicks::Now() - async_start_time_;
  if (found_candidate_) {
    Finish(/*success=*/true, /*error=*/std::string());
    return;
  }
  if (!async_timeout_.is_zero() && elapsed < async_timeout_ &&
      async_retry_count_ < kMaxLocatorResolveRetries) {
    ++async_retry_count_;
    const base::TimeDelta remaining = async_timeout_ - elapsed;
    const base::TimeDelta retry_delay = std::min(async_retry_delay_, remaining);
    LOG(INFO) << "[LocatorResolver] Scheduling retry "
              << async_retry_count_ << "/" << kMaxLocatorResolveRetries
              << " in " << retry_delay.InMilliseconds() << " ms";
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&LocatorResolver::TryResolveOnceAsync,
                       weak_ptr_factory_.GetWeakPtr(), async_locators_,
                       async_scope_),
        retry_delay);
    return;
  }

  Finish(/*success=*/false,
         async_timeout_.is_zero() ? "No matching element found"
                                 : "No matching element found within timeout");
}

void LocatorResolver::OnHardTimeout() {
  if (cancelled_)
    return;
  if (!async_callback_)
    return;
  LOG(WARNING) << "[LocatorResolver] Resolve hard-timeout after "
               << (base::TimeTicks::Now() - async_start_time_).InMilliseconds()
               << " ms";
  Finish(/*success=*/false, "No matching element found within timeout");
}

}  // namespace dashboard

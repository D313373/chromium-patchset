#include "chrome/browser/ui/webui/dashboard/actions/listen_action.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_metadata_promotion.h"
#include "chrome/browser/ui/webui/dashboard/actions/native_page_map_matcher.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/execution_run_context.h"
#include "chrome/browser/ui/webui/dashboard/listener_watch_controller.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "content/public/browser/web_contents.h"
#include "third_party/re2/src/re2/re2.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace {

std::string TruncateForLog(std::string input, size_t max_len) {
  if (input.size() <= max_len)
    return input;
  return input.substr(0, max_len) + "...";
}

std::string ValueToCompactLog(const base::Value& value, size_t max_len = 2000) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  return TruncateForLog(std::move(json), max_len);
}

std::optional<content::GlobalRenderFrameHostId> ParseGlobalIdLike(
    const std::string& s) {
  // Accept "child_id,frame_routing_id".
  const size_t comma = s.find(',');
  if (comma == std::string::npos)
    return std::nullopt;
  int a = 0;
  int b = 0;
  if (!base::StringToInt(s.substr(0, comma), &a))
    return std::nullopt;
  if (!base::StringToInt(s.substr(comma + 1), &b))
    return std::nullopt;
  return content::GlobalRenderFrameHostId(a, b);
}

std::string NormalizeMatchText(std::string text) {
  return base::CollapseWhitespaceASCII(std::move(text), true);
}

bool ConditionSupportsLocatorFallback(const std::string& condition) {
  const std::string lowered = base::ToLowerASCII(condition);
  return lowered == "visible" || lowered == "hidden" ||
         lowered == "becomes_visible" || lowered == "becomes_hidden" ||
         lowered == "element_exists" || lowered == "exists";
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
  return base::ToLowerASCII(haystack).find(base::ToLowerASCII(needle)) !=
         std::string::npos;
}

std::string AttributeValueForCandidate(
    const dashboard::NativePageMapCandidate& candidate,
    const std::string& attribute) {
  const std::string attr = base::ToLowerASCII(attribute);
  if (attr == "class" || attr == "classname") {
    return candidate.class_attr;
  }
  if (attr == "id") {
    return candidate.id_attr;
  }
  if (attr == "name") {
    return candidate.name_attr;
  }
  if (attr == "role") {
    return candidate.role;
  }
  if (attr == "aria-label" || attr == "accessibility_name" ||
      attr == "accessibilityname") {
    return candidate.accessibility_name;
  }
  if (attr == "text") {
    return candidate.text;
  }
  if (attr == "tag") {
    return candidate.tag;
  }
  if (attr == "frame" || attr == "frame_id" || attr == "frameid") {
    return candidate.frame_id;
  }
  if (attr == "css" || attr == "css_selector" || attr == "cssselector") {
    return candidate.css_selector;
  }
  if (attr == "xpath") {
    return candidate.xpath;
  }
  if (attr == "resource_path" || attr == "resourcepath") {
    return candidate.resource_path;
  }
  return std::string();
}

bool CandidateSupportsAttribute(const std::string& attribute) {
  const std::string attr = base::ToLowerASCII(attribute);
  return attr.empty() || attr == "class" || attr == "classname" ||
         attr == "id" || attr == "name" || attr == "role" ||
         attr == "aria-label" || attr == "accessibility_name" ||
         attr == "accessibilityname" || attr == "text" || attr == "tag" ||
         attr == "frame" || attr == "frame_id" || attr == "frameid" ||
         attr == "css" || attr == "css_selector" || attr == "cssselector" ||
         attr == "xpath" || attr == "resource_path" ||
         attr == "resourcepath";
}

int ImmediateChildCountForCandidate(const base::Value::Dict& page_map,
                                    const dashboard::NativePageMapCandidate& target) {
  if (target.resource_path.empty()) {
    return 0;
  }
  const std::string prefix = target.resource_path + "/";
  const size_t target_depth =
      static_cast<size_t>(std::count(target.resource_path.begin(),
                                     target.resource_path.end(), '/'));
  int count = 0;
  std::vector<dashboard::NativePageMapCandidate> all =
      dashboard::ExtractPageMapCandidates(page_map);
  for (const auto& candidate : all) {
    if (candidate.frame_id != target.frame_id || candidate.resource_path.empty()) {
      continue;
    }
    if (!base::StartsWith(candidate.resource_path, prefix,
                          base::CompareCase::SENSITIVE)) {
      continue;
    }
    const size_t candidate_depth =
        static_cast<size_t>(std::count(candidate.resource_path.begin(),
                                       candidate.resource_path.end(), '/'));
    if (candidate_depth == target_depth + 1) {
      ++count;
    }
  }
  return count;
}

std::string RegexMatchValue(const RE2* re, const std::string& haystack) {
  if (!re || haystack.empty() || !re->ok()) {
    return std::string();
  }
  re2::StringPiece input(haystack);
  re2::StringPiece groups[2];
  const int group_count = re->NumberOfCapturingGroups() > 0 ? 2 : 1;
  if (!re->Match(input, 0, input.size(), RE2::UNANCHORED, groups,
                 group_count)) {
    return std::string();
  }
  const re2::StringPiece& match = group_count > 1 ? groups[1] : groups[0];
  return std::string(match.data(), match.size());
}

std::string ValidateRegexPattern(const std::string& pattern) {
  RE2 re(pattern);
  if (re.ok()) {
    return std::string();
  }
  return re.error();
}

// Returns a Promise that resolves with a dict (also mirrored into a DOM
// attribute so the browser can poll for completion):
//  { ok: bool, satisfied: bool, reason?: string, exists?: bool, visible?: bool,
//    childCount?: int, text?: string, attr?: string }
[[maybe_unused]] const char kListenWaitJs[] = R"JS(
(function(token, locators, condition, needle, attribute, textUseRegex, attributeUseRegex, timeoutMs, axRoleHint, axNameHint, interactiveAncestorRoleHint){
  const ATTR_TOKEN = 'data-simulachrome-listen-token';
  const ATTR_STATE = 'data-simulachrome-listen-state';
  const ATTR_NODE_ID = 'data-simulachrome-listen-node-id';
  const ATTR_NODE_NAME = 'data-simulachrome-listen-node-name';
  const ATTR_AX_ROLE = 'data-simulachrome-listen-ax-role';
  const ATTR_AX_NAME = 'data-simulachrome-listen-ax-name';
  const BAD_TAGS = new Set(['HTML', 'BODY']);
  let lastPick = {type:'', index:-1, tag:'', value:''};
  const condLower = String(condition || '').toLowerCase();
  const wantText = String(needle || '').replace(/\s+/g, ' ').trim();
  const wantTextFold = wantText.toLowerCase();
  const wantAxRole = String(axRoleHint || '').replace(/\s+/g, ' ').trim();
  const wantAxRoleFold = wantAxRole.toLowerCase();
  const wantAxName = String(axNameHint || '').replace(/\s+/g, ' ').trim();
  const wantAxNameFold = wantAxName.toLowerCase();
  const wantInteractiveRole = String(interactiveAncestorRoleHint || '')
      .replace(/\s+/g, ' ').trim().toLowerCase();
  const useTextRegex = !!textUseRegex;
  const useAttrRegex = !!attributeUseRegex;
  let textRegex = null;
  let attrRegex = null;
  if (useTextRegex && wantText) {
    try { textRegex = new RegExp(wantText, 'i'); } catch (e) { textRegex = null; }
  }
  if (useAttrRegex && wantText) {
    try { attrRegex = new RegExp(wantText, 'i'); } catch (e) { attrRegex = null; }
  }
  function deriveListenerMetadata(){
    const meta = {
      nodeId:'',
      nodeName:'',
      axRole: wantAxRole || '',
      axName: wantAxName || '',
    };
    try {
      const items = Array.isArray(locators) ? locators : [];
      for (const loc of items) {
        const type = String(loc && loc.type || '').toUpperCase();
        const value = String(loc && loc.value || '').trim();
        if (!value) continue;
        if (!meta.nodeId && type === 'ID') meta.nodeId = value;
        if (!meta.nodeName && type === 'NAME') meta.nodeName = value;
        if (meta.nodeId && meta.nodeName) break;
      }
    } catch (e) {}
    return meta;
  }
  const listenerMetadata = deriveListenerMetadata();
  try {
    if (document && document.documentElement) {
      document.documentElement.setAttribute(ATTR_TOKEN, String(token || ''));
      document.documentElement.setAttribute(ATTR_NODE_ID, listenerMetadata.nodeId || '');
      document.documentElement.setAttribute(ATTR_NODE_NAME, listenerMetadata.nodeName || '');
      document.documentElement.setAttribute(ATTR_AX_ROLE, listenerMetadata.axRole || '');
      document.documentElement.setAttribute(ATTR_AX_NAME, listenerMetadata.axName || '');
      document.documentElement.setAttribute(
        ATTR_STATE,
        JSON.stringify({
          ok:true,
          satisfied:false,
          reason:'armed',
          token:String(token||''),
          pageUrl:String(location && location.href || ''),
          readyState:String(document && document.readyState || '')
        }));
    }
  } catch (e) {}
  function byXPath(xpath){ try{ const snap=document.evaluate(xpath, document, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null); const arr=[]; for(let i=0;i<snap.snapshotLength;i++){arr.push(snap.snapshotItem(i));} return arr; }catch(e){ return []; } }
  function safeQueryAll(sel){
    try { return Array.from(document.querySelectorAll(String(sel||''))); } catch(e) { return []; }
  }
  function parseCoords(v){
    try {
      if (v && typeof v === 'object') {
        const x = Number(v.x);
        const y = Number(v.y);
        if (Number.isFinite(x) && Number.isFinite(y)) return {x, y};
      }
      const s = String(v || '').trim();
      if (!s) return null;
      if (s[0] === '{') {
        const obj = JSON.parse(s);
        const x = Number(obj && obj.x);
        const y = Number(obj && obj.y);
        if (Number.isFinite(x) && Number.isFinite(y)) return {x, y};
      }
      const comma = s.indexOf(',');
      if (comma !== -1) {
        const x = Number(s.slice(0, comma).trim());
        const y = Number(s.slice(comma + 1).trim());
        if (Number.isFinite(x) && Number.isFinite(y)) return {x, y};
      }
    } catch(e) {}
    return null;
  }
  function setLastPick(loc, idx, el){
    try {
      lastPick = {
        type: String(loc && loc.type || ''),
        index: Number(idx),
        tag: String(el && el.tagName || ''),
        value: String(loc && loc.value || ''),
      };
    } catch(e) {
      lastPick = {type:'', index:-1, tag:'', value:''};
    }
  }
  function matchesFor(loc){
    switch(String(loc.type||'')){
      case 'ID': return [document.getElementById(loc.value)].filter(Boolean);
      case 'CSS_SELECTOR': return safeQueryAll(loc.value);
      case 'NAME': {
        try { return Array.from(document.getElementsByName(String(loc.value||''))); } catch(e) { return []; }
      }
      case 'CLASS_NAME': return Array.from(document.getElementsByClassName(String(loc.value||'')));
      case 'TAG_NAME': return Array.from(document.getElementsByTagName(String(loc.value||'')));
      case 'XPATH': return byXPath(String(loc.value||''));
      case 'VISUAL_COORDS': {
        const c = parseCoords(loc.value);
        if (!c) return [];
        try {
          const el = document.elementFromPoint(c.x, c.y);
          return el ? [el] : [];
        } catch (e) { return []; }
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
      case 'LINK_TEXT': {
        const out=[]; const a=document.getElementsByTagName('a');
        for(let i=0;i<a.length;i++){ const t=((a[i].innerText||a[i].textContent)||'').trim(); if(t===String(loc.value||'')) out.push(a[i]); }
        return out;
      }
      case 'PARTIAL_LINK_TEXT': {
        const out=[]; const a=document.getElementsByTagName('a');
        for(let i=0;i<a.length;i++){ const t=((a[i].innerText||a[i].textContent)||'').trim(); if(t.includes(String(loc.value||''))) out.push(a[i]); }
        return out;
      }
      case 'TEXT_CONTENT': {
        const out=[]; const all=document.getElementsByTagName('*');
        for(let i=0;i<all.length;i++){ const el=all[i]; const t=((el.innerText||el.textContent)||'').trim(); if(t===String(loc.value||'')) out.push(el); }
        return out;
      }
      case 'ACCESSIBILITY_NAME': {
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
            if (!name && el.tagName) {
              name = norm(el.innerText || el.textContent || '');
            }
          } catch(e) {}
          if (name && name === want) out.push(el);
        }
        return out;
      }
      case 'JS_EVAL': {
        try { const fn = new Function('return ('+String(loc.value)+')'); const res = fn(); if (res instanceof Element) return [res]; if (Array.isArray(res)) return res.filter(e=>e && e.nodeType===1); } catch (e) {}
        return [];
      }
      default: return [];
    }
  }
  function locatorDiagnostics(){
    if(!Array.isArray(locators)) return [];
    const out = [];
    for(let i=0;i<locators.length;i++){
      const loc = locators[i] || {};
      const type = String(loc.type || '');
      const value = String(loc.value || '');
      let count = 0;
      let error = '';
      try {
        const m = matchesFor(loc);
        count = Array.isArray(m) ? m.length : 0;
      } catch (e) {
        error = String(e || '');
      }
      out.push({
        type,
        value: value.slice(0, 200),
        count,
        error: error.slice(0, 160),
      });
    }
    return out;
  }
  function candidateDiagnostics(limit){
    const out = [];
    const seen = new Set();
    const maxItems = Math.max(1, Number(limit || 5));
    if(!Array.isArray(locators)) return out;
    for(let i=0;i<locators.length;i++){
      const loc = locators[i];
      const matches = matchesFor(loc) || [];
      for(let j=0;j<matches.length;j++){
        const el = matches[j];
        if(!el || el.nodeType !== 1) continue;
        const key = matchKeyFor(el);
        if (key && seen.has(key)) continue;
        if (key) seen.add(key);
        const role = semanticRoleFor(el);
        const name = accessibleNameFor(el);
        const textValue = textFor(el);
        const score = candidateScore(el, loc, i);
        out.push({
          locatorType: String(loc && loc.type || ''),
          locatorIndex: Number(i),
          tag: String(el.tagName || '').toLowerCase(),
          visible: !!isVisible(el, rectFor(el)),
          stateOk: !!satisfiesStateNow(el),
          role: String(role || ''),
          axName: String(name || '').slice(0, 120),
          text: String(textValue || '').slice(0, 120),
          score: Number(score),
        });
        if (out.length >= maxItems) {
          out.sort((a, b) => Number(b.score || 0) - Number(a.score || 0));
          return out.slice(0, maxItems);
        }
      }
    }
    out.sort((a, b) => Number(b.score || 0) - Number(a.score || 0));
    return out.slice(0, maxItems);
  }
  function norm(s){
    return String(s||'').replace(/\s+/g,' ').trim();
  }
  function textFor(el){
    if (!el) return '';
    try {
      const tag = String(el.tagName || '').toLowerCase();
      if (tag === 'input' || tag === 'textarea') return norm(el.value || '');
    } catch (e) {}
    try { return norm(el.innerText || el.textContent || ''); } catch (e) {}
    return '';
  }
  function attrFor(el){
    if (!el || !attribute) return '';
    try { return String(el.getAttribute(String(attribute)) || ''); } catch (e) {}
    return '';
  }
  function includesFold(haystack, needleRaw, needleFold){
    const h = String(haystack || '');
    if (!h || !needleRaw) return false;
    return h.includes(needleRaw) || h.toLowerCase().includes(needleFold);
  }
  function equalsFold(haystack, needleRaw, needleFold){
    const h = norm(haystack || '');
    if (!h || !needleRaw) return false;
    return h === needleRaw || h.toLowerCase() === needleFold;
  }
  function regexMatchValue(re, haystack){
    if (!re) return '';
    try {
      const m = String(haystack || '').match(re);
      if (!m) return '';
      if (m.length > 1 && m[1]) return String(m[1]);
      return String(m[0] || '');
    } catch (e) {}
    return '';
  }
  function textMatchValue(textValue){
    if (!wantText) return '';
    if (useTextRegex) return regexMatchValue(textRegex, textValue);
    if (includesFold(textValue, wantText, wantTextFold)) return wantText;
    if (equalsFold(textValue, wantText, wantTextFold)) return norm(textValue || '');
    return '';
  }
  function attrMatchValue(attrValue){
    if (!wantText) return '';
    if (useAttrRegex) return regexMatchValue(attrRegex, attrValue);
    if (includesFold(attrValue, wantText, wantTextFold)) return wantText;
    if (equalsFold(attrValue, wantText, wantTextFold)) return norm(attrValue || '');
    return '';
  }
  function accessibleNameFor(el){
    if (!el) return '';
    try {
      const aria = el.getAttribute && el.getAttribute('aria-label');
      if (aria) return norm(aria);
      const labelledBy = el.getAttribute && el.getAttribute('aria-labelledby');
      if (labelledBy) {
        const ids = String(labelledBy).split(/\s+/).filter(Boolean);
        const parts = [];
        for (const id of ids) {
          const ref = document.getElementById(String(id));
          if (ref) parts.push(norm(ref.innerText || ref.textContent || ''));
        }
        const joined = norm(parts.join(' '));
        if (joined) return joined;
      }
      const title = el.getAttribute && el.getAttribute('title');
      if (title) return norm(title);
      const placeholder = el.getAttribute && el.getAttribute('placeholder');
      if (placeholder) return norm(placeholder);
    } catch (e) {}
    return textFor(el);
  }
  function semanticRoleFor(el){
    if (!el) return '';
    try {
      const explicit = norm(el.getAttribute && el.getAttribute('role'));
      if (explicit) return explicit.toLowerCase();
    } catch (e) {}
    const tag = String(el.tagName || '').toLowerCase();
    if (tag === 'a' && el.getAttribute && el.getAttribute('href')) return 'link';
    if (tag === 'button') return 'button';
    if (tag === 'textarea') return 'textbox';
    if (tag === 'select') return 'combobox';
    if (tag === 'option') return 'option';
    if (tag === 'article') return 'article';
    if (tag === 'img') return 'img';
    if (tag === 'input') {
      const type = String(el.getAttribute && el.getAttribute('type') || 'text')
          .toLowerCase();
      if (type === 'button' || type === 'submit' || type === 'reset')
        return 'button';
      if (type === 'checkbox') return 'checkbox';
      if (type === 'radio') return 'radio';
      return 'textbox';
    }
    if (/^h[1-6]$/.test(tag)) return 'heading';
    if (tag === 'li') return 'listitem';
    if (tag === 'ul' || tag === 'ol') return 'list';
    const text = textFor(el);
    if (text) return 'statictext';
    return '';
  }
  function nearestInteractiveAncestor(el){
    if (!el || !el.closest) return null;
    try {
      return el.closest(
          '[role="button"],[role="link"],button,a[href],input[type="button"],input[type="submit"],input[type="reset"]');
    } catch (e) {}
    return null;
  }
  function roleMatchesHint(el){
    if (!wantAxRoleFold) return true;
    const role = semanticRoleFor(el);
    if (role && (role === wantAxRoleFold || role.includes(wantAxRoleFold))) {
      return true;
    }
    if (wantAxRoleFold === 'statictext') {
      return !!textFor(el);
    }
    return false;
  }
  function axNameMatchesHint(el){
    if (!wantAxName) return true;
    const name = accessibleNameFor(el);
    return includesFold(name, wantAxName, wantAxNameFold) ||
        equalsFold(name, wantAxName, wantAxNameFold);
  }
  function interactiveAncestorMatchesHint(el){
    if (!wantInteractiveRole) return true;
    const ancestor = nearestInteractiveAncestor(el);
    if (!ancestor) return false;
    const role = semanticRoleFor(ancestor);
    return !!role &&
        (role === wantInteractiveRole || role.includes(wantInteractiveRole));
  }
  function candidateScore(el, loc, idx){
    if (!el || el.nodeType !== 1) return -100000;
    let score = 0;
    const textValue = textFor(el);
    const r = rectFor(el);
    if (isVisible(el, r)) score += 40;
    if (!BAD_TAGS.has(String(el.tagName || ''))) score += 10;
    if (satisfiesStateNow(el)) score += 70;
    if (roleMatchesHint(el)) score += 35;
    if (axNameMatchesHint(el)) score += 25;
    if (interactiveAncestorMatchesHint(el)) score += 20;
    if ((condLower === 'text_contains' || condLower === 'contains_text') &&
        wantText) {
      if (textMatchValue(textValue)) {
        score += 50;
        score -= Math.min(
            30, Math.floor(Math.max(0, textValue.length - wantText.length) / 12));
      } else {
        score -= 40;
      }
    }
    if (loc && String(loc.type || '').toUpperCase() === 'ACCESSIBILITY_NAME') {
      score += 8;
    }
    score -= Math.min(10, Number(idx || 0));
    return score;
  }
  function articleMetaFor(el){
    let author = '';
    let timestamp = '';
    let articleText = '';
    try {
      const article = el && el.closest ? el.closest('[role=\"article\"], article') : null;
      if (!article) return {author, timestamp, articleText};
      const authorNode = article.querySelector(
        '[data-testid=\"User-Name\"], [data-author], [itemprop=\"author\"], a[rel=\"author\"], header a[role=\"link\"]');
      if (authorNode) {
        author = norm(
          authorNode.getAttribute && authorNode.getAttribute('data-author') ||
          authorNode.innerText || authorNode.textContent || '');
      }
      const timeNode = article.querySelector('time,[datetime],[data-time]');
      if (timeNode) {
        timestamp = norm(
          timeNode.getAttribute && (
            timeNode.getAttribute('datetime') || timeNode.getAttribute('data-time')) ||
          timeNode.innerText || timeNode.textContent || '');
      }
      articleText = norm(article.innerText || article.textContent || '');
    } catch (e) {}
    if (articleText.length > 240) articleText = articleText.slice(0, 240);
    if (author.length > 120) author = author.slice(0, 120);
    if (timestamp.length > 80) timestamp = timestamp.slice(0, 80);
    return {author, timestamp, articleText};
  }
  function matchKeyFor(el){
    if (!el) return '';
    const meta = articleMetaFor(el);
    if (meta.author || meta.timestamp || meta.articleText) {
      return 'a|' + meta.author + '|' + meta.timestamp + '|' + meta.articleText;
    }
    function attrVal(node, name){
      try { return norm(node && node.getAttribute ? (node.getAttribute(name) || '') : ''); } catch (e) {}
      return '';
    }
    function domPath(node){
      const parts = [];
      let cur = node;
      let depth = 0;
      while (cur && cur.nodeType === 1 && depth < 6) {
        const tagName = String(cur.tagName || '').toLowerCase();
        if (!tagName) break;
        let index = 1;
        try {
          let sib = cur.previousElementSibling;
          while (sib) {
            if (String(sib.tagName || '').toLowerCase() === tagName) index++;
            sib = sib.previousElementSibling;
          }
        } catch (e) {}
        parts.push(tagName + ':' + String(index));
        cur = cur.parentElement;
        depth++;
      }
      return parts.join('>');
    }
    const tag = String(el.tagName || '').toLowerCase();
    const id = norm(el.id || '');
    const cls = norm(el.className || '');
    const href = attrVal(el, 'href');
    const src = attrVal(el, 'src');
    const dataId = attrVal(el, 'data-id') || attrVal(el, 'data-testid') ||
                   attrVal(el, 'data-urn') || attrVal(el, 'data-item-id');
    const dateTime = attrVal(el, 'datetime');
    const aria = attrVal(el, 'aria-label');
    const role = attrVal(el, 'role');
    const path = domPath(el);
    const text = textFor(el).slice(0, 180);
    const childCount = (el && el.children && Number.isFinite(el.children.length)) ? el.children.length : 0;
    return 'e|' + tag + '|' + id + '|' + cls + '|' + dataId + '|' + href + '|' + src +
           '|' + dateTime + '|' + aria + '|' + role + '|' + path + '|' +
           childCount + '|' + text;
  }
  function satisfiesStateNow(el){
    // For transition conditions, include matching candidates so dedupe has a
    // richer candidate set than only the currently attached element.
    if (condLower === 'new_child' || condLower === 'child_count_increases' ||
        condLower === 'new_child_contains' || condLower === 'becomes_visible' ||
        condLower === 'becomes_hidden') {
      return true;
    }
    if (condLower === 'visible') return isVisible(el, rectFor(el));
    if (condLower === 'hidden') return !isVisible(el, rectFor(el));
    if ((condLower === 'text_contains' || condLower === 'contains_text') && wantText) {
      const textValue = textFor(el);
      if (useTextRegex) return !!regexMatchValue(textRegex, textValue);
      return includesFold(textValue, wantText, wantTextFold);
    }
    if ((condLower === 'text_equals' || condLower === 'text_is') && wantText) {
      return equalsFold(textFor(el), wantText, wantTextFold);
    }
    if (condLower === 'attribute_contains' && attribute && wantText) {
      const attrValue = attrFor(el);
      if (useAttrRegex) return !!regexMatchValue(attrRegex, attrValue);
      return includesFold(attrValue, wantText, wantTextFold);
    }
    if (condLower === 'attribute_equals' && attribute && wantText) {
      return equalsFold(attrFor(el), wantText, wantTextFold);
    }
    return false;
  }
  function collectSatisfiedMatchKeys(preferredKey){
    const out = [];
    const seen = new Set();
    function add(k){
      const key = String(k || '').trim();
      if (!key || seen.has(key)) return;
      seen.add(key);
      out.push(key);
    }
    add(preferredKey);
    if (!Array.isArray(locators)) return out;
    for (let i = 0; i < locators.length; i++) {
      const loc = locators[i];
      const arr = matchesFor(loc) || [];
      for (let j = 0; j < arr.length; j++) {
        const el = arr[j];
        if (!el || el.nodeType !== 1) continue;
        if (!satisfiesStateNow(el)) continue;
        add(matchKeyFor(el));
        if (out.length >= 32) return out;
      }
    }
    return out;
  }
  function rectFor(el){
    try { return el.getBoundingClientRect(); } catch(e) { return null; }
  }
  function isVisible(el, r){
    if (!el) return false;
    if (!r) r = rectFor(el);
    if (!r) return false;
    if (!Number.isFinite(r.width) || !Number.isFinite(r.height)) return false;
    if (r.width <= 0 || r.height <= 0) return false;
    try {
      const cs = window.getComputedStyle ? window.getComputedStyle(el) : null;
      if (cs) {
        if (cs.display === 'none') return false;
        if (cs.visibility === 'hidden' || cs.visibility === 'collapse') return false;
        const op = parseFloat(cs.opacity || '1');
        if (!Number.isNaN(op) && op <= 0.01) return false;
      }
    } catch(e) {}
    return true;
  }
  function pickElement(){
    if(!Array.isArray(locators)) return null;
    let best = null;
    let bestLoc = null;
    let bestIdx = -1;
    let bestScore = -100000;
    for(let i=0;i<locators.length;i++){
      const loc=locators[i];
      const m=matchesFor(loc);
      if(!m || !m.length) continue;
      for(let j=0;j<m.length;j++){
        const el=m[j];
        if(!el || el.nodeType!==1) continue;
        const score = candidateScore(el, loc, i);
        if (score > bestScore) {
          best = el;
          bestLoc = loc;
          bestIdx = i;
          bestScore = score;
        }
      }
    }
    if (best) {
      setLastPick(bestLoc || {}, bestIdx, best);
    }
    return best;
  }
  function stateFor(el){
    const exists = !!el;
    const visible = el ? isVisible(el, rectFor(el)) : false;
    const childCount = el && el.children ? el.children.length : 0;
    let text = '';
    if (el) {
      try {
        const tag = String(el.tagName || '').toLowerCase();
        const role = (el.getAttribute && el.getAttribute('role')) || '';
        if (tag === 'input' || tag === 'textarea') {
          text = norm(el.value || '');
        } else if (el.isContentEditable || role === 'textbox') {
          text = norm(el.innerText || el.textContent || '');
        } else {
          text = norm(el.innerText || el.textContent || '');
        }
      } catch (e) {
        text = norm(el.innerText || el.textContent || '');
      }
    }
    let attrVal = '';
    if (el && attribute) {
      try { attrVal = String(el.getAttribute(String(attribute)) || ''); } catch(e) {}
    }
    const meta = articleMetaFor(el);
    return {
      exists,
      visible,
      childCount,
      text,
      attr: attrVal,
      matchValue: '',
      author: meta.author,
      timestamp: meta.timestamp,
      articleText: meta.articleText
    };
  }

  function mirrorProgress(reason, el){
    const st = stateFor(el);
    const payload = {
      ok: true,
      satisfied: false,
      reason: String(reason || 'waiting'),
      exists: st.exists,
      visible: st.visible,
      childCount: st.childCount,
      text: st.text,
      attr: st.attr,
      author: st.author,
      timestamp: st.timestamp,
      articleText: st.articleText,
      chosenType: lastPick.type,
      chosenIndex: lastPick.index,
      chosenTag: lastPick.tag,
      chosenValue: lastPick.value,
      pageUrl: String(location && location.href || ''),
      readyState: String(document && document.readyState || ''),
      locatorDebug: locatorDiagnostics(),
      candidateDebug: candidateDiagnostics(5),
    };
    mirror(payload);
  }

  function isSatisfied(s, baselineChildCount, prevVisible){
    const cond = String(condition||'').toLowerCase();
    const want = norm(needle || '');
    if (!s.exists) return false;
    if (cond === 'visible') return s.visible;
    if (cond === 'hidden') return !s.visible;
    if (cond === 'becomes_visible') return !prevVisible && s.visible;
    if (cond === 'becomes_hidden') return !!prevVisible && !s.visible;
    if (cond === 'text_contains' || cond === 'contains_text') {
      if (!want) return false;
      const mv = textMatchValue(s.text);
      if (!mv) return false;
      s.matchValue = mv;
      return true;
    }
    if (cond === 'text_equals' || cond === 'text_is') return !!want && equalsFold(s.text, wantText, wantTextFold);
    if (cond === 'new_child' || cond === 'child_count_increases') return s.childCount > Math.max(0, Number(baselineChildCount || 0));
    if (cond === 'new_child_contains') return s.childCount > Math.max(0, Number(baselineChildCount || 0)) && (!!want && includesFold(s.text, wantText, wantTextFold));
    if (cond === 'attribute_contains') {
      if (!attribute || !want) return false;
      const mv = attrMatchValue(s.attr || '');
      if (!mv) return false;
      s.matchValue = mv;
      return true;
    }
    if (cond === 'attribute_equals') return !!attribute && !!want && equalsFold(s.attr || '', wantText, wantTextFold);
    return false;
  }

  return new Promise((resolve) => {
    let done = false;
    let baseline = null;
    let hasCapturedInitial = false;
    let prevVisible = false;
    let docObserver = null;
    let elObserver = null;
    let io = null;
    let timeoutId = null;
    let attachedEl = null;

    function cleanup(){
      try { if (timeoutId) clearTimeout(timeoutId); } catch(e) {}
      timeoutId = null;
      try { if (docObserver) docObserver.disconnect(); } catch(e) {}
      try { if (elObserver) elObserver.disconnect(); } catch(e) {}
      try { if (io) io.disconnect(); } catch(e) {}
      docObserver = null; elObserver = null; io = null;
    }

    function clearListenerMetadata(){
      try {
        if (document && document.documentElement) {
          document.documentElement.removeAttribute(ATTR_NODE_ID);
          document.documentElement.removeAttribute(ATTR_NODE_NAME);
          document.documentElement.removeAttribute(ATTR_AX_ROLE);
          document.documentElement.removeAttribute(ATTR_AX_NAME);
        }
      } catch (e) {}
    }

    function mirror(obj){
      try {
        if (!obj || typeof obj !== 'object') return;
        obj.token = String(token || '');
        if (document && document.documentElement) {
          document.documentElement.setAttribute(ATTR_TOKEN, String(token || ''));
          document.documentElement.setAttribute(ATTR_STATE, JSON.stringify(obj));
        }
      } catch (e) {}
    }

    function finish(obj){
      if (done) return;
      done = true;
      cleanup();
      mirror(obj);
      clearListenerMetadata();
      resolve(obj);
    }

    function failBootstrap(reason, errorText){
      finish({
        ok:false,
        satisfied:false,
        reason:String(reason || 'bootstrap_error'),
        error:String(errorText || '').slice(0, 300),
        chosenType:lastPick.type,
        chosenIndex:lastPick.index,
        chosenTag:lastPick.tag,
        chosenValue:lastPick.value,
        pageUrl:String(location && location.href || ''),
        readyState:String(document && document.readyState || ''),
        locatorDebug: locatorDiagnostics(),
        candidateDebug: candidateDiagnostics(5),
      });
    }

    function maybeFinishFor(el){
      const st = stateFor(el);
      if (!hasCapturedInitial) {
        hasCapturedInitial = true;
        // Capture the initial state without treating it as a "transition" so
        // 'becomes_visible' / 'becomes_hidden' don't fire immediately when the
        // listener is armed.
        if (st.exists) {
          baseline = st.childCount;
          prevVisible = st.visible;
        } else {
          prevVisible = false;
        }
        // For state-based conditions (visible/hidden/text/attribute/etc), we
        // *do* want to fire immediately if already satisfied. For transition
        // conditions ('becomes_*') this is safe because prevVisible is set to
        // the current value above, so they won't trigger immediately.
        const satisfied_initial = isSatisfied(st, baseline || 0, prevVisible);
        if (satisfied_initial) {
          const matchKey = matchKeyFor(el);
          finish({ok:true, satisfied:true, exists:st.exists, visible:st.visible, childCount:st.childCount, text:st.text, attr:st.attr, matchValue:st.matchValue, author:st.author, timestamp:st.timestamp, articleText:st.articleText, matchKey: matchKey, matchKeys: collectSatisfiedMatchKeys(matchKey), chosenType:lastPick.type, chosenIndex:lastPick.index, chosenTag:lastPick.tag, chosenValue:lastPick.value, pageUrl:String(location && location.href || ''), readyState:String(document && document.readyState || ''), locatorDebug: locatorDiagnostics()});
          return true;
        }
        mirrorProgress('waiting', el);
        return false;
      }
      if (baseline == null && st.exists) baseline = st.childCount;
      const satisfied = isSatisfied(st, baseline || 0, prevVisible);
      prevVisible = st.exists ? st.visible : false;
      if (satisfied) {
        const matchKey = matchKeyFor(el);
        finish({ok:true, satisfied:true, exists:st.exists, visible:st.visible, childCount:st.childCount, text:st.text, attr:st.attr, matchValue:st.matchValue, author:st.author, timestamp:st.timestamp, articleText:st.articleText, matchKey: matchKey, matchKeys: collectSatisfiedMatchKeys(matchKey), chosenType:lastPick.type, chosenIndex:lastPick.index, chosenTag:lastPick.tag, chosenValue:lastPick.value, pageUrl:String(location && location.href || ''), readyState:String(document && document.readyState || ''), locatorDebug: locatorDiagnostics()});
        return true;
      }
      mirrorProgress('waiting', el);
      return false;
    }

    function attachToElement(el){
      if (!el) return;
      attachedEl = el;

      // Re-attaching: drop any old observers to avoid leaking handlers and to
      // ensure we only monitor the most-relevant candidate.
      try { if (elObserver) elObserver.disconnect(); } catch (e) {}
      try { if (io) io.disconnect(); } catch (e) {}
      elObserver = null;
      io = null;

      // If already satisfied, resolve immediately.
      if (maybeFinishFor(el)) return;

      // For <input>/<textarea>, value changes do not produce DOM mutations.
      // Listen to input/change events to re-evaluate without polling.
      try {
        const tag = String(el.tagName || '').toLowerCase();
        if (tag === 'input' || tag === 'textarea') {
          const onVal = () => { if (done) return; maybeFinishFor(el); };
          el.addEventListener('input', onVal, {capture: true});
          el.addEventListener('change', onVal, {capture: true});
          el.addEventListener('keyup', onVal, {capture: true});
        }
      } catch (e) {}

      // Watch for DOM/attribute/text changes under the element.
      try {
        elObserver = new MutationObserver(() => {
          if (done) return;
          // If the element was replaced/removed, re-locate via doc observer.
          try {
            if (!document.contains(el)) { tryLocateAndAttach(); return; }
          } catch (e) {}
          maybeFinishFor(el);
        });
        elObserver.observe(el, {
          attributes: true,
          childList: true,
          characterData: true,
          subtree: true,
        });
      } catch (e) {}

      // IntersectionObserver helps with 'visible/hidden' without polling.
      try {
        if (typeof IntersectionObserver === 'function') {
          io = new IntersectionObserver(() => {
            if (done) return;
            maybeFinishFor(el);
          }, {threshold: [0, 0.01, 0.5, 1]});
          io.observe(el);
        }
      } catch (e) {}
    }

    function tryLocateAndAttach(){
      try {
        const el = pickElement();
        if (!el) {
          mirrorProgress('no_candidate', null);
          return false;
        }
        attachToElement(el);
        return true;
      } catch (e) {
        failBootstrap('initial_probe_error', String(e || ''));
        return false;
      }
    }

    // Observe document mutations until the target element appears. This is
    // event-driven (no polling).
    try {
      docObserver = new MutationObserver(() => {
        if (done) return;
        try {
          if (attachedEl && document.contains(attachedEl)) {
            maybeFinishFor(attachedEl);
          }
        } catch (e) {}
        tryLocateAndAttach();
      });
      docObserver.observe(document, {
        subtree: true,
        childList: true,
        attributes: true,
        characterData: true,
        attributeFilter: ['class', 'style', 'hidden', 'aria-hidden', 'disabled'],
      });
    } catch (e) {}
    if (!docObserver) {
      failBootstrap('doc_observer_unavailable', '');
      return;
    }

    // Timeout to avoid hanging forever.
    const ms = Math.max(0, Number(timeoutMs || 0));
    timeoutId = setTimeout(() => {
      if (done) return;
      const el = pickElement();
      const st = stateFor(el);
      finish({ok:false, satisfied:false, reason:'timeout', exists:st.exists, visible:st.visible, childCount:st.childCount, text:st.text, attr:st.attr, matchValue:st.matchValue, author:st.author, timestamp:st.timestamp, articleText:st.articleText, chosenType:lastPick.type, chosenIndex:lastPick.index, chosenTag:lastPick.tag, chosenValue:lastPick.value, pageUrl:String(location && location.href || ''), readyState:String(document && document.readyState || ''), locatorDebug: locatorDiagnostics()});
    }, ms > 0 ? ms : 60000);

    mirrorProgress('bootstrapped', null);

    // Initial attempt.
    tryLocateAndAttach();
  });
})
)JS";

}  // namespace

ListenAction::ListenAction(Profile* profile) : dashboard::actions::Action(profile) {}
ListenAction::~ListenAction() = default;

bool ListenAction::SetParams(const base::Value::Dict& params) {
  const base::Value::List* locators_list = params.FindList("locators");
  if (!locators_list || locators_list->empty()) {
    return false;
  }
  locators_ = dashboard::ConvertLocators(*locators_list);

  frame_hint_.reset();
  if (const std::string* v = params.FindString("frame_id")) {
    frame_hint_ = ParseGlobalIdLike(*v);
  }

  const std::string* c = params.FindString("condition");
  condition_ = c ? *c : std::string();
  if (condition_.empty()) {
    condition_ = "visible";
  }

  text_.clear();
  if (const std::string* t = params.FindString("text"))
    text_ = *t;
  text_use_regex_ = params.FindBool("textUseRegex").value_or(false);
  cached_text_regex_.reset();
  if (text_use_regex_ && !text_.empty()) {
    auto compiled = std::make_unique<RE2>(text_);
    if (compiled->ok()) {
      cached_text_regex_ = std::move(compiled);
    }
  }
  attribute_.clear();
  if (const std::string* a = params.FindString("attribute"))
    attribute_ = *a;
  attribute_use_regex_ = params.FindBool("attributeUseRegex").value_or(false);
  cached_attribute_regex_.reset();
  if (attribute_use_regex_ && !text_.empty()) {
    auto compiled = std::make_unique<RE2>(text_);
    if (compiled->ok()) {
      cached_attribute_regex_ = std::move(compiled);
    }
  }
  ax_role_hint_.clear();
  if (const std::string* s = params.FindString("ax_role"))
    ax_role_hint_ = *s;
  ax_name_hint_.clear();
  if (const std::string* s = params.FindString("ax_name"))
    ax_name_hint_ = *s;
  interactive_ancestor_role_hint_.clear();
  if (const std::string* s = params.FindString("interactive_ancestor_role"))
    interactive_ancestor_role_hint_ = *s;

  on_match_script_id_.clear();
  if (const std::string* s = params.FindString("onMatchScriptId"))
    on_match_script_id_ = *s;

  timeout_ms_ = params.FindInt("timeout_ms").value_or(60000);
  sample_interval_ms_ = params.FindInt("sampleIntervalMs").value_or(250);
  if (sample_interval_ms_ < 100) {
    sample_interval_ms_ = 100;
  }

  base::Value::Dict out = params.Clone();
  if (frame_hint_) {
    out.Set("frame_id", base::StringPrintf("%d,%d", frame_hint_->child_id,
                                          frame_hint_->frame_routing_id));
  }
  out.Set("condition", condition_);
  out.Set("text", text_);
  out.Set("textUseRegex", text_use_regex_);
  out.Set("attribute", attribute_);
  out.Set("attributeUseRegex", attribute_use_regex_);
  if (!ax_role_hint_.empty())
    out.Set("ax_role", ax_role_hint_);
  if (!ax_name_hint_.empty())
    out.Set("ax_name", ax_name_hint_);
  if (!interactive_ancestor_role_hint_.empty()) {
    out.Set("interactive_ancestor_role", interactive_ancestor_role_hint_);
  }
  out.Set("timeout_ms", timeout_ms_);
  if (!on_match_script_id_.empty()) {
    out.Set("onMatchScriptId", on_match_script_id_);
  }
  SetSerializedAction("listen", std::move(out));
  return true;
}

void ListenAction::Execute(content::WebContents* web_contents, Callback callback) {
  callback_ = std::move(callback);
  web_contents_ = web_contents;
  resolver_page_map_fallback_attempted_ = false;
  resolver_page_map_extractor_.reset();
  native_baseline_initialized_ = false;
  native_prev_visible_ = false;
  native_baseline_child_count_ = 0;
  start_time_ = base::TimeTicks::Now();
  poll_count_ = 0;

  if (!web_contents) {
    Finish(false, "No web contents");
    return;
  }

  LOG(INFO) << "[ListenAction] Begin condition=" << condition_
            << " timeout_ms=" << timeout_ms_
            << " locators=" << locators_.size()
            << " url="
            << (web_contents->GetLastCommittedURL().is_valid()
                    ? web_contents->GetLastCommittedURL().spec()
                    : std::string("<invalid>"));

  if (dashboard::IsVerboseLoggingEnabled()) {
    base::Value::List locators_log;
    for (const auto& l : locators_) {
      locators_log.Append(dashboard::LocatorToValue(l));
    }
    LOG(INFO) << "[ListenAction] Execute: locators=" << locators_.size()
              << " condition=" << condition_
              << " text=" << TruncateForLog(text_, 160)
              << " textUseRegex=" << (text_use_regex_ ? 1 : 0)
              << " attribute=" << attribute_
              << " attributeUseRegex=" << (attribute_use_regex_ ? 1 : 0)
              << " axRoleHint=" << ax_role_hint_
              << " axNameHint=" << TruncateForLog(ax_name_hint_, 120)
              << " interactiveAncestorRoleHint="
              << interactive_ancestor_role_hint_
              << " timeout_ms=" << timeout_ms_
              << " frame_id_hint="
              << (frame_hint_
                      ? base::StringPrintf("%d,%d", frame_hint_->child_id,
                                           frame_hint_->frame_routing_id)
                      : std::string("<primary>"))
              << " url="
              << (web_contents->GetLastCommittedURL().is_valid()
                      ? web_contents->GetLastCommittedURL().spec()
                      : std::string("<invalid>"))
              << " locators_json="
              << ValueToCompactLog(base::Value(locators_log.Clone()), 1400);
  }

  LOG(WARNING) << "[ListenAction] Execute native_snapshot timeout_ms="
               << timeout_ms_ << " locators=" << locators_.size()
               << " url="
               << (web_contents->GetLastCommittedURL().is_valid()
                       ? web_contents->GetLastCommittedURL().spec()
                       : std::string("<invalid>"));
  StartNativeListen(web_contents);
}

void ListenAction::StartNativeListen(content::WebContents* web_contents) {
  if (!CandidateSupportsAttribute(attribute_)) {
    Finish(false, "Native listen does not support attribute '" + attribute_ +
                      "'");
    return;
  }
  if (text_use_regex_) {
    const std::string regex_error = ValidateRegexPattern(text_);
    if (!regex_error.empty()) {
      Finish(false, "Invalid text regex: " + regex_error);
      return;
    }
  }
  if (attribute_use_regex_) {
    const std::string regex_error = ValidateRegexPattern(text_);
    if (!regex_error.empty()) {
      Finish(false, "Invalid attribute regex: " + regex_error);
      return;
    }
  }
  if (!web_contents) {
    Finish(false, "No web contents");
    return;
  }
  listener_watch_registration_id_ = dashboard::ListenerWatchController::Register(
      web_contents,
      base::BindRepeating(&ListenAction::OnListenerWatchTriggered,
                          weak_ptr_factory_.GetWeakPtr()));
  listener_watch_active_ = listener_watch_registration_id_ != 0;
  PollNativeListenerState();
}

void ListenAction::OnListenerWatchTriggered() {
  if (callback_.is_null() || !web_contents_) {
    return;
  }
  PollNativeListenerState();
}

void ListenAction::ScheduleNextNativePoll() {
  if (callback_.is_null()) {
    return;
  }
  const int interval_ms =
      listener_watch_active_
          ? std::max(sample_interval_ms_ * 8, 2000)
          : sample_interval_ms_;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ListenAction::PollNativeListenerState,
                     weak_ptr_factory_.GetWeakPtr()),
      base::Milliseconds(interval_ms));
}

void ListenAction::PollNativeListenerState() {
  if (callback_.is_null()) {
    return;
  }
  if (!web_contents_) {
    Finish(false, "No web contents");
    return;
  }
  if (timeout_ms_ > 0 &&
      base::TimeTicks::Now() - start_time_ > base::Milliseconds(timeout_ms_)) {
    Finish(false, "Listen timed out");
    return;
  }
  if (!dashboard::ExecutionRunContext::Get().ShouldUseUnifiedPageMap()) {
    if (ConditionSupportsLocatorFallback(condition_)) {
      StartResolverLocatorFallback("page_map_disabled_in_script_mode");
    } else {
      StartResolverPageMapFallback("page_map_disabled_in_script_mode");
    }
    return;
  }
  dashboard::FetchUnifiedPageMapSnapshot(
      web_contents_,
      base::BindOnce(&ListenAction::OnNativePageMapCaptured,
                     weak_ptr_factory_.GetWeakPtr()),
      dashboard::mojom::SearchScope::kAll);
}

void ListenAction::OnNativePageMapCaptured(dashboard::UnifiedPageMapResult result) {
  if (callback_.is_null()) {
    return;
  }
  ++poll_count_;
  if (!result.ok) {
    if (resolver_page_map_extractor_ || resolver_locator_fallback_active_) {
      return;
    }
    if (!resolver_page_map_fallback_attempted_ && web_contents_) {
      if (ConditionSupportsLocatorFallback(condition_)) {
        StartResolverLocatorFallback(
            result.error.empty() ? "Native listen page snapshot failed"
                                 : result.error);
      } else {
        StartResolverPageMapFallback(
            result.error.empty() ? "Native listen page snapshot failed"
                                 : result.error);
      }
      return;
    }
    Finish(false, result.error.empty() ? "Native listen page snapshot failed"
                                       : result.error);
    return;
  }

  const std::string condition = base::ToLowerASCII(condition_);
  const std::string want_text = NormalizeMatchText(text_);
  const std::string page_url =
      result.page_map.FindString("url") ? *result.page_map.FindString("url")
                                         : std::string();
  dashboard::PageMapMatchOptions match_options;
  if (frame_hint_) {
    match_options.frame_id = base::StringPrintf("%d,%d", frame_hint_->child_id,
                                                frame_hint_->frame_routing_id);
  }
  if (const std::string* shadow_chain =
          serialized_params_.FindString("shadow_chain")) {
    match_options.shadow_chain = *shadow_chain;
  }
  if (const std::string* resource_path =
          serialized_params_.FindString("resource_path")) {
    match_options.resource_path = *resource_path;
  }
  match_options.ax_role = ax_role_hint_;
  match_options.ax_name = ax_name_hint_;
  match_options.interactive_ancestor_role = interactive_ancestor_role_hint_;

  std::vector<dashboard::NativePageMapCandidate> ranked_candidates =
      dashboard::FindMatchingPageMapCandidates(result.page_map, locators_,
                                               match_options,
                                               /*max_candidates=*/24);
  const dashboard::NativePageMapCandidate* candidate =
      ranked_candidates.empty() ? nullptr : &ranked_candidates.front();
  auto compute_child_count =
      [&](const dashboard::NativePageMapCandidate* candidate_ptr) {
        return candidate_ptr
                   ? ImmediateChildCountForCandidate(result.page_map,
                                                     *candidate_ptr)
                   : 0;
      };
  auto compute_text = [&](const dashboard::NativePageMapCandidate* candidate_ptr) {
    return candidate_ptr ? NormalizeMatchText(candidate_ptr->text)
                         : std::string();
  };
  auto compute_attr = [&](const dashboard::NativePageMapCandidate* candidate_ptr) {
    return candidate_ptr ? AttributeValueForCandidate(*candidate_ptr, attribute_)
                         : std::string();
  };
  bool exists = candidate != nullptr;
  bool visible = candidate && candidate->visible;
  int child_count = compute_child_count(candidate);
  std::string text = compute_text(candidate);
  std::string attr = compute_attr(candidate);

  const auto find_matching_candidate =
      [&](bool use_attribute_value, bool require_equal)
          -> std::pair<const dashboard::NativePageMapCandidate*, std::string> {
    for (const auto& candidate_entry : ranked_candidates) {
      const std::string candidate_text =
          use_attribute_value
              ? AttributeValueForCandidate(candidate_entry, attribute_)
              : NormalizeMatchText(candidate_entry.text);
      std::string value;
      if (use_attribute_value) {
        value = attribute_use_regex_
                    ? RegexMatchValue(cached_attribute_regex_.get(),
                                      candidate_text)
                    : ((require_equal
                            ? base::EqualsCaseInsensitiveASCII(candidate_text,
                                                               want_text)
                            : ContainsCaseInsensitive(candidate_text,
                                                      want_text))
                           ? want_text
                           : std::string());
      } else {
        value = text_use_regex_ ? RegexMatchValue(cached_text_regex_.get(),
                                                 candidate_text)
                                : (((require_equal
                                         ? base::EqualsCaseInsensitiveASCII(
                                               candidate_text, want_text)
                                         : ContainsCaseInsensitive(
                                               candidate_text, want_text)))
                                       ? want_text
                                       : std::string());
      }
      if (!value.empty()) {
        return {&candidate_entry, value};
      }
    }
    return {nullptr, std::string()};
  };

  if (!native_baseline_initialized_) {
    native_baseline_initialized_ = true;
    native_prev_visible_ = visible;
    native_baseline_child_count_ = child_count;
  }

  bool satisfied = false;
  std::string match_value;
  if (condition == "visible") {
    satisfied = visible;
  } else if (condition == "hidden") {
    satisfied = !visible;
  } else if (condition == "becomes_visible") {
    satisfied = !native_prev_visible_ && visible;
  } else if (condition == "becomes_hidden") {
    satisfied = native_prev_visible_ && !visible;
  } else if (condition == "text_contains" || condition == "contains_text") {
    if (text_use_regex_) {
      std::tie(candidate, match_value) = find_matching_candidate(false, false);
      satisfied = candidate != nullptr && !match_value.empty();
    } else {
      std::tie(candidate, match_value) = find_matching_candidate(false, false);
      satisfied = candidate != nullptr && !match_value.empty();
      if (satisfied) {
        match_value = want_text;
      }
    }
  } else if (condition == "text_equals" || condition == "text_is") {
    if (text_use_regex_) {
      std::tie(candidate, match_value) = find_matching_candidate(false, true);
      satisfied = candidate != nullptr && !match_value.empty();
    } else {
      std::tie(candidate, match_value) = find_matching_candidate(false, true);
      satisfied = candidate != nullptr && !match_value.empty();
    }
    if (satisfied) {
      if (match_value.empty()) {
        match_value = candidate ? NormalizeMatchText(candidate->text)
                                : std::string();
      }
    }
  } else if (condition == "attribute_contains") {
    if (attribute_use_regex_) {
      std::tie(candidate, match_value) = find_matching_candidate(true, false);
      satisfied = candidate != nullptr && !match_value.empty();
    } else {
      std::tie(candidate, match_value) = find_matching_candidate(true, false);
      satisfied = candidate != nullptr && !match_value.empty();
      if (satisfied) {
        match_value = want_text;
      }
    }
  } else if (condition == "attribute_equals") {
    if (attribute_use_regex_) {
      std::tie(candidate, match_value) = find_matching_candidate(true, true);
      satisfied = candidate != nullptr && !match_value.empty();
    } else {
      std::tie(candidate, match_value) = find_matching_candidate(true, true);
      satisfied = candidate != nullptr && !match_value.empty();
    }
    if (satisfied) {
      if (match_value.empty()) {
        match_value =
            candidate ? AttributeValueForCandidate(*candidate, attribute_)
                      : std::string();
      }
    }
  } else if (condition == "new_child" ||
             condition == "child_count_increases") {
    satisfied = child_count > native_baseline_child_count_;
  } else if (condition == "new_child_contains") {
    if (text_use_regex_) {
      std::tie(candidate, match_value) = find_matching_candidate(false, false);
      satisfied = compute_child_count(candidate) > native_baseline_child_count_ &&
                  candidate != nullptr && !match_value.empty();
    } else {
      std::tie(candidate, match_value) = find_matching_candidate(false, false);
      satisfied = compute_child_count(candidate) > native_baseline_child_count_ &&
                  candidate != nullptr && !match_value.empty();
    }
    if (satisfied) {
      if (match_value.empty()) {
        match_value = want_text;
      }
    }
  } else {
    Finish(false, "Unsupported native listen condition: " + condition_);
    return;
  }

  exists = candidate != nullptr;
  visible = candidate && candidate->visible;
  child_count = compute_child_count(candidate);
  text = compute_text(candidate);
  attr = compute_attr(candidate);

  native_prev_visible_ = visible;
  if (candidate) {
    native_baseline_child_count_ = std::max(native_baseline_child_count_,
                                            child_count);
  }

  if (satisfied) {
    base::Value::Dict match;
    match.Set("ok", true);
    match.Set("satisfied", true);
    match.Set("exists", exists);
    match.Set("visible", visible);
    match.Set("childCount", child_count);
    if (!text.empty()) {
      match.Set("text", text);
    }
    if (!attr.empty()) {
      match.Set("attr", attr);
    }
    if (!match_value.empty()) {
      match.Set("matchValue", match_value);
    }
    if (!page_url.empty()) {
      match.Set("pageUrl", page_url);
    }
    if (candidate) {
      if (!candidate->role.empty()) {
        match.Set("role", candidate->role);
      }
      if (!candidate->accessibility_name.empty()) {
        match.Set("accessibilityName", candidate->accessibility_name);
      }
      if (!candidate->tag.empty()) {
        match.Set("tag", candidate->tag);
      }
      dashboard::PromoteActionParamsFromPageMapCandidate(&serialized_params_,
                                                         *candidate);
    }
    std::string payload;
    base::JSONWriter::Write(match, &payload);
    FinishAfterMatch(payload);
    return;
  }

  ScheduleNextNativePoll();
}

void ListenAction::StartResolverPageMapFallback(const std::string& reason) {
  if (!web_contents_ || resolver_page_map_fallback_attempted_) {
    Finish(false, reason);
    return;
  }
  resolver_page_map_fallback_attempted_ = true;
  resolver_page_map_extractor_ =
      std::make_unique<dashboard::PageMapExtractor>();
  LOG(INFO) << "[ListenAction] Unified page-map unavailable (" << reason
            << "), trying resolver page-map fallback";
  resolver_page_map_extractor_->Run(
      web_contents_,
      base::BindOnce(&ListenAction::OnResolverPageMapFallbackReady,
                     weak_ptr_factory_.GetWeakPtr()),
      /*greedy=*/false,
      /*is_agent_mode=*/false,
      /*timeout_ms=*/4000);
}

void ListenAction::StartResolverLocatorFallback(const std::string& reason) {
  if (!web_contents_ || resolver_locator_fallback_active_) {
    Finish(false, reason);
    return;
  }
  resolver_locator_fallback_active_ = true;
  LOG(INFO) << "[ListenAction] Trying resolver locator fallback (" << reason
            << ")";
  int remaining_ms = timeout_ms_ > 0
                         ? timeout_ms_ - (base::TimeTicks::Now() - start_time_)
                               .InMilliseconds()
                         : 2000;
  remaining_ms = std::max(remaining_ms, 0);
  const int resolve_timeout_ms = std::clamp(remaining_ms, 250, 5000);
  resolver_locator_fallback_ =
      std::make_unique<dashboard::LocatorResolver>(web_contents_);
  resolver_locator_fallback_->ResolveUniqueAsync(
      locators_, dashboard::SearchScope::kAll, resolve_timeout_ms,
      base::BindOnce(&ListenAction::OnResolverLocatorFallbackReady,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ListenAction::OnResolverLocatorFallbackReady(
    bool success,
    const dashboard::ResolvedElement& element,
    const std::string& error) {
  resolver_locator_fallback_.reset();
  resolver_locator_fallback_active_ = false;
  if (callback_.is_null()) {
    return;
  }
  if (!error.empty() && !success && dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[ListenAction] Resolver locator fallback miss error="
              << TruncateForLog(error, 240);
  }

  const std::string condition = base::ToLowerASCII(condition_);
  const bool exists = success;
  const bool visible = success && element.visible;

  if (!native_baseline_initialized_) {
    native_baseline_initialized_ = true;
    native_prev_visible_ = visible;
    native_baseline_child_count_ = 0;
  }

  bool satisfied = false;
  if (condition == "visible") {
    satisfied = visible;
  } else if (condition == "hidden") {
    satisfied = success && !visible;
  } else if (condition == "becomes_visible") {
    satisfied = !native_prev_visible_ && visible;
  } else if (condition == "becomes_hidden") {
    satisfied = native_prev_visible_ && success && !visible;
  } else if (condition == "element_exists" || condition == "exists") {
    satisfied = exists;
  } else {
    Finish(false, "Unsupported native listen condition: " + condition_);
    return;
  }

  native_prev_visible_ = visible;

  if (satisfied) {
    base::Value::Dict match;
    match.Set("ok", true);
    match.Set("satisfied", true);
    match.Set("exists", exists);
    match.Set("visible", visible);
    if (web_contents_) {
      match.Set("pageUrl", web_contents_->GetLastCommittedURL().spec());
    }
    if (!element.text.empty()) {
      match.Set("text", element.text);
    }
    if (!element.role.empty()) {
      match.Set("role", element.role);
    }
    if (!element.accessibility_name.empty()) {
      match.Set("accessibilityName", element.accessibility_name);
    }
    std::string payload;
    base::JSONWriter::Write(match, &payload);
    FinishAfterMatch(payload);
    return;
  }

  ScheduleNextNativePoll();
}

void ListenAction::OnResolverPageMapFallbackReady(
    base::Value::Dict resolver_page_map) {
  resolver_page_map_extractor_.reset();
  if (callback_.is_null()) {
    return;
  }
  if (resolver_page_map.empty()) {
    Finish(false, "Resolver page-map fallback was empty");
    return;
  }
  dashboard::UnifiedPageMapResult result;
  result.ok = true;
  result.page_map = std::move(resolver_page_map);
  OnNativePageMapCaptured(std::move(result));
}

void ListenAction::FinishAfterMatch(const std::string& match_payload) {
  // Persistent listener sessions (ListenerManager) strip onMatch from the
  // ListenAction params and own rearm themselves. In a normal Play timeline,
  // a listen step is one-shot: wait → optional onMatch once → done (no rearm).
  if (on_match_script_id_.empty() ||
      dashboard::ExecutionRunContext::Get().listen_oneshot_depth() > 0) {
    Finish(true, match_payload);
    return;
  }
  StartOneShotOnMatch(match_payload);
}

void ListenAction::StartOneShotOnMatch(const std::string& match_payload) {
  if (!profile_) {
    Finish(true, match_payload);
    return;
  }
  // ScriptStore::Read does PathExists/ReadFileToString. The UI thread has
  // DisallowUnresponsiveTasks() from BrowserMainLoop::PreMainMessageLoopRun.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, std::string script_id)
              -> std::pair<bool, base::Value::Dict> {
            base::Value::Dict script;
            const bool ok =
                dashboard::ScriptStore::Read(profile, script_id, &script) &&
                !script.empty();
            return {ok, std::move(script)};
          },
          base::Unretained(profile_), on_match_script_id_),
      base::BindOnce(
          [](base::WeakPtr<ListenAction> self, std::string payload,
             std::pair<bool, base::Value::Dict> loaded) {
            if (!self) {
              return;
            }
            self->OnOnMatchScriptLoaded(std::move(payload), loaded.first,
                                        std::move(loaded.second));
          },
          weak_ptr_factory_.GetWeakPtr(), match_payload));
}

void ListenAction::OnOnMatchScriptLoaded(std::string match_payload,
                                         bool ok,
                                         base::Value::Dict script) {
  if (callback_.is_null()) {
    return;
  }
  if (!ok) {
    Finish(false, "One-shot onMatch script not found: " + on_match_script_id_);
    return;
  }

  base::Value::List timeline;
  if (const base::Value::List* acts = script.FindList("actions");
      acts && !acts->empty()) {
    for (const auto& a : *acts) {
      timeline.Append(a.Clone());
    }
  } else if (const base::Value::List* ctxs = script.FindList("url_contexts")) {
    for (const auto& c : *ctxs) {
      if (!c.is_dict()) {
        continue;
      }
      if (const base::Value::List* nested = c.GetDict().FindList("actions")) {
        for (const auto& a : *nested) {
          timeline.Append(a.Clone());
        }
      }
    }
  }
  if (timeline.empty()) {
    Finish(false, "One-shot onMatch script has no actions: " + on_match_script_id_);
    return;
  }

  Browser* browser = chrome::FindLastActiveWithProfile(profile_);
  if (!browser) {
    Finish(false, "No browser for one-shot onMatch run");
    return;
  }
  std::string start_url;
  if (const base::Value::Dict* settings = script.FindDict("settings")) {
    if (const std::string* u = settings->FindString("startUrl")) {
      start_url = *u;
    }
  }
  if (start_url.empty()) {
    if (const base::Value::List* ctxs = script.FindList("url_contexts");
        ctxs && !ctxs->empty() && ctxs->front().is_dict()) {
      if (const std::string* u = ctxs->front().GetDict().FindString("url")) {
        start_url = *u;
      }
    }
  }
  const GURL nav_url = start_url.empty() ? GURL("about:blank") : GURL(start_url);
  NavigateParams np(browser, nav_url.is_valid() ? nav_url : GURL("about:blank"),
                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  np.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
  Navigate(&np);
  content::WebContents* target = np.navigated_or_inserted_contents;
  if (!target) {
    Finish(false, "Failed to open tab for one-shot onMatch run");
    return;
  }

  auto* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (!exec_service) {
    Finish(false, "No ScriptExecutionService for one-shot onMatch");
    return;
  }

  auto& ctx = dashboard::ExecutionRunContext::Get();
  ctx.set_listen_oneshot_depth(ctx.listen_oneshot_depth() + 1);
  const std::string run_id = exec_service->StartScriptRun(
      on_match_script_id_, timeline, target, /*key_timeline=*/nullptr,
      base::BindOnce(
          [](base::WeakPtr<ListenAction> self, std::string match_payload,
             bool success, int /*failed_step*/, std::string message) {
            auto& nested_ctx = dashboard::ExecutionRunContext::Get();
            nested_ctx.set_listen_oneshot_depth(
                std::max(0, nested_ctx.listen_oneshot_depth() - 1));
            if (!self) {
              return;
            }
            if (!success) {
              self->Finish(false,
                           message.empty()
                               ? "One-shot onMatch script failed"
                               : ("One-shot onMatch failed: " + message));
              return;
            }
            self->Finish(true, match_payload);
          },
          weak_ptr_factory_.GetWeakPtr(), match_payload));
  if (run_id.empty()) {
    ctx.set_listen_oneshot_depth(std::max(0, ctx.listen_oneshot_depth() - 1));
    Finish(false, "Failed to start one-shot onMatch script");
  }
}

void ListenAction::Finish(bool success, const std::string& message) {
  if (dashboard::IsVerboseLoggingEnabled()) {
    LOG(INFO) << "[ListenAction] Finish success=" << (success ? 1 : 0)
              << " message=" << TruncateForLog(message, 1400);
  }
  resolver_locator_fallback_.reset();
  resolver_locator_fallback_active_ = false;
  resolver_page_map_extractor_.reset();
  if (web_contents_ && listener_watch_registration_id_ != 0) {
    dashboard::ListenerWatchController::Unregister(web_contents_,
                                                   listener_watch_registration_id_);
    listener_watch_registration_id_ = 0;
    listener_watch_active_ = false;
  }
  web_contents_ = nullptr;
  if (!callback_) {
    return;
  }
  std::move(callback_).Run(success, message);
}

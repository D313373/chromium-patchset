export function formatActionTitle(action, resolveScriptName = () => '') {
  const name = String(action?.action || 'action').toUpperCase();
  const p = action?.params || {};
  const isRootScopeLocator = (locator) => {
    const type = String(locator?.type || '').toUpperCase();
    const value = String(locator?.value || '').trim().toLowerCase();
    if (type === 'CSS_SELECTOR') {
      return value === 'body' || value === 'html';
    }
    if (type === 'XPATH') {
      return value === '//body' || value === '/html/body' ||
        value === '//html' || value === '/html';
    }
    return false;
  };
  const formatTopLocators = (locs, count = 2) => {
    const arr = Array.isArray(locs) ? locs.slice() : [];
    const sorted = arr
      .map((l) => {
        const type = String(l?.type || '').toUpperCase();
        let inferredPriority = 900;
        if (type === 'ACCESSIBILITY_NAME') inferredPriority = 0;
        else if (type === 'LINK_TEXT') inferredPriority = 1;
        else if (type === 'PARTIAL_LINK_TEXT') inferredPriority = 2;
        else if (type === 'TEXT_CONTENT') inferredPriority = 3;
        else if (type === 'ID') inferredPriority = 4;
        else if (type === 'NAME') inferredPriority = 5;
        else if (type === 'CSS_SELECTOR') inferredPriority = 6;
        else if (type === 'XPATH') inferredPriority = 7;
        else if (type === 'TAG_NAME') inferredPriority = 8;
        else if (type === 'FRAME_ID' || type === 'SHADOW_DOM') inferredPriority = 50;
        else if (type === 'VISUAL_COORDS') inferredPriority = 100;
        return {
          ...l,
          priority: (typeof l?.priority === 'number' ? l.priority : inferredPriority),
        };
      })
      .sort((a, b) => a.priority - b.priority)
      .slice(0, count);
    return sorted.map(l => {
      const val = String(l?.value ?? '').slice(0, 60);
      return `${l?.type}:${val}`;
    }).join(' | ');
  };

  let details = '';
  switch (action?.action) {
    case 'click':
    case 'hover': {
      const tag = p.tag || (Array.isArray(p.locators) ? 'element' : '');
      const text = p.text ? String(p.text).slice(0, 40) : '';
      const xy = (p.x != null && p.y != null) ? `@(${p.x},${p.y})` : '';
      const locStr = formatTopLocators(p.locators, 2);
      details = [tag && `<${tag}>`, text && `"${text}"`, locStr, xy]
                    .filter(Boolean)
                    .join(' ');
      break;
    }
    case 'right_click': {
      const locStr = formatTopLocators(p.locators, 2);
      const xy = (p.x != null && p.y != null) ? `@(${p.x},${p.y})` : '';
      details = [locStr, xy].filter(Boolean).join(' ');
      break;
    }
    case 'type': {
      const field = p.name || p.selector || '';
      const t = p.type || '';
      let val = p.text || '';
      if (p.sensitive) val = '****';
      else if (val) val = `"${String(val).slice(0, 40)}"`;
      details = [field && `name=${field}`, t && `type=${t}`, val]
                    .filter(Boolean)
                    .join(' ');
      break;
    }
    case 'navigate': {
      details = p.url ? String(p.url).slice(0, 80) : '';
      break;
    }
    case 'request': {
      const method = String(p.method || 'GET').toUpperCase();
      const url = p.url ? String(p.url).slice(0, 72) : '';
      const capture = p.captureAs ? `as=${String(p.captureAs)}` : '';
      details = [method, url, capture].filter(Boolean).join(' ');
      break;
    }
    case 'wait': {
      if (p && p.time != null && Number.isFinite(Number(p.time))) {
        details = `${Number(p.time)}ms`;
      } else {
        details = '';
      }
      break;
    }
    case 'scrape_table': {
      const rowLocators = Array.isArray(p.rowLocators) ? p.rowLocators : p.locators;
      const rowStr = formatTopLocators(rowLocators, 1);
      const rootScoped =
        Array.isArray(rowLocators) && rowLocators.length > 0 &&
        rowLocators.every((locator) => isRootScopeLocator(locator));
      const fields = Array.isArray(p.fields) ? p.fields : [];
      const namedCount = fields.filter((field) => {
        return typeof field?.name === 'string' && field.name.trim();
      }).length;
      const csvLabel = p.saveCsv ? `csv=${String(p.csvFileName || 'table_rows.csv')}` : '';
      details = [
        rowStr && (rootScoped ? `scope:${rowStr}` : `rows:${rowStr}`),
        `fields=${fields.length}`,
        namedCount ? `named=${namedCount}` : '',
        p.variableName ? `var=${String(p.variableName)}` : '',
        csvLabel,
      ].filter(Boolean).join(' | ');
      break;
    }
    case 'loop': {
      const mode = String(p.loopMode || 'count').toLowerCase();
      const steps = Array.isArray(action?.steps) ? action.steps :
        (Array.isArray(p.steps) ? p.steps : []);
      if (mode === 'for_each') {
        const locStr = formatTopLocators(p.locators, 1);
        const cap = Number(p.maxIterations);
        const strategy = String(
            p.forEachStrategy || 'snapshot_once').toLowerCase();
        details = [
          'for each element',
          strategy === 'requery_each_iteration' ?
            'requery each iteration' :
            'snapshot once',
          locStr,
          Number.isFinite(cap) ? `cap=${cap}` : '',
          `steps=${steps.length}`,
        ].filter(Boolean).join(' | ');
      } else if (mode === 'while_element_exists') {
        const locStr = formatTopLocators(p.locators, 1);
        const cap = Number(p.maxIterations);
        details = [
          'while element exists',
          locStr,
          Number.isFinite(cap) ? `cap=${cap}` : '',
          `steps=${steps.length}`,
        ].filter(Boolean).join(' | ');
      } else {
        const iters = Number(p.iterations ?? p.maxIterations ?? 1);
        details = [
          `count=${Number.isFinite(iters) ? Math.max(1, Math.floor(iters)) : 1}`,
          `steps=${steps.length}`,
        ].join(' | ');
      }
      break;
    }
    case 'listen': {
      const cond = p.condition ? String(p.condition) : '';
      const text = p.text ? String(p.text).slice(0, 40) : '';
      const attr = p.attribute ? String(p.attribute).slice(0, 30) : '';
      const locStr = formatTopLocators(p.locators, 1);
      const sid = String(p.onMatchScriptId || '').trim();
      let scriptLabel = '';
      if (sid) {
        const nm = String(resolveScriptName(sid) || '').trim();
        scriptLabel = nm ? `-> ${nm}` : `-> ${sid.slice(0, 8)}`;
      }
      details = [cond && `when=${cond}`, attr && `attr=${attr}`,
                 text && `"${text}"`, locStr, scriptLabel]
                    .filter(Boolean)
                    .join(' ');
      break;
    }
    case 'glic_prompt': {
      const prompt = String(p.prompt || '').trim();
      const focusAreas = Array.isArray(p.focusAreas) ? p.focusAreas :
        (Array.isArray(p.locators) && p.locators.length
          ? [{label: 'Primary focus', locators: p.locators}]
          : []);
      const labels = focusAreas
        .map((area, index) => {
          const label = String(area?.label || '').trim();
          return label || `element ${index + 1}`;
        })
        .slice(0, 2);
      const focusSummary = focusAreas.length ?
        `focus=${focusAreas.length}${labels.length ? ` (${labels.join(', ')})` : ''}` :
        'focus=none';
      const promptSummary = prompt ?
        `"${prompt.slice(0, 72)}${prompt.length > 72 ? '...' : ''}"` : '';
      details = [focusSummary, promptSummary].filter(Boolean).join(' | ');
      break;
    }
    case 'authenticate': {
      const methods = Array.isArray(p.methods) ? p.methods : [];
      const labels = methods
          .filter((method) => method && method.enabled !== false)
          .map((method) => String(method.label || method.id || '').trim())
          .filter(Boolean);
      details = labels.join(' → ');
      break;
    }
    case 'if_condition': {
      const cond = String(p.conditionType || p.condition_type || '').trim();
      const text = String(p.text || '').trim();
      const attr = String(p.attribute || '').trim();
      const locStr = formatTopLocators(p.locators, 1);
      const thenSteps = Array.isArray(p.then) ? p.then.length : 0;
      const elseSteps = Array.isArray(p.else) ? p.else.length : 0;
      details = [
        cond && `when=${cond}`,
        attr && `attr=${attr.slice(0, 30)}`,
        text && `"${text.slice(0, 40)}"`,
        locStr,
        thenSteps ? `then=${thenSteps}` : 'then=0',
        elseSteps ? `else=${elseSteps}` : 'else=0',
      ].filter(Boolean).join(' | ');
      break;
    }
    default: {
      const keys = Object.entries(p)
        .filter(([k]) => k !== 'locators' && k !== 'iframePath')
        .slice(0, 3)
        .map(([k, v]) =>
          `${k}=${typeof v === 'string' ? v.slice(0, 40) : JSON.stringify(v)}`);
      details = keys.join(' ');
    }
  }
  return details ? `${name} - ${details}` : name;
}

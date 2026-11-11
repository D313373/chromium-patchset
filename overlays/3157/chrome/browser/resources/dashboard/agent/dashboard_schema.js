export function installDashboardSchema({
  windowRef = window,
  handler = null,
  actionMetadata = {},
} = {}) {
  function validateActionLocal(action, backendErrorHint = '') {
    const invalid = (msg) => ({valid: false, error: msg});
    if (!action || typeof action !== 'object') return invalid('Invalid action.');
    const type = String(action.action || '').toLowerCase();
    if (!type) return invalid('Action type is missing.');
    const params = (action.params && typeof action.params === 'object') ?
      action.params :
      {};

    const hasNonEmptyLocators = (locs) => {
      if (!Array.isArray(locs)) return false;
      return locs.some((l) => {
        if (!l || typeof l !== 'object') return false;
        const v = l.value;
        if (typeof v === 'string') return v.trim().length > 0;
        if (typeof v === 'number') return Number.isFinite(v);
        return v != null;
      });
    };

    const hasCoordinates = (p) => {
      const c = (p && typeof p.coordinates === 'object') ? p.coordinates : null;
      const x = c ? Number(c.x) : Number(p?.x);
      const y = c ? Number(c.y) : Number(p?.y);
      return Number.isFinite(x) && Number.isFinite(y);
    };

    if (type === 'click' || type === 'hover') {
      if (!hasNonEmptyLocators(params.locators) && !hasCoordinates(params)) {
        return invalid(
            type === 'hover' ?
              'Hover requires locators or coordinates.' :
              'Click requires locators or coordinates.');
      }
    }

    if (type === 'listen') {
      if (!hasNonEmptyLocators(params.locators)) {
        return invalid('Listen requires at least one locator.');
      }
      const cond = String(params.condition || '').trim();
      if (!cond) return invalid('Listen requires a condition.');
      const onMatch = String(params.onMatchScriptId || '').trim();
      if (!onMatch) return invalid("Listen requires 'On Match: Run Script'.");
      if (cond.includes('text_') || cond === 'new_child_contains') {
        if (!String(params.text || '').trim()) {
          return invalid('Listen condition requires text.');
        }
      }
      if (cond.includes('attribute_')) {
        if (!String(params.attribute || '').trim()) {
          return invalid('Listen condition requires an attribute.');
        }
        if (!String(params.text || '').trim()) {
          return invalid('Listen condition requires text.');
        }
      }
    }

    if (type === 'loop') {
      const loopMode = String(params.loopMode || 'count').toLowerCase();
      const steps = Array.isArray(action.steps) ?
        action.steps :
        (Array.isArray(params.steps) ? params.steps : []);
      if (!steps.length) {
        return invalid('Loop requires at least one nested step.');
      }
      if (loopMode === 'count') {
        const n = Number(params.iterations ?? params.maxIterations);
        if (!Number.isFinite(n) || n < 1) {
          return invalid('Count loop requires iterations >= 1.');
        }
      } else if (loopMode === 'for_each') {
        if (!hasNonEmptyLocators(params.locators)) {
          return invalid(
              'for_each loop requires at least one target locator.');
        }
        const strategy = String(
            params.forEachStrategy || 'snapshot_once').toLowerCase();
        if (strategy !== 'snapshot_once' &&
            strategy !== 'requery_each_iteration') {
          return invalid(
              "for_each strategy must be 'snapshot_once' or 'requery_each_iteration'.");
        }
      } else if (loopMode === 'while_element_exists') {
        if (!hasNonEmptyLocators(params.locators)) {
          return invalid(
              'while_element_exists loop requires at least one locator.');
        }
      } else {
        return invalid(
            "Loop mode must be 'count', 'for_each', or 'while_element_exists'.");
      }
    }

    if (type === 'authenticate') {
      const methods = params.methods;
      if (!Array.isArray(methods) || !methods.length) {
        return invalid('Authenticate requires at least one login method.');
      }
    }

    if (type === 'if_condition') {
      let conditionType = String(
          params.conditionType || params.condition_type || '').trim().toLowerCase();
      if (!conditionType) {
        conditionType = String(params.text || '').trim() ?
          'text_contains' :
          (hasNonEmptyLocators(params.locators) ? 'element_exists' : '');
      }
      if (!conditionType) {
        return invalid('If Condition requires a condition type.');
      }
      if (conditionType !== 'element_exists' && conditionType !== 'text_contains') {
        return invalid(
            "If Condition type must be 'element_exists' or 'text_contains'.");
      }
      if (conditionType === 'element_exists' && !hasNonEmptyLocators(params.locators)) {
        return invalid('If Condition element_exists requires at least one locator.');
      }
      if (conditionType === 'text_contains' && !String(params.text || '').trim()) {
        return invalid('If Condition text_contains requires text to find.');
      }
    }

    if (type === 'wait') {
      let waitType = String(
          params.waitType || params.wait_type || '').trim().toLowerCase();
      if (!waitType) {
        waitType = hasNonEmptyLocators(params.locators) || params.timeout != null ?
          'element' :
          'time';
      }
      if (waitType !== 'time' && waitType !== 'element') {
        return invalid("Wait type must be 'time' or 'element'.");
      }
      if (waitType === 'time') {
        const ms = Number(params.time);
        if (!Number.isFinite(ms) || ms <= 0) {
          return invalid("Wait type 'time' requires time > 0.");
        }
      } else {
        if (!hasNonEmptyLocators(params.locators)) {
          return invalid("Wait type 'element' requires at least one locator.");
        }
        const timeout = Number(params.timeout);
        if (!Number.isFinite(timeout) || timeout <= 0) {
          return invalid("Wait type 'element' requires timeout > 0.");
        }
      }
    }

    const meta = actionMetadata[type];
    if (!meta || !meta.params) {
      if (backendErrorHint) {
        return invalid(
            `Action could not be validated by backend: ${backendErrorHint}`);
      }
      return {valid: true, error: ''};
    }

    const evalShowIf = (expr, p) => {
      if (!expr || typeof expr !== 'string') return true;
      try {
        const m = expr.match(
            /\s*([a-zA-Z0-9_]+)\s*===\s*['\"]?([^'\"]+)['\"]?\s*/);
        if (!m) return true;
        const key = m[1];
        const val = m[2];
        return String(p?.[key]) === val;
      } catch (_) {
        return true;
      }
    };

    for (const [key, spec] of Object.entries(meta.params)) {
      if (!spec || spec.required !== true) continue;
      if (spec.showIf && !evalShowIf(spec.showIf, params)) continue;
      const v = params[key];
      if (spec.type === 'locator-list') {
        if (!hasNonEmptyLocators(v)) {
          return invalid(`Missing required locators: ${spec.label || key}`);
        }
        continue;
      }
      if (spec.type === 'select') {
        if (!String(v || '').trim()) {
          return invalid(`Missing required field: ${spec.label || key}`);
        }
        continue;
      }
      if (spec.type === 'number') {
        const n = Number(v);
        if (!Number.isFinite(n)) {
          return invalid(`Missing required number: ${spec.label || key}`);
        }
        continue;
      }
      if (v == null || String(v).trim() === '') {
        return invalid(`Missing required field: ${spec.label || key}`);
      }
    }

    return {valid: true, error: ''};
  }

  const schema = {
    _supportedActions: null,
    _handler: null,

    init(nextHandler) {
      this._handler = nextHandler;
    },

    createAction(type, params = {}) {
      return {
        action: String(type || '').toLowerCase(),
        params,
        comment: '',
      };
    },

    async getActionTemplate(type) {
      if (!this._handler) {
        console.error('[DashboardSchema] Handler not initialized');
        return this.createAction(type);
      }
      if (typeof this._handler.getActionTemplate !== 'function') {
        return this.createAction(type);
      }
      try {
        const result = await this._handler.getActionTemplate(type);
        return result.actionTemplate || this.createAction(type);
      } catch (e) {
        console.error('[DashboardSchema] Failed to get template:', e);
        return this.createAction(type);
      }
    },

    async getSupportedActions() {
      if (this._supportedActions) return this._supportedActions;
      if (!this._handler) {
        console.error('[DashboardSchema] Handler not initialized');
        return [];
      }
      if (typeof this._handler.getSupportedActions !== 'function') {
        this._supportedActions = [];
        return this._supportedActions;
      }
      try {
        const result = await this._handler.getSupportedActions();
        this._supportedActions = result.actions || [];
        return this._supportedActions;
      } catch (e) {
        console.error('[DashboardSchema] Failed to get supported actions:', e);
        return [];
      }
    },

    async validateAction(action) {
      if (!this._handler) {
        console.error('[DashboardSchema] Handler not initialized');
        return validateActionLocal(action);
      }
      if (typeof this._handler.validateAction !== 'function') {
        return validateActionLocal(action);
      }
      try {
        const result = await this._handler.validateAction(action);
        return {valid: result.valid, error: result.error};
      } catch (e) {
        console.error('[DashboardSchema] Failed to validate action:', e);
        return validateActionLocal(action, e?.message);
      }
    },
  };

  schema.init(handler);
  if (windowRef && typeof windowRef === 'object') {
    windowRef.DashboardSchema = schema;
  }
  return schema;
}

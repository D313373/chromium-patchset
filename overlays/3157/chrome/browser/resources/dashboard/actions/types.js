// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Locator types for element selection
// Layer hints (FRAME_ID / SHADOW_DOM) are edited in LayerEditor instead of the
// locator list to avoid treating ephemeral context routing as element identity.
export const LOCATOR_TYPES = ['CSS_SELECTOR','XPATH','ID','NAME','CLASS_NAME','TAG_NAME','LINK_TEXT','PARTIAL_LINK_TEXT','TEXT_CONTENT','ACCESSIBILITY_NAME','JS_EVAL','VISUAL_COORDS','VISUAL_AI','RESOURCE_TREE'];

export const ACTION_TYPES = {
  NAVIGATE: 'navigate',
  BACK: 'back',
  FORWARD: 'forward',
  CLICK: 'click',
  HOVER: 'hover',
  DRAG: 'drag',
  TYPE: 'type',
  KEY_DOWN: 'key_down',
  KEY_PRESS: 'key_press',

  SCRAPE_ELEMENT: 'scrape_element',
  SCRAPE_TABLE: 'scrape_table',
  SCROLL: 'scroll',
  LOOP: 'loop',
  IF_CONDITION: 'if_condition',
  AUTHENTICATE: 'authenticate',
  WAIT: 'wait',
  LISTEN: 'listen',
  GLIC_PROMPT: 'glic_prompt',
  CALL_SCRIPT: 'call_script'
};

// Action metadata including parameters and display info
export const ACTION_METADATA = {
  [ACTION_TYPES.NAVIGATE]: {
    name: 'Navigate',
    description: 'Opens a website by URL',
    icon: 'world',
    params: {
      url: {
        type: 'string',
        label: 'URL',
        placeholder: 'https://example.com',
        required: true
      }
    },
    canNest: false
  },

  [ACTION_TYPES.BACK]: {
    name: 'Back',
    description: 'Go back one step in browser history (trusted navigation).',
    icon: 'arrow-left',
    params: {},
    canNest: false
  },

  [ACTION_TYPES.FORWARD]: {
    name: 'Forward',
    description: 'Go forward one step in browser history (trusted navigation).',
    icon: 'arrow-right',
    params: {},
    canNest: false
  },

  [ACTION_TYPES.CLICK]: {
    name: 'Click',
    description: 'Clicks an element using locators or explicit coordinates.',
    icon: 'mouse-pointer',
    params: {
      locators: {
        type: 'locator-list',
        label: 'Element Locators',
        // Locators are optional because clicks can also be issued at explicit
        // coordinates; validation on the backend enforces that at least one
        // of locators or coordinates is provided.
        required: false
      },
      clickCount: {
        type: 'number',
        label: 'Click Count (1-3)',
        placeholder: '1=single, 2=double, 3=triple',
        required: false,
        default: 1
      }
    },
    canNest: false
  },

  [ACTION_TYPES.HOVER]: {
    name: 'Hover',
    description: 'Moves the cursor over an element without clicking.',
    icon: 'mouse-pointer',
    params: {
      locators: {
        type: 'locator-list',
        label: 'Element Locators',
        required: false
      }
    },
    canNest: false
  },

  [ACTION_TYPES.DRAG]: {
    name: 'Drag',
    description: 'Drags from a start target to an end target.',
    icon: 'move',
    params: {
      fromLocators: {
        type: 'locator-list',
        label: 'Start Locators',
        required: false
      },
      toLocators: {
        type: 'locator-list',
        label: 'End Locators',
        required: false
      },
      steps: {
        type: 'number',
        label: 'Drag Steps',
        placeholder: '12',
        required: false,
        default: 12
      },
      waitTimeout: {
        type: 'number',
        label: 'Wait Timeout (ms)',
        placeholder: '15000',
        required: false,
        default: 15000
      },
      humanOffset: {
        type: 'boolean',
        label: 'Human Offset',
        required: false,
        default: true
      }
    },
    canNest: false
  },

  [ACTION_TYPES.TYPE]: {
    name: 'Type',
    description: 'Types text into an input. If locators are omitted, it types into the currently focused element.',
    icon: 'keyboard',
    params: {
      locators: {
        type: 'locator-list',
        label: 'Element Locators (optional)',
        required: false
      },
      text: {
        type: 'string',
        label: 'Text',
        placeholder: 'Text to type',
        required: true
      }
    },
    canNest: false
  },

  [ACTION_TYPES.KEY_DOWN]: {
    name: 'Key Down',
    description: 'Simulates pressing down a key.',
    icon: 'arrow-down',
    params: {
      key: {
        type: 'string',
        label: 'Key',
        required: true
      }
    },
    canNest: false
  },

  [ACTION_TYPES.KEY_PRESS]: {
    name: 'Key Press',
    description: 'Simulates a key press event.',
    icon: 'arrow-down-up',
    params: {
      key: {
        type: 'string',
        label: 'Key',
        required: true
      },
      count: {
        type: 'number',
        label: 'Press Count',
        placeholder: '1',
        required: false,
        default: 1
      }
    },
    canNest: false
  },



  [ACTION_TYPES.SCRAPE_ELEMENT]: {
    name: 'Scrape Element',
    description: 'Extracts text from an element and saves it to a variable.',
    icon: 'scissors',
    params: {
      locators: {
        type: 'locator-list',
        label: 'Element Locators',
        required: true
      },
      variableName: {
        type: 'string',
        label: 'Save to Variable',
        placeholder: 'variableName',
        required: true
      }
    },
    canNest: false
  },

  [ACTION_TYPES.SCRAPE_TABLE]: {
    name: 'Scrape Table',
    description: 'Scrapes data from a table into structured format.',
    icon: 'table',
    params: {
      rowLocators: {
        type: 'locator-list',
        label: 'Row Locators',
        required: true
      },
      fields: {
        type: 'field-list',
        label: 'Fields to Extract',
        required: true
      },
      variableName: {
        type: 'string',
        label: 'Save to Variable',
        placeholder: 'tableData',
        required: false
      },
      rowSelectionMode: {
        type: 'select',
        label: 'Row Selection',
        options: ['all', 'index'],
        required: false,
        default: 'all'
      },
      rowIndex: {
        type: 'string',
        label: 'Row Index/List (1-based)',
        placeholder: '1 or 1,3,5-8',
        required: false,
        default: '1',
        showIf: "rowSelectionMode === 'index'"
      },
      saveCsv: {
        type: 'boolean',
        label: 'Save CSV',
        required: false,
        default: true
      },
      appendCsv: {
        type: 'boolean',
        label: 'Append CSV',
        required: false,
        default: false
      },
      csvFileName: {
        type: 'string',
        label: 'CSV File Name',
        placeholder: 'table_rows.csv',
        required: false
      }
    },
    canNest: false
  },

  [ACTION_TYPES.LOOP]: {
    name: 'Loop',
    description: 'Repeats nested steps with an explicit stop condition.',
    icon: 'repeat',
    params: {
      loopMode: {
        type: 'select',
        label: 'Loop Mode',
        options: ['count', 'for_each', 'while_element_exists'],
        required: true,
        default: 'count'
      },
      iterations: {
        type: 'number',
        label: 'Repeat Count',
        placeholder: '3',
        required: true,
        default: 3,
        showIf: "loopMode === 'count'"
      },
      locators: {
        type: 'locator-list',
        label: 'Target Locators (for for_each / while)',
        required: false
      },
      forEachStrategy: {
        type: 'select',
        label: 'For-Each Target Refresh',
        options: ['snapshot_once', 'requery_each_iteration'],
        required: false,
        default: 'snapshot_once',
        showIf: "loopMode === 'for_each'"
      },
      maxIterations: {
        type: 'number',
        label: 'Safety Cap',
        placeholder: '50',
        required: false,
        default: 50
      },
      variable: {
        type: 'string',
        label: 'Legacy Variable (optional)',
        placeholder: 'items',
        required: false
      }
    },
    canNest: true
  },

  [ACTION_TYPES.SCROLL]: {
    name: 'Scroll',
    description: 'Scrolls by pixel distance (actor parity) or to a percentage position.',
    icon: 'arrows-vertical',
    params: {
      distance: {
        type: 'number',
        label: 'Distance (px)',
        placeholder: 'Positive=down, Negative=up',
        required: false,
        default: 600
      },
      percentY: {
        type: 'number',
        label: 'Vertical Scroll (%)',
        placeholder: '0-100',
        required: false,
        default: 0
      },
      percentX: {
        type: 'number',
        label: 'Horizontal Scroll (%)',
        placeholder: '0-100',
        required: false,
        default: 0
      },
      element: {
        type: 'string',
        label: 'Scroll Target',
        placeholder: 'window or element',
        required: false,
        default: 'window'
      },
      locators: {
        type: 'array',
        label: 'Element Locators',
        placeholder: 'CSS selectors for element scroll',
        required: false
      }
    }
  },

  [ACTION_TYPES.IF_CONDITION]: {
    name: 'If Condition',
    description: 'Executes different actions based on a condition.',
    icon: 'git-branch',
    params: {
      conditionType: {
        type: 'select',
        label: 'Condition Type',
        options: ['element_exists', 'text_contains'],
        required: true
      },
      locators: {
        type: 'locator-list',
        label: 'Element Locators',
        required: false,
        showIf: 'conditionType === "element_exists"'
      },
      text: {
        type: 'string',
        label: 'Text to Find',
        required: false,
        showIf: 'conditionType === "text_contains"'
      }
    },
    canNest: true,  // Can contain two branches of actions
    branches: {
      then: [],  // Actions for true condition
      else: []   // Actions for false condition
    },
    canBeNested: false  // Cannot be nested inside another if_condition
  },

  [ACTION_TYPES.AUTHENTICATE]: {
    name: 'Authenticate',
    description: 'Login flow with priority-ordered methods. The far left method is tried first. Each enabled method to the right is the next fallback.',
    icon: 'log-in',
    params: {},
    canNest: false
  },

  [ACTION_TYPES.WAIT]: {
    name: 'Wait',
    description: 'Pause for time or until an element is ready.',
    icon: 'clock',
    params: {
      waitType: {
        type: 'select',
        label: 'Wait Type',
        options: ['time', 'element'],
        required: true
      },
      time: {
        type: 'number',
        label: 'Time (ms) — 1000 ms = 1 s',
        placeholder: '1000',
        required: false,
        showIf: 'waitType === "time"'
      },
      locators: {
        type: 'locator-list',
        label: 'Element Locators',
        required: false,
        showIf: 'waitType === "element"'
      },
      timeout: {
        type: 'number',
        label: 'Timeout (ms)',
        placeholder: '5000',
        required: false,
        showIf: 'waitType === "element"'
      }
    },
    canNest: false
  },
  [ACTION_TYPES.LISTEN]: {
    name: 'Listen',
    description:
      'Wait until a condition is true. In a normal script this is one-shot ' +
      '(match once, optional On Match once, then continue — no rearm). ' +
      'Only scripts with kind=listener rearm after each trigger.',
    icon: 'ear',
    params: {
      locators: {
        type: 'locator-list',
        label: 'Element Locators',
        required: true
      },
      condition: {
        type: 'select',
        label: 'Condition',
        options: [
          'visible',
          'hidden',
          'text_contains',
          'text_equals',
          'new_child',
          'new_child_contains',
          'attribute_contains',
          'attribute_equals',
        ],
        required: true
      },
      text: {
        type: 'string',
        label: 'Text (optional)',
        placeholder: 'kittens',
        regexToggleParam: 'textUseRegex',
        required: false
      },
      textUseRegex: {
        type: 'boolean',
        label: 'Text: Use Regex',
        required: false
      },
      attribute: {
        type: 'string',
        label: 'Attribute (optional)',
        placeholder: 'class',
        regexToggleParam: 'attributeUseRegex',
        required: false
      },
      attributeUseRegex: {
        type: 'boolean',
        label: 'Attribute: Use Regex',
        required: false
      },
      captureVariable: {
        type: 'string',
        label: 'Capture Match To Variable',
        placeholder: 'listener.latestUrl',
        required: false
      },
      fanOutPerMatchedUrl: {
        type: 'boolean',
        label: 'Fan Out Per Matched URL',
        required: false,
        default: true
      },
      timeout_ms: {
        type: 'number',
        label: 'Timeout (ms)',
        placeholder: '60000',
        required: false
      },
      autoRefresh: {
        type: 'boolean',
        label: 'Auto Refresh Page',
        required: false
      },
      executionFrequencyMs: {
        type: 'number',
        label: 'Refresh Interval (ms)',
        placeholder: '60000',
        required: false
      },
      initialScrollPercentX: {
        type: 'number',
        label: 'Initial Scroll X (%)',
        placeholder: '-100..100, +right -left, 0=disabled',
        required: false,
        default: 0
      },
      initialScrollPercentY: {
        type: 'number',
        label: 'Initial Scroll Y (%)',
        placeholder: '-100..100, +down -up, 0=disabled',
        required: false,
        default: 0
      },
      refreshCycleScrollPercentX: {
        type: 'number',
        label: 'Cycle Scroll X (%)',
        placeholder: '-100..100, +right -left, 0=disabled',
        required: false,
        default: 0
      },
      refreshCycleScrollPercentY: {
        type: 'number',
        label: 'Cycle Scroll Y (%)',
        placeholder: '-100..100, +down -up, 0=disabled',
        required: false,
        default: 0
      },
      sampleIntervalMs: {
        type: 'number',
        label: 'Sample Interval (ms)',
        placeholder: '5000',
        required: false
      },
      triggerThrottleMs: {
        type: 'number',
        label: 'Trigger Throttle (ms)',
        placeholder: '5000',
        required: false
      },
      refreshJitter: {
        type: 'boolean',
        label: 'Refresh Jitter',
        required: false
      },
      extremeEvasion: {
        type: 'boolean',
        label: 'Extreme Evasion',
        required: false
      },
      onMatchScriptId: {
        type: 'string',
        widget_type: 'script-select',
        label: 'On Match: Run Script or Collection',
        required: false
      },
    },
    canNest: false
  },
  [ACTION_TYPES.GLIC_PROMPT]: {
    name: 'Glic Prompt',
    description: 'Run the autonomous agent loop on the current page using snapshot context and returned action JSON.',
    icon: 'message-circle',
    params: {
      focusAreas: {
        type: 'focus-area-list',
        label: 'Focus Elements (optional)',
        required: false
      },
      focusOnlyContext: {
        type: 'boolean',
        label: 'Focus Elements Only',
        required: false,
        default: false
      },
      model: {
        type: 'string',
        label: 'Model Override (optional)',
        placeholder: 'Leave blank to use Settings > Dashboard Agent Model',
        required: false
      },
      prompt: {
        type: 'string',
        label: 'Prompt',
        placeholder: 'Summarize what changed on this page and what I should do next.',
        multiline: true,
        rows: 8,
        required: true
      }
    },
    canNest: false
  },
  [ACTION_TYPES.CALL_SCRIPT]: {
    name: 'Call Script',
    description: 'Execute another script (e.g., login dropper)',
    icon: 'play-circle',
    params: {
      scriptId: {
        type: 'string',
        widget_type: 'script-select',
        label: 'Script ID',
        placeholder: 'uuid-of-script',
        required: true
      },
      waitFor: {
        type: 'select',
        label: 'Wait For',
        options: ['none','url','selector'],
        required: false
      },
      checkSelector: {
        type: 'string',
        label: 'Smart Login Check (CSS selector)',
        placeholder: '#profile-avatar, .user-menu',
        required: false
      }
    },
    canNest: false
  }
};

// Visual styling for timeline display
export const ACTION_STYLES = {
  defaultHeight: 40,
  branchSpacing: 20,
  indentSize: 30,
  connectorWidth: 2,
  connectorStyle: {
    normal: 'solid',
    conditional: 'dashed'
  },
  colors: {
    navigate: '#4A90E2',
    click: '#7ED321',
    hover: '#77C66E',
    drag: '#4DB6AC',
    type: '#F5A623',
    key_down: '#D8D8D8',
    key_press: '#D8D8D8',

    scrape_element: '#BD10E0',
    scrape_table: '#9013FE',
    scroll: '#50B5FF',
    loop: '#50E3C2',
    if_condition: '#D0021B',
    authenticate: '#C47B2B',
    wait: '#9B9B9B',
    glic_prompt: '#FFB347'
  },
  branchColors: {
    then: '#28a745',
    else: '#dc3545'
  }
};

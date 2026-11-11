// Example: invoke from the Simulachrome / Glic-replacement panel.
// Requires chrome.send bindings (SimulachromeAgentUI sets kWebUIBindingsPolicySet).

/**
 * Load smolvlm2 weights once (directory with *.gguf + mmproj).
 * @param {string=} modelDir defaults to ~/simulachrome_models on this machine
 */
export function loadSimulachromeModel(modelDir) {
  return new Promise((resolve) => {
    const cb = '__simulachromeLoadModelCb_' + Date.now();
    window[cb] = (result) => {
      delete window[cb];
      resolve(result);
    };
    chrome.send('load_simulachrome_model', [
      modelDir || '/Users/matcauthon/simulachrome_models',
      cb,
    ]);
  });
}

/**
 * Ask the embedded VLM to act on the active browser tab.
 * @param {string} instruction e.g. "Click the search bar"
 * @returns {Promise<{ok:boolean, error:string, action?:object}>}
 */
export function executeSimulachromeAction(instruction) {
  return new Promise((resolve) => {
    const cb = '__simulachromeActionCb_' + Date.now();
    window[cb] = (result) => {
      delete window[cb];
      resolve(result);
    };
    chrome.send('execute_simulachrome_action', [instruction, cb]);
  });
}

// One-liner from DevTools on chrome://simulachrome-agent:
//   chrome.send('execute_simulachrome_action', ['Click the search bar']);

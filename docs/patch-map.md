# Patch map

The patch series maps to these Chromium integration areas:

- **Dashboard WebUI — 3147, 3149:** `chrome/browser/resources/dashboard/` and `chrome/browser/ui/webui/dashboard/`; action editors, locators, timeline UI, recorder, script storage, and execution.
- **Glic and unified agents — 3149, 3150:** `chrome/browser/glic/`, Glic Mojo interfaces, `UnifiedAgentContextAdapter`, and `UnifiedAgentOrchestrator`.
- **Actor behavior — 3148, 3149:** `chrome/renderer/actor/`; humanized click movement and typing behavior.
- **Blink agent support — 3149:** `third_party/blink/renderer/core/agent/`; context, state extraction, action dispatch, decoding, vectorization, and agent loop.
- **Headless and development tooling — 3149:** `headless_listener_host.py` and `chrome/test/data/webui/glic/test_client/`.

After applying and building Chromium, the intended development entry point is:

```bash
chrome/test/data/webui/glic/test_client/start_glic_dev.sh
```

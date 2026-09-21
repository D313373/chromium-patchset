# Patchset guide

## Purpose

This repository is the Chromium integration layer for Simulachrome. It packages browser, renderer, Blink, WebUI, GN, and development-tool changes for an external Chromium checkout.

## Revision contract

- `BASE_COMMIT` is the expected starting Chromium revision.
- `HEAD_COMMIT` records the target marker from the source patch workflow.
- Apply patches in order: `3147`, `3148`, `3149`, `3150`.
- Patches 3147–3149 are mail-formatted; 3150 is a raw unified diff.

## Feature map

| Feature | Patches | Representative Chromium paths |
|---|---|---|
| Dashboard WebUI | 3147, 3149 | `chrome/browser/resources/dashboard/`, `chrome/browser/ui/webui/dashboard/` |
| Script actions and locators | 3147, 3149 | `actions/*`, `locator_resolver.*`, `script_executor.*`, `script_schema.*` |
| Recording and playback | 3147, 3149 | `recorder_controller.*`, `recorder_agent_impl.*`, `script_execution_service.*` |
| Glic integration | 3149, 3150 | `chrome/browser/glic/`, Glic Mojo interfaces |
| Unified agent | 3150 | `unified_agent_context_adapter.*`, `unified_agent_orchestrator.*` |
| Actor behavior | 3148, 3149 | `chrome/renderer/actor/` |
| Blink agent support | 3149 | `third_party/blink/renderer/core/agent/` |
| Headless/dev tooling | 3149 | `headless_listener_host.py`, `chrome/test/data/webui/glic/test_client/` |

## Development entry point

Once the patchset is applied and Chromium is built, run the intended Glic development client from the Chromium checkout:

```bash
chrome/test/data/webui/glic/test_client/start_glic_dev.sh
```

This launcher is part of the patched Chromium tree. The adjacent `start_glic_dev_tmux.sh` and `start_proxy.sh` scripts support related development workflows.

## Scope boundary

This repository owns Chromium revisions, ordered patches, compatibility notes, and patch tooling. The main Simulachrome repository owns product-level architecture, demos, examples, external services, and user-facing workflows.

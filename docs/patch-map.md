# Patch map

This document maps the patch series to the major Chromium integration areas.

## Dashboard WebUI

Patches 3147 and 3149 add and evolve the dashboard surface under `chrome/browser/resources/dashboard/` and `chrome/browser/ui/webui/dashboard/`. The resource side includes action editors, locator editing, dashboard state, timeline rendering, debugging views, and UI bootstrap code. The browser side includes action factories, action implementations, WebUI handlers, recorder support, script storage, schema handling, and execution services.

## Recording, scripts, and playback

The dashboard script model is represented by JavaScript state/persistence modules and browser-side components such as `ScriptSchema`, `ScriptStore`, `RecorderController`, and `ScriptExecutionService`. Patch 3149 exposes Glic Mojo methods for listing scripts, starting and stopping runs, controlling recording, saving recordings, appending recordings, and capturing locators.

## Glic and unified agents

Patches 3149 and 3150 connect dashboard automation to Glic. Patch 3150 adds `UnifiedAgentContextAdapter`, `UnifiedAgentOrchestrator`, and unified agent types. Context capture uses Glic tab context and annotated page content, converts page-map elements into locator records, and maintains bounded run/trace state.

## Actor and renderer behavior

Patch 3148 changes Chromium actor playback behavior. It adds the `kGlicActorHumanizedClick` feature and parameters for movement steps, jitter, offsets, micro-moves, and delay. `chrome/renderer/actor/click_dispatcher.cc` uses eased mouse paths before the click, and `type_tool.cc` changes how empty text and deletion are handled.

## Blink agent support

Patch 3149 adds agent-related files under `third_party/blink/renderer/core/agent/`, including context, state extraction, action dispatch, decoding, vectorization, and the Blink agent loop. These changes provide renderer-side support for the agent model used by the dashboard/Glic integration.

## Headless and development tooling

Patch 3149 adds headless listener behavior to Chromium startup and includes `headless_listener_host.py`, Glic test-client scripts, an Ollama client, payload debugging, sanitization tests, and startup helpers. These are development-oriented components rather than a general-purpose release packaging system.

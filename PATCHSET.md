# Patchset guide

## Purpose

This repository is the Chromium integration layer for Simulachrome. It packages changes that must be applied to an external Chromium checkout while keeping the upstream source out of this repository.

The patch series spans browser C++, renderer/Blink C++, Chromium WebUI JavaScript/CSS, GN build definitions, and development/test tooling. The main Simulachrome repository remains the place for product-level documentation, examples, clients, and workflows.

## Revision contract

- `BASE_COMMIT` identifies the expected starting Chromium revision.
- `HEAD_COMMIT` records the target marker from the source patch workflow.
- Patch files are applied in numeric order: `3147` … `3156`.
- Patches `3147`–`3149` and `3151`–`3153` are mail-formatted patches; `3150` is a raw unified diff; `3154`–`3156` are applied from `overlays/`.

The application script deliberately checks for a clean checkout and an exact base revision. This prevents silently applying the series to an incompatible Chromium tree.

## Feature map

| Feature | Patches | Representative paths in the Chromium checkout |
|---|---|---|
| Dashboard WebUI | 3147, 3149 | `chrome/browser/resources/dashboard/`, `chrome/browser/ui/webui/dashboard/` |
| Script actions and locators | 3147, 3149 | `actions/*`, `locator_resolver.*`, `script_executor.*`, `script_schema.*` |
| Recording and playback | 3147, 3149 | `recorder_controller.*`, `recorder_agent_impl.*`, `script_execution_service.*` |
| Glic integration | 3149, 3150 | `chrome/browser/glic/`, Glic Mojo interfaces and page handlers |
| Unified agent orchestration | 3150 | `unified_agent_context_adapter.*`, `unified_agent_orchestrator.*`, `unified_agent_types.*` |
| Actor behavior | 3148, 3149 | `chrome/renderer/actor/`, actor policy and tool utilities |
| Blink agent support | 3149 | `third_party/blink/renderer/core/agent/` |
| Headless automation | 3149 | `headless_listener_host.py`, headless listener switches |
| Glic development client | 3149, 3154 | `chrome/test/data/webui/glic/test_client/` |
| Script export / secrets | 3152, 3153 | dashboard script export + secret migration helpers |
| Page-map / native navigation | 3151 | script vs agent page-map split, native directory nav |
| Product defaults / packaged guest | 3156 | `simulachrome_startup.*`, `simulachrome_agent/`, Local provider, chrome:// guest |

## Patch statistics

The following figures come from the patch headers and are per-patch, not a deduplicated total:

- Patch 3147: 127 files changed, 35,491 insertions
- Patch 3148: 12 files changed, 195 insertions, 10 deletions
- Patch 3149: 214 files changed, 40,676 insertions, 5,360 deletions
- Patch 3150: a raw revision delta; its file count is not included in the patch header
- Patches 3151–3153: follow-on mail-formatted feature patches
- Patch 3154: full-file overlay under `overlays/3154/` (preferred over the placeholder hunk patch)
- Patch 3155: full-file overlay under `overlays/3155/` (auto-heal Anthropic + launch dir bootstrap)
- Patch 3156: full-file overlay under `overlays/3156/` (bake Glic defaults + packaged agent UI)

Later patches modify files introduced by earlier patches, so summing these figures will overcount the final tree.

## Reproduction workflow

1. Follow Chromium's official checkout instructions using `depot_tools`.
2. Check out the revision in `BASE_COMMIT`.
3. Confirm the Chromium worktree is clean.
4. Run `scripts/apply-patches.sh --chromium-dir <path>`.
5. Configure and build Chromium using the normal GN/Ninja workflow for the target platform.
6. Follow `docs/GETTING_STARTED.md` and run `start_glic_dev.sh` (creates profile/Scripts/Headless automatically).
7. Use the Simulachrome repository for product-level runtime instructions and examples.

## Failure recovery

If `git am` stops on a conflict, inspect the conflict and either resolve it or abort with:

```bash
git am --abort
```

If patch 3150 fails, return the checkout to the pre-application state before retrying. Prefer a disposable Chromium clone or Git worktree when experimenting with revisions other than the recorded base.

## Scope boundary

### This repository owns

- Chromium revision metadata
- Ordered integration patches
- Patch application and verification helpers
- Chromium-specific compatibility documentation

### Simulachrome owns

- Product-level architecture and user documentation
- Demo scripts and examples
- External services and test clients
- Release packaging and user-facing workflows

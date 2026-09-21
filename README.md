# Simulachrome Chromium Patchset

Chromium integration patches for [Simulachrome](https://github.com/D313373/Simulachrome).

This repository intentionally does not vendor Chromium or the complete Simulachrome project. It records the Chromium revisions, ordered patches, compatibility notes, and helper scripts needed to reproduce the Simulachrome-enabled Chromium integration in a separate Chromium checkout.

## What this contains

- Ordered Chromium patches under [`patches/`](patches/)
- Expected revisions in [`BASE_COMMIT`](BASE_COMMIT) and [`HEAD_COMMIT`](HEAD_COMMIT)
- A machine-readable inventory in [`patchset.json`](patchset.json)
- Patch application and verification helpers under [`scripts/`](scripts/)
- Feature documentation in [`PATCHSET.md`](PATCHSET.md) and [`docs/patch-map.md`](docs/patch-map.md)

Chromium source is intentionally external; the full Simulachrome project is maintained separately.

## Compatibility

| Item | Value |
|---|---|
| Chromium base revision | `223dfbac1c7542a06b422390d954afe5b560b607` |
| Patchset target marker | `836f788f2d7db` |
| Patch order | `3147 -> 3148 -> 3149 -> 3150` |
| Build system | Chromium GN/Ninja |
| Status | Development patchset |

## Patch inventory

1. **3147** — Dashboard WebUI resources, action editors, recorder, script execution, and locator support.
2. **3148** — Actor/dashboard playback, humanized clicks, Glic feature flags, and typing behavior.
3. **3149** — Dashboard/Glic Phase A/B, recording and playback APIs, Blink agent support, headless tooling, and test-client support.
4. **3150** — Chromium m144-specific unified-agent and compatibility delta.

## Applying the patchset

```bash
./scripts/apply-patches.sh --chromium-dir ../chromium
```

The script checks that the Chromium checkout is clean and at the expected base revision, applies patches in numeric order, and stops on failure. Use a disposable checkout or Git worktree when experimenting.

To validate metadata and patch applicability without modifying Chromium:

```bash
./scripts/verify-patches.sh
./scripts/verify-patches.sh --chromium-dir ../chromium
```

## Running the Simulachrome development client

After applying the patchset and building the patched Chromium, the intended development entry point is the checked-in Glic test client:

```bash
chrome/test/data/webui/glic/test_client/start_glic_dev.sh
```

Run that command from the Chromium checkout. The script is part of the patched Chromium tree and is the primary development/test-client launcher for this integration. Related helpers include `start_glic_dev_tmux.sh` and `start_proxy.sh` in the same directory.

## Building

Use Chromium's normal GN/Ninja workflow for the target platform. Product-level runtime setup, examples, and external services belong in the main Simulachrome repository.

## Related repositories

- [Simulachrome](https://github.com/D313373/Simulachrome) — product code, tooling, examples, and documentation
- [Chromium checkout instructions](https://www.chromium.org/developers/how-tos/get-the-code/)

## Known limitations

Patch compatibility is tied to the recorded Chromium revision. This repository contains Chromium integration work, not every file or tool in the Simulachrome product.

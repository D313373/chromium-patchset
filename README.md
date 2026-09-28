# Simulachrome Chromium Patchset

Chromium integration patches for [Simulachrome](https://github.com/D313373/Simulachrome).

**Demo:** [`docs/media/simulachrome_demo.mp4`](docs/media/simulachrome_demo.mp4): the agent signing in and filing a support request with Data-tab tokens, saving the run as a script, then replaying it from the dashboard with zero model calls.

**Prebuilt macOS (Intel) binary:** see [Releases](https://github.com/D313373/chromium-patchset/releases).

This repository intentionally does **not** vendor Chromium or the complete Simulachrome project. It records the Chromium revisions, ordered patch series, compatibility notes, and helper scripts needed to reproduce the Simulachrome-enabled Chromium integration in a separate Chromium checkout.

## What this repository contains

- Ordered Chromium patch files under [`patches/`](patches/) (3147–3155)
- Full-file overlays under [`overlays/`](overlays/) for patches 3154–3156
- The expected Chromium base and target revisions in [`BASE_COMMIT`](BASE_COMMIT) and [`HEAD_COMMIT`](HEAD_COMMIT)
- A machine-readable patch inventory in [`patchset.json`](patchset.json)
- Patch application and metadata validation scripts under [`scripts/`](scripts/)
- Feature-level documentation in [`PATCHSET.md`](PATCHSET.md) and [`docs/patch-map.md`](docs/patch-map.md)

## What it does not contain

- The Chromium source tree
- A standalone browser binary
- The full Simulachrome product repository
- A complete dependency mirror or release artifact

Keeping Chromium external avoids duplicating a very large upstream repository and makes the integration boundary explicit.

## Compatibility

| Item | Value |
|---|---|
| Chromium base revision | `223dfbac1c7542a06b422390d954afe5b560b607` |
| Patchset target marker | `836f788f2d7db` |
| Patch order | `3147 -> … -> 3155` |
| Expected build system | Chromium GN/Ninja |
| Repository status | Development patchset |

The target marker is recorded exactly as supplied by the source checkout and is abbreviated. Confirm compatibility against the intended Chromium branch before applying the series.

## Patch inventory

1. **3147 — Add dashboard WebUI resources**: dashboard resources, action editors, WebUI plumbing, recorder, script execution, and locator support.
2. **3148 — Simulachrome actor + dashboard playback**: actor/dashboard playback integration, Glic feature flags, humanized click input, and typing behavior changes.
3. **3149 — Simulachrome dashboard + Glic Phase A/B**: dashboard automation APIs, recording and playback integration, Glic page handlers, Blink agent support, headless tooling, and test-client support.
4. **3150 — Simulachrome m144 apply delta**: revision-specific changes including unified Glic agent context capture/orchestration and removal of an older recorder binding.
5. **3151 — Script/agent page-map split + native nav**: separates script vs agent page-map pipelines and restores native directory navigation.
6. **3152 — Script export PII/credential guard**: scans exports for credentials/PII before sharing.
7. **3153 — Export secret auto-migrate**: suggests secret names and one-click migrates sensitive literals.
8. **3154 — Glic coordinate click + screenshot grid**: coordinate-only click/type/hover, CSS-pixel grid screenshots with CTA hotspot labels, and destination-tab rebind recovery. Applied via [`overlays/3154/`](overlays/3154/).
9. **3155 — Sync running tip**: auto-heal Anthropic provider/model wiring, model alias cleanup, and `start_glic_dev.sh` profile/dir bootstrap. Applied via [`overlays/3155/`](overlays/3155/).

See [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) for a first-run walkthrough and [`PATCHSET.md`](PATCHSET.md) for the detailed feature map.

## Prerequisites

- Chromium's official `depot_tools` checkout and `fetch` workflow
- Git
- A Chromium checkout at the revision recorded in `BASE_COMMIT`
- The normal Chromium build dependencies for your platform

## Applying the patchset

From the patchset repository, create or use a separate Chromium checkout and run:

```bash
./scripts/apply-patches.sh --chromium-dir ../chromium
```

The script verifies that the Chromium checkout is clean and at the expected base revision, applies patches in numeric order, and stops on the first failure. It uses `git am` for mail-formatted patches 3147–3149 and 3151–3153, `git apply` for the raw delta 3150, and copies `overlays/3154/` plus `overlays/3155/` for the Glic client and running-tip sync.

To validate the repository without a Chromium checkout:

```bash
./scripts/verify-patches.sh
```

To validate patch application without changing the Chromium checkout, use a disposable clone or worktree and run:

```bash
./scripts/verify-patches.sh --chromium-dir ../chromium
```

## First run after applying

Follow [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md). Short version:

```bash
# after build
chrome/test/data/webui/glic/test_client/start_glic_dev.sh
```

Chromium creates `DevProfile/`; the script creates `Scripts/` and `Headless/` (override root with `SIMULACHROME_HOME`). Secrets and saved scripts are created later through the Dashboard UI — they are intentionally not vendored here.

## Building patched Chromium

After applying the patches, use the standard Chromium GN/Ninja workflow for the target platform. This repository does not prescribe a single `args.gn` configuration because the appropriate Glic, branding, platform, and build flags depend on the development environment. Consult the main Simulachrome repository for product-level setup, test clients, and runtime examples.

## Related repositories

- [Simulachrome](https://github.com/D313373/Simulachrome) — product-level code, tooling, examples, and documentation
- [Chromium](https://www.chromium.org/developers/how-tos/get-the-code/) — upstream source and build instructions

## Known limitations

- Patch compatibility is tied to the recorded Chromium revision and may require conflict resolution on other milestones.
- Full Chromium compilation is intentionally not performed by the lightweight metadata validation script.
- The patchset contains Chromium integration work, not every file or tool in the Simulachrome product.

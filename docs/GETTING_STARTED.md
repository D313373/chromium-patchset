# Getting started

This guide takes you from a clean Chromium checkout to a Simulachrome-enabled build that matches the tip recorded in this repository.

## What you need

1. [depot_tools](https://www.chromium.org/developers/how-tos/depottools/)
2. A Chromium checkout at the revision in [`BASE_COMMIT`](../BASE_COMMIT)
3. This patchset repository
4. Platform build dependencies for Chromium

You do **not** need to manually create Simulachrome folders. Chromium creates the profile (`DevProfile`); `start_glic_dev.sh` creates `Scripts/` and `Headless/` on first run.

## 1. Apply the patchset

```bash
./scripts/apply-patches.sh --chromium-dir /path/to/chromium/src
```

This applies patches `3147`–`3153`, then copies overlays `3154`–`3156` (Glic client + running-tip sync).

Validate metadata any time with:

```bash
./scripts/verify-patches.sh
```

## 2. Build Chromium

From the Chromium `src` directory, use your normal GN/Ninja configuration, then build at least:

```bash
autoninja -C out/Default chrome
autoninja -C out/Default chrome/browser/resources/dashboard:resources
autoninja -C out/Default chrome/test/data/webui/glic:generate_test_files
```

Exact `args.gn` flags depend on your platform; enable Glic-related features as you would for local Glic development.

## 3. Launch the Simulachrome / Glic environment

After overlay 3156, a Release/Default `Chromium.app` applies product defaults in C++
(`simulachrome::ApplyBrowserCommandLineDefaults`): Glic features, open-on-startup,
occlusion/timer/visible-widget switches, Scripts/Headless under `SIMULACHROME_HOME`,
and guest URL `chrome://simulachrome-agent/…` (packaged). Remote debugging stays off
unless Settings enables it. `start_glic_dev.sh` remains useful for packaging the
agent UI into Resources and optional localhost/`--glic-dev` workflows
(`USE_LOCALHOST_GUEST=1`).

## 3b. Dev launcher (optional)

```bash
chrome/test/data/webui/glic/test_client/start_glic_dev.sh
```

(from the Chromium `src` tree, after the patches are applied)

### Directories created automatically

On macOS the defaults are under `~/Library/Application Support/Simulachrome/`:

| Path | Purpose | Created by |
|---|---|---|
| `DevProfile/` | Chromium `--user-data-dir` | Chromium on first launch |
| `DevProfile/Default/SimulachromeData/` | secrets, scrapes, agent run artifacts | Dashboard / script code on first use |
| `Scripts/` | dashboard scripts + `global_settings.json` | `start_glic_dev.sh` |
| `Headless/` | headless listener profile + logs | `start_glic_dev.sh` |

Override the root with `SIMULACHROME_HOME` if you want a different location.

### What is *not* shipped (and should not be)

- API keys / `secrets.ini` — add via Dashboard **Data → Secrets** (e.g. `ANTHROPIC_API_KEY`)
- Your saved scripts and `global_settings.json` — created when you save in the Dashboard
- Browser cookies / logins — live only in `DevProfile`

A first-run profile is empty until you configure provider/model in **Settings** and secrets in **Data**.

## 4. Minimal first-run checklist

1. Launch with `start_glic_dev.sh`
2. Open the Dashboard / Glic UI
3. Settings → provider **Anthropic** (or Local OpenAI-compatible) → choose a model → Save
4. Data → Secrets → set `ANTHROPIC_API_KEY` (for hosted providers)
5. Run a Glic task or enable auto-heal and exercise a failing script

## Related docs

- [`PATCHSET.md`](../PATCHSET.md) — feature map and revision contract
- [`docs/patch-map.md`](patch-map.md) — which patches touch which areas
- [Simulachrome](https://github.com/D313373/Simulachrome) — product-level docs and examples

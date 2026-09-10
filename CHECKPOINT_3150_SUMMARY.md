# Checkpoint 3150 — Agent context starvation + listeners

**Date:** 2026-09-10  
**Base:** after patch `3149-Simulachrome-dashboard-glic-phase-A-B`  
**Local tree tip (pre-git-repair):** `c45811bdfc8a1` + uncommitted WIP  

## Why this checkpoint

This is a packaged stopping point after the Moonspin headed-SPA starvation investigation and related agent/listener work. Prefer a product build (`autoninja -C out/Default chrome`) after applying; schedule a full `all` build only when disk has ≥50GB free.

## Summary of work since 3149

### Page load / paint starvation (critical)
- Stopped `multi_source_page_context_fetcher` from **forcing** `on_critical_path = true` on every APC fetch (honors caller).
- Glic page-context fetch stays **off** the renderer critical path; APC limited to **same-site only** for agent/guest context.
- Failure-replay `CopyFromSurface` (screenshot ring buffer) is **opt-in** via Settings → Performance → “Failure replay capture” (or `--enable-dashboard-failure-replay`). Cap lowered to **≤480×270**.
- Root cause of “navigate fine, splash stuck for minutes / ~1fps” was compositor + critical-path APC contention, not navigate waits.

### Agent context (text-only / local Ollama)
- Dashboard page-map options: no viewport screenshots, no innerText HTML crawl.
- Agent page-map trimmed to accessibility/interactive nodes (no CSS/XPath dump).
- Hard-block encoded images for non-vision models in `glic_prompt_action`.
- Glic guest `ollama_client`: no screenshots / no innerText; throttle load-wait context polling.
- Prompt contract: don’t type credentials unless login UI is in current context; don’t auto-refresh APC between form fields.

### Listeners / headless / startup
- Native listener-watch plumbing (`listener_watch.mojom` + registry + controller).
- Headless listener host / prewarm / force-poll refresh paths.
- Startup agent launcher + settings for auto-start Moonspin-style tasks, long model step timeout.
- Script collections / auto-heal / model prompt contract JS + C++ unit tests.

### Build hygiene found during full `all`
- Fixed `attempt_form_filling_tool_browsertest` mock of removed `EnqueueFollowupAction`.
- Fixed broken AX cherry-pick test (`RadioButtonsInGroup` protected + missing includes).
- Full `all` aborted after ~6h when disk filled; product binary was still stale until a `chrome` rebuild.

## Cadence going forward

Every **5–10** meaningful change sets:
1. Stop at a coherent feature package (like this checkpoint).
2. `autoninja -C out/Default chrome` (always).
3. When disk allows (≥50GB free): `autoninja -C out/Default -k 0` once.
4. Export the next numbered patch into `chromium-patchset`, push a branch + short summary, then continue.

## Disk note

`IterativeDevelopment-OldBuilds` (~131GB) was removed to free space. This tree’s `.git` was a **worktree** pointing into that folder, so **local git metadata is broken** until reattached to a new object store. Working tree files are intact. Repair before further commits:

```bash
# Proposed repair (keeps working tree):
# init/fetch M144 baseline, update-ref + read-tree, then commit current overlay.
```

## Not included / leave alone

- `~/0llamaModels` (~152GB) — model weights, keep unless you explicitly want them pruned.
- Current `out/Default` (~140GB) — needed for incremental builds.

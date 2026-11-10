# Checkpoint 3158 — Request action + traffic capture

**Date:** 2026-11-09
**Base:** after `3155-Sync-running-tip-autoheal-anthropic-launch-dirs`
**Apply:** `overlays/3158/` (see `APPLY.txt`)

## Summary

First-class HTTP request replay in Simulachrome scripts, plus network capture during recording.

### Request action
- Canonical `request` (aliases: `fetch`, `http`, `http_request`, `xhr`)
- Page-context `fetch` with `credentials: 'include'`
- Params: method, url, headers/headersJson, body, captureAs, persistToData, assertStatus, timeoutMs
- Token expansion via existing ScriptExecutionService `{Token}` / `{{Token}}` pass

### Traffic capture
- CDP `Network.enable` during explicit recording
- Bounded ring buffer; skips image/font/css/media
- Lazy response body fetch on promote/inspect

### UI
- Dashboard Traffic panel: list + **Add as request**
- Secret values rewritten to `{TokenName}` when matched in secrets.ini

## Not included
- Windows-only adaptations (merge from Windows branch when pushed)
- Browser-process URLLoader mode, HAR, repeater/intruder, JS/WASM analysis

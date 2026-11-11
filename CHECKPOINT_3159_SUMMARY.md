# Checkpoint 3159 — Windows ScriptsDir fix onto 3158

**Date:** 2026-11-11
**Base:** after merge of `windows-patches` onto main (incl. 3158)
**Apply:** `overlays/3159/` (see APPLY.txt)

## Summary
Merged `windows-patches` into `main` (3156/3157 + Windows port). 3158’s
full-file `dashboard_ui_message_handler.cc` would overwrite the Windows
`ScriptsDir` PathService fix. 3159 rebases that fix onto the 3158 handler.

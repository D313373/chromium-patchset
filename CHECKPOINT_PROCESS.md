# Simulachrome patch checkpoint cadence

Use this when iterating on the M144 overlay.

1. Land 5–10 related changes as one coherent package (not mid-spike).
2. Always product-build: `autoninja -C out/Default chrome`
3. Full tree build only when disk has ≥50GB free: `autoninja -C out/Default -k 0`
4. Export the next `patches/NNNN-*.patch` into this repo.
5. Push a branch + short summary (and open a PR into `main` when ready).
6. Then continue the next spike.

Prefer fixing compile breakages discovered by `chrome`/`all` before starting the next feature package.

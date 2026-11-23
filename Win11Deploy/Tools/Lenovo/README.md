# Lenovo tools

Prefer **Thin Installer** for offline USB imaging:

1. Download Thin Installer from Lenovo Support / commercial tools.
2. Place `ThinInstaller.exe` (and its accompanying files) under this folder.
3. Optional: populate `Repository\` with packages from **Update Retriever** for
   your ThinkPad machine types so firmware/BIOS/driver updates apply offline.

`Install-Firmware.ps1` runs:

```text
ThinInstaller.exe /CM -search A -action INSTALL -includerebootpackages 1 -noicon -noreboot -repository <Repository>
```

## Fallback: System Update

If Thin Installer is absent, place a Lenovo **System Update** installer (`.exe`/`.msi`)
here. The script installs it silently, then runs a similar `/CM -search A -action INSTALL`
pass (requires network unless you configure a local repo separately).

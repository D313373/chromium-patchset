# Dell tools

Place the **Dell Command Update** Windows installer here (`.exe` or `.msi`).

`Install-Firmware.ps1` looks for a filename matching `Dell-Command-Update`, `Command_Update`, or `DCU`, installs it silently if needed, then runs `dcu-cli.exe`.

Download from Dell Support (search “Dell Command \| Update”). Prefer the Universal Windows Platform / Windows version that includes the CLI.

Optional: you may also keep a local catalog here; the default flow uses DCU’s online catalog when the machine has network after drivers are installed.

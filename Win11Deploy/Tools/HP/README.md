# HP tools

Unpack **HP Image Assistant (HPIA)** so that `HPImageAssistant.exe` exists somewhere under this folder (root or a subfolder).

`Install-Firmware.ps1` runs HPIA silently with BIOS/Firmware/Driver categories.

## Offline SoftPaqs (optional)

Create `SoftPaqs\` under this folder and populate it with SoftPaqs downloaded via HPIA “download only” or SoftPaq Download Manager. When present, the script passes `/SoftPaqDownloadFolder` so installs can proceed with less (or no) live download traffic.

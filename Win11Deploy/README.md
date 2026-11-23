# Win11 Pro Model-Aware Install USB (Dell + HP + Lenovo)

One Rufus-built Windows 11 Pro USB that autodetects Dell, HP, or Lenovo ThinkPad model, installs the matching offline driver pack with `pnputil`, then runs the vendor firmware tool (DCU / HPIA / Thin Installer).

Build and maintain this on a **Windows PC**. The Mac is not used for imaging.

## Layout

```
Win11Deploy/
  Autounattend.xml          # Win11 Pro, UEFI/GPT, wipes Disk 0
  models.json               # model → driverPath / firmwareTool map
  Scripts/
    Common.ps1
    Detect-Hardware.ps1
    Install-Drivers.ps1
    Install-Firmware.ps1
    Run-ModelAwareSetup.ps1
    SetupComplete.cmd       # first-boot entry (also staged under $OEM$)
  Drivers/
    Dell/<ModelFolder>/     # extracted INF trees / CABs
    HP/<ModelFolder>/
    Lenovo/<ModelFolder>/   # ThinkPad INF trees
  Tools/
    Dell/                   # Dell Command Update installer
    HP/                     # HP Image Assistant (+ optional SoftPaqs/)
    Lenovo/                 # Thin Installer (+ optional Repository/)
  Build/
    Build-USB.ps1           # copy payload + validate packs onto USB
```

## How it works

1. Rufus writes the official Windows 11 ISO to USB (GPT/UEFI).
2. `Build-USB.ps1` copies this payload onto the stick and stages  
   `sources\$OEM$\$$\Setup\Scripts\SetupComplete.cmd`.
3. Windows Setup installs **Windows 11 Pro** using `Autounattend.xml`.
4. After specialize, `SetupComplete.cmd` finds the still-attached USB (looks for `models.json` + `Drivers`), then runs `Run-ModelAwareSetup.ps1`.
5. Detection uses WMI:
   - **Dell:** `Win32_ComputerSystem.Model` (e.g. `Latitude 5440`), optional SKU.
   - **HP:** product / board product first (e.g. `6F5Y2UT#ABA`), then model patterns.
   - **Lenovo ThinkPad:** `Win32_ComputerSystemProduct.Version` (e.g. `ThinkPad T14 Gen 3`) and/or machine type (`21AH*`) on Model/Product.
6. Matched pack → `pnputil /add-driver ... /subdirs /install`.
7. Firmware → silent **DCU** (Dell), **HPIA** (HP), or **Thin Installer / System Update** (Lenovo). A `RunOnce` resume is registered if a reboot is needed.

Unmatched hardware logs to `C:\Windows\Setup\Scripts\model-detect.log` and applies **nothing** (no wrong drivers).

```text
USB → Setup + Autounattend → OOBE/specialize → SetupComplete
  → Detect-Hardware → models.json → pnputil → DCU / HPIA / Lenovo
```

## Prerequisites

- Windows PC
- Windows 11 Pro ISO ([Microsoft](https://www.microsoft.com/software-download/windows11))
- [Rufus](https://rufus.ie/) (latest)
- USB stick **32 GB+** (64 GB+ recommended once many model packs are present)
- Dell CAB / HP SoftPaq / Lenovo driver packs for each model
- Dell Command Update installer under `Tools\Dell\`
- HP Image Assistant under `Tools\HP\`
- Lenovo Thin Installer (preferred) or System Update under `Tools\Lenovo\`

## Build the USB

### 1. Write the ISO with Rufus

1. Open Rufus → select the USB → select the Win11 ISO.
2. **Partition scheme:** GPT  
3. **Target system:** UEFI (non CSM)  
4. Do **not** enable Windows To Go.  
5. Start. If Rufus warns about a large `install.wim`, accept NTFS (or split WIM) as prompted.

### 2. Fill driver packs and tools

See vendor sections below. Place tools per `Tools\*\README.md`.

### 3. Sanity-check model matching (optional)

```powershell
cd C:\path\to\Win11Deploy\Build
.\Test-ModelMatch.ps1
```

### 4. Copy payload

From an elevated PowerShell:

```powershell
cd C:\path\to\Win11Deploy\Build
.\Build-USB.ps1 -UsbDrive E:
```

Validation fails if a `models.json` entry points at a missing folder or a folder with zero `.inf` files, or if vendor tool binaries are missing. To stage an incomplete tree while testing:

```powershell
.\Build-USB.ps1 -UsbDrive E: -SkipValidation
```

### 5. Install on a laptop

1. Leave the USB plugged in through first boot.
2. Boot the target machine from USB (UEFI).
3. **Autounattend wipes Disk 0** — confirm in firmware that Disk 0 is the internal drive, not another disk you care about.
4. Default local admin from Autounattend: `Admin` / `ChangeMe!23` — change immediately.
5. Review `C:\Windows\Setup\Scripts\model-detect.log`.

## models.json

Each entry:

| Field | Purpose |
|--------|---------|
| `id` | Stable id for logs |
| `manufacturer` | `Dell`, `HP`, or `Lenovo` |
| `match.modelPatterns` | Wildcards against `Win32_ComputerSystem.Model` |
| `match.skuPatterns` | Optional SKU wildcards |
| `match.productPatterns` | HP product / Lenovo product wildcards |
| `match.boardProductPatterns` | HP `Win32_BaseBoard.Product` |
| `match.versionPatterns` | Lenovo friendly name (`Win32_ComputerSystemProduct.Version`) |
| `match.machineTypePatterns` | Lenovo MTM prefix (e.g. `21AH*`) against Model/Product/Sku |
| `driverPath` | Relative path under the USB root |
| `firmwareTool` | `dcu`, `hpia`, or `lenovo` |

Read identity on a sample laptop:

```powershell
Get-CimInstance Win32_ComputerSystem | Select Manufacturer, Model, SystemSKUNumber
Get-CimInstance Win32_ComputerSystemProduct | Select Name, Version, IdentifyingNumber
Get-CimInstance Win32_BaseBoard | Select Manufacturer, Product
```

## Adding a Dell model

1. On support.dell.com (or Dell Command | Configure / driver CAB catalog), download the Windows 11 **CAB** or driver pack for that Exact Model Name.
2. Extract so INF files live under e.g. `Drivers\Dell\Latitude_5440\` (subfolders OK).
3. Keep packs lean: chipset, storage, NIC, GPU, audio, dock. Skip bloatware/apps.
4. Add a `models.json` entry with `modelPatterns` matching `Win32_ComputerSystem.Model` exactly when possible (`Latitude 5440`).
5. Set `"firmwareTool": "dcu"`.
6. Place/update the DCU installer in `Tools\Dell\`.
7. Re-run `Build-USB.ps1`.

## Adding an HP model

1. Prefer matching by **product number** (on the BIOS sticker / `Win32_ComputerSystemProduct.Name`), not marketing name alone.
2. Use SoftPaq Download Manager or HPIA “download only” for that product.
3. Extract SoftPaqs that contain drivers into e.g. `Drivers\HP\EliteBook_840_G9\` (INF trees; subfolders OK).
4. Add `models.json` with `productPatterns` (e.g. `6F5Y2UT*`) and optional `modelPatterns`.
5. Set `"firmwareTool": "hpia"`.
6. Unpack HPIA into `Tools\HP\` (must contain `HPImageAssistant.exe`). Optional offline cache: `Tools\HP\SoftPaqs\`.
7. Re-run `Build-USB.ps1`.

## Adding a Lenovo ThinkPad

1. On a sample unit, record `Version` (friendly name) and Model/Product (often the full MTM like `21AH004HUS`).
2. Download the Windows 11 drivers for that machine type from Lenovo Support (or Update Retriever) and extract INF trees into e.g. `Drivers\Lenovo\ThinkPad_T14_G3\`.
3. Add a `models.json` entry, for example:

```json
{
  "id": "lenovo-thinkpad-t14-g3",
  "manufacturer": "Lenovo",
  "match": {
    "versionPatterns": ["ThinkPad T14 Gen 3*"],
    "machineTypePatterns": ["21AH*", "21AK*"],
    "modelPatterns": ["ThinkPad T14 Gen 3*"]
  },
  "driverPath": "Drivers\\Lenovo\\ThinkPad_T14_G3",
  "firmwareTool": "lenovo"
}
```

4. Place **Thin Installer** under `Tools\Lenovo\` (preferred). Optionally fill `Tools\Lenovo\Repository\` via Update Retriever for offline BIOS/firmware.
5. Or place a System Update installer as a network-dependent fallback.
6. Re-run `Build-USB.ps1`.

## Critical storage/NIC drivers (optional)

If a model cannot complete Setup without a storage or NIC driver, inject **only that model’s** critical drivers into `boot.wim` / `install.wim` with DISM — do not inject all packs into the WIMs. The first-boot path remains the default for everything else.

## Security notes

- Change the Autounattend `Admin` password before production use (or remove AutoLogon and create accounts in OOBE).
- No domain join, Autopilot, or BitLocker pre-provisioning is configured.
- Firmware updates need AC power; BIOS flashes reboot the machine.

## Troubleshooting

| Symptom | Check |
|---------|--------|
| No drivers applied | `model-detect.log` — unmatched model? USB removed too early? |
| `deploy USB not found` | Stick unplugged before SetupComplete; marker files missing on root |
| pnputil warnings | Some INFs already present — often OK; read log |
| Vendor tool missing | `Tools\Dell`, `Tools\HP\HPImageAssistant.exe`, or `Tools\Lenovo\ThinInstaller.exe` |
| ThinkPad unmatched | Log `Version` + Model; add `versionPatterns` / `machineTypePatterns` |
| Wrong edition | ISO must include **Windows 11 Pro**; Autounattend selects `/IMAGE/NAME = Windows 11 Pro` |
| Disk wiped unexpectedly | Autounattend targets **Disk 0** — verify disk order |

## Out of scope

- Building the stick from macOS  
- Injecting every model pack into `install.wim`  
- Autopilot / domain join / BitLocker provisioning  

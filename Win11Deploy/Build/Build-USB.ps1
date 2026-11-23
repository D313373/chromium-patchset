<#
.SYNOPSIS
    Copies the model-aware deploy payload onto a Rufus-written Windows 11 USB.
.DESCRIPTION
    Does not write the ISO itself — run Rufus first (GPT/UEFI), then invoke
    this script with the USB drive letter. Validates models.json paths and
    installs SetupComplete.cmd into sources\$OEM$\$$\Setup\Scripts\.
.EXAMPLE
    .\Build-USB.ps1 -UsbDrive E:
.EXAMPLE
    .\Build-USB.ps1 -UsbDrive E: -SkipValidation
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z]:?$')]
    [string]$UsbDrive,

    [string]$PayloadRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,

    [switch]$SkipValidation,
    [switch]$WhatIf
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Step([string]$Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

# Normalize to "E:"
$UsbDrive = $UsbDrive.TrimEnd('\').TrimEnd(':') + ':'
$usbRoot = "$UsbDrive\"

if (-not (Test-Path -LiteralPath $usbRoot)) {
    throw "USB drive $UsbDrive not found."
}

$sources = Join-Path $usbRoot 'sources'
if (-not (Test-Path -LiteralPath $sources)) {
    throw @"
$UsbDrive does not look like a Windows install USB (missing sources\).
Write the Windows 11 ISO with Rufus first:
  - Partition scheme: GPT
  - Target system: UEFI (non CSM)
  - File system: FAT32 (or NTFS if Rufus prompts for large WIM)
  - Do NOT enable Windows To Go
Then re-run this script.
"@
}

Write-Step "Payload root: $PayloadRoot"
Write-Step "Target USB:   $UsbDrive"

# --- Validate models.json driver paths ---
$modelsPath = Join-Path $PayloadRoot 'models.json'
if (-not (Test-Path -LiteralPath $modelsPath)) {
    throw "models.json missing at $modelsPath"
}

$config = Get-Content -LiteralPath $modelsPath -Raw -Encoding UTF8 | ConvertFrom-Json
$missing = New-Object System.Collections.Generic.List[string]

foreach ($entry in $config.models) {
    $rel = [string]$entry.driverPath
    $full = Join-Path $PayloadRoot $rel
    if (-not (Test-Path -LiteralPath $full)) {
        $missing.Add("$($entry.id): folder missing → $rel")
        continue
    }
    $infCount = @(Get-ChildItem -LiteralPath $full -Filter '*.inf' -Recurse -File -ErrorAction SilentlyContinue).Count
    if ($infCount -eq 0) {
        $missing.Add("$($entry.id): no INF files under $rel (add extracted drivers)")
    }
    else {
        Write-Host ("  OK {0}: {1} INF(s) in {2}" -f $entry.id, $infCount, $rel)
    }
}

# Tools presence (soft warnings)
$dcuTools = Join-Path $PayloadRoot 'Tools\Dell'
$hpTools = Join-Path $PayloadRoot 'Tools\HP'
$needsDcu = @($config.models | Where-Object { $_.firmwareTool -eq 'dcu' }).Count -gt 0
$needsHpia = @($config.models | Where-Object { $_.firmwareTool -eq 'hpia' }).Count -gt 0

if ($needsDcu) {
    $dcuInstaller = Get-ChildItem -LiteralPath $dcuTools -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in '.exe', '.msi' }
    if (-not $dcuInstaller) {
        $missing.Add('Tools\Dell: no DCU installer (.exe/.msi) found')
    }
}
if ($needsHpia) {
    $hpia = Get-ChildItem -LiteralPath $hpTools -Recurse -Filter 'HPImageAssistant.exe' -File -ErrorAction SilentlyContinue
    if (-not $hpia) {
        $missing.Add('Tools\HP: HPImageAssistant.exe not found')
    }
}

if ($missing.Count -gt 0) {
    Write-Host ''
    Write-Host 'Validation issues:' -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    if (-not $SkipValidation) {
        throw "Validation failed ($($missing.Count) issue(s)). Fix packs or pass -SkipValidation."
    }
    Write-Host 'Continuing because -SkipValidation was set.' -ForegroundColor Yellow
}

# --- Copy payload to USB root ---
$copyItems = @(
    @{ Src = 'Autounattend.xml'; Dst = 'Autounattend.xml'; Recurse = $false }
    @{ Src = 'models.json';      Dst = 'models.json';      Recurse = $false }
    @{ Src = 'Scripts';          Dst = 'Scripts';          Recurse = $true }
    @{ Src = 'Drivers';          Dst = 'Drivers';          Recurse = $true }
    @{ Src = 'Tools';            Dst = 'Tools';            Recurse = $true }
)

foreach ($item in $copyItems) {
    $src = Join-Path $PayloadRoot $item.Src
    $dst = Join-Path $usbRoot $item.Dst
    if (-not (Test-Path -LiteralPath $src)) {
        throw "Missing payload item: $src"
    }
    Write-Step "Copy $($item.Src) → $dst"
    if ($WhatIf) { continue }
    if ($item.Recurse) {
        if (Test-Path -LiteralPath $dst) {
            Remove-Item -LiteralPath $dst -Recurse -Force
        }
        Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
    }
    else {
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }
}

# --- $OEM$ SetupComplete so Windows Setup copies it to %WINDIR%\Setup\Scripts ---
$oemDir = Join-Path $usbRoot 'sources\$OEM$\$$\Setup\Scripts'
Write-Step "Stage SetupComplete.cmd → $oemDir"
if (-not $WhatIf) {
    New-Item -ItemType Directory -Path $oemDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PayloadRoot 'Scripts\SetupComplete.cmd') `
        -Destination (Join-Path $oemDir 'SetupComplete.cmd') -Force
}

Write-Host ''
Write-Host 'USB payload ready.' -ForegroundColor Green
Write-Host @'
Next steps:
  1. Keep this USB plugged in through first boot (SetupComplete needs it).
  2. Boot the target laptop from the USB (UEFI).
  3. After install, check C:\Windows\Setup\Scripts\model-detect.log
  4. Change the Autounattend local Admin password (default ChangeMe!23).
'@

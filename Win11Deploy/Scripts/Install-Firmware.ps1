<#
.SYNOPSIS
    Runs Dell Command Update or HP Image Assistant for firmware/BIOS updates.
.DESCRIPTION
    Expects vendor tools under Tools\Dell or Tools\HP on the deploy USB.
    Schedules a RunOnce continuation when a reboot is required mid-chain.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('dcu', 'hpia')][string]$FirmwareTool,
    [string]$UsbRoot,
    [switch]$SkipReboot
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

function Install-DellCommandUpdate {
    param([string]$ToolsRoot)

    $candidates = @(
        (Join-Path $env:ProgramFiles 'Dell\CommandUpdate\dcu-cli.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Dell\CommandUpdate\dcu-cli.exe')
    )
    $existing = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

    if ($existing) {
        Write-SetupLog "DCU already installed: $existing"
        return $existing
    }

    $installer = Get-ChildItem -LiteralPath $ToolsRoot -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'Dell-Command-Update|Command_Update|DCU' -and $_.Extension -in '.exe', '.msi' } |
        Select-Object -First 1

    if (-not $installer) {
        throw "Dell Command Update installer not found under $ToolsRoot. See Tools/Dell/README.md."
    }

    Write-SetupLog ("Installing DCU from {0}" -f $installer.FullName)
    if ($installer.Extension -eq '.msi') {
        $p = Start-Process -FilePath 'msiexec.exe' -ArgumentList @('/i', "`"$($installer.FullName)`"", '/qn', '/norestart') -Wait -PassThru
    }
    else {
        $p = Start-Process -FilePath $installer.FullName -ArgumentList @('/s', '/v"/qn /norestart"') -Wait -PassThru
        if ($p.ExitCode -ne 0) {
            # Fallback common silent switches
            $p = Start-Process -FilePath $installer.FullName -ArgumentList @('/silent', '/norestart') -Wait -PassThru
        }
    }

    Write-SetupLog ("DCU installer exit code {0}" -f $p.ExitCode)

    $cli = @(
        (Join-Path $env:ProgramFiles 'Dell\CommandUpdate\dcu-cli.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Dell\CommandUpdate\dcu-cli.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

    if (-not $cli) {
        throw 'dcu-cli.exe not found after DCU install.'
    }
    return $cli
}

function Invoke-DellFirmware {
    param([string]$UsbRoot, [switch]$SkipReboot)

    $tools = Join-Path $UsbRoot 'Tools\Dell'
    $cli = Install-DellCommandUpdate -ToolsRoot $tools

    Write-SetupLog 'Scanning with Dell Command Update...'
    & $cli /scan 2>&1 | ForEach-Object { Write-SetupLog ("dcu-cli: {0}" -f $_) }

    $applyArgs = @('/applyUpdates', '-silent', '-updateType=bios,firmware,driver')
    if ($SkipReboot) {
        $applyArgs += '-reboot=disable'
    }

    Write-SetupLog ("Applying updates: dcu-cli {0}" -f ($applyArgs -join ' '))
    $output = & $cli @applyArgs 2>&1
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-SetupLog ("dcu-cli: {0}" -f $_) }
    Write-SetupLog ("dcu-cli applyUpdates exit code {0}" -f $code)

    # DCU uses specific codes; 0/1/2/5 commonly indicate success / reboot needed.
    if ($code -in 1, 2, 5) {
        Register-FirmwareContinuation -UsbRoot $UsbRoot -FirmwareTool 'dcu'
    }
}

function Invoke-HpFirmware {
    param([string]$UsbRoot, [switch]$SkipReboot)

    $tools = Join-Path $UsbRoot 'Tools\HP'
    $hpia = Get-ChildItem -LiteralPath $tools -Recurse -Filter 'HPImageAssistant.exe' -File -ErrorAction SilentlyContinue |
        Select-Object -First 1

    if (-not $hpia) {
        throw "HPImageAssistant.exe not found under $tools. See Tools/HP/README.md."
    }

    $report = Join-Path $env:SystemRoot 'Setup\Scripts\hpia-report'
    New-Item -ItemType Directory -Path $report -Force | Out-Null

    # SoftPaq cache can live beside HPIA on the USB for offline installs.
    $softpaq = Join-Path $tools 'SoftPaqs'
    $args = @(
        '/Operation:Analyze',
        '/Action:Install',
        '/Silent',
        '/Category:BIOS,Firmware,Driver,Software',
        '/Selection:All',
        "/ReportFolder:$report"
    )
    if (Test-Path -LiteralPath $softpaq) {
        $args += "/SoftPaqDownloadFolder:$softpaq"
    }

    Write-SetupLog ("Running HPIA: {0} {1}" -f $hpia.FullName, ($args -join ' '))
    $p = Start-Process -FilePath $hpia.FullName -ArgumentList $args -Wait -PassThru
    Write-SetupLog ("HPIA exit code {0}" -f $p.ExitCode)

    # HPIA: 0 success, non-zero may mean reboot required or partial apply.
    if ($p.ExitCode -ne 0 -and -not $SkipReboot) {
        Register-FirmwareContinuation -UsbRoot $UsbRoot -FirmwareTool 'hpia'
    }
}

function Register-FirmwareContinuation {
    param(
        [string]$UsbRoot,
        [ValidateSet('dcu', 'hpia')][string]$FirmwareTool
    )

    $scripts = Join-Path $UsbRoot 'Scripts'
    $runner = Join-Path $env:SystemRoot 'Setup\Scripts\Resume-Firmware.cmd'
    $cmd = @"
@echo off
REM Resume firmware pass after reboot (model-aware deploy)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$scripts\Install-Firmware.ps1" -FirmwareTool $FirmwareTool -UsbRoot "$UsbRoot" -SkipReboot
"@
    Set-Content -LiteralPath $runner -Value $cmd -Encoding ASCII

    $runOnce = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce'
    New-ItemProperty -Path $runOnce -Name 'ModelAwareFirmware' -Value $runner -PropertyType String -Force | Out-Null
    Write-SetupLog "Registered RunOnce firmware continuation via $runner" -Level WARN
}

try {
    if (-not $UsbRoot) {
        $UsbRoot = Find-DeployUsbRoot
    }

    Write-SetupLog ("Install-Firmware tool={0} usb={1}" -f $FirmwareTool, $UsbRoot)

    switch ($FirmwareTool) {
        'dcu'  { Invoke-DellFirmware -UsbRoot $UsbRoot -SkipReboot:$SkipReboot }
        'hpia' { Invoke-HpFirmware -UsbRoot $UsbRoot -SkipReboot:$SkipReboot }
    }

    Write-SetupLog 'Firmware step finished.'
    exit 0
}
catch {
    Write-SetupLog ("Install-Firmware failed: {0}" -f $_) -Level ERROR
    exit 1
}

<#
.SYNOPSIS
    Orchestrates hardware detection, driver install, and firmware updates.
.DESCRIPTION
    Intended to run from SetupComplete.cmd while the deploy USB is still
    attached. Logs to C:\Windows\Setup\Scripts\model-detect.log.
#>
[CmdletBinding()]
param(
    [string]$UsbRoot
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$logDir = 'C:\Windows\Setup\Scripts'
Write-SetupLog '==== Model-aware setup starting ====' -LogDirectory $logDir

try {
    if (-not $UsbRoot) {
        $UsbRoot = Find-DeployUsbRoot
    }
    Write-SetupLog "Deploy USB root: $UsbRoot"

    $detected = & (Join-Path $PSScriptRoot 'Detect-Hardware.ps1') -UsbRoot $UsbRoot -PassThru
    if (-not $detected -or -not $detected.Match) {
        Write-SetupLog 'No model match — exiting without applying drivers/firmware.' -Level WARN
        exit 0
    }

    $match = $detected.Match
    $driverExit = 0
    & (Join-Path $PSScriptRoot 'Install-Drivers.ps1') -UsbRoot $UsbRoot -DriverPath ([string]$match.driverPath)
    $driverExit = $LASTEXITCODE
    Write-SetupLog "Install-Drivers exit code $driverExit"

    $tool = [string]$match.firmwareTool
    if ($tool -in @('dcu', 'hpia')) {
        & (Join-Path $PSScriptRoot 'Install-Firmware.ps1') -UsbRoot $UsbRoot -FirmwareTool $tool
        Write-SetupLog ("Install-Firmware exit code {0}" -f $LASTEXITCODE)
    }
    else {
        Write-SetupLog "No firmwareTool configured for $($match.id); skipping firmware." -Level WARN
    }

    Write-SetupLog '==== Model-aware setup completed ===='
    exit 0
}
catch {
    Write-SetupLog ("Run-ModelAwareSetup failed: {0}" -f $_) -Level ERROR
    exit 1
}

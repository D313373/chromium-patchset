<#
.SYNOPSIS
    Installs INF drivers from the matched model pack on the deploy USB.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DriverPath,
    [string]$UsbRoot,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

try {
    if (-not $UsbRoot) {
        $UsbRoot = Find-DeployUsbRoot
    }

    if ([System.IO.Path]::IsPathRooted($DriverPath)) {
        $fullPath = $DriverPath
    }
    else {
        $fullPath = Join-Path $UsbRoot $DriverPath
    }

    if (-not (Test-Path -LiteralPath $fullPath)) {
        throw "Driver pack folder not found: $fullPath"
    }

    $infFiles = Get-ChildItem -LiteralPath $fullPath -Filter '*.inf' -Recurse -File -ErrorAction SilentlyContinue
    if (-not $infFiles -or $infFiles.Count -eq 0) {
        Write-SetupLog "No INF files under $fullPath — nothing to install." -Level WARN
        exit 0
    }

    Write-SetupLog ("Installing {0} INF(s) from {1}" -f $infFiles.Count, $fullPath)

    if ($WhatIf) {
        $infFiles | Select-Object -First 20 | ForEach-Object { Write-SetupLog ("WhatIf: {0}" -f $_.FullName) }
        exit 0
    }

    # pnputil returns non-zero for some "already present" cases; capture and log.
    $args = @('/add-driver', (Join-Path $fullPath '*.inf'), '/subdirs', '/install')
    Write-SetupLog ("pnputil {0}" -f ($args -join ' '))
    $output = & pnputil.exe @args 2>&1
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-SetupLog ("pnputil: {0}" -f $_) }

    if ($code -ne 0) {
        Write-SetupLog ("pnputil exited with code {0} (drivers may still be partially installed)" -f $code) -Level WARN
    }
    else {
        Write-SetupLog 'Driver installation completed.'
    }

    exit 0
}
catch {
    Write-SetupLog ("Install-Drivers failed: {0}" -f $_) -Level ERROR
    exit 1
}

<#
.SYNOPSIS
    Detects the local Dell/HP model and resolves a models.json entry.
.DESCRIPTION
    Writes identity + match details to the setup log. Returns the matched
    entry object when used via -PassThru, or exits 0/1 for cmd wrappers.
#>
[CmdletBinding()]
param(
    [string]$UsbRoot,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

try {
    if (-not $UsbRoot) {
        $UsbRoot = Find-DeployUsbRoot
    }

    $config = Get-ModelsConfig -UsbRoot $UsbRoot
    $identity = Get-HardwareIdentity

    Write-SetupLog ("Identity Manufacturer='{0}' Model='{1}' Sku='{2}' Product='{3}' Version='{4}' BoardProduct='{5}'" -f `
        $identity.Manufacturer, $identity.Model, $identity.Sku, $identity.Product, $identity.Version, $identity.BoardProduct)

    $match = Resolve-ModelMatch -Config $config -Identity $identity
    if (-not $match) {
        Write-SetupLog 'No models.json entry matched this hardware. Skipping drivers/firmware.' -Level WARN
        if ($PassThru) { return $null }
        exit 2
    }

    Write-SetupLog ("Matched id='{0}' manufacturer='{1}' driverPath='{2}' firmwareTool='{3}'" -f `
        $match.id, $match.manufacturer, $match.driverPath, $match.firmwareTool)

    $result = [pscustomobject]@{
        UsbRoot  = $UsbRoot
        Identity = $identity
        Match    = $match
        Config   = $config
    }

    if ($PassThru) { return $result }
    exit 0
}
catch {
    Write-SetupLog ("Detect-Hardware failed: {0}" -f $_) -Level ERROR
    if ($PassThru) { throw }
    exit 1
}

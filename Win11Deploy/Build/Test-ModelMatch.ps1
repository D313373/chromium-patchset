<#
.SYNOPSIS
    Offline unit checks for models.json matching (no WMI / no USB required).
#>
[CmdletBinding()]
param(
    [string]$PayloadRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PayloadRoot 'Scripts\Common.ps1')

$config = Get-ModelsConfig -UsbRoot $PayloadRoot
$failures = 0

function Assert-Match {
    param($Identity, [string]$ExpectedId, [string]$Label)
    $m = Resolve-ModelMatch -Config $config -Identity $Identity
    $got = if ($m) { [string]$m.id } else { '<null>' }
    if ($got -ne $ExpectedId) {
        Write-Host "FAIL $Label — expected '$ExpectedId', got '$got'" -ForegroundColor Red
        $script:failures++
    }
    else {
        Write-Host "OK   $Label → $got" -ForegroundColor Green
    }
}

Assert-Match -Label 'Dell Latitude 5440' -ExpectedId 'dell-latitude-5440' -Identity ([pscustomobject]@{
        Manufacturer = 'Dell Inc.'; Model = 'Latitude 5440'; Sku = '0B14'; Product = 'Latitude 5440'; BoardProduct = ''
    })

Assert-Match -Label 'Dell Latitude 5540' -ExpectedId 'dell-latitude-5540' -Identity ([pscustomobject]@{
        Manufacturer = 'Dell Inc.'; Model = 'Latitude 5540'; Sku = ''; Product = 'Latitude 5540'; BoardProduct = ''
    })

Assert-Match -Label 'HP by product number' -ExpectedId 'hp-elitebook-840-g9' -Identity ([pscustomobject]@{
        Manufacturer = 'HP'; Model = 'HP EliteBook 840 G9 Notebook PC'; Sku = ''; Product = '6F5Y2UT#ABA'; BoardProduct = '89C1'
    })

Assert-Match -Label 'Unknown model skips' -ExpectedId '<null>' -Identity ([pscustomobject]@{
        Manufacturer = 'Dell Inc.'; Model = 'XPS 13 9315'; Sku = ''; Product = 'XPS 13 9315'; BoardProduct = ''
    })

if ($failures -gt 0) {
    throw "$failures assertion(s) failed"
}

Write-Host 'All model-match checks passed.' -ForegroundColor Green

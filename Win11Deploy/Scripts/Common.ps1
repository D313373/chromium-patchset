# Shared helpers for model-aware first-boot setup.
# Dot-source from other scripts in this folder.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-SetupLog {
    param(
        [Parameter(Mandatory)][string]$Message,
        [ValidateSet('INFO', 'WARN', 'ERROR')][string]$Level = 'INFO',
        [string]$LogDirectory = 'C:\Windows\Setup\Scripts'
    )

    if (-not (Test-Path -LiteralPath $LogDirectory)) {
        New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
    }

    $logFile = Join-Path $LogDirectory 'model-detect.log'
    $line = '{0:yyyy-MM-dd HH:mm:ss} [{1}] {2}' -f (Get-Date), $Level, $Message
    Add-Content -LiteralPath $logFile -Value $line -Encoding UTF8
    Write-Host $line
}

function Test-WildcardMatch {
    param(
        [AllowNull()][string]$Value,
        [string[]]$Patterns
    )

    if ([string]::IsNullOrWhiteSpace($Value) -or -not $Patterns -or $Patterns.Count -eq 0) {
        return $false
    }

    foreach ($pattern in $Patterns) {
        if ([string]::IsNullOrWhiteSpace($pattern)) { continue }
        if ($Value -like $pattern) { return $true }
    }

    return $false
}

function Find-DeployUsbRoot {
    param(
        [string]$MarkerFile = 'models.json'
    )

    # Prefer the volume that hosts this script when it already looks like the deploy tree.
    $scriptRoot = $null
    if ($PSScriptRoot) {
        $candidate = Split-Path -Parent $PSScriptRoot
        if ((Test-Path -LiteralPath (Join-Path $candidate $MarkerFile)) -and
            (Test-Path -LiteralPath (Join-Path $candidate 'Drivers'))) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
        $scriptRoot = $PSScriptRoot
    }

    # Scan drive letters C-Z for the deploy marker (do not use return inside ForEach-Object).
    foreach ($code in 67..90) {
        $letter = [char]$code
        $root = '{0}:\' -f $letter
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $marker = Join-Path $root $MarkerFile
        $drivers = Join-Path $root 'Drivers'
        if ((Test-Path -LiteralPath $marker) -and (Test-Path -LiteralPath $drivers)) {
            return $root.TrimEnd('\')
        }
    }

    if ($scriptRoot) {
        Write-SetupLog "USB marker not found; falling back to script parent of $scriptRoot" -Level WARN
        return (Split-Path -Parent $scriptRoot)
    }

    throw 'Could not locate deploy USB (models.json + Drivers). Leave the install USB plugged in.'
}

function Get-HardwareIdentity {
    $cs = Get-CimInstance -ClassName Win32_ComputerSystem
    $csp = Get-CimInstance -ClassName Win32_ComputerSystemProduct
    $bb = Get-CimInstance -ClassName Win32_BaseBoard

    [pscustomobject]@{
        Manufacturer  = [string]$cs.Manufacturer
        Model         = [string]$cs.Model
        Sku           = [string]$cs.SystemSKUNumber
        Product       = [string]$csp.Name
        IdentifyingNumber = [string]$csp.IdentifyingNumber
        BoardProduct  = [string]$bb.Product
        BoardManufacturer = [string]$bb.Manufacturer
    }
}

function Get-ModelsConfig {
    param(
        [Parameter(Mandatory)][string]$UsbRoot
    )

    $path = Join-Path $UsbRoot 'models.json'
    if (-not (Test-Path -LiteralPath $path)) {
        throw "models.json not found at $path"
    }

    $raw = Get-Content -LiteralPath $path -Raw -Encoding UTF8
    return ($raw | ConvertFrom-Json)
}

function Get-MatchPatterns {
    param(
        $MatchObject,
        [Parameter(Mandatory)][string]$Name
    )

    if (-not $MatchObject) { return @() }
    $prop = $MatchObject.PSObject.Properties[$Name]
    if (-not $prop -or $null -eq $prop.Value) { return @() }
    return @($prop.Value)
}

function Resolve-ModelMatch {
    param(
        [Parameter(Mandatory)]$Config,
        [Parameter(Mandatory)]$Identity
    )

    $manufacturer = $Identity.Manufacturer
    $isDell = $manufacturer -match 'Dell'
    $isHp = $manufacturer -match 'HP|Hewlett'

    foreach ($entry in $Config.models) {
        $entryMfr = [string]$entry.manufacturer
        if ($isDell -and $entryMfr -notmatch 'Dell') { continue }
        if ($isHp -and $entryMfr -notmatch 'HP') { continue }
        if (-not $isDell -and -not $isHp -and $entryMfr -and $manufacturer -notlike "*$entryMfr*") {
            continue
        }

        $match = $entry.match
        $modelHit = Test-WildcardMatch -Value $Identity.Model -Patterns (Get-MatchPatterns $match 'modelPatterns')
        $skuHit = Test-WildcardMatch -Value $Identity.Sku -Patterns (Get-MatchPatterns $match 'skuPatterns')
        $productHit = Test-WildcardMatch -Value $Identity.Product -Patterns (Get-MatchPatterns $match 'productPatterns')
        $boardHit = Test-WildcardMatch -Value $Identity.BoardProduct -Patterns (Get-MatchPatterns $match 'boardProductPatterns')

        if ($isHp) {
            # HP: prefer product / board product; model patterns alone are often too generic.
            if ($productHit -or $boardHit -or $modelHit) {
                return $entry
            }
        }
        else {
            # Dell: model is usually stable; SKU is optional fallback.
            if ($modelHit -or $skuHit) {
                return $entry
            }
        }
    }

    return $null
}

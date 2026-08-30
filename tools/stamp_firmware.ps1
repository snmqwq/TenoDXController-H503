[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputDir,

    [Parameter(Mandatory = $true)]
    [string]$BaseName
)

$resolvedInputDir = (Resolve-Path -LiteralPath $InputDir -ErrorAction Stop).Path
$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'

foreach ($extension in @('hex', 'bin')) {
    $sourcePath = Join-Path $resolvedInputDir "$BaseName.$extension"
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "Firmware artifact not found: $sourcePath"
    }

    $destinationPath = Join-Path $resolvedInputDir "$($BaseName)_$timestamp.$extension"
    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    Write-Output "Timestamped firmware: $destinationPath"
}

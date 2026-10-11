# Run the TouchHost-kaede <-> melonloader <-> STM32 raw-CDC parity.
#
# Input is a raw dataset directory:
#
#   <dataset>/frames.bin
#   <dataset>/mapping.csv
#   <dataset>/capture.json
#
# This script does not fabricate acceptance data. It refuses to run unless
# frames.bin exists and reports the dataset SHA256 in its output.

param(
    [Parameter(Mandatory = $true)]
    [string]$Dataset,

    [int]$FramesPerBatch = 250,

    [double]$RateHz = 180.0,

    [string]$OutRoot = (Join-Path $env:TEMP 'tenodx-raw-parity')
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

$root = (Resolve-Path '..').Path
$touchHost = Join-Path $root 'TouchHost-kaede'
$devkit = Join-Path $root 'melonloader-mod\TenoDXIO\src\TenoDXIO.DevKit\bin\Release\net8.0\tenodx-devkit.dll'
$datasetPath = (Resolve-Path -LiteralPath $Dataset).Path
$frames = Join-Path $datasetPath 'frames.bin'
$mapping = Join-Path $datasetPath 'mapping.csv'
$capture = Join-Path $datasetPath 'capture.json'

if (-not (Test-Path -LiteralPath $frames)) {
    throw "raw frames.bin not found: $frames"
}
if (-not (Test-Path -LiteralPath $mapping)) {
    throw "mapping.csv not found: $mapping"
}
if (-not (Test-Path -LiteralPath $capture)) {
    throw "capture.json not found: $capture"
}

$captureData = Get-Content -LiteralPath $capture -Raw | ConvertFrom-Json
$sha = (Get-FileHash -LiteralPath $frames -Algorithm SHA256).Hash
if ($captureData.sha256.'frames.bin' -ne $sha) {
    throw "frames.bin SHA256 does not match capture.json"
}

$out = Join-Path $OutRoot ((Split-Path -Leaf $datasetPath) + '-raw')
New-Item -ItemType Directory -Force -Path $out | Out-Null

$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    throw 'gcc is required for the STM32 raw driver.'
}

$perDriver = Join-Path $PSScriptRoot 'raw_cdc_replay_driver.exe'
& $gcc.Source `
    '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-Wno-unused-parameter' `
    '-DTOUCH_PIPELINE_TEST_PROBES=1' `
    '-I' 'App/touch' '-I' 'Core/Inc' '-I' '../touch_core/include' `
    '-o' $perDriver `
    'tests/raw_cdc_replay_driver.c' `
    'App/touch/touch_pipeline.c' `
    'App/touch/touch_core_port.c' `
    '../touch_core/src/teno_touch_core.c' `
    'App/touch/tenodata_config.c'
if ($LASTEXITCODE -ne 0) {
    throw 'STM32 raw replay driver build failed.'
}

python (Join-Path $touchHost 'tools\raw_cdc_replay.py') `
    --frames $frames `
    --mapping $mapping `
    --frames-per-batch $FramesPerBatch `
    --rate-hz $RateHz `
    --out-dir $out
if ($LASTEXITCODE -ne 0) {
    throw 'TouchHost-kaede raw replay failed.'
}

dotnet $devkit raw-parity `
    --frames $frames `
    --mapping $mapping `
    --frames-per-batch $FramesPerBatch `
    --rate-hz $RateHz `
    --out (Join-Path $out 'csharp.csv')
if ($LASTEXITCODE -ne 0) {
    throw 'melonloader raw replay failed.'
}

& $perDriver $frames $FramesPerBatch $RateHz $mapping (Join-Path $out 'per.csv')
if ($LASTEXITCODE -ne 0) {
    throw 'STM32 raw replay failed.'
}

$result = python (Join-Path $PSScriptRoot 'compare_three_way.py') `
    (Join-Path $out 'python.csv') `
    (Join-Path $out 'csharp.csv') `
    (Join-Path $out 'per.csv') 2>&1
$result | ForEach-Object { Write-Host $_ }

Write-Host ''
Write-Host "dataset: $($captureData.dataset_id)"
Write-Host "sha256:  $sha"
Write-Host "output:  $out"

if ($LASTEXITCODE -ne 0) {
    exit 1
}

exit 0

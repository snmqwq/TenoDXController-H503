# Build and smoke-test the raw 70B CDC replay driver.
#
# The test uses a synthetic stream only to prove the driver plumbing; it is
# not acceptance data and must not be recorded as a raw-cdc-70b result.

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

$cc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $cc) {
    throw 'gcc is required for the raw CDC replay driver.'
}

$outDir = 'build/raw-cdc-test'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$driver = Join-Path $outDir 'raw_cdc_replay_driver.exe'
$fixture = Join-Path $outDir 'synthetic.bin'
$mapping = Join-Path $outDir 'mapping.csv'
$output = Join-Path $outDir 'per.csv'

& $cc.Source `
    '-std=gnu11' '-Wall' '-Wextra' '-Werror' '-Wno-unused-parameter' `
    '-DTOUCH_PIPELINE_TEST_PROBES=1' `
    '-I' 'App/touch' '-I' 'Core/Inc' '-I' '../touch_core/include' `
    '-o' $driver `
    'tests/raw_cdc_replay_driver.c' `
    'App/touch/touch_pipeline.c' `
    'App/touch/touch_core_port.c' `
    '../touch_core/src/teno_touch_core.c' `
    'App/touch/tenodata_config.c'
if ($LASTEXITCODE -ne 0) {
    throw 'raw CDC replay driver build failed.'
}

$areas = @(
    'B8','A8','E8','D8','B7','A7','C2','E7','D7','B6','A6','E6','D6','B5','A5','E5','D5',
    'B4','A4','E4','D4','B3','A3','C1','E3','D3','B2','A2','E2','D2','B1','A1','E1','D1'
)
Set-Content -LiteralPath $mapping -Value @('physical,area') -Encoding ascii
for ($channel = 0; $channel -lt 34; $channel++) {
    Add-Content -LiteralPath $mapping -Value "$channel,$($areas[$channel])" -Encoding ascii
}

$frames = [System.Collections.Generic.List[byte[]]]::new()
for ($frameIndex = 0; $frameIndex -lt 420; $frameIndex++) {
    $frame = [byte[]]::new(70)
    $frame[0] = 0
    for ($channel = 0; $channel -lt 34; $channel++) {
        $value = 1000 + (($frameIndex + $channel) % 5)
        $offset = 1 + $channel * 2
        $frame[$offset] = [byte]($value -band 0xFF)
        $frame[$offset + 1] = [byte](($value -shr 8) -band 0xFF)
    }
    $sum = 0
    for ($index = 0; $index -lt 69; $index++) {
        $sum = ($sum + $frame[$index]) -band 0xFF
    }
    $frame[69] = [byte]$sum
    $frames.Add($frame)
}

$stream = [System.IO.File]::Create($fixture)
foreach ($frame in $frames) {
    $stream.Write($frame, 0, $frame.Length)
}
$stream.Close()

& $driver $fixture 16 180 $mapping $output
if ($LASTEXITCODE -ne 0) {
    throw 'raw CDC replay driver failed.'
}

$dataRow = Get-Content -LiteralPath $output -TotalCount 2 | Select-Object -Last 1
if ([string]::IsNullOrWhiteSpace($dataRow)) {
    throw 'raw CDC replay produced no rows; expected detection rows after calibration.'
}

Write-Host "raw CDC replay smoke test produced detection rows" -ForegroundColor Green
exit 0

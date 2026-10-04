# Build and run the host-side parity test for the firmware TouchHost port.
#
# It compiles the REAL touch_pipeline.c / touch_core_port.c / config sources
# plus the shared teno_touch_core.c (the single detector source of truth),
# against a tiny mai2touch stub, so the frame-counting and detector behaviour
# can be checked without a board.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\tests\build_pipeline_parity_test.ps1

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

$cc = $null
foreach ($candidate in @('gcc', 'clang', 'cc')) {
    $found = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($found) { $cc = $found.Source; break }
}

if (-not $cc) {
    Write-Host 'No host C compiler found (gcc/clang/cc).' -ForegroundColor Red
    exit 1
}

Write-Host "Compiler: $cc" -ForegroundColor Cyan

$outDir = 'build/native-test'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'pipeline_parity_test.exe'

$args = @(
    '-std=gnu11',
    '-Wall',
    '-Wextra',
    '-Wno-unused-parameter',
    '-DTOUCH_PIPELINE_TEST_PROBES=1',
    '-I', 'App/touch',
    '-I', '../touch_core/include',
    '-o', $exe,
    'tests/pipeline_parity_test.c',
    'App/touch/touch_pipeline.c',
    'App/touch/touch_core_port.c',
    '../touch_core/src/teno_touch_core.c',
    'App/touch/tenodata_config.c'
)

& $cc @args
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Build failed' -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host 'Running pipeline parity test ...' -ForegroundColor Cyan
Write-Host ''
& $exe
exit $LASTEXITCODE

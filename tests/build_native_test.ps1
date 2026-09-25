# Build and run the native (host-side) unit test for the CDC0 mode auto-detection.
#
# It compiles the REAL firmware sources (cdc_manager.c / touch_mode.c / mai2touch.c)
# against stub headers for TinyUSB / HAL / touch_pipeline, so the mode-switch and
# blank-frame logic can be verified without flashing the board.
#
# ASCII-only on purpose: Windows PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\tests\build_native_test.ps1

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

$cc = $null
foreach ($candidate in @('gcc', 'clang', 'cc')) {
    $found = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($found) { $cc = $found.Source; break }
}

if (-not $cc) {
    Write-Host 'No host C compiler found (gcc/clang/cc).' -ForegroundColor Red
    Write-Host 'Install one (e.g. w64devkit / MSYS2 / LLVM) and retry.' -ForegroundColor Yellow
    exit 1
}

Write-Host "Compiler: $cc" -ForegroundColor Cyan

$outDir = 'build/native-test'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'native_mode_test.exe'

$args = @(
    '-std=gnu11',
    '-Wall',
    '-Wextra',
    '-Wno-unused-parameter',
    '-I', 'tests/native_stubs',
    '-I', 'App/touch',
    '-o', $exe,
    'tests/native_mode_test.c'
)

& $cc @args
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Build failed' -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host 'Running native mode test ...' -ForegroundColor Cyan
Write-Host ''
& $exe
exit $LASTEXITCODE

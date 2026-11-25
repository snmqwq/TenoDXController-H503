# Build and run the native CDC IO protocol and end-to-end input tests.
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

$outDir = 'build/native-io-test'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'native_io_mode_test.exe'

& $cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter `
    -I tests/native_stubs -I App -I App/touch `
    -o $exe tests/native_io_mode_test.c

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exe = Join-Path $outDir 'native_io_touch_test.exe'
$sources = @(
    'tests/native_io_touch_test.c',
    'App/touch/cdc_manager.c', 'App/touch/io_mode.c', 'App/touch/mai2touch.c',
    'App/touch/touch_mode.c', 'App/touch/tenodata.c', 'App/touch/tenodata_config.c',
    'App/touch/touch_app.c', 'App/touch/psoc_comm.c', 'App/touch/touch_pipeline.c',
    'App/touch/touch_core_port.c', '../touch_core/src/teno_touch_core.c',
    'App/button/button_app.c', 'App/button/third_party/multi_button.c', 'App/kb/keyboard_app.c'
)
& $cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter `
    -I tests/native_button_stubs -I tests/native_stubs -I App -I App/touch -I ../touch_core/include `
    -o $exe @sources -lm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $exe
exit $LASTEXITCODE

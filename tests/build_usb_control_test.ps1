# Build and run the host-side USB reconnect timing test. ASCII-only.
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

$outDir = 'build/native-usb-control-test'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'usb_control_test.exe'

& $cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter `
    -I App/config -I tests/native_stubs `
    -o $exe tests/usb_control_test.c

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $exe
exit $LASTEXITCODE

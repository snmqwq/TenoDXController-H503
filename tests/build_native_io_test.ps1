# Build and run the native CDC IO-mode test. ASCII-only on purpose.
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
exit $LASTEXITCODE

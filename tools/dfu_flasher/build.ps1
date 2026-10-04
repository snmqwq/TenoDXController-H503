# Build TenoDX DFU flasher into a self-contained app folder.
#
# Usage (from tools/dfu_flasher):
#     powershell -ExecutionPolicy Bypass -File .\build.ps1
#
# Output:
#     dist\TenoDXDFUFlasher\TenoDXDFUFlasher.exe   (双击 = GUI)
#
# Note: on this machine PyInstaller onefile + windowed hangs during the
# bootloader stage even for a minimal script, so this ships as an app folder.
# The folder is slower to copy but starts reliably and keeps the bundled
# STM32CubeProgrammer CLI next to the app.
#
# ASCII-only on purpose: Windows PowerShell 5.1 reads .ps1 as ANSI by default.

$ErrorActionPreference = "Stop"

$AppName = "TenoDXDFUFlasher"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$ProgrammerRoot = $env:STM32_CUBE_PROGRAMMER_ROOT
if (-not $ProgrammerRoot) {
    $candidates = @(
        "D:\Programs\STM32CubeCLT_1.19.0\STM32CubeProgrammer",
        "D:\Programs\STM32CubeProgrammer",
        "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate "bin\STM32_Programmer_CLI.exe")) {
            $ProgrammerRoot = $candidate
            break
        }
    }
}

$DfuDriverRoot = $env:STM32_DFU_DRIVER_ROOT
if (-not $DfuDriverRoot) {
    $candidates = @(
        "D:\Programs\STM32CubeProgrammer\Drivers\DFU_Driver\Driver",
        "D:\Files\TenoDX2\tools\DFU-driver"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate "STM32Bootloader.inf")) {
            $DfuDriverRoot = $candidate
            break
        }
    }
}

if (-not $ProgrammerRoot) {
    throw "STM32CubeProgrammer not found. Set STM32_CUBE_PROGRAMMER_ROOT to its install directory."
}
if (-not $DfuDriverRoot) {
    throw "STM32 DFU driver not found. Set STM32_DFU_DRIVER_ROOT to the folder containing STM32Bootloader.inf."
}

Push-Location $ScriptDir

try {
    Write-Host "[1/4] Checking PyInstaller ..." -ForegroundColor Cyan

    $python = $null
    foreach ($candidate in @("3.14", "3.13", "3.12")) {
        try {
            & py "-$candidate" -c "import PyInstaller" 2>$null
            if ($LASTEXITCODE -eq 0) {
                $python = "-$candidate"
                break
            }
        } catch {
            # try the next interpreter
        }
    }

    if (-not $python) {
        Write-Host "      Installing PyInstaller for the default Python 3 ..." -ForegroundColor Yellow
        py -3 -m pip install --upgrade pyinstaller
        $python = "-3"
    }

    Write-Host "[2/4] Cleaning previous output ..." -ForegroundColor Cyan
    # A running instance locks files under dist\, which makes the clean fail.
    Get-Process $AppName -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
    Start-Sleep -Milliseconds 500
    Remove-Item -Recurse -Force build, dist -ErrorAction SilentlyContinue
    Remove-Item -Force "$AppName.spec" -ErrorAction SilentlyContinue

    Write-Host "[3/4] Building exe ..." -ForegroundColor Cyan
    Write-Host "      Programmer : $ProgrammerRoot"
    Write-Host "      DFU driver : $DfuDriverRoot"

    $args = @(
        "-m", "PyInstaller",
        "--noconfirm",
        "--clean",
        "--onedir",
        "--windowed",
        "--name", $AppName,
        "--hidden-import", "serial",
        "--hidden-import", "serial.tools.list_ports",
        "--collect-submodules", "serial",
        "--add-data", "$ProgrammerRoot\bin;bin",
        "--add-data", "$ProgrammerRoot\Data_Base;Data_Base",
        "--add-data", "$DfuDriverRoot;drivers\dfu",
        "dfu_flasher.py"
    )

    & py $python @args

    if ($LASTEXITCODE -ne 0) {
        throw "PyInstaller failed with exit code $LASTEXITCODE"
    }

    Write-Host "[4/4] Done." -ForegroundColor Cyan
    Get-ChildItem dist | Select-Object Name, LastWriteTime | Format-Table -AutoSize
    Write-Host "Output folder: $ScriptDir\dist\$AppName" -ForegroundColor Green
    Write-Host "GUI   : $ScriptDir\dist\$AppName\$AppName.exe" -ForegroundColor Green
}
finally {
    Pop-Location
}

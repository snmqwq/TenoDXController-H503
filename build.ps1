#Requires -Version 5.1
<#
.SYNOPSIS
    TenoDXController-H503 (STM32H503CBT6) 固件构建脚本。

.DESCRIPTION
    使用 CMake + Ninja + arm-none-eabi-gcc 构建固件，产出 ELF / BIN / HEX / MAP，
    默认额外生成带本地时间戳的 BIN/HEX 副本（与 CubeIDE 构建流程一致）。

    工具链优先使用 PATH 上已有的程序；找不到时自动探测 STM32CubeCLT 与
    STM32CubeIDE 自带的 GNU Tools for STM32。

.PARAMETER BuildType
    构建类型，Release（默认，-Os/-g0）或 Debug（-O0/-g3，并定义 DEBUG）。

.PARAMETER BuildDir
    构建目录，默认 build\firmware（与 CI 一致），相对路径以工程根目录解析。

.PARAMETER Clean
    构建前删除构建目录，强制执行全新的 CMake 配置。

.PARAMETER Flash
    构建完成后用 STM32_Programmer_CLI 通过 SWD 烧录并复位。

.PARAMETER ToolchainDir
    手动指定 arm-none-eabi-* 所在目录（即含 arm-none-eabi-gcc 的 bin 目录）。

.PARAMETER Jobs
    并行编译任务数，默认交给 CMake 决定（通常等于 CPU 核心数）。

.PARAMETER NoStamp
    不生成带时间戳的 BIN/HEX 副本。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\build.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\build.ps1 -BuildType Debug -Clean

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\build.ps1 -Flash
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$BuildType = 'Release',

    [string]$BuildDir = 'build\firmware',

    [switch]$Clean,

    [switch]$Flash,

    [string]$ToolchainDir,

    [ValidateRange(0, 64)]
    [int]$Jobs = 0,

    [switch]$NoStamp
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

$FirmwareName   = 'maimai_controller_H503'
$ToolchainFile  = 'cmake\arm-none-eabi.cmake'
$CubeCltGlobs   = @(
    'C:\ST\STM32CubeCLT_*'
    'C:\Program Files\STMicroelectronics\STM32CubeCLT_*'
    'C:\Program Files (x86)\STMicroelectronics\STM32CubeCLT_*'
    'D:\Programs\STM32CubeCLT_*'
    'D:\ST\STM32CubeCLT_*'
    "$env:LOCALAPPDATA\Programs\STM32CubeCLT_*"
)
$CubeIdeGlobs   = @(
    'C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*\tools\bin'
    'D:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*\tools\bin'
    'D:\Programs\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*\tools\bin'
)

# ============================================================
# 辅助函数
# ============================================================

function Write-Step {
    param([string]$Text)
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Write-Note {
    param([string]$Text)
    Write-Host "    $Text" -ForegroundColor DarkGray
}

function Test-Tool {
    param([string]$Name)
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Add-ToPath {
    param([string]$Directory)

    if ([string]::IsNullOrWhiteSpace($Directory)) { return }
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return }

    $separator = [System.IO.Path]::PathSeparator
    $entries = $env:PATH -split [regex]::Escape($separator)
    if ($entries -contains $Directory) { return }

    $env:PATH = "$Directory$separator$env:PATH"
}

function Resolve-Directory {
    param([string]$Path)
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Get-CacheValue {
    param([string]$CacheFile, [string]$Key)

    $match = Select-String -LiteralPath $CacheFile -ErrorAction SilentlyContinue `
        -Pattern "^$([regex]::Escape($Key)):[^=]*=(.*)$" | Select-Object -First 1
    if (-not $match) { return $null }

    return $match.Matches.Groups[1].Value.Trim()
}

function Get-GlobbedDirectory {
    param([string[]]$Patterns)

    $found = @()
    foreach ($pattern in $Patterns) {
        $found += @(Get-Item -Path $pattern -ErrorAction SilentlyContinue |
            Where-Object { $_.PSIsContainer } |
            ForEach-Object { $_.FullName })
    }
    return $found
}

# 找不到工具时，从 STM32CubeCLT / STM32CubeIDE 安装目录里把 bin 目录补进 PATH
function Initialize-Toolchain {
    param([string]$ExplicitToolchainDir)

    $needsCompiler = -not (Test-Tool 'arm-none-eabi-gcc')
    $needsCmake    = -not (Test-Tool 'cmake')
    $needsNinja    = -not (Test-Tool 'ninja')
    $needsProg     = $Flash -and -not (Test-Tool 'STM32_Programmer_CLI')

    if (-not ($needsCompiler -or $needsCmake -or $needsNinja -or $needsProg)) { return }

    $candidates = @()
    if ($ExplicitToolchainDir) { $candidates += (Resolve-Directory $ExplicitToolchainDir) }

    $cltRoots  = @()
    if ($env:STM32_CUBE_CLT) { $cltRoots += $env:STM32_CUBE_CLT }
    $cltRoots += Get-GlobbedDirectory $CubeCltGlobs

    foreach ($root in $cltRoots) {
        $candidates += (Join-Path $root 'GNU-tools-for-stm32\bin')
        if ($needsCmake) { Add-ToPath (Join-Path $root 'CMake\bin') }
        if ($needsNinja) { Add-ToPath (Join-Path $root 'Ninja\bin') }
        if ($needsProg)  { Add-ToPath (Join-Path $root 'STM32CubeProgrammer\bin') }
    }

    $candidates += Get-GlobbedDirectory $CubeIdeGlobs

    if ($needsCompiler) {
        foreach ($candidate in $candidates) {
            if (Test-Path -LiteralPath (Join-Path $candidate 'arm-none-eabi-gcc.exe') -PathType Leaf) {
                Add-ToPath $candidate
                break
            }
        }
    }

    # STM32CubeIDE 里的 GNU 工具链可能带版本后缀（arm-none-eabi-gcc.exe 形式一致，无需额外处理）
    $needsProg = $Flash -and -not (Test-Tool 'STM32_Programmer_CLI')
    if ($needsProg) {
        foreach ($pattern in @(
            'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin'
            'C:\Program Files (x86)\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin'
            'D:\Programs\STM32CubeProgrammer\bin'
        )) {
            if (Test-Path -LiteralPath (Join-Path $pattern 'STM32_Programmer_CLI.exe') -PathType Leaf) {
                Add-ToPath $pattern
                break
            }
        }
    }
}

function Assert-Tool {
    param([string]$Name, [string]$Hint)

    if (-not (Test-Tool $Name)) {
        throw "找不到 $Name。$Hint"
    }
}

# ============================================================
# [1/6] 定位工具链
# ============================================================

Write-Step "[1/6] 检查工具链"
Initialize-Toolchain -ExplicitToolchainDir $ToolchainDir

Assert-Tool 'arm-none-eabi-gcc' '请安装 STM32CubeCLT（含 GNU Tools for STM32），或用 -ToolchainDir 指定 bin 目录。'
Assert-Tool 'cmake' '请安装 CMake 3.24+，或把它加入 PATH。'

$compilerVersion = (& arm-none-eabi-gcc --version | Select-Object -First 1)
$cmakeVersion    = (& cmake --version | Select-Object -First 1)
Write-Note "编译器 : $compilerVersion"
Write-Note "CMake  : $cmakeVersion"

$generator = $null
if (Test-Tool 'ninja') {
    $generator = 'Ninja'
    Write-Note "Ninja  : $(ninja --version)"
} else {
    Write-Note 'Ninja  : 未找到，交给 CMake 选择默认生成器'
}

# ============================================================
# [2/6] 准备构建目录
# ============================================================

Write-Step "[2/6] 准备构建目录"
$buildPath = Resolve-Directory $BuildDir
Write-Note "构建类型 : $BuildType"
Write-Note "构建目录 : $buildPath"

if ($Clean -and (Test-Path -LiteralPath $buildPath)) {
    Remove-Item -LiteralPath $buildPath -Recurse -Force
    Write-Note '已清理旧构建目录'
}

# 缓存与当前环境不符时 CMake 会直接报错，这里先探测并自动清理。
# 常见来源：随仓库拷贝过来的 build 目录、换机器/换路径、换生成器。
$cacheFile = Join-Path $buildPath 'CMakeCache.txt'
if (Test-Path -LiteralPath $cacheFile -PathType Leaf) {
    $reasons = @()

    $cachedSource = Get-CacheValue -CacheFile $cacheFile -Key 'CMAKE_HOME_DIRECTORY'
    if ($cachedSource -and
        $cachedSource.Replace('\', '/').TrimEnd('/') -ne $PSScriptRoot.Replace('\', '/').TrimEnd('/')) {
        $reasons += "缓存里的源码目录是 $cachedSource"
    }

    if ($generator) {
        $cachedGenerator = Get-CacheValue -CacheFile $cacheFile -Key 'CMAKE_GENERATOR'
        if ($cachedGenerator -and $cachedGenerator -ne $generator) {
            $reasons += "缓存里的生成器是 $cachedGenerator，本次使用 $generator"
        }
    }

    if ($reasons.Count -gt 0) {
        Write-Host "    旧 CMake 缓存与当前环境不匹配（$($reasons -join '；')），自动清理后重新配置" -ForegroundColor Yellow
        Remove-Item -LiteralPath $buildPath -Recurse -Force
    }
}

# ============================================================
# [3/6] CMake 配置
# ============================================================

Write-Step "[3/6] CMake 配置"
$configureArgs = @(
    '-S', $PSScriptRoot
    '-B', $buildPath
    "-DCMAKE_BUILD_TYPE=$BuildType"
    "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
)
if ($generator) { $configureArgs += @('-G', $generator) }

Write-Note "cmake $($configureArgs -join ' ')"
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败 (exit $LASTEXITCODE)" }

# ============================================================
# [4/6] 编译
# ============================================================

Write-Step "[4/6] 编译固件"
$buildArgs = @('--build', $buildPath, '--parallel')
if ($Jobs -gt 0) { $buildArgs += $Jobs }

& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "编译失败 (exit $LASTEXITCODE)" }

# ============================================================
# [5/6] 检查产物
# ============================================================

Write-Step "[5/6] 检查产物"

$artifacts = [ordered]@{
    'ELF' = (Join-Path $buildPath "$FirmwareName.elf")
    'BIN' = (Join-Path $buildPath "$FirmwareName.bin")
    'HEX' = (Join-Path $buildPath "$FirmwareName.hex")
    'MAP' = (Join-Path $buildPath "$FirmwareName.map")
}

foreach ($name in $artifacts.Keys) {
    if (-not (Test-Path -LiteralPath $artifacts[$name] -PathType Leaf)) {
        throw "缺少产物 $name : $($artifacts[$name])"
    }
    Write-Note ("{0,-3} {1}" -f $name, $artifacts[$name])
}

Write-Host ''
& arm-none-eabi-size $artifacts['ELF']

if (-not $NoStamp) {
    $stampScript = Join-Path $PSScriptRoot 'tools\stamp_firmware.ps1'
    if (Test-Path -LiteralPath $stampScript -PathType Leaf) {
        & $stampScript -InputDir $buildPath -BaseName $FirmwareName
        if ($LASTEXITCODE -ne 0) { throw "生成时间戳副本失败 (exit $LASTEXITCODE)" }
    } else {
        Write-Host "    未找到 $stampScript，跳过时间戳副本" -ForegroundColor Yellow
    }
}

# ============================================================
# [6/6] 可选烧录
# ============================================================

Write-Step "[6/6] 烧录"

if ($Flash) {
    Assert-Tool 'STM32_Programmer_CLI' '请安装 STM32CubeProgrammer，或用 CubeIDE 手动下载。'

    $flashArgs = @('-c', 'port=SWD', '-w', $artifacts['HEX'], '-v', '-rst')
    Write-Note "STM32_Programmer_CLI $($flashArgs -join ' ')"
    & STM32_Programmer_CLI @flashArgs
    if ($LASTEXITCODE -ne 0) { throw "烧录失败 (exit $LASTEXITCODE)" }
    Write-Host '    烧录完成并已复位' -ForegroundColor Green
} else {
    Write-Note '未指定 -Flash，跳过烧录'
}

# ============================================================
# 完成
# ============================================================

Write-Host ''
Write-Host "构建完成: $BuildType" -ForegroundColor Green
Write-Host "  固件: $($artifacts['HEX'])" -ForegroundColor Green
Write-Host "        $($artifacts['BIN'])" -ForegroundColor Green
Write-Host "  符号: $($artifacts['ELF'])" -ForegroundColor Green
Write-Host "  烧录: powershell -ExecutionPolicy Bypass -File .\build.ps1 -Flash" -ForegroundColor Yellow

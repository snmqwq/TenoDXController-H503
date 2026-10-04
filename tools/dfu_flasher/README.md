# TenoDX DFU 自动刷写工具

双击 `dist\TenoDXDFUFlasher\TenoDXDFUFlasher.exe` 后按下面四步操作：

1. 选择要刷入的固件 (`.hex` / `.bin` / `.elf` / `.srec` / `.s19`)。
2. 在“目标控制器”下拉框里选择要刷写的设备。
3. 点“一键: 进入 DFU 并自动刷写”。
4. 按弹窗提示拔插 USB: 第一次让 Windows 枚举出 DFU 设备, 工具自动刷写并校验;
   刷完后工具会再提示拔插一次, 让设备运行新固件, 并等待 Debug Port 重新出现。

> **Windows 上 USB DFU 不能自动复位。** `STM32_Programmer_CLI` 在 USB DFU 下
> 只能用 `-detach` 结束下载会话, 而 Windows 后端不支持通过 DFU 复位芯片,
> `-rst` / `-hardRst` 都会报 "Reset command is only available with JTAG/SWD"。
> 所以刷写完成后必须手动拔插一次 USB (或给设备断电重新上电) 才会运行新固件。
> 工具的“等待设备重新出现”就是为此准备的。

全程走 USB DFU, 不需要 ST-Link。

## 多设备

工具通过 USB VID/PID 和接口号枚举 TenoDX 控制器, 只列出 Debug Port
(`VID_0483` / `PID_5740` / `MI_06`)。下拉框里显示 COM 口、USB 序列号和描述，
所以同时连接多台控制器时不会按顺序猜设备。

如果同一台电脑上有多个 DFU 设备, 工具会弹出编号列表要求明确选择, 不会自动挑第一个。

## DFU 命令

工具通过 Debug Port 发送现有 Magic 命令:

```text
module = 0x00
cmd    = 0x84
param  = 0x00
payload = [0xA5]
```

设备收到后先回 ACK, 约 100 ms 后跳转 ROM DFU。工具随后等待 Windows 重新识别
DFU 设备。

刷写命令固定为:

```text
STM32_Programmer_CLI.exe -c port=USB1 [sn=<DFU序列号>] -w firmware.hex [-v] -detach
```

该步骤已经实机验证: H503 设备 ID `0x0474`, Flash 128 KB, detach 成功。

## DFU 驱动

设备进入 DFU 后, Windows 必须能把它识别成 `STM32 Bootloader` / DFU 设备,
否则 `STM32_Programmer_CLI` 看不到它。首次在新电脑上使用前, 建议先点工具里的
“安装 DFU 驱动”, 或手动运行:

```text
tools\DFU-driver\install_dfu_driver.cmd
```

安装驱动需要管理员权限。装的是 ST 签名的 WinUSB 驱动；切换后旧版 ST DfuSe
工具可能无法再访问同一设备。

## 环境要求

- Windows 10/11 x64
- STM32CubeProgrammer 或 STM32CubeCLT, 包含 `STM32_Programmer_CLI.exe`
- pyserial (打包进 exe, 源码运行时需要 `pip install pyserial`)

## 命令行

```powershell
TenoDXDFUFlasher.exe --list
TenoDXDFUFlasher.exe --list-dfu
TenoDXDFUFlasher.exe --enter-dfu --serial <控制器序列号>
TenoDXDFUFlasher.exe --flash --firmware firmware.hex --port COM25
TenoDXDFUFlasher.exe --install-driver
```

`--flash` 不带 `--port` 时, 只刷写当前已经处于 DFU 模式的设备。

## 打包

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

产物是自带运行环境的应用文件夹, 双击其中的 exe 即可使用:

```text
dist\TenoDXDFUFlasher\TenoDXDFUFlasher.exe
```

分发时请整个 `TenoDXDFUFlasher` 文件夹一起拷贝。

> 说明: 本机实测 PyInstaller 的 `--onefile --windowed` 引导器即使对最小脚本
> 也会卡死, 所以这里改用目录版 (`--onedir`)。目录版启动可靠, 只是分发时是
> 一个文件夹而不是单个文件。

构建脚本会优先从下面这些位置查找编程器和驱动, 也可以用
`STM32_CUBE_PROGRAMMER_ROOT` 和 `STM32_DFU_DRIVER_ROOT` 覆盖:

- `D:\Programs\STM32CubeCLT_1.19.0\STM32CubeProgrammer`
- `D:\Programs\STM32CubeProgrammer`
- `C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer`

exe 内会带上 `bin`、`Data_Base` 和 DFU 驱动, 目标机器无须另装 STM32CubeProgrammer。

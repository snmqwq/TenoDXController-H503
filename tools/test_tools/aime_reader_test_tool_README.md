# Aime 读卡器测试工具

`aime_reader_test_tool.py` 根据
[Sucareto/Arduino-Aime-Reader](https://github.com/Sucareto/Arduino-Aime-Reader)
当前 `main` 分支中的 `Aime_Reader.h` 和 `Arduino-Aime-Reader.ino`
实现串口协议测试。

## 功能

- 刷新并选择读卡器串口。
- 显示 Windows 的“总线已报告设备描述”、串口原描述和 VID/PID。
- 支持 115200（837-15396）和 38400（TN32MSEC003S）。
- 连接时发送 `CMD_GET_FW_VERSION` 和 `CMD_GET_HW_VERSION`，校验读卡器响应。
- 自动发送开始寻卡命令并循环执行卡片检测。
- MIFARE 显示 UID。
- FeliCa 显示 IDm 和 PMm。
- 支持停止、重新开始读卡以及清除界面结果。
- 只读取卡片标识，不执行写卡命令。

部分 Arduino 板在打开串口时需要 DTR/RTS。工具会主动置位 DTR 和 RTS，
等待设备重启后再进行版本探测。

## 运行

在 `tools/test_tools` 目录中执行：

```powershell
..\.venv\Scripts\python.exe .\aime_reader_test_tool.py
```

或者运行打包版本：

```text
dist\aime_reader_test_tool.exe
```

## 使用

1. 刷新串口。
2. 根据总线报告描述选择 Arduino-Aime-Reader 对应的 COM 口。
3. 根据固件设置选择 115200 或 38400。
4. 点击“连接”。版本校验成功后会自动进入“等待刷卡”状态。
5. 放置 MIFARE 或 FeliCa 卡片，检查卡片类型和标识。

如果版本命令超时，先检查波特率；错误选择其他设备时，工具不会把普通串口连接
误判为读卡器。

## 协议范围

测试工具实现以下只读命令：

| 命令 | 值 | 用途 |
| --- | --- | --- |
| `CMD_GET_FW_VERSION` | `0x30` | 读取固件版本 |
| `CMD_GET_HW_VERSION` | `0x32` | 读取硬件版本 |
| `CMD_START_POLLING` | `0x40` | 开始寻卡 |
| `CMD_STOP_POLLING` | `0x41` | 停止寻卡 |
| `CMD_CARD_DETECT` | `0x42` | 获取卡片类型和标识 |

帧处理包含 `0xE0` 帧头、`0xD0` 转义、长度、序号和累加校验。

## 测试与打包

```powershell
..\.venv\Scripts\python.exe .\aime_reader_test_tool.py --self-test
..\.venv\Scripts\python.exe .\aime_reader_test_tool.py --ui-smoke-test

..\.venv\Scripts\pyinstaller.exe --noconfirm --clean --onefile --windowed `
  --name aime_reader_test_tool `
  aime_reader_test_tool.py
```

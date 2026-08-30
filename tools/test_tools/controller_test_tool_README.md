# Maimai 综合测试工具

`controller_test_tool.py` 用于同时检查八路灯光、八个主按键和
Mai2Touch 的 34 个触摸区域，并测试 Arduino-Aime-Reader 兼容读卡器。

## 功能

- 刷新并分别选择灯光串口、按键键盘设备、触摸串口、读卡器串口及读卡器
  波特率，显示统一连接/断开状态。
- 灯光、触摸和读卡器串口均可留空；连接时只初始化已选择的串口设备。
- 串口设备列表显示 Windows 的“总线已报告设备描述”、原描述和 VID/PID。
- 按键设备不限制 VID/PID，可从 Windows 枚举出的键盘设备中选择。
- 按键设备列表显示 Windows 的“总线已报告设备描述”、VID/PID 和实例标识。
- 连接时按已选择的设备校验灯板串口协议、初始化 Mai2Touch、读取读卡器
  FW/HW 版本，并确认所选按键设备仍然在线。
- 在 1P、2P 键位之间切换，并按固件中的对应映射判断 BTN1～BTN8。
- 用 `images/button_off.png` 显示未按下按键，用
  `images/button_on.png` 显示橙色按下状态。
- 八个按键从右上方 BTN1 开始，按顺时针方向排列到 BTN8。
- 实时解析 Mai2Touch 的 `(xxxxxxx)` 状态帧。
- A/B/D/E 各八区按顺时针排列，C 区为 C1、C2。
- 触摸按下区域使用橙色素材，透明区域使用程序背景色。
- 仅显示当前触摸状态，不统计或判断 34 区是否全部覆盖。
- 读卡器支持 115200 和 38400，自动寻卡并显示 MIFARE UID 或
  FeliCa IDm/PMm。
- 读卡器轮询在后台线程执行，不阻塞灯光、按键和触摸界面。
- 读卡测试只读取卡片标识，不执行写卡。
- 八个灯块显示当前发送的颜色。
- 支持自定义 RGB、RGBW 四色、逐灯追踪、目标色→黑色→目标色淡入淡出和停止。

1P 与 2P 的主键映射直接取自 `Core/Src/app_config.c`：

| 按键 | 1P | 2P |
| --- | --- | --- |
| BTN1 | W | 小键盘 8 |
| BTN2 | E | 小键盘 9 |
| BTN3 | D | 小键盘 6 |
| BTN4 | C | 小键盘 3 |
| BTN5 | X | 小键盘 2 |
| BTN6 | Z | 小键盘 1 |
| BTN7 | A | 小键盘 4 |
| BTN8 | Q | 小键盘 7 |

键位选择只改变测试工具的判断规则，不修改或保存控制器配置。2P 使用物理扫描码判断，不受 Num Lock 状态影响。

## 运行

在 `tools/test_tools` 目录中执行：

```powershell
..\.venv\Scripts\python.exe .\controller_test_tool.py
```

或者直接运行打包后的：

```text
dist\controller_test_tool.exe
```

串口下拉框的第一项为空白，可将暂时不测试的灯光、触摸或读卡器串口留空。
已选择的串口不能重复，不要求使用相同的 VID/PID，也不要求来自同一个 USB
复合设备。按键设备仍为必选项；电脑存在多个键盘时，工具不会自动猜测，必须
明确选择实际使用的键盘 HID，连接后只判断该设备产生的输入。

## 测试与打包

```powershell
..\.venv\Scripts\python.exe .\controller_test_tool.py --self-test
..\.venv\Scripts\python.exe .\controller_test_tool.py --ui-smoke-test

..\.venv\Scripts\pyinstaller.exe --noconfirm --clean --onefile --windowed `
  --name controller_test_tool `
  --add-data "images;images" `
  controller_test_tool.py
```

# Mai2Touch 触摸测试工具

通过串口实时显示 Mai2Touch 的 34 个触摸区域。未按下区域使用灰色 `sensor.png` 底图，图片的透明空白部分与程序窗口背景颜色一致；设备上报按下后直接叠加原始橙色区域素材，松开后立即恢复灰色。

工具不记录测试历史、不判断区域覆盖率，也不发送 Ratio 或 Sensitivity 等灵敏度命令。

## 启动

打包版本：

```text
dist\touch_test_tool.exe
```

从源代码运行：

```powershell
..\.venv\Scripts\python.exe .\touch_test_tool.py
```

## 使用方法

1. 点击“刷新”并选择 Mai2Touch 对应串口。
2. 点击“连接”。工具以 `9600 / 8N1` 打开串口，发送 `{RSET}`，等待约 100 ms 后发送 `{STAT}`。
3. 触摸面板区域。当前按下区域显示橙色，松开后恢复黑色；多个区域可同时显示。
4. 点击“断开”或关闭窗口结束测试。

超过 500 ms 没有收到有效触摸报告时，界面会清除全部按下状态。

## 区域与协议映射

区域编号按顺时针增加：

- A、B、D、E：各 8 区，区域 1 素材每次顺时针旋转 45°得到后续区域。
- C：C1 素材顺时针旋转 180°得到 C2。

协议位序以 `Mai2Touch.ino` 的实际代码为准：

```text
bit  0～7   A1～A8
bit  8～15  B1～B8
bit 16～17  C1～C2
bit 18～25  D1～D8
bit 26～33  E1～E8
bit 34      保留并忽略
```

每个触摸报告为 `(`、7 个数据字节、`)`。固件先发送 `TouchData & 0x1F`，然后右移 5 位，因此工具按低位 5-bit 数据块优先解析。

## 依赖、自检与打包

```powershell
..\.venv\Scripts\pip.exe install -r .\requirements.txt
..\.venv\Scripts\python.exe .\touch_test_tool.py --self-test
..\.venv\Scripts\python.exe .\touch_test_tool.py --ui-smoke-test
..\.venv\Scripts\pyinstaller.exe --noconfirm --onefile --windowed --name touch_test_tool --add-data "images;images" .\touch_test_tool.py
```

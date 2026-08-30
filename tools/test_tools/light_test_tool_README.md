# Mai2LED 灯光测试工具

用于通过 USB CDC 串口测试当前 STM32F072 控制器的 8 路逻辑灯光。工具只发送灯光控制命令，不修改灯珠数量、待机灯效或其他设备配置，也不会写入 Flash。

## 启动

打包版本：

```text
dist\light_test_tool.exe
```

从源代码运行：

```powershell
..\.venv\Scripts\python.exe .\light_test_tool.py
```

## 使用方法

1. 点击“刷新”，选择控制器对应的串口。
2. 点击“连接”。工具读取 `15070-04` 板卡信息成功后，灯光测试按钮才会启用。
3. 根据需要编辑全局 R、G、B 测试颜色，数值范围为 0～255。
4. 选择测试：
   - `RGBW 四色测试`：红、绿、蓝、白各保持 800 ms 并循环。
   - `逐灯追踪`：使用编辑后的 RGB 颜色，按 BTN1 到 BTN8 每路保持 300 ms 并循环。
   - `单色淡入淡出`：目标颜色保持 300 ms，然后以约 600 ms 淡到黑色，再以约 600 ms 回到目标颜色并循环。
   - `显示测试颜色`：让 8 路灯同时常亮为编辑后的 RGB 颜色。
5. 点击“停止测试”立即取消当前循环并熄灭全部灯。

界面中的 8 个色块表示工具发送的目标颜色；淡变期间显示的是按相同时间计算的近似颜色。工具没有光学传感器，不能判断灯珠是否实际点亮。

## 注意事项

- RGBW 测试会发送不受限制的全亮白色 `255,255,255`，请确保灯带电源容量和接线能够承受最大电流。
- 主动断开串口或关闭窗口时，工具会先尝试熄灭全部灯。
- 测试过程中拔出设备会停止测试并将状态切换为“未连接”。
- 当前工具测试 8 路逻辑灯区，不提供单颗物理灯珠控制。

## 自检与打包

```powershell
..\.venv\Scripts\python.exe .\light_test_tool.py --self-test
..\.venv\Scripts\python.exe .\light_test_tool.py --ui-smoke-test
..\.venv\Scripts\pyinstaller.exe --noconfirm --onefile --windowed --name light_test_tool .\light_test_tool.py
```

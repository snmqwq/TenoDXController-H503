# Maimai 硬件测试工具

本目录集中存放本项目制作的硬件测试程序：

| 工具 | 用途 |
| --- | --- |
| `controller_test_tool.py` | 综合测试灯光、按键、触摸与 Aime 读卡器 |
| `light_test_tool.py` | 单独测试八路 RGB 灯光 |
| `touch_test_tool.py` | 单独测试 Mai2Touch 的 34 个触摸区域 |
| `aime_reader_test_tool.py` | 单独测试 Arduino-Aime-Reader 兼容读卡器 |

日常测试建议优先使用综合工具；独立工具用于排查单个设备或协议问题。

## 运行

在 `tools/test_tools` 目录中执行：

```powershell
..\.venv\Scripts\pip.exe install -r .\requirements.txt
..\.venv\Scripts\python.exe .\controller_test_tool.py
```

也可以直接运行 `dist` 目录中的对应 EXE。图片素材统一存放在 `images`，
PyInstaller 配置与临时构建文件分别存放在本目录和 `build` 中。

各工具的设备选择、协议范围和测试方法请查看对应的 `_README.md`。

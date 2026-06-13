# Aime / PN532 参考资料

- `aime_bridge.py`：旧电脑上的 PC 串口桥接参考实现。
- `porting_guide_zh.txt`：基于该脚本整理的中文移植说明。

这两个文件用于理解 PN532 状态机、卡片数据转换和 Aime 主机协议。当前 STM32 实现位于
`Core/Src/aime_reader_app.c`，主机侧已由脚本中的串口改为 TinyUSB CDC2。

`aime_bridge.py` 中的 `PN532_PORT` 和 `GAME_PORT` 是旧电脑示例端口，运行前必须按实际环境
修改。它不是 `tools/magic_config_tool.py` 的替代品。

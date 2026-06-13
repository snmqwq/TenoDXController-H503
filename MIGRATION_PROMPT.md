# 迁移后 Codex 导入提示词

将下面内容粘贴到新电脑上的 Codex 线程，并把 `<新电脑实际路径>` 替换为真实路径：

```text
这是从旧电脑迁移的 STM32 工程。请先读取当前工作目录和 Git 状态，再进行任何判断或修改，
不要按旧电脑路径操作，也不要假设旧对话中的改动与当前文件完全一致。

当前工作目录：
<新电脑实际路径>\TenoDXController-H503

GitHub：
https://github.com/snmqwq/TenoDXController-H503

当前开发分支：
1.4-aime-touch_test

迁移基线：
- 当前固件功能基线提交为 fbf0522，提交说明“新增aime支持，测试输出psoc raw数据”。
- 根目录 MIGRATION.md 是迁移说明。
- ref/Aime、ref/Mai2LED、ref/Mai2Touch、ref/Touch_Algorithm 保存参考资料。
- tools/magic_config_tool.py 是交互式配置工具，需要 pyserial。

协作规则：
1. 开始前先执行 git status、查看当前分支和最近提交，并阅读 README.md、MIGRATION.md 及相关源码。
2. 不允许自行修改 maimai_controller_H503.ioc。需要新增或调整外设配置时，明确告诉我，由我修改 IOC。
3. 所有不明确且会影响硬件行为、协议兼容或数据格式的问题，先向我确认，再修改文件。
4. 不要覆盖或回退我已有的未提交修改。
5. 修改后应构建验证；提交或推送前说明涉及的文件和测试结果。

项目硬件与接口：
- MCU：STM32H503CBT6。
- USB：TinyUSB，3 CDC + 1 HID keyboard。
- CDC0：触摸。
- CDC1：Mai2LED、灯光、magic 配置。
- CDC2：Aime 读卡器主机协议。
- HID：11KRO keyboard。
- I2C1：PB7 SDA、PB8 SCL，PSoC 从设备地址 0x08 和 0x09。
- USART2：PB4 TX、PB5 RX，115200 8N1，连接 PN532。
- USART1：调试串口；当前 PN532 UART 调试已关闭。

当前触摸测试状态：
- 每个 PSoC 从 0x00 开始读 35 字节。
- I2C1 使用中断方式：先 Transmit_IT 写寄存器地址，再 Receive_IT 读数据。
- 连续 4 次失败判定掉线；掉线后 500 ms 轮询重连。
- CDC0 每 5 ms 输出固定 69 字节：
  00 + 0x08 的 raw[1..34] + 0x09 的 raw[1..34]
- 不发送两个设备各自的第一个状态字节。
- 未连接设备对应的 34 字节填 00。
- 当前是 PSoC raw 数据测试路径，最终触摸 setup raw、算法处理、区域映射和约 100 Hz
  处理后输出尚需继续完善。

当前 Aime/PN532 状态：
- Core/Src/aime_reader_app.c 已实现 PN532 状态机及 Aime 主机协议。
- PN532 使用 USART2 中断和 Receive-to-IDLE 环形缓冲。
- 支持 FeliCa IDm 和 MIFARE Block 2 路径。
- Aime 主机协议走 CDC2。
- PN532_UART_DEBUG_ENABLED 当前为 0U。
- 原始 PC Python 桥接与移植说明位于 ref/Aime。

按键要求：
- 只有 BTN0..BTN10，共 11 个，不存在 BTN11/BTN12。
- HID 键盘为 11KRO。
- BTN0..BTN7 固定为 w、e、d、c、x、z、a、q。
- 只有 BTN8..BTN10 可配置和保存，默认 3、keypad_multiply、9。
- keyboard flash 配置中不能包含 BTN0..BTN7 的键值配置。

灯光要求：
- Mai2LED 使用 CDC1（itf=1）。
- DATA_BITS=8，只控制 8 个逻辑按钮灯。
- LED_PER_BIT 默认 2 且可配置；LED_TOTAL=DATA_BITS*LED_PER_BIT。
- 外部普通 Mai2LED IO 控制时必须停止空闲灯效，不能被空闲灯效覆盖。
- 不存在 IO 停止 300 秒后自动恢复彩虹的逻辑。
- IO 可以关闭空闲灯效，之后仅通过按键行为恢复。
- 长按 BTN8/PB0 5 秒恢复，一次长按只触发一次，松手后才能再次触发。
- 恢复时 rainbow 关闭则全亮白灯，rainbow 开启则进入缓慢环形彩虹。
- 默认 rainbow 关闭，上电白灯全亮；清灯为全黑。
- Magic 配置协议不算普通外部灯光控制。

Flash 与 magic：
- 最后 8 KiB Flash 分为 touch、light、reader、keyboard 四个独立 2 KiB 槽。
- module：global=0x00、touch=0x10、light=0x20、reader=0x30、keyboard=0x40。
- WRITE 只写 RAM，SAVE 才写 Flash。
- Magic 走 CDC1，SPECIAL_MAGIC_CMD=0xB7。
- magic sequence：91 3E ED 20 7C 99 58 AC。
- 基础命令 READ=01、WRITE=02、SAVE=03、LOAD_DEFAULT=04、GET_INFO=05。
- 全局命令 READ_ALL=81、WRITE_ALL=82、SAVE_ALL=83、ENTER_DFU=84。
- DFU confirm=A5。

请先只汇报：
1. 当前分支、HEAD、工作树状态。
2. 你从当前源码确认到的 CDC、I2C1、USART2 和主循环实际实现。
3. MIGRATION.md 与当前源码是否存在不一致。
在我提出具体修改任务前，不要改代码。
```

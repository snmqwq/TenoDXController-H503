# 电脑迁移说明

本文记录 `TenoDXController-H503` 在 2026-06-13 的可迁移状态。新电脑应以 GitHub
仓库为准，不要复制旧 STM32CubeIDE workspace 的 `.metadata`、索引缓存或构建输出。

## Git 基线

- 仓库：`https://github.com/snmqwq/TenoDXController-H503`
- 当前开发分支：`1.4-aime-touch_test`
- 当前固件功能基线：`fbf0522`（新增 Aime 支持，测试输出 PSoC raw 数据）
- 稳定灯光/按键基线：`1.0-led-button-only`
- 触摸开发分支：`1.2-touch-i2c`

分支用途：

| 分支 | 用途 |
| --- | --- |
| `main` | 初始工程基线 |
| `1.0-led-button-only` | 灯光、按键和配置功能基线 |
| `1.1-touch-test` | 早期触摸通信测试 |
| `1.2-touch-i2c` | 双 PSoC I2C 触摸开发 |
| `1.4-aime-touch_test` | Aime/PN532 支持和当前触摸 raw 输出测试 |

## 新电脑准备

建议安装：

1. Git for Windows。
2. STM32CubeIDE 1.19 或兼容版本。
3. Python 3。
4. STM32CubeIDE 所需的 STM32H5 固件包和 GNU Tools for STM32。

克隆当前开发分支：

```powershell
git clone --branch 1.4-aime-touch_test https://github.com/snmqwq/TenoDXController-H503.git
cd TenoDXController-H503
git status
```

不要沿用旧电脑的绝对路径。新线程开始时，应明确告诉 Codex 新电脑上的实际仓库路径。

## STM32CubeIDE 导入

1. 打开 STM32CubeIDE。
2. 选择 **File > Import > General > Existing Projects into Workspace**。
3. 选择克隆后的仓库根目录。
4. 不要勾选“Copy projects into workspace”，让 IDE 直接使用 Git 工作树。
5. 选择 `Debug` 配置并构建。

目标 MCU 为 `STM32H503CBT6`。链接脚本
`STM32H503CBTX_FLASH.ld` 使用前 120 KiB Flash，最后 8 KiB 保留给配置存储。

`.settings/language.settings.xml` 可能在不同电脑上因工具链路径或 `env-hash` 自动变化。
这种机器相关变化不要提交，除非确认它包含工程必须共享的设置。

## Python 工具

安装配置工具依赖：

```powershell
python -m pip install -r tools/requirements.txt
```

运行交互式 magic 配置工具：

```powershell
python tools/magic_config_tool.py
```

`ref/Aime/aime_bridge.py` 是原始 PC 串口桥接参考，不是固件配置工具。其 COM 端口是旧环境
示例值，不能直接作为新电脑配置使用。

## 当前硬件与接口

- MCU：STM32H503CBT6。
- USB：TinyUSB，3 CDC + 1 HID keyboard。
- CDC0：触摸 raw 数据。
- CDC1：Mai2LED、灯光和 magic 配置协议。
- CDC2：Aime 主机协议。
- HID：11KRO keyboard。
- I2C1：PB7 SDA、PB8 SCL，连接地址 `0x08` 和 `0x09` 的两个 PSoC。
- USART2：PB4 TX、PB5 RX，115200 8N1，连接 PN532。
- USART1：保留为调试串口；当前 PN532 UART 调试开关关闭。

外设配置由用户通过 CubeMX/IOC 决定。协作时不得自行修改
`maimai_controller_H503.ioc`。

## 当前触摸测试行为

`Core/Src/mai2touch_app.c` 当前是 raw 数据测试路径，还不是最终触摸算法：

- 每个 I2C 设备从寄存器 `0x00` 开始读取 35 字节。
- 使用 `HAL_I2C_Master_Transmit_IT` 写寄存器地址，再用
  `HAL_I2C_Master_Receive_IT` 中断接收。
- 连续 4 次失败后判定掉线。
- 已连接设备约每 5 ms 轮询；掉线设备约每 500 ms 重试。
- CDC0 每 5 ms 尝试发送一个固定 70 字节帧。
- 帧格式：`00 + 0x08 raw[1..34] + 0x09 raw[1..34] + checksum`。
- `checksum` 为前 69 字节的 8 位累加和。
- 两个 35 字节块的第一个状态字节均不发送。
- 设备未连接时，其 34 字节区域填充 `00`。

最终目标仍包括 setup raw、触摸算法、区域映射和约 100 Hz 的处理后数据输出。恢复算法时
应以 `ref/Touch_Algorithm/` 中的资料为参考，并先确认测试输出格式是否仍需保留。

## 当前 Aime/PN532 行为

`Core/Src/aime_reader_app.c`：

- PN532 使用 USART2 中断和 Receive-to-IDLE 环形缓冲。
- 支持 FeliCa IDm 和 MIFARE Block 2 读取路径。
- Aime 主机协议通过 CDC2 收发。
- PN532 调试宏 `PN532_UART_DEBUG_ENABLED` 当前为 `0U`。
- 原始 Python 参考和移植手册保存在 `ref/Aime/`。

## 长期工程约束

- 按键只有 BTN0..BTN10，共 11 个，不存在 BTN11/BTN12。
- BTN0..BTN7 固定为 `w e d c x z a q`。
- 只有 BTN8..BTN10 可配置并保存，默认 `3`、`keypad_multiply`、`9`。
- 灯光只控制 8 个逻辑按钮；`DATA_BITS = 8`。
- `LED_PER_BIT` 默认 2 且可配置，`LED_TOTAL = DATA_BITS * LED_PER_BIT`。
- 外部普通 Mai2LED IO 接管后，空闲灯效不能覆盖外部灯光。
- 长按 BTN8/PB0 5 秒恢复空闲灯效，一次长按只触发一次。
- 默认 rainbow 关闭，上电全亮白灯；清灯为全黑。
- Magic 配置仍通过 CDC1，`SPECIAL_MAGIC_CMD = 0xB7`。
- Flash 最后 8 KiB 分为 touch/light/reader/keyboard 四个独立 2 KiB 槽。
- Magic `WRITE` 只改 RAM，`SAVE` 才写 Flash。

## 参考资料

| 路径 | 内容 |
| --- | --- |
| `ref/Aime/` | PC 版 Aime/PN532 桥接参考和中文移植手册 |
| `ref/Mai2LED/` | Mai2LED 参考实现和协议资料 |
| `ref/Mai2Touch/` | Mai2Touch 参考实现和协议资料 |
| `ref/Touch_Algorithm/` | PSoC raw 数据、通道映射和触摸算法参考 |
| `ref/Touch_Algorithm/main_psoc_no_i2c_irq_debug.c` | 抓包阶段关闭 EZI2C 临界区中断保护及 UART raw 输出的测试快照 |

迁移后给 Codex 的完整提示词见 `MIGRATION_PROMPT.md`。

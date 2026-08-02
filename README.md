# TenoDXController-H503

> Designed by AI

基于 STM32H503CBT6 的 TenoDX / maimai 控制器固件。工程使用 STM32CubeIDE、STM32 HAL、TinyUSB 和 WS28XX 驱动，提供触摸、灯光、Aime 读卡器和 12KRO 键盘接口。

> 当前 `main` 已整合 `1.4-aime-touch_test`。触摸模块目前输出 PSoC raw 测试数据，尚未接入最终触摸判定算法。

## 硬件接口

| 功能 | 外设与引脚 | 当前用途 |
| --- | --- | --- |
| MCU | STM32H503CBT6 | 主控制器 |
| USB | TinyUSB Device | 3 CDC + 1 HID keyboard |
| I2C1 | PB7 SDA / PB8 SCL | PSoC 触摸从机 `0x08`、`0x09` |
| USART2 | PB4 TX / PB5 RX，115200 8N1 | PN532 |
| USART1 | PA9 TX / PA10 RX，115200 8N1 | 调试串口，默认无输出 |

USB 标识为 VID `0xCAFE`、PID `0x4313`，产品名为 `TenoDX Controller`。

## USB 接口

| 接口 | 名称 | 功能 |
| --- | --- | --- |
| CDC0 | TenoDX Touch Port | 双 PSoC raw 触摸数据 |
| CDC1 | TenoDX LED Port | Mai2LED 灯光协议 |
| CDC2 | TenoDX Aime Port | Aime 主机协议与 Magic 配置协议 |
| HID0 | Keyboard | 12KRO 键盘报告 |

## 按键

工程使用 `BTN0..BTN11`，共 12 个按键。BTN0..BTN7 为主按键，可在 1P 和 2P 两套布局之间选择；BTN8..BTN11 为四个可独立修改键值的副按键。

四个副按键依次映射为 `BTN8=PB0`、`BTN9=PB1`、`BTN10=PB2`、`BTN11=PB10`，均为上拉输入、低电平有效。

| 按键 | 1P 布局 | 2P 布局 | 配置方式 |
| --- | --- | --- | --- |
| BTN0..BTN7 | `w e d c x z a q` | `keypad_8 keypad_9 keypad_6 keypad_3 keypad_2 keypad_1 keypad_4 keypad_7` | Magic 选择布局，默认 1P，可保存到 Flash |
| BTN8 | `3` | `3` | Magic 修改键值，可保存到 Flash |
| BTN9 | `keypad_multiply` | `keypad_multiply` | Magic 修改键值，可保存到 Flash |
| BTN10 | `8` | `8` | Magic 修改键值，可保存到 Flash |
| BTN11 | `9` | `9` | Magic 修改键值，可保存到 Flash |

键盘以 12KRO 自定义 HID 报告发送。长按 BTN8 约 5 秒会恢复空闲灯效；一次按住只触发一次，松开后才可再次触发。

## 状态灯

PC13 状态灯为高电平点亮，状态只表示固件运行情况，不区分 1P/2P 布局：

- 开机初始化期间至少常亮 1 秒。
- 正常运行时每秒心跳一次（亮 100 ms、灭 900 ms）。
- `SAVE` / `SAVE_ALL` 成功写入 Flash 后快速闪烁 3 次（每相位 80 ms）。
- 配置保存失败或进入致命错误时锁定常亮。

## 灯光

- 只控制 8 个逻辑按钮灯。
- `LED_PER_BIT` 默认是 2，当前允许配置为 1 至 4。
- 修改 `LED_PER_BIT` 时会先向全部灯珠发送一次全灭帧，再应用新的灯珠数量。
- 实际灯数为 `8 * LED_PER_BIT`。
- 灯珠连续排列；逻辑灯 `i` 映射到物理范围
  `[i * LED_PER_BIT, (i + 1) * LED_PER_BIT)`，其余灯珠每帧保持全灭。
- Mai2LED `0x31` / `0x32` 只修改暂存颜色，`0x33` 准备 Fade，
  收到 `0x3C` 后才提交显示或启动 Fade。
- 上述命令只要封包校验通过就立即回复 `ACK OK`，不等待 LED DMA 完成。
- LED 输出由单一 DMA 状态机发送；Fade 每次任务循环计算最新状态，
  DMA 忙时合并为最新待发送帧，不改写正在发送的缓冲区；Fade 的
  起始帧和最终帧会保留到发送成功。
- 默认关闭彩虹模式，上电显示全亮白灯。
- 彩虹模式下 8 个逻辑按钮具有错开的色相并持续流动，按下按钮时提高亮度并保留少量当前色调。
- 普通 Mai2LED IO 指令会接管灯光并停止空闲灯效；Magic 已迁移至 CDC2，不会进入 Mai2LED 协议解析器。
- 外部 IO 停止后不会定时自动恢复；需要长按 BTN8 恢复。
- 恢复时，彩虹配置关闭则显示白灯，开启则恢复流动彩虹。

## 触摸 Raw 数据

I2C1 通过中断读取两个 PSoC。每个设备从寄存器 `0x00` 开始返回 35 字节：第 0 字节为状态，后续 34 字节为 17 个 16-bit raw 通道。

- 已连接设备约每 8 ms 读取一次。
- 连续 4 次失败后判定掉线。
- 掉线设备约每 500 ms 重新检测一次。
- CDC0 约每 16 ms 发送一帧，即约 62.5 Hz。
- PSoC 原始通道为高字节在前，发送到 CDC0 前转换为低字节在前。
- 未连接设备对应的 34 字节数据区域填充 `00`。

CDC0 固定发送 70 字节：

| 偏移 | 长度 | 内容 |
| --- | ---: | --- |
| 0 | 1 | 帧头 `00` |
| 1 | 34 | `0x08` 的 17 个 raw 通道，小端序 |
| 35 | 34 | `0x09` 的 17 个 raw 通道，小端序 |
| 69 | 1 | `sum(frame[0..68]) & 0xFF` |

两个 PSoC 的状态字节不会发送。通道映射和待移植的触摸算法见 `ref/Touch_Algorithm/`。

## Aime 与 PN532

- PN532 通过 USART2 通信，接收使用中断和环形缓冲。
- 支持 FeliCa IDm 读取路径。
- 支持 MIFARE 验证并读取 Block 2。
- Aime 主机协议通过 CDC2 收发，并与 Magic 配置协议共用该接口。
- CDC2 只在 Aime 帧解析器空闲时检测 Magic 固定前导码；Aime 帧内部不会触发 Magic。
- Aime 与 Magic 共用非阻塞接收分流和串行发送队列，响应不会交错。
- USART1 调试由 `PN532_UART_DEBUG_ENABLED` 控制，当前默认值为 `0U`。

原始 Python 桥接程序和移植资料保存在 `ref/Aime/`，仅作为协议参考。

## Magic 配置协议

Magic 协议与 Aime 主机协议共用 CDC2，固定序列为：

```text
91 3E ED 20 7C 99 58 AC
```

请求格式：

```text
magic_seq + [module, cmd, param, len, payload..., sum]
```

响应格式：

```text
[AC, status, module, cmd, param, len, payload..., sum]
```

模块编号：

| 模块 | 编号 | 当前状态 |
| --- | ---: | --- |
| global | `0x00` | 全局命令 |
| touch | `0x10` | 空配置占位，当前不注册 |
| light | `0x20` | 已实现 |
| keyboard | `0x40` | 已实现主按键 1P/2P 布局选择及 BTN8..BTN11 配置 |

Aime 和 button 不提供 Magic 配置模块；`0x30` 保留为空洞，不分配给 Aime。

基础命令为 `READ 0x01`、`WRITE 0x02`、`SAVE 0x03`、`LOAD_DEFAULT 0x04` 和 `GET_INFO 0x05`；全局命令为 `READ_ALL 0x81`、`WRITE_ALL 0x82`、`SAVE_ALL 0x83` 和 `ENTER_DFU 0x84`。

`WRITE` 只修改 RAM，必须使用 `SAVE` 或 `SAVE_ALL` 才会写入 Flash。

light 模块的 `READ_ALL` / `WRITE_ALL` 载荷版本为 `0x02`，格式为
`[version, led_per_bit, rainbow_enable]`。Mai2LED 的 8 字节 dummy EEPROM
不对 Magic 开放，也不写入 light Flash 配置；它只由 CDC1 上的 Mai2LED
`SetEEPRom 0x7B` 和 `GetEEPRom 0x7C` 命令访问。

## Flash 配置

链接脚本将内部 128 KiB Flash 的前 120 KiB 分配给固件，最后 8 KiB 用于配置。配置区划分为四个 2 KiB 槽：

| 槽 | 用途 |
| --- | --- |
| 0 | touch |
| 1 | light |
| 2 | 历史布局保留，不分配给 Aime |
| 3 | keyboard |

## 配置工具

需要 Python 3 和 pyserial：

```powershell
python -m pip install -r tools/requirements.txt
python tools/magic_config_tool.py
```

`magic_config_tool.py` 是交互式命令行工具，支持串口选择、灯光配置、主按键 1P/2P 布局选择、BTN8..BTN11 键值配置、原始 Magic 请求和进入系统 DFU。连接时应选择 `TenoDX Aime Port`；配置期间不要让游戏或其他程序同时占用该 COM 口。

键盘配置示例：

```text
keyboard layout 1p
keyboard layout 2p
keyboard player 2p
keyboard set 11 enter
keyboard set-all 3 keypad_multiply 8 9
keyboard save
```

`keyboard layout`（或别名 `keyboard player`）不带参数时读取当前主按键布局，带 `1p` 或 `2p` 时修改 RAM 中的布局。布局参数为 `0x81`；与副按键键值一样，修改后需执行 `keyboard save` 才会保存到 Flash。

## 构建

推荐使用 STM32CubeIDE 1.19 或兼容版本：

1. 克隆主分支。

   ```powershell
   git clone https://github.com/snmqwq/TenoDXController-H503.git
   cd TenoDXController-H503
   ```

2. 在 STM32CubeIDE 中选择 **File > Import > General > Existing Projects into Workspace**。
3. 选择仓库根目录，不勾选 **Copy projects into workspace**。
4. 选择 `Debug` 配置并构建工程。

外设和引脚配置以 `maimai_controller_H503.ioc` 为准。修改 CubeMX 外设配置前请先确认硬件设计，避免生成代码覆盖现有应用逻辑。

## 工程结构

| 路径 | 内容 |
| --- | --- |
| `App/` | 应用模块及统一的 `app_init()` / `app_task()` 入口 |
| `App/aime/` | PN532 通信、Aime 协议、Magic 协议识别与 CDC2 分流 |
| `App/config/` | Magic 命令、led/kb 配置，以及空的 touch 配置占位 |
| `App/led/` | Mai2LED 控制 |
| `App/button/` | 按键扫描接口及内部第三方 MultiButton 实现 |
| `App/kb/` | USB 键盘报告与运行时键值 |
| `App/status/` | PC13 运行状态灯状态机与错误指示 |
| `Core/` | CubeMX 生成的外设初始化、中断代码，以及暂时保留原位置的 touch 模块 |
| `Drivers/` | STM32 HAL 与 CMSIS |
| `tinyusb/` | TinyUSB 协议栈 |
| `Middlewares/` | WS28XX 驱动 |
| `tools/` | Magic 配置工具 |
| `ref/Aime/` | Aime/PN532 参考代码和移植说明 |
| `ref/Mai2LED/` | Mai2LED 协议和参考实现 |
| `ref/Mai2Touch/` | Mai2Touch 协议和参考实现 |
| `ref/Touch_Algorithm/` | PSoC 数据格式、通道映射和触摸算法参考 |

为避免影响并行 PR，touch 当前仍保留在 `Core/Src/touch/`，其文件结构和实现不调整；`App/config/touch_config.*` 仅为空占位，不引用或注册 touch。

## 许可证

项目原创代码依据 [PolyForm Noncommercial License 1.0.0](LICENSE) 公开：个人及非商业用途可免费使用、修改和分享；任何商业用途均须事先取得明确书面授权，商业授权目前免费提供。

本项目属于源码可用项目，并非 OSI 定义的开源软件。许可证适用范围、商业授权方式和第三方声明分别见 [LICENSE-SCOPE.md](LICENSE-SCOPE.md)、[COMMERCIAL_USE.md](COMMERCIAL_USE.md) 和 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

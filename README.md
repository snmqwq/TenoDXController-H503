# TenoDXController-H503

基于 STM32H503CBT6 的 TenoDX / maimai 控制器固件。工程使用 STM32CubeIDE、STM32 HAL、TinyUSB 和 WS28XX 驱动，提供触摸、灯光、Aime 读卡器和 11KRO 键盘接口。

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
| CDC1 | TenoDX LED Port | Mai2LED 灯光协议与 Magic 配置协议 |
| CDC2 | TenoDX Aime Port | Aime 主机协议 |
| HID0 | Keyboard | 11KRO 键盘报告 |

## 按键

工程只使用 `BTN0..BTN10`，共 11 个按键。

| 按键 | 默认键值 | 配置方式 |
| --- | --- | --- |
| BTN0..BTN7 | `w e d c x z a q` | 固定，不写入键盘配置 |
| BTN8 | `3` | Magic 配置，可保存到 Flash |
| BTN9 | `keypad_multiply` | Magic 配置，可保存到 Flash |
| BTN10 | `9` | Magic 配置，可保存到 Flash |

键盘以 11KRO 自定义 HID 报告发送。长按 BTN8 约 5 秒会恢复空闲灯效；一次按住只触发一次，松开后才可再次触发。

## 灯光

- 只控制 8 个逻辑按钮灯。
- `LED_PER_BIT` 默认是 2，当前允许配置为 1 至 4。
- 实际灯数为 `8 * LED_PER_BIT`。
- 默认关闭彩虹模式，上电显示全亮白灯。
- 彩虹模式下 8 个逻辑按钮具有错开的色相并持续流动，按下按钮时提高亮度并保留少量当前色调。
- 普通 Mai2LED IO 指令会接管灯光并停止空闲灯效，Magic 配置请求不会触发接管。
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
- Aime 主机协议通过 CDC2 收发。
- USART1 调试由 `PN532_UART_DEBUG_ENABLED` 控制，当前默认值为 `0U`。

原始 Python 桥接程序和移植资料保存在 `ref/Aime/`，仅作为协议参考。

## Magic 配置协议

Magic 协议与 Mai2LED 共用 CDC1，识别命令为 `0xB7`，固定序列为：

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
| touch | `0x10` | 编号与 Flash 槽已预留 |
| light | `0x20` | 已实现 |
| reader | `0x30` | 编号与 Flash 槽已预留 |
| keyboard | `0x40` | 已实现 BTN8..BTN10 配置 |

基础命令为 `READ 0x01`、`WRITE 0x02`、`SAVE 0x03`、`LOAD_DEFAULT 0x04` 和 `GET_INFO 0x05`；全局命令为 `READ_ALL 0x81`、`WRITE_ALL 0x82`、`SAVE_ALL 0x83` 和 `ENTER_DFU 0x84`。

`WRITE` 只修改 RAM，必须使用 `SAVE` 或 `SAVE_ALL` 才会写入 Flash。

## Flash 配置

链接脚本将内部 128 KiB Flash 的前 120 KiB 分配给固件，最后 8 KiB 用于配置。配置区划分为四个 2 KiB 槽：

| 槽 | 用途 |
| --- | --- |
| 0 | touch |
| 1 | light |
| 2 | reader |
| 3 | keyboard |

## 配置工具

需要 Python 3 和 pyserial：

```powershell
python -m pip install -r tools/requirements.txt
python tools/magic_config_tool.py
```

`magic_config_tool.py` 是交互式命令行工具，支持串口选择、灯光配置、BTN8..BTN10 键值配置、原始 Magic 请求和进入系统 DFU。

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
| `Core/` | 应用、外设初始化和中断代码 |
| `Drivers/` | STM32 HAL 与 CMSIS |
| `tinyusb/` | TinyUSB 协议栈 |
| `Middlewares/` | WS28XX 驱动 |
| `tools/` | Magic 配置工具 |
| `ref/Aime/` | Aime/PN532 参考代码和移植说明 |
| `ref/Mai2LED/` | Mai2LED 协议和参考实现 |
| `ref/Mai2Touch/` | Mai2Touch 协议和参考实现 |
| `ref/Touch_Algorithm/` | PSoC 数据格式、通道映射和触摸算法参考 |
| `MIGRATION.md` | 分支历史、迁移说明和开发约束 |

## 许可证

STM32 HAL、CMSIS、TinyUSB 和 WS28XX 驱动遵循各自目录中附带的许可证。项目自有代码的使用范围以仓库后续补充的许可证文件为准。

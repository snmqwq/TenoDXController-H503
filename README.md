# TenoDXController-H503

> Designed by AI

基于 STM32H503CBT6 的 TenoDX / maimai 控制器固件，提供触摸、灯光、Aime 读卡器、12KRO 键盘和运行时配置接口。

## 功能概览

| 模块 | 功能 |
| --- | --- |
| Touch | 读取双 PSoC 触摸数据并通过 USB CDC 输出 |
| LED | 支持 Mai2LED 协议、灯珠数量配置和空闲灯效 |
| Aime | 支持 PN532、FeliCa IDm 和 MIFARE 数据读取 |
| Keyboard | 12 个按键、1P/2P 主按键布局和副按键键值配置 |
| Config | 通过 Magic 协议修改触摸、灯光和键盘配置并保存到 Flash |
| Status | 使用 PC13 指示初始化、运行、写入和错误状态 |

## 硬件接口

| 功能 | 外设与引脚 | 用途 |
| --- | --- | --- |
| MCU | STM32H503CBT6 | 主控制器 |
| USB | TinyUSB Device | 3 CDC + 1 HID keyboard |
| I2C1 | PB7 SDA / PB8 SCL | PSoC 触摸从机 `0x08`、`0x09` |
| USART2 | PB4 TX / PB5 RX，115200 8N1 | PN532 |
| USART1 | PA9 TX / PA10 RX，115200 8N1 | 调试串口，默认无输出 |
| GPIO | PC13 | 状态灯，高电平点亮 |

USB 标识为 VID `0xCAFE`、PID `0x4313`，产品名为 `TenoDX Controller`。

## USB 接口

| 接口 | 名称 | 功能 |
| --- | --- | --- |
| CDC0 | TenoDX Touch Port | 双 PSoC 触摸数据 |
| CDC1 | TenoDX LED Port | Mai2LED 灯光协议 |
| CDC2 | TenoDX Aime Port | Aime 主机协议与 Magic 配置协议 |
| HID0 | Keyboard | 12KRO 键盘报告 |

## 按键与键盘

工程使用 `BTN0..BTN11`，共 12 个上拉、低电平有效按键：

- `BTN0..BTN7`：主按键，可选择 1P 或 2P 布局。
- `BTN8..BTN11`：副按键，可分别修改键值并保存到 Flash。
- 副按键引脚依次为 `PB0`、`PB1`、`PB2`、`PB10`。

| 按键 | 1P 布局 | 2P 布局 | 配置方式 |
| --- | --- | --- | --- |
| BTN0..BTN7 | `w e d c x z a q` | `keypad_8 keypad_9 keypad_6 keypad_3 keypad_2 keypad_1 keypad_4 keypad_7` | Magic 选择布局，默认 1P |
| BTN8 | `3` | `3` | Magic 修改键值 |
| BTN9 | `keypad_multiply` | `keypad_multiply` | Magic 修改键值 |
| BTN10 | `8` | `8` | Magic 修改键值 |
| BTN11 | `9` | `9` | Magic 修改键值 |

长按 BTN8 约 5 秒可恢复空闲灯效。

## 状态灯

PC13 状态灯不区分 1P/2P 布局：

- 初始化：常亮至少 1 秒。
- 正常运行：每秒闪烁一次。
- 配置写入：快速闪烁 3 次。
- 保存失败或致命错误：保持常亮。

## 灯光

- 支持 8 个逻辑按钮灯，灯珠连续排列。
- `LED_PER_BIT` 可设置为 1 至 4，默认值为 2。
- 实际灯珠数量为 `8 * LED_PER_BIT`。
- 修改灯珠数量时先发送全灭帧，再应用新配置。
- Mai2LED 颜色、更新和 Fade 命令通过 CDC1 接收，合法命令立即回复 `ACK OK`。
- 默认显示白色空闲灯效，可通过 Magic 开启流动彩虹模式。
- Mai2LED IO 接管灯光后，可长按 BTN8 恢复空闲灯效。

## 触摸数据

I2C1 读取地址为 `0x08` 和 `0x09` 的两个 PSoC。CDC0 约每 5 ms 发送一次当前快照；两个 PSoC 交替轮询，单个 PSoC 的数据约每 16 ms 更新一次。

CDC0 帧固定为 70 字节：

| 偏移 | 长度 | 内容 |
| --- | ---: | --- |
| 0 | 1 | 状态：正常运行时为 `00`，重新初始化或写入 PSoC 配置时为 `02`，校准阶段为当前 PSoC 状态 |
| 1 | 34 | `0x08` 的 17 个 16-bit 通道，小端序 |
| 35 | 34 | `0x09` 的 17 个 16-bit 通道，小端序 |
| 69 | 1 | `sum(frame[0..68]) & 0xFF` |

设备掉线后会自动重新检测；未连接设备对应的 raw 数据区域填充 `00`。启动时会等待 500 ms 确认单设备或无设备状态；确认后，一片 PSoC 缺失时另一片继续运行，并将缺失设备 17 个通道当前映射到的 Mai2Touch 区域全部强制置 `1`。两片都缺失时，当前映射表覆盖的所有区域均置 `1`。

每个已发现的 PSoC 都会先通过 I2C 写入 `[00 AD]` 请求软复位，确认其重新报告状态 `00` 后，再下发扫描配置并等待硬件校准。运行中连续 3 次读取失败会将对应设备判为离线；后台每 500 ms 快速探测一次缺失设备。恢复设备必须重新完成软复位、配置、硬件校准和软件基线，期间其区域继续保持强制置 `1`。初始化失败会按 5–60 秒退避重试，避免故障设备反复中断健康设备。仅切换 raw/Mai2Touch 输出模式不会复位或重新配置 PSoC。

通道映射和相关资料见 `ref/Touch_Algorithm/`。

### 通道映射与扫描配置

34 个物理通道按 `0..33` 编号，统一映射表的每项由 1 字节 Mai2Touch 区域编号和 1 字节 `block` 组成：

- 区域编号依次为 `A1..A8 = 0..7`、`B1..B8 = 8..15`、`C1..C2 = 16..17`、`D1..D8 = 18..25`、`E1..E8 = 26..33`。
- 每个物理通道必须且只能映射一个区域；多个物理通道可以映射到同一区域。
- `block` 由所选区域自动确定为 `A` / `B` / `C` / `D` / `E`，在线格式中保留用于显示和校验，同时供 Detector 与 PSoC 选择固定扫描参数，不能独立配置。
- 最终 Mai2Touch 输出会重新汇总所有已触发通道，因此共享同一区域的任一通道保持按下时，该区域都会保持置位。

更换映射表后，固件会立即清除旧触摸输出，安全等待当前 I2C 操作结束，再按完整 `init` 流程重新探测 PSoC、执行 `0xAD` 软复位、写入扫描配置、执行硬件校准并重置软件基线流程。Mai2Touch 模式会随后采集新基线；raw 模式下则在切换回 Mai2Touch 后开始采集。

## Aime 与 PN532

- PN532 使用 USART2 通信。
- 支持 FeliCa IDm 读取。
- 支持 MIFARE 验证及 Block 2 读取。
- Aime 主机协议与 Magic 配置协议共用 CDC2，并由接收分流和发送队列避免响应交错。
- USART1 调试由 `PN532_UART_DEBUG_ENABLED` 控制，默认关闭。

相关参考资料保存在 `ref/Aime/`。

## Magic 配置

Magic 协议通过 CDC2 通信，固定前导序列为：

```text
91 3E ED 20 7C 99 58 AC
```

请求与响应格式：

```text
request  = magic_seq + [module, cmd, param, len, payload..., sum]
response = [AC, status, module, cmd, param, len, payload..., sum]
```

| 模块 | 编号 | 功能 |
| --- | ---: | --- |
| global | `0x00` | 全局命令 |
| touch | `0x10` | 通道映射和 CDC0 输出模式 |
| light | `0x20` | 灯珠数量和彩虹模式 |
| keyboard | `0x40` | 1P/2P 布局和副按键键值 |

Touch 模块参数：

| 参数 | 读写 | 载荷 |
| --- | --- | --- |
| `0x01` | 读/写 | 68 字节完整映射表；按物理通道顺序排列 34 个 `[region, block]` 记录 |
| `0x02` | 读/写 | 1 字节 CDC0 模式：`0` 为 PSoC raw，`1` 为 Mai2Touch |
| `0x03` | 写 | 批量部分更新；每条 3 字节 `[channel, region, block]` |

`0x03` 允许一次写入 1–34 个通道，同一批次不得重复指定物理通道。固件会先校验全部记录，再原子应用整批修改，并只触发一次完整重新初始化。Magic 单包载荷上限为 248 字节。

Touch 的 Flash 及 `ALL` 序列化格式固定为 `[version=2, mode, mapping[68]]`，共 70 字节。`WRITE` 只修改 RAM 配置，执行 `SAVE` 或 `SAVE_ALL` 后才写入 Flash。Aime 和 button 不提供独立的 Magic 配置模块。

## Flash 配置

内部 Flash 最后 8 KiB 用于配置，划分为四个 2 KiB 槽：

| 槽 | 用途 |
| --- | --- |
| 0 | touch |
| 1 | light |
| 2 | 保留 |
| 3 | keyboard |

## 配置工具

配置工具需要 Python 3 和 pyserial：

```powershell
python -m pip install -r tools/requirements.txt
python tools/magic_config_tool.py
```

连接时选择 `TenoDX Aime Port`。工具支持触摸映射与输出模式、灯光配置、1P/2P 布局、副按键键值、原始 Magic 请求和系统 DFU。

常用触摸命令：

```text
touch get
touch set 0 A1
touch set 1 D4
touch set-many 2 A2 3 A2 12 B4
touch mode
touch mode mai2touch
touch save
```

`touch set` 和 `touch set-many` 为每个通道指定唯一的区域，`block` 自动派生；这些命令修改 RAM，需要再执行 `touch save` 才会持久化。

常用键盘命令：

```text
keyboard layout 1p
keyboard layout 2p
keyboard set 11 enter
keyboard set-all 3 keypad_multiply 8 9
keyboard save
```

## 构建

推荐使用 STM32CubeIDE 1.19 或兼容版本：

1. 克隆仓库。

   ```powershell
   git clone https://github.com/snmqwq/TenoDXController-H503.git
   cd TenoDXController-H503
   ```

2. 选择 **File > Import > General > Existing Projects into Workspace**。
3. 选择仓库根目录，不勾选 **Copy projects into workspace**。
4. 选择 `Debug` 或 `Release` 配置并构建。

构建完成后会保留 CubeIDE 的原始 BIN/HEX，并额外生成使用本地构建时间命名的副本：

```text
maimai_controller_H503_YYYYMMDD_HHMMSS.bin
maimai_controller_H503_YYYYMMDD_HHMMSS.hex
```

同一次构建生成的 BIN 和 HEX 共用同一个时间戳，分别保存在当前 `Debug/` 或 `Release/` 目录。

外设和引脚配置以 `maimai_controller_H503.ioc` 为准。

## 工程结构

| 路径 | 内容 |
| --- | --- |
| `App/` | 应用模块及统一的 `app_init()` / `app_task()` 入口 |
| `App/aime/` | PN532、Aime 协议及 CDC2 分流 |
| `App/config/` | Magic 命令及触摸、灯光、键盘配置 |
| `App/touch/` | PSoC 通信、触摸判定、Mai2Touch、CDC0 路由及统一任务入口 |
| `App/led/` | Mai2LED 控制 |
| `App/button/` | 按键扫描与 MultiButton 封装 |
| `App/kb/` | USB 键盘报告与键值管理 |
| `App/status/` | PC13 状态灯 |
| `Core/` | CubeMX 外设初始化、中断和 HAL 接入 |
| `Drivers/` | STM32 HAL 与 CMSIS |
| `tinyusb/` | TinyUSB 协议栈 |
| `tools/` | 配置和测试工具 |
| `ref/` | 协议、原理图和移植参考资料 |

## 许可证

项目原创代码依据 [PolyForm Noncommercial License 1.0.0](LICENSE) 公开：个人及非商业用途可免费使用、修改和分享；任何商业用途均须事先取得明确书面授权，商业授权目前免费提供。

本项目属于源码可用项目，并非 OSI 定义的开源软件。许可证适用范围、商业授权方式和第三方声明分别见 [LICENSE-SCOPE.md](LICENSE-SCOPE.md)、[COMMERCIAL_USE.md](COMMERCIAL_USE.md) 和 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

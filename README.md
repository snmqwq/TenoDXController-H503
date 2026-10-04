# TenoDXController-H503

> Designed by AI

基于 STM32H503CBT6 的 TenoDX / maimai 控制器固件，提供触摸、灯光、Aime 读卡器、12KRO 键盘和运行时配置接口。

> **触摸算法版本：`per-1.0`，对齐 `alpha-1.0`。**
> 三端流程与版本规范见根目录
> [`docs/TOUCH-ALGORITHM-PIPELINE.md`](../docs/TOUCH-ALGORITHM-PIPELINE.md)。
> STM32 只有收到项目负责人明确的同步指令后，才从 TouchHost 搬入新算法；
> 最终验收必须从**原始数据入口**回放，完整经过去重、IIR、预热、校准和判定。
>
> 当前已完成 `202609-source-csv` 固定数据集的 alpha/beta/per 三端逐帧验证：
> 23/23 文件、122,298 行通道结果一致，`first_fork=none`。实机验证仍为 pending。

## 功能概览

| 模块 | 功能 |
| --- | --- |
| Touch | 读取双 PSoC 触摸数据并通过 USB CDC 输出 |
| LED | 支持 Mai2LED 协议、灯珠数量配置和空闲灯效 |
| Aime | 支持 PN532、FeliCa IDm 和 MIFARE 数据读取 |
| Keyboard | 12 个按键、1P/2P 主按键布局和副按键键值配置 |
| Config | 通过 Magic 协议修改触摸、灯光和键盘配置并保存到 Flash |
| Status | 使用 PC13 指示初始化、运行、写入和错误状态 |

### USB 重连与 DFU

固件支持通过 Magic 全局命令 `0x85` 请求一次 USB 软件重枚举，
用于修改 Windows 设备串口后无需手动插拔即可重新识别。命令格式和
上位机等待/重连流程见
[`docs/UPPER-COMPUTER-FLASH-AND-RECONNECT.md`](docs/UPPER-COMPUTER-FLASH-AND-RECONNECT.md)。

DFU 方面，`0x84` 命令仍可用于跳转系统 Bootloader；上位机完成下载后必须由
主机侧发送 DFU 退出/复位请求（例如 `STM32_Programmer_CLI` 的 `-rst`）
才会运行新固件。当前实测中 DFU 设备枚举尚不稳定，可靠的替代路径是
ST-Link/SWD 直接烧录并 `-rst`。

## 硬件接口

| 功能 | 外设与引脚 | 用途 |
| --- | --- | --- |
| MCU | STM32H503CBT6 | 主控制器 |
| USB | TinyUSB Device | 4 CDC + 1 HID keyboard |
| I2C1 | PB7 SDA / PB8 SCL | PSoC 触摸从机 `0x08`、`0x09` |
| USART2 | PB4 TX / PB5 RX，115200 8N1 | PN532 |
| USART1 | PA9 TX / PA10 RX，115200 8N1 | 调试串口，默认无输出 |
| GPIO | PC13 | 状态灯，高电平点亮 |

USB 标识为 VID `0x0483`、PID `0x5740`，产品名为 `TenoDX Controller`。

## USB 接口

| 接口 | 名称 | 功能 |
| --- | --- | --- |
| CDC0 | TenoDX Touch Port | 双 PSoC 触摸数据 |
| CDC1 | TenoDX LED Port | Mai2LED 灯光协议 |
| CDC2 | TenoDX Aime Port | Aime 主机协议 |
| CDC3 | TenoDX Debug Port | Magic 配置协议 |
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
| BTN9 | `9` | `9` | Magic 修改键值 |
| BTN10 | `8` | `8` | Magic 修改键值 |
| BTN11 | 无（`0`） | 无（`0`） | Magic 修改键值 |

长按 BTN8 约 5 秒可恢复空闲灯效。

### CDC IO 模式

CDC0 还可以由上位机切换到一个独立的游戏 IO 模式。该模式默认关闭；上电后只有收到命令才启用，
拔掉 USB 后自动恢复默认：

```text
启用: A6 02 01 A9
关闭: A6 02 00 A8
```

启用后 CDC0 转为 9 字节状态帧：

```text
A5 buttons_lo buttons_hi touch0 touch1 touch2 touch3 touch4 seq
```

- `buttons` bit0..7 = BTN0..BTN7，bit8 = BTN8（选择键），bit9 = BTN9（Test），bit10 = BTN10（Service）。
- BTN11 在该模式下仍保持 HID 键盘输出，便于保留正常键盘功能。
- `touch` 是 34 位游戏区域掩码，与 Mai2Touch 的位序一致。
- 周期为 2 ms；触摸管线未就绪时触摸位为 0。
- IO 模式独占 CDC0，raw 70B 和 Mai2Touch 9B 不会同时输出。

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

### 两种 CDC0 模式的数据处理工作流（重要）

上位机是**调试端**，不是最终运行链路。判定逻辑由三端共用的共享核心提供
（根目录 `touch_core/`），不在此处单独实现；两种模式的职责必须严格区分：

```text
raw 模式（TouchHost 调试链路）
PSoC -> STM32 读取原始帧 -> CDC0 70B -> TouchHost
                                      └─ SerialReader：按片 34B 去重
                                      └─ SignalPipeline：IIR -> 100 帧预热
                                      └─ 共享判定核心：A-E 判定
                                      └─ COM17 -> 游戏

Mai2Touch 模式（游戏直连链路）
PSoC -> STM32 读取原始帧 -> STM32 内部执行
       去重 / 限速 / IIR / 预热 / 校准，再调用共享判定核心
                              └─ 34 位区域掩码 -> 9B Mai2Touch 帧 -> 游戏
```

> 判定只有一份实现，位于根目录 `touch_core/`；分发与验收约束见
> `docs/TENO-TOUCH-CORE-DISTRIBUTION.md`。

关键规则：

- **raw 模式下 STM32 不做判定，也不运行 `touch_pipeline`**。它只按 2 ms 发送
  70B 快照；去重、IIR、预热、校准和判定全部由 TouchHost 完成。
- **Mai2Touch 模式下 STM32 自己运行完整前处理与判定**，最后只把 9B 掩码发给游戏。
- 两种模式不能同时处理同一份数据，CDC0 路由互斥。

### STM32 的 TouchHost 等价前处理（Mai2Touch 模式）

设备端处理顺序固定为：

```text
1. 收集两片 PSoC 的最新快照
2. 每个 2 ms CDC 包提交一次，不按单次 I2C 完成推进
3. 按片比较 34 字节 payload，完全相同的片丢弃
4. 对发生变化的片执行 IIR: history = (history + raw) / 2
5. 100 个有效包预热
6. 再收集 100 个有效包的 setup_raw 基线
7. 对发生变化的片逐通道执行 detector.py 等价判定
8. raw == 0 强制按下；同区域多通道按最后更新通道覆写
9. 打包 34 位区域掩码为 9B Mai2Touch 数据帧
```

> **去重语义**：按片 34B payload 比较。只要该片任一字节变化，该片 17 个通道
> 全部进入管线；整片完全相同则完全不推进 IIR、预热、校准或判定。
> 这与 TouchHost `SerialReader` 的 `dev1_changed` / `dev2_changed` 一致。

> **为什么要按包提交**：如果每完成一片 I2C 就读入管线，两片交替会让
> 预热/校准计数速度翻倍；只有每 2 ms 提交一次两片快照，计数才与 TouchHost
> 每个 70B packet 的行为一致。

> **精度**：IIR 历史与 `setup_raw` 累加使用 `double`，与上位机一致；最终
> 通道值按 Python `int()` 向零截断。
>
> **判定不在本仓库实现**：判定语义与参数统一由共享核心提供
> （根目录 `touch_core/`），固件只保留适配层与管线前处理。
> 细节以核心代码为准，见根目录 `docs/TENO-TOUCH-CORE-DISTRIBUTION.md`。

### CDC0 输出模式：默认自动识别

CDC0（Touch 口）被两方共用，固件**默认按收到的帧特征自动选择**，不需要手工切模式：

| 当前侧 | 判据 | 动作 |
| --- | --- | --- |
| raw（上位机调试） | 收齐一条**语法合法**的 6 字节 Mai2Touch 命令 `{` + 4 字节 + `}` | 切到 Mai2Touch，并把这条命令**补喂**给 Mai2Touch 解析 |
| Mai2Touch（游戏） | 收到 raw 参数帧头 `0xAA 0x01` | 切回 raw，剩余字节留给 raw 流程 |

- **为什么要校验完整的 6 字节**：raw 参数帧的 136 字节 payload 全是扫描参数（`res/mod/sns/div` ≤ 16），
  但**校验和字节可能等于 `0x7B`（`{`）**。上位机每秒重发一次参数帧，若"见到 `{` 就切"迟早会误切。
  Mai2Touch 的数据字节只用到低 5 位、命令全是 ASCII，流里**不可能出现 `0xAA`**，所以反方向只需两字节确认。
- **为什么要补喂**：`{STAT}`（开始推流）和 `{L/R..}`（游戏靠逐条回显推进配置）都不是"复位"语义，
  被 raw 侧读走用于校验后必须原样重放给 Mai2Touch，否则游戏会卡在配置阶段或在 Drive 阶段收不到触摸数据。
- **`{STAT}` 之后立刻开始推流**：软件基线管线还没就绪时先发**空白帧（34 位全 0）**，4 ms 一帧，
  不等 PSoC 数据 —— 游戏进入 Drive 后只收不发，收不到任何触摸结果就会报错。
- `{HALT}` 之后**完全静默**（游戏靠读超时进入配置模式），`{RSET}` 无条件复位发送状态。
- 切到 Mai2Touch **不会复位/重配 PSoC**（游戏此时正在配置阶段）；切回 raw 会重走一次初始化（约 1~2 秒触摸输出中断）。
- Magic `touch` 参数 `0x02` 写 `0`/`1` 可**强制**固定到某一侧（调试用），写 `2` 恢复自动识别；
  写入与当前一致的值不会退出自动识别。`touch defaults` 也恢复自动识别。

I2C1 读取地址为 `0x08` 和 `0x09` 的两个 PSoC。CDC0 约每 2 ms 发送一次当前快照；两个 PSoC 交替轮询，单个 PSoC 的数据约每 4 ms 更新一次（约 250 Hz）。

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
- Aime 主机协议独占 CDC2，不再接收或响应 Magic 配置命令。
- USART1 调试由 `PN532_UART_DEBUG_ENABLED` 控制，默认关闭。

相关参考资料保存在 `ref/Aime/`。

## Magic 配置

Magic 协议通过独立的 CDC3 `TenoDX Debug Port` 通信。该端口只承载 Magic 二进制协议，不输出文本日志。固定前导序列为：

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
| `0x02` | 读/写 | 1 字节 CDC0 模式：`0` 强制 PSoC raw，`1` 强制 Mai2Touch，`2` **自动识别**（默认，读回也是 `2`；当前实际在跑哪一侧见 `0x04` 状态） |
| `0x03` | 写 | 批量部分更新；每条 3 字节 `[channel, region, block]` |
| `0x04` | 读 | PSoC 状态快照；`GET_INFO` 回 10 字节能力表（第 8 字节 bit0 = 支持 `0x05`，第 9/10 字节为只读参数 `0x04`/`0x06`） |
| `0x05` | 写 | 1 字节 `0x01`：清除固件缓存的上位机扫描参数并重走初始化，使 CDC0 重新上报 `0x01`，让上位机重新下发扫描参数帧 |
| `0x06` | 读 | 校准量快照：**106 字节** = 每片 `寄生电容 Cp(17 通道 × 2 字节大端) + 补偿 IDAC(17 通道 × 1 字节) + 快照年龄(2 字节大端, ms)`，片内步进 53 字节；未校准成功的片整片为 0 |

`0x03` 允许一次写入 1–34 个通道，同一批次不得重复指定物理通道。固件会先校验全部记录，再原子应用整批修改，并只触发一次完整重新初始化。Magic 单包载荷上限为 248 字节。

> 扫描参数只在固件上报 `0x01` 时才由上位机下发，而首次配置成功后固件不会再回到该状态。
> 因此更换扫描参数需要显式触发：Magic `0x05`（配置工具命令 `touch resend`）、切换 CDC0 模式，或由上位机主动重发参数帧。

Touch 的 Flash 及 `ALL` 序列化格式固定为 `[version=2, mode, mapping[68]]`，共 70 字节。`WRITE` 只修改 RAM 配置，执行 `SAVE` 或 `SAVE_ALL` 后才写入 Flash。Aime 和 button 不提供独立的 Magic 配置模块。

## Flash 配置

内部 Flash 最后 8 KiB 用于配置，按 **64 条 × 128 字节滚动记录**保存，而不是四个固定槽。
每条记录带 CRC32 并覆盖 touch / light / reserved / keyboard 四个逻辑槽；读取时取
`sequence` 最新的一条。兼容读取旧版四个 2 KiB 槽的记录，新写入一律使用滚动记录。

| 逻辑槽 | 用途 |
| --- | --- |
| 0 | touch |
| 1 | light |
| 2 | 保留 |
| 3 | keyboard |

## 配置工具

设备配置、综合测试与 DFU 刷写统一由工作区根目录的
`tools/TenoDXConfigurationTool/` 提供（独立仓库，含图形界面）。安装与用法见该目录 README。

连接时选择 `TenoDX Debug Port`。工具支持触摸映射与输出模式、灯光配置、1P/2P 布局、副按键键值、原始 Magic 请求和系统 DFU。

常用触摸命令：

```text
touch get
touch set 0 A1
touch set 1 D4
touch set-many 2 A2 3 A2 12 B4
touch mode
touch mode mai2touch
touch save
touch resend
touch cp
touch status
```

`touch set` 和 `touch set-many` 为每个通道指定唯一的区域，`block` 自动派生；这些命令修改 RAM，需要再执行 `touch save` 才会持久化。
`touch resend` 用于让上位机重新下发扫描参数（见上文 `0x05` 说明）。
`touch cp` 读取两片 PSoC 的寄生电容 Cp 与补偿 IDAC（并显示快照年龄）。
这两项只在 PSoC 每次校准时测量一次（上电或重新下发参数触发重配），
所以**数值长时间不变是正常的**；快照年龄在重配后归零，之后单调增长。

常用键盘命令：

```text
keyboard layout 1p
keyboard layout 2p
keyboard set 11 enter
keyboard set-all 3 keypad_multiply 8 9
keyboard save
```

## 构建

> 上位机实现 USB 自动重连与 DFU 刷写后自动启动时，请参考
> [`docs/UPPER-COMPUTER-FLASH-AND-RECONNECT.md`](docs/UPPER-COMPUTER-FLASH-AND-RECONNECT.md)。

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

仓库还提供独立于 CubeIDE 的 CMake 构建入口。默认使用与 CubeIDE `Release`
一致的 `-Os/-g0` 参数，也可以通过 `CMAKE_BUILD_TYPE=Debug` 构建调试版本
（Debug 默认 `-Og`；`-O0` 会让 FLASH 超出 120K 区域）：

```powershell
cmake -S . -B build/firmware `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build/firmware --parallel
```

### 本机单元测试（不需要板子）

`tests/native_mode_test.c` 直接编译**真实**的 `cdc_manager.c` / `touch_mode.c` /
`mai2touch.c`（只 stub TinyUSB、HAL、`touch_pipeline` 和 `tenodata` 的少数函数），
复刻真实主循环的 tick 顺序，覆盖 CDC0 模式自动识别与 Mai2Touch 会话行为：

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\build_native_test.ps1
```

覆盖场景：完整握手（`{RSET}` → 配置回显 → `{STAT}`）、`{STAT}` 立即发空白帧、
`{HALT}` 完全静默、双向自动切换、raw 帧校验和等于 `0x7B` 时不误切、
非法命令体不切换、触发切换的命令被正确补喂、Magic 强制模式优先、半截帧头不切换。

另有 `tests/pipeline_parity_test.c`，直接编译真实的
`touch_pipeline.c` / `button_detector.c` / `detector.py` 对应逻辑，验证：

- 片级 34B 去重；
- 100 个有效包预热 + 100 个有效包校准后才 ready；
- IIR 首帧 seed 与截断；
- A 区延迟触发、触发锁存和半幅释放；
- B/C/D/E 的死区记忆；
- 真实 TouchHost 日志中的 A5 idle 原始样本不产生幻触。

> 当前仓库里的 TouchHost CSV 只记录了判定后的 `Raw/SetupRaw/Diff/...` 行，
> 没有保存 CDC0 的原始 70B 帧，所以完整 raw 流回放仍需补抓一份原始日志；
> 现有回归用 CSV 中记录的真实 Raw 序列验证判定器。

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\build_pipeline_parity_test.ps1
```

三端完整管线对比使用：

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\build_three_way_parity.ps1
```

推送到 `main`、创建面向 `main` 的 Pull Request 或手动运行 **Build firmware**
工作流时，GitHub Actions 会使用 Arm GNU Toolchain 13.3.Rel1 构建 Release 固件。
运行结果中的 Artifact 包含带 UTC 时间戳的 BIN/HEX、ELF、MAP、空间占用报告和
对应源码提交号；Artifact 保留 30 天。

## 工程结构

| 路径 | 内容 |
| --- | --- |
| `App/` | 应用模块及统一的 `app_init()` / `app_task()` 入口 |
| `App/aime/` | PN532、Aime 协议及专用 CDC2 接口 |
| `App/config/` | Magic 命令、专用 CDC3 Debug 接口及触摸、灯光、键盘配置 |
| `App/touch/` | PSoC 通信、触摸判定、Mai2Touch、CDC0 路由及统一任务入口 |
| `App/led/` | Mai2LED 控制 |
| `App/button/` | 按键扫描与 MultiButton 封装 |
| `App/kb/` | USB 键盘报告与键值管理 |
| `App/status/` | PC13 状态灯 |
| `Core/` | CubeMX 外设初始化、中断和 HAL 接入 |
| `Drivers/` | STM32 HAL 与 CMSIS |
| `tinyusb/` | TinyUSB 协议栈 |
| `tests/` | 本机单元测试（编译真实模块，不需要板子） |
| `ref/` | 协议、原理图和移植参考资料 |

## 许可证

项目原创代码依据 [PolyForm Noncommercial License 1.0.0](LICENSE) 公开：个人及非商业用途可免费使用、修改和分享；任何商业用途均须事先取得明确书面授权，商业授权目前免费提供。

本项目属于源码可用项目，并非 OSI 定义的开源软件。许可证适用范围、商业授权方式和第三方声明分别见 [LICENSE-SCOPE.md](LICENSE-SCOPE.md)、[COMMERCIAL_USE.md](COMMERCIAL_USE.md) 和 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

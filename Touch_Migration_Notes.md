# Touch 功能架构迁移记录

> 触摸算法版本：`per-1.0`，对齐 `alpha-1.0`。
> 三端流程、版本号与回放数据规范见根目录
> `docs/TOUCH-ALGORITHM-PIPELINE.md`。
> 仅当项目负责人明确要求同步时才执行 α → β → per 迁移。

## 迁移节点

### 节点 1: 重命名 mai2touch → tenodata
- **日期**: 2026-07-29
- **变更**: 将 `mai2touch_app.*` 重命名为 `tenodata.*`，所有内部标识符同步替换
- **原因**: 原始命名不准确，该模块实现的是与 C#/PSoC 的 CDC 数据通讯，并非标准 Mai2Touch 协议

### 节点 2: 代码分离重构
- **日期**: 2026-07-29
- **变更**:
  - 提取 `tenodata_config.c/.h` — 34 通道硬件扫描参数硬编码表
  - 提取 `psoc_comm.c/.h` — PSoC I2C 通信层（探测/配置/校准/数据读取）
  - `tenodata.c` 保留状态机 + CDC 推流逻辑
- **原因**: 解耦通信与处理逻辑，为 MCU 独立运行做准备（不再依赖上位机下发配置）

### 节点 3: 取消上位机配置依赖
- **日期**: 2026-07-29
- **变更**: 删除 `process_cdc_rx()` 和 `WAIT_HOST_CONFIG` 状态，改为直接读取本地硬件扫描参数
- **影响**: STM32 不再等待 PC 下发 139 字节配置帧，上电后直接配置 PSoC 并启动

### 节点 4: 修正物理通道映射
- **日期**: 2026-07-29
- **变更**: 修正 34 通道 PhysicalToLogicalMap 顺序为:
  `E4,D4,B3,A3,C1,E3,D3,B2,A2,E2,D2,B1,A1,E1,D1,B8,A8,E8,D8,B7,A7,C2,E7,D7,B6,A6,E6,D6,B5,A5,E5,D5,B4,A4`
- **影响**: C# HardwareConfig.cs 和 STM32 tenodata_config.c 同步更新

### 节点 5: CDC 通信管理层 + mai2touch 测试模块 (本次)
- **日期**: 2026-07-29
- **新增**:
  - `cdc_manager.c/.h` — CDC 收发统一管理层，带模块路由开关
  - `mai2touch.c/.h` — Mai2Touch 协议测试模块（ASCII 帧格式）
- **修改**:
  - `tenodata.c` — 将直接 CDC 调用替换为 cdc_manager API
  - `main.c` — 引入 mai2touch 模块

### 节点 6: Touch 应用层迁移与统一配置
- **日期**: 2026-08-03
- **变更**:
  - 将 Touch 检测、PSoC 通信、CDC 路由和 Mai2Touch 协议整体迁移到 `App/touch/`
  - 新增 `touch_app_init()` / `touch_app_task()`，顶层应用不再直接调度内部 Touch 子模块
  - 将物理通道映射集中为“单一 Mai2Touch 区域编号 + 自动派生 block”表，并接入 Magic/Flash；区域允许被多个通道复用
  - 映射表变更后立即清除旧输出并排空当前 I2C 操作，再完整执行探测、写配置、硬件校准和软件基线流程重置；raw 模式下的新基线采集延后到切回 Mai2Touch
- **影响**: `Core/` 仅保留生成的外设与 HAL 基础代码，不再承载 Touch 业务实现

### 节点 7: PSoC 软复位与缺失设备降级运行
- **日期**: 2026-08-05
- **变更**:
  - 初始化时向已发现 PSoC 的状态偏移写入 `0xAD`，确认重启状态后再配置和校准
  - 支持 0/1/2 台 PSoC 运行；缺失设备 raw 数据保持为零，其当前映射区域在 Mai2Touch 输出中强制置位
  - 运行时按设备累计通信失败并隔离，后台快速探测恢复设备；恢复完成完整初始化和软件基线前继续强制置位
  - 增加复位、校准和异步 I2C 事务超时，以及故障恢复退避，避免单台故障阻塞健康设备
- **影响**: 单片 PSoC 缺失或运行中掉线时，另一片仍可继续提供 raw 数据和触摸判定

### 节点 8: 上位机扫描参数「二次下发」入口 + 软复位加严
- **日期**: 2026-09-25
- **背景**: 首次配置成功后，`host_config_override_valid` 永不复位，固件因此永远不再进入
  `STATE_WAIT_HOST_CONFIG`、不再上报 `0x01`；而上位机只在收到 `0x01` 时才下发 139 B
  扫描参数帧，导致"再次下发新参数"这条链路整体断开（改 yaml + 重启上位机也无效）。
  同时该标志跨模式粘连，使 raw 模式调过参后切回 Mai2Touch 会沿用上位机参数而非本地档位。
- **变更**:
  - `tenodata.h/c`: 新增 `tenodata_clear_host_config_override()`（只清覆盖，不触发重配）与
    `tenodata_force_host_config_resend()`（清覆盖 + `request_initialization()`，从而重新
    回到 `STATE_WAIT_HOST_CONFIG` 并上报 `0x01`）
  - `touch_app.h/c`: `touch_app_apply_config()` 增加 `force_host_config_resend` 形参；
    **模式切换 (`mode_changed`) 时清除 host 参数覆盖**；新增 `touch_app_force_host_config_resend()`
  - `App/config/touch_config.c`: Magic touch 新增参数 `0x05 FORCE_RESEND`（载荷 1 字节 `0x01`）；
    `GET_INFO` 由 7 B 扩为 8 B，末字节为能力位（bit0 = 支持 `0x05`）
  - `App/touch/psoc_comm.c`: `psoc_comm_soft_reset()` 先做一次状态读刷新 I2C 状态，再**连写两遍
    `0xAD`**（间隔 2 ms）；PSoC 侧 `Check_Soft_Reset_Command()` 每次调用都会清 EZI2C 活动位，
    双重写入可保证至少一次被观测到，避免偶发漏检被误判成 legacy 固件而要求断电重启
  - `App/touch/tenodata.c` 的软复位/legacy 判定逻辑本身未改（`STATE_PREPARE_PSOC` 与
    `STATE_VERIFY_PSOC_REBOOT` 的重试与判定保持不变）
- **影响**: 上位机可随时通过 Magic `0x05` 或重新下发 139 B 帧让固件重走
  「软复位 → 写配置 → 校准 → 重建基线」；raw 模式下参数来源不再跨模式粘连。
  `0x05` 会清除本地缓存的上位机参数，因此若上位机不在线，重配后将落到固件本地固定档位。
- **验证**: CMake + Ninja + Arm GNU Toolchain 13.3.rel1 Release 全量构建通过
  （`text 66056 / data 188 / bss 15656`）

### 节点 9: 寄生电容 Cp 与补偿 IDAC 读取（Magic 只读参数 0x06）
- **日期**: 2026-09-25
- **背景**: PSoC 在校准流程里会把两个校准量写进 EZI2C——状态 `0x13` 写 17 通道寄生电容 Cp
  （偏移 `35`，每通道 2 字节大端），状态 `0x14` 的 SAR 校准（`Tune_CapSense_CompIDAC_SAR()`，
  按目标 raw 百分比调整补偿 IDAC）之后写 17 通道补偿 IDAC（偏移 `69`，每通道 1 字节）。
  但 STM32 的读取长度硬编码为 35 字节（只读 Status + Raw），这两段从未被读取，
  上位机自然看不到。
- **变更**:
  - `App/touch/psoc_comm.h/c`: 新增 `PSOC_EZI2C_OFFSET_CP = 0x23`(35)、
    `PSOC_EZI2C_OFFSET_COMP_IDAC = 0x45`(69)、`PSOC_COMM_CP_LENGTH = 34`、
    `PSOC_COMM_COMP_IDAC_LENGTH = 17`、`PSOC_COMM_CAL_LENGTH = 51`、
    `PSOC_STATUS_CP_READY = 0x13`、`PSOC_STATUS_COMP_READY = 0x14`；
    新增 `psoc_comm_read_cal()`（**一次事务读连续 51 字节**并拆成 Cp + 补偿 IDAC），
    `psoc_comm_read_cp()` 保留为薄封装
  - `App/touch/tenodata.h/c`: 新增 `cp_raw[2][34]` / `cp_valid[2]` 与
    `comp_idac[2][17]` / `comp_idac_valid[2]` 快照；`capture_cal_if_ready()` 在
    `STATE_WAIT_CALIBRATION` 轮询中**按各自写入时机**分别到点读一次
    （Cp ≥ `0x13`、补偿 IDAC ≥ `0x14`），之后作为静态量缓存；
    `start_initialization()` 一并清空（重配会重测）；新增 `tenodata_get_cp()` /
    `tenodata_get_comp_idac()`
  - `App/touch/touch_app.h/c`: 新增 `touch_app_get_cp()` / `touch_app_get_comp_idac()`
  - `App/config/touch_config.c`: Magic touch 只读参数 `0x06` 载荷由 68 B 扩为 **102 B**
    （每片 = Cp 34B + 补偿 IDAC 17B，片内步进 51 B）；`_Static_assert` 同步为 102
  - `tools/magic_config_tool.py`: `touch cp` 命令同时打印 Cp 与补偿 IDAC
- **影响**: Cp 与补偿 IDAC 成为可通过 Magic 按需读取的静态量，**不改动触摸数据流**
  （CDC0 帧格式不变）；上位机据此在密集型表格中显示 `Cp` 与 `Comp` 两列。
  两者都只在 PSoC 配置/校准时更新，运行时不变。
- **验证**: Release 全量构建通过；上位机以"按真实 Magic 帧应答 102 B 载荷的伪造串口"
  完成端到端验证（识别 Debug Port → 读 102 B → 分别解析出 Cp 与 Comp → 读完关闭端口）

### 节点 10: Debug 构建超出 120K FLASH —— Debug 默认优化级别改为 -Og
- **日期**: 2026-09-25
- **背景**: STM32CubeIDE 的 **Debug** 配置链接失败：
  `section '.rodata' will not fit in region 'FLASH'` / `region 'FLASH' overflowed by 1616 bytes`。
  Release 配置能过，所以之前没暴露。
- **分析**:
  - 链接脚本给 FLASH 的是 **120K**（`STM32H503CBTX_FLASH.ld:48`，= 128K 减去最后 8K 配置区），
    上限 122880 B。
  - CubeIDE 两个配置的差异只有优化/调试级别：**Release = `-Os -g0`**、**Debug = `-O0 -g3`**
    （`.cproject` 里 Debug 的 `optimization.level` 为空 → 默认 `-O0`）。
  - 实测三组数据：
    | 配置 | .text | .rodata | .data | FLASH 合计 | 余量 |
    |---|---:|---:|---:|---:|---:|
    | Release `-Os -g0` | 64256 | 1408 | 188 | 66468 | +56412 |
    | Debug `-Og -g3` | 76780 | 1436 | 188 | 79020 | +43860 |
    | Debug `-O0 -g3` | 122236 | 1444 | 192 | **123872** | **−1616** |
  - 结论：**不是某个新功能写爆了，而是 `-O0` 让全工程代码体积约为 `-Os` 的 1.9 倍**，
    把 120K 顶掉了约 1.6K（仅超出 1.3%）。`-g3` 的调试段不占 FLASH（非 alloc 段）。
- **变更**:
  - `CMakeLists.txt`: 新增缓存变量 `FIRMWARE_DEBUG_OPT`（默认 **`-Og`**），
    Debug 配置的优化级别由原来的硬编码 `-O0` 改为引用它：
    `cmake -DCMAKE_BUILD_TYPE=Debug -DFIRMWARE_DEBUG_OPT=-O0` 可恢复完整 `-O0`（会超出 120K）。
  - CubeIDE 侧需要用户自己在工程属性里把 **Debug 的 Optimization Level 设为 `-Og`**
    （或 `-Os`）；`.cproject` 是 IDE 管理的文件，未擅自改动。
- **影响**: Debug 配置恢复可构建（余量约 42.8K）；`-Og` 的 DWARF 仍然是"真实"的
  （不改变变量存在性），可在该级别下单步调试；`-O0` 会超出 FLASH 约 1.6K。
- **验证**: CMake + Ninja + Arm GNU Toolchain 13.3.rel1 下 Debug(`-Og`) 链接成功
  （text 78832 / data 188 / bss 15808）；Release 复核仍为 66468 B。

### 节点 11: 修复 Cp / 补偿 IDAC 永远抓不到（改抓 0x02 + RUNNING 定期补抓）
- **日期**: 2026-09-25
- **背景**: 上位机读 Magic `0x06` 恒返回 102 字节全 0 。用
  `TouchHost/tools/magic_probe.py` 实测：固件状态机 = `0x06`(RUNNING)、
  两片 status 均为 `0x02` 且 flags 正常、`0x06` 长度正确（102）——说明固件版本与
  协议都对，只是**校准量快照从未被填充**。
- **分析**: `STATE_WAIT_CALIBRATION` 每 `TENODATA_CALIB_CHECK_PERIOD_MS`(100ms)
  才轮询一次 PSoC 状态，而 PSoC 的进度是顺序写 `0x13`(Cp 已写) →
  `0x14`(SAR 校准完 / 补偿 IDAC 已写) → `0x15` → `0x02`，每一级只持续
  **一条 I2C 写入的时间（几毫秒）**。节点 9 里 `capture_cal_if_ready()` 的条件要求
  "状态 ≥0x13 且 ≤0x02"，实际**几乎不可能命中那两个中间进度**；
  而 `0x02` 是持久状态，所以状态机能看到"校准完成"却永远没抓到校准量。
- **变更** (`App/touch/tenodata.c`):
  - `capture_cal_if_ready()`：把"状态 == `0x02`（校准完成）"也算作**可读时机**。
    依据：Cp 区(35..68) 与补偿 IDAC 区(69..85) 由 PSoC 在初始化时写一次、
    运行中不再改动，`0xAD` 软复位才清零，所以 `0x02` 读到的一样是本次校准结果。
  - 新增 `retry_cal_capture()` + `last_cal_capture_tick[]`：RUNNING 状态下若某片快照
    仍无效，**每 `TENODATA_CAL_CAPTURE_RETRY_MS`(500ms) 补抓一次**
    （仅在总线空闲且无在途读时执行，不干扰 2ms 的 I2C 轮询节奏）。
  - `start_initialization()` 一并清 `last_cal_capture_tick`。
- **影响**: 每次校准后都能可靠抓到 Cp / 补偿 IDAC；上位机可**随时重复读取**
  （不再出现"只能读一次、要重开软件"的现象）。
- **验证**: 重新烧录后实测 `0x06` 连续 5 次读取全部成功（每次 34 个非零字节、
  两片 `valid=[True,True]`，耗时 2~6ms）；Cp 实测每通道 3~6。
- **注意**: 本次实测**补偿 IDAC 全为 0**，而 Cp 有值。上位机显示逻辑已就绪，
  一旦 PSoC 写出非零补偿值即会显示；若确认应当非零，需另查 PSoC 侧的
  `Tune_CapSense_CompIDAC_SAR()`（属 `PSoC Creator/` 内容，本次未改动）。

### 节点 12: 校准量"确认读" —— 消除刚校准完读到的半写状态
- **日期**: 2026-09-25
- **背景**: 用户反馈"要重新插拔才有新的 Cp/补偿 IDAC"。实测复核（用 Magic `0x05`
  触发重配各 3 轮，只走 Debug Port 不占 Touch Port）：
  - **Cp 每次重配都会刷新**（`0xAD` 软复位有效），不需要插拔：
    实测片0 通道1 在 `193 → 0` 之间变化、通道7 `5 → 28`、通道8 `5 → 10`；
    用户看到的"必须插拔"是**旧固件**的行为（节点 11 之前 RUNNING 后不再抓取）。
  - 但第 1 轮出现**片0 前 7 个通道为 0**，之后各轮完全稳定 —— 说明"刚看到 `0x02`
    就立刻读"有时会读到 PSoC **尚未写完**的区域。
- **变更** (`App/touch/tenodata.c`):
  - 新增 `cal_capture_confirm_pending[]` 与 `confirm_cal_capture()`：
    首轮抓到成功后**延迟 `TENODATA_CAL_CONFIRM_DELAY_MS`(250ms) 再读一次**，
    用稳定值覆盖首轮快照（仅在总线空闲且无在途读时执行）。
  - `start_initialization()` 一并清 `cal_capture_confirm_pending`。
- **影响**: 上位机读到的 Cp/补偿 IDAC 不再出现"前几个通道为 0"的瞬态。
- **验证**: Release 构建通过（text 66880 / data 188 / bss 15776）；
  重配后连续读取稳定（3 轮中第 2、3 轮逐字节一致）。
- **遗留待查**: 补偿 IDAC 实测恒为 0（两片 × 17 通道），而 Cp 有值（3~6）。
  主机侧读取路径已用 Cp 验证正确，故倾向 PSoC 侧
  `CapSense_1_GetCompensationIDAC()` 在 SAR 标定后确实算出 0；
  需要 PSoC 侧打印 raw / Ccomp / target 才能判定（属 `PSoC Creator/`，本次未改动）。

### 节点 13: 0x06 增加"快照年龄"字段（102 → 106 字节）
- **日期**: 2026-09-25
- **背景**: 用户反馈"要插拔才能读到新的 Cp/comp"。实测复核（发"确定不同"的参数触发重配）：
  状态轨迹 `0x02 → 0x05 → 0x06`，说明**确实重配了**，且重配后 `valid=[True,True]`
  —— 每次校准都会重新抓取，不需要插拔。**Cp 逐字节不变是因为测量对象没变化**
  （用户当时没接触摸面板、只有一块裸 PCB，寄生电容物理恒定）；补偿 IDAC 则有变化
  （`[0,0,1,...] → [0,2,1,...]`），证明 SAR 标定确实重跑了。
  真正的问题是：**上位机无法判断读到的是"刚校准测的"还是"上电时的"**。
- **变更**:
  - `App/touch/tenodata.h/c`：新增 `cal_captured_tick[]` 与
    `tenodata_get_cal_age_ms()`（返回快照距今毫秒数，无有效快照返回 false）。
  - `App/touch/touch_app.h/c`：新增 `touch_app_get_cal_age_ms()`。
  - `App/config/touch_config.c`：`0x06` 每片载荷由 `Cp(34B) + 补偿IDAC(17B)`
    扩为 `Cp(34B) + 补偿IDAC(17B) + 快照年龄(2B 大端, ms)`，
    片内步进 51 → **53 字节**，总长 102 → **106 字节**；`_Static_assert` 同步。
  - `tools/magic_config_tool.py`：`touch cp` 按新步进解析并打印"快照年龄"。
- **影响**: 上位机可以显示"这份 Cp/Comp 是多久以前测的"：数值不变而年龄在涨
  = PSoC 没有重新测量（只有上电或重新下发参数触发重配时才会重测，年龄归零）。
- **验证**: Release 构建通过；重配前后实测 age 值随重配归零、随后单调增长。
- **兼容性**: **契约不向后兼容**——上位机已按 106 字节解析，需与固件同步升级
  （旧固件会因长度不符被上位机判为错误）。

### 节点 14: CDC0 走 raw 还是 Mai2Touch —— 改成自动识别 + {STAT} 立即发空白帧
- **日期**: 2026-09-25
- **背景**: 用户同时用两套协议：`tenodata`(raw) 是自己调试用的，`mai2touch` 是游戏读的。
  每次切换都要手工改 Magic 模式，太麻烦。诉求：**发过来的是 raw 特征就走 raw 流程，
  是 Mai2Touch 特征就用 STM32 自带的扫描参数工作**；而且 Mai2Touch 在跑完 `{STAT}` 前的
  交流之后**必须先发空白触摸帧，不要等 PSoC 数据**，否则游戏收不到触摸结果会报错。
- **分析**:
  - 两个方向的可靠度**不对称**：
    - Mai2Touch 的流里**不可能出现 `0xAA`**（触摸数据每字节只用低 5 bit ≤ `0x1F`，
      命令全是 ASCII），所以 `0xAA 0x01` 是可靠标志。
    - raw 帧的 136 B payload 全是小数值（`res/mod/sns/div` ≤ 16），但**校验和字节可能是
      `0x7B`(`{`)**。上位机每秒重发一次参数帧，若"见到 `{` 就切"，迟早误切。
  - 因此 raw→Mai2Touch 必须**凑齐 6 字节并校验命令语法**才切。但这样一来整条命令已被
    raw 侧读走，而 `{STAT}`（开始推流）与 `{L/R..}`（游戏靠逐条回显推进配置，`_countRecv`
    用回显里的 sensor 字符当游标）都**不是"复位"语义**，靠 `mai2touch_init()` 顶替会：
    丢 `{STAT}` → Drive 阶段一个字节都收不到 → 游戏报错；丢 `{LAr2}` → 配置阶段卡死。
    → 必须**补喂（replay）**触发切换的那条原始命令。
  - `{HALT}` 之后必须**完全静默**（游戏靠读超时进配置模式），所以空白帧只能在 `{STAT}`
    之后发，不能在 `{HALT}` 之后发。
- **变更**:
  - **新增 `App/touch/touch_mode.h/c`**：模式识别器。
    - raw 侧：按位置收集 `{` + 4 B + `}`，再校验命令体语法
      （`RSET`/`HALT`/`STAT`，或 `L|R` + sensor `0x41..0x62` + `r|k`）。
      sensor 是 **A1 起连续 34 个 ASCII 码**（C 区只有 2 个点，不是"区字母+序号"）；
      sensitivity 的值**可能是控制字符**（默认档 A 区 `0x28`、B~E 区 `0x14`），
      所以第 4 个命令字节允许任意值。
    - Mai2Touch 侧：只找 `0xAA` 然后 `0x01`。
    - 切换**不在读字节当刻执行**，而是置 `switch_pending` 由 `touch_mode_task()` 执行；
      两个消费者用 `touch_mode_pending_to_*()` 决定"是否立刻停手"，避免吃掉对方命令的剩余字节。
    - `touch_mode_take_replay()` 把触发切换的原始 6 字节交回。
  - **`App/touch/cdc_manager.h/c`**：`cdc_mode_t` 扩成 4 态
    （`AUTO_RAW` / `AUTO_MAI2TOUCH` / `FORCED_RAW` / `FORCED_MAI2TOUCH`），默认 `AUTO_RAW`
    （没接游戏时上位机链路直接可用）。新增 `cdc_manager_is_auto_mode()`、
    `cdc_manager_set_auto_detected()`、`cdc_manager_set_auto_mode()`。
    `cdc_manager_set_mai2touch_active()` 改为**值没变就不动**——`touch_app_apply_config()`
    每次配置变更都会按"当前模式"调用它，若无条件变成强制模式，自动识别会被永久关掉。
  - **`App/touch/mai2touch.c/h`**：
    - 新增 `mai2touch_send_bits()` / `mai2touch_pack_and_send()`：`touch_pipeline_is_ready()`
      为假时发 **`bits = 0` 的空白帧**（合法的"无人触摸"帧），4 ms 一帧。
    - `{STAT}`：`sending = true` 且 `next_send_tick = HAL_GetTick()` —— **当拍就发第一帧**。
    - `{RSET}`：新增 `mai2touch_cmd_rset()` 无条件复位 `touch_bits/sending/rx_len`（§8.7）。
    - `{HALT}`：只停发、不回包（§8.2）。
    - 单字节解析抽出为 `mai2touch_handle_byte()`，新增
      `mai2touch_replay_command()` 供切换时补喂；`mai2touch_process_rx()` 改成非 static
      并在**读之前**检查 `pending_to_raw`/非活跃，便于本机单测直接驱动。
  - **`App/touch/tenodata.c`**：`consume_host_config_frame()` 每个字节喂识别器；
    `tenodata_task()` 里 raw 侧消费已由 `!cdc_manager_is_mai2touch_active()` 守卫
    （Mai2Touch 模式独占 CDC RX）。
  - **`App/touch/touch_app.c/h`**：`touch_app_task()` 先跑 `touch_mode_task()`；
    新增 `touch_app_is_auto_mode()` / `touch_app_set_auto_mode()`。
  - **`App/config/touch_config.c`**：`cdc_mode` 新增 **`2` = 自动识别**（默认值，读回也是 `2`；
    当前实际在跑哪一侧由状态参数 `0x04` 显示）。写 `0`/`1` 会无条件把强制标记立起来
    （即使输出侧本来就在那一侧），走的是新增的 `touch_app_force_cdc_mode()`；
    写 `2` 恢复 AUTO。`touch defaults` 改为走 AUTO，不再把模式钉死在 Mai2Touch。
    存 Flash 的载荷也记 AUTO，避免"上次存配置时游戏正好在跑 → 下次上电被钉死在 Mai2Touch"。
  - **`tools/magic_config_tool.py`**：`touch mode` 支持 `auto`（`2`），帮助文本更新。
  - **新增本机单测 `tests/native_mode_test.c` + `tests/build_native_test.ps1`**：用 MinGW gcc
    直接编译**真实**的 `cdc_manager.c`/`touch_mode.c`/`mai2touch.c`（只 stub TinyUSB/HAL/
    touch_pipeline/tenodata 少数函数），复刻真实 tick 顺序，11 个场景全绿。
- **影响**:
  - 插上游戏就能跑，不用再切模式；拔掉游戏、上位机一发参数帧就自动切回 raw。
  - 切到 Mai2Touch **不动 PSoC**（游戏正在配置阶段，插一次 1~2 秒的软复位+校准会打断握手）；
    切回 raw 会重走初始化（约 1~2 秒触摸输出中断），并清除跨模式的参数覆盖。
  - 用 Magic 强制 `0`/`1` 后仍可固定模式（调试用），写 `2` 或 `touch defaults` 恢复自动识别；
    默认与读回都是 `2`（自动识别）。
- **验证**:
  - 本机单测 11 场景全部通过（含"校验和 = `0x7B` 不误切"、"`{STAT}`/`{LAr2}` 触发切换后
    补喂成功"、"`{STAT}` 立即发空白帧"、"`{HALT}` 后 20 个发送周期零字节"）。
  - Release 构建通过：`text 67960 / data 184 / bss 15808`（FLASH 上限 122880，余量充足）。
- **注意**: 固件需要重新烧录（本次未接 ST-Link，需用 CubeIDE 烧）。

### 节点 15: Mai2Touch 内嵌 TouchHost 等价前处理与判定

> ⚠️ **本节点描述的本地判定器已在节点 18 被共享判定核心取代。**
> 以下内容保留为历史记录，其中的 `button_detector.*` 与阈值真源说明均已作废；
> 现行方案见节点 18 与根目录 `docs/TENO-TOUCH-CORE-DISTRIBUTION.md`。

- **日期**: 2026-09-26
- **背景**: 原本只在 TouchHost 里跑 `SerialReader + SignalPipeline + detector.py`，
  STM32 的 `touch_pipeline` 只是近似实现。目标明确为：**TouchHost 只用于调试，
  最终算法搬到 STM32**。因此 Mai2Touch 模式必须在设备内完成去重、IIR、预热、
  校准和判定，只向游戏发送 9B 结果。
- **确认的差异**:
  - raw 模式下 STM32 原本就只发 70B 快照；去重发生在 TouchHost，STM32 不去重。
  - `touch_pipeline` 原来按“单个 I2C 片完成”推进，导致双片交替时预热/校准计数
    比 TouchHost 的 70B packet 语义快一倍。
  - `touch_pipeline` 原来按“通道 2 字节”判变化，TouchHost 是按“整片 34 字节”
    判变化；二者在单通道变化时行为不同。
  - `button_detector.c` 的 A 区冻结阈值、回升路径、`decline_frames`、简单区
    `prev_on` 死区记忆和阈值常量均与 `detector.py` 漂移。
- **变更**:
  - `App/touch/touch_pipeline.h/c`：接口改为
    `touch_pipeline_stage_snapshot()` + `touch_pipeline_commit_snapshot()`；
    I2C 完成只更新 staged 快照，`STATE_RUNNING` 每 2 ms CDC 包提交一次。
    片级 34B `memcmp` 去重，100 个有效包预热、100 个有效包校准，随后按片更新
    IIR 与判定。IIR 与基线累加改为 `double`。
  - `App/touch/tenodata.c`：每片 I2C 完成后只 `stage_snapshot`；2 ms CDC 发送点
    先 `commit_snapshot`，再发送 9B/raw 帧。
  - `App/touch/button_detector.h/c`：状态结构按 Python 的 A 区与简单区拆开，
    逐项移植 `detector.py` 的 A 区状态机、有效信号导数、限幅和平滑、
    `released_min_diff` 回升、`large_diff_latched`、简单区 `prev_on` 死区记忆。
  - `App/touch/button_detector_config.h`：阈值全部改为 `detector.py` 当前值。
  - `tests/pipeline_parity_test.c` + `build_pipeline_parity_test.ps1`：新增主机侧
    原生回归，直接编译真实管线与判定器，验证片级去重、100+100 门槛、
    判定分支和真实 A5 idle 样本。
  - `tests/detector_parity_driver.c` + `generate_detector_parity.py` +
    `build_detector_parity.ps1`：直接导入 TouchHost 的真实 `detector.py`，
    用固定场景和 2000 帧随机序列对比固件判定器的 `pressed`、状态机、
    基线、有效位移、一阶/二阶导数。
- **工作流**:
  ```text
  raw 模式:
    PSoC -> STM32 2ms 快照 -> CDC0 70B -> TouchHost SerialReader
         -> SignalPipeline -> detector.py -> COM17 -> 游戏

  Mai2Touch 模式:
    PSoC -> STM32 stage/commit -> 片级去重 -> IIR -> 100+100 校准
         -> detector.py 等价判定 -> 9B 区域掩码 -> 游戏
  ```
- **验证**:
  - `tests/build_native_test.ps1`：11 个模式切换场景通过。
  - `tests/build_pipeline_parity_test.ps1`：片级去重、100+100 门槛、A 区延迟触发/
    半幅释放、简单区死区、真实 A5 idle 样本全部通过。
  - `tests/build_detector_parity.ps1`：TouchHost 与固件逐帧对比通过
    （固定场景、简单区突发、C 区边界和 2000 帧随机序列）。
  - 当前仓库日志只有 TouchHost 判定后的 CSV，没有保留原始 70B CDC 流；
    因此本次真实数据验证使用 CSV 中记录的 Raw/SetupRaw 样本做判定回归，
    raw 70B 全流回放仍需要后续抓一份原始 CDC 日志。
- **阈值真源**: `detector.py` 顶部写死的 `_A_CONFIG` / `_B_CONFIG` /
  `_C_CONFIG` / `_D_CONFIG` / `_E_CONFIG`。`config.yaml: zone_configs`
  当前只被 `config.py` 加载，`SignalPipeline` 不消费它；固件
  `button_detector_config.h` 已按 `detector.py` 对齐。若以后要改成
  YAML 可调，需要先改 TouchHost 的判定器接口，再同步设备端。

### 节点 16: 三端固定回放一致性验证

- **日期**: 2026-09-26
- **版本记录**:
  ```text
  per_version: per-1.0
  aligned_alpha_version: alpha-1.0
  aligned_beta_version: beta-1.0
  dataset_id: 202609-source-csv
  first_fork: none
  real_device_validation: pending
  ```
- **背景**: beta 同步完成后，需要确认 STM32 的完整管线与 TouchHost、
  melonloader 对同一固定数据给出相同判定，而不只是判定器函数等价。
- **修正**:
  - `touch_pipeline` 的提交接口改为 `touch_pipeline_commit_packet()` /
    `touch_pipeline_commit_batch()`，packet 携带每片有效位与两个 34B
    payload；`tenodata` 在每个 CDC 周期把期间完成的片级快照合并成 packet
    批次提交。
  - 批次内先按片做 34B 去重，再从最新帧向最旧帧分配两片独立的 180 Hz
    额度，最后按原始顺序把获准 packet 送入 IIR、预热/校准和判定。
  - 校准完成帧只冻结 `setup_raw`、不执行判定，首判定帧从下一个有效 packet
    开始，与 `SignalPipeline.process_packet()` 返回 `None` 的边界一致。
  - 新增 `tests/pipeline_replay_driver.c`、`tests/compare_three_way.py`、
    `tests/build_three_way_parity.ps1`，直接把同一 `frames.csv` 送入真实
    `touch_pipeline`，并与 TouchHost/melonloader 输出逐行比较。
- **验证**:
  - `powershell -ExecutionPolicy Bypass -File .\tests\build_three_way_parity.ps1`
  - 结果：固定数据集 `202609-source-csv` 的 23/23 文件、122,298 行通道结果
    三端一致；比较列为 `pressed/fsm/baseline/effective_diff/deriv/deriv2`，
    首个分叉帧为 `none`。
  - `tests/build_pipeline_parity_test.ps1`：包含批次最新优先的新断言通过。
  - `tests/build_detector_parity.ps1`：固定、简单区、C 区边界与 2000 帧
    随机序列全部通过。
  - `build.ps1 -BuildType Release`：Release 链接通过
    （`text 71984 / data 184 / bss 20896`）。
- **限制**: 固定 CSV 是判定后数据，不含原始 70B CDC；IIR 原始字节入口仍需
  `raw-cdc-70b` 数据集补验。实机触摸验证仍为 pending。

### 节点 17: raw 70B 原始流回放入口补齐（未执行真实数据验收）

- **日期**: 2026-10-02
- **版本记录**:
  ```text
  per_version: per-1.0
  aligned_alpha_version: alpha-1.0
  aligned_beta_version: beta-1.0
  dataset_id: none (raw-cdc-70b 未采集)
  frames_sha256: none
  first_fork: not_applicable
  real_device_validation: pending
  ```
- **变更**:
  - 新增 `tests/raw_cdc_replay_driver.c`，从连续 70B 原始流入口调用真实
    `touch_pipeline_commit_packet()`，保留片级去重、限速、IIR、100 帧预热、
    100 帧校准、每通道独立判定与 A-E 状态机。
  - 新增 `tests/run_raw_cdc_parity.ps1`，串行执行 TouchHost、melonloader 和
    STM32 三端 raw 回放，再用 `tests/compare_three_way.py` 比较
    `decision/baseline/eff/deriv_eff/deriv2_eff/fsm/large_latch`。
  - 新增 `tests/build_raw_cdc_replay_test.ps1`，仅用 synthetic fixture 验证
    驱动可编译可运行，不作为 raw 验收证据。
- **验证**:
  - raw 驱动 host 编译通过：`gcc -std=gnu11 ... -DTOUCH_PIPELINE_TEST_PROBES=1`
  - synthetic fixture 驱动冒烟通过：三端均输出，驱动接线可用。
  - 真实 `raw-cdc-70b` 数据集未采集，因此首个分叉帧、逐帧数值一致性、
    STM32 实机触摸均保持 `pending`。
- **限制**: 未修改 `touch_pipeline.c` / `button_detector.c` 等算法文件。

### 节点 18: 判定收敛到共享核心（旧判定实现删除）

- **日期**: 2026-10-03
- **背景**: 判定此前在固件、游戏内 Mod 与上位机各有一份手写实现，
  已经发生真实语义漂移。改为"一份判定、三端共用"。
- **变更**:
  - 删除 `App/touch/button_detector.c/.h` 与 `button_detector_config.c/.h`。
  - 新增 `App/touch/touch_core_port.c/.h`：便携适配层，把固件的通道映射
    与逐帧数据接进共享判定核心，并导出核心版本信息。
  - `App/touch/touch_pipeline.c`：判定调用改走共享核心适配层；
    管线自身的去重、限速、IIR、预热、校准逻辑保持不变。
  - `CMakeLists.txt`：编入根目录 `touch_core/` 的源码并加入头文件路径；
    找不到核心时构建直接失败，避免退回本地副本。
  - `tests/`：宿主管线回归与 raw 回放驱动的编译清单改指向共享核心。
- **验证**:
  - `tests/build_pipeline_parity_test.ps1` 全部通过。
  - `tests/build_raw_cdc_replay_test.ps1` 冒烟通过。
  - 三端一致性比对在多个数据集上逐帧一致（详见根目录
    `docs/TENO-TOUCH-CORE-DISTRIBUTION.md`）。
- **限制**: 实机触摸验证仍为 pending；真实 `raw-cdc-70b` 数据集仍未采集。

### 节点 19（2026-11-25） IO 冷启动、触摸上报与主键恢复

**背景与复现**　原版从冷启动进入 IO 后等待上位机参数。先用 raw 配置两片 PSoC 再切入 IO 时，两片已校准且持续采样，固件触摸管线仍停在预热，对 E4 加入按下信号后 CDC0 触摸位仍为零。开机按住的主键也会在本次上电期间一直屏蔽。

**变更**

- IO 启动时优先沿用有效的 raw 参数覆盖。没有覆盖时，按当前映射的本地扫描参数配置并校准两片 PSoC。raw 继续用 `0x01` 请求缺少的参数。
- IO 与 Mai2Touch 都提交校准后的 I²C 样本。预热和基线采集完成后，IO 将共享核心的 34 位结果放入 `A5` 状态帧。Mai2Touch 保持原来的 5 bit 分组。
- raw 与 Mai2Touch 的接收端均识别完整 IO 命令，关闭 IO 后把后续字节交回原自动或强制模式。处于接收中的 raw 参数帧独占载荷和校验和。
- 开机为高的主键先屏蔽，连续三个 5 ms 采样点为低后启用。正常游戏输入直接读取主键，副键保持消抖。IO 上报 BTN0..10，BTN11 走 HID。

**验证**　宿主回归模拟两片 PSoC 的 I²C 寄存器、GPIO 和 USB 端点，运行固件的初始化、采样、判定和发包代码。检查 E4/E8 分别按下与释放、两种 9 B 帧、参数覆盖与协议切换，以及 HID 实际键码。入口为 `tests/build_native_io_test.ps1` 和 `tests/build_native_button_test.ps1`。CubeIDE ARM Release 构建通过。实物板验证待进行。

---

## 当前文件结构
```
App/touch/
├── touch_app.h/c                 ← Touch 统一初始化、任务和配置入口
├── touch_mode.h/c                ← CDC0 模式自动识别（raw ↔ Mai2Touch）
├── tenodata_config.h/c           ← 统一通道映射与固定 A–E 扫描档位
├── psoc_comm.h/c                 ← PSoC I2C 通信
├── tenodata.h/c                  ← 初始化/重配置状态机 + raw CDC 推流
├── touch_pipeline.h/c            ← 软件基线、逐通道调用共享判定核心、区域 OR 合并
├── touch_core_port.h/c           ← 固件侧适配层（映射 + 逐帧数据 → 共享判定核心）
├── mai2touch.h/c                 ← Mai2Touch 协议
├── io_mode.h/c                   ← IO 模式命令、按键和触摸 A5 帧
└── cdc_manager.h/c               ← CDC0 收发管理 + 路由开关（AUTO / 强制）
tests/
├── native_mode_test.c            ← 本机单测（编译真实模块，不需板子）
├── native_io_touch_test.c        ← 从双 PSoC 寄存器和 GPIO 验证 CDC0 与 HID 报文
├── pipeline_parity_test.c        ← Mai2Touch/TouchHost 管线等价回归
├── native_stubs/                 ← TinyUSB / HAL stub 头
├── build_native_test.ps1         ← 用 MinGW gcc 构建并运行模式识别测试
└── build_pipeline_parity_test.ps1 ← 构建并运行管线等价测试
```

> 判定实现不在本仓库：它属于根目录 `touch_core/`，由本固件编入源码使用。

## CDC 路由规则
| 当前模式 | CDC0 发送端 | CDC0 接收 |
| --- | --- | --- |
| `AUTO_RAW` / `FORCED_RAW` | tenodata 的 70 B 帧 | tenodata 接收参数，帧间可识别 IO。自动模式也识别完整的 Mai2Touch 命令 |
| `AUTO_MAI2TOUCH` / `FORCED_MAI2TOUCH` | mai2touch 的回显与位图帧 | mai2touch 接收游戏命令或 IO 命令，自动模式也识别 raw 帧头 |
| `CDC_MODE_IO` | io_mode 的 9 B 状态帧 | tenodata 接收 IO 控制命令，关闭后恢复进入前的模式 |

自动识别只在 `AUTO_*` 下生效；`FORCED_*` 由 Magic `touch` 参数 `0x02` 写入 `0`/`1` 设定，
写 `2` 恢复 AUTO。

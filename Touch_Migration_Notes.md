# Touch 功能架构迁移记录

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

---

## 当前文件结构
```
App/touch/
├── touch_app.h/c                 ← Touch 统一初始化、任务和配置入口
├── tenodata_config.h/c           ← 统一通道映射与固定 A–E 扫描档位
├── psoc_comm.h/c                 ← PSoC I2C 通信
├── tenodata.h/c                  ← 初始化/重配置状态机 + raw CDC 推流
├── touch_pipeline.h/c            ← 软件基线、Detector 和区域 OR 合并
├── button_detector*.h/c          ← A–E 触摸判定逻辑
├── mai2touch.h/c                 ← Mai2Touch 协议
└── cdc_manager.h/c               ← CDC0 收发管理 + 路由开关
```

## CDC 路由规则
| `is_mai2touch_active` | tenodata 发送 | mai2touch 发送 | CDC RX 分发 |
|:---:|:---:|:---:|:---:|
| `false` | 正常发送 | 丢弃 | tenodata |
| `true` | 丢弃 | 正常发送 | mai2touch |

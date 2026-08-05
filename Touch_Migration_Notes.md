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

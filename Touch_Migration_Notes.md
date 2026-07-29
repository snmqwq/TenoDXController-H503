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

---

## 当前文件结构
```
Core/Src/touch/
├── tenodata_config.h/c   ← 硬件扫描参数
├── psoc_comm.h/c         ← PSoC I2C 通信
├── tenodata.h/c          ← tenodata 状态机 + CDC 推流
├── mai2touch.h/c         ← Mai2Touch ASCII 协议 (测试版)
├── cdc_manager.h/c       ← CDC 收发管理 + 路由开关
```

## CDC 路由规则
| `is_mai2touch_active` | tenodata 发送 | mai2touch 发送 | CDC RX 分发 |
|:---:|:---:|:---:|:---:|
| `false` | 正常发送 | 丢弃 | tenodata |
| `true` | 丢弃 | 正常发送 | mai2touch |

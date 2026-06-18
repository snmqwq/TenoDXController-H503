# TenoDXController-H503

基于 STM32H503CBT6 的 TenoDX / maimai 控制器固件工程，使用 STM32CubeIDE、TinyUSB 和 WS28XX 灯带驱动。

## 当前功能

- TinyUSB 复合设备：3 个 CDC 接口和 1 个 HID 键盘接口。
- CDC0：触摸接口。
- CDC1：Mai2LED 灯光协议和 magic 配置协议。
- CDC2：读卡器接口。
- HID：11KRO 键盘，共 BTN0..BTN10 十一个按键。
- BTN0..BTN7 固定映射为 `w e d c x z a q`。
- BTN8..BTN10 可配置并保存到 Flash，默认映射为 `3`、`keypad_multiply`、`9`。
- 八个逻辑按钮灯，`LED_PER_BIT` 默认值为 2，可通过 magic 协议配置。
- 空闲白灯或流动彩虹灯效，支持按键高亮。
- 外部 Mai2LED 指令接管灯光后停止空闲灯效；长按 BTN8 五秒恢复。

当前 `1.4-aime-touch_test` 分支已接入 PN532/Aime 读卡器状态机。触摸部分当前通过 I2C1
中断读取两个 PSoC，并在 CDC0 输出固定 70 字节 raw 测试帧；末字节为前 69 字节的
8 位累加校验和。这不是最终触摸算法输出。
电脑迁移、分支基线和当前测试格式见 `MIGRATION.md`。

## 工程结构

| 路径 | 内容 |
| --- | --- |
| `Core/` | 应用代码、外设初始化和启动文件 |
| `Drivers/` | STM32 HAL 与 CMSIS |
| `tinyusb/` | TinyUSB 协议栈 |
| `Middlewares/` | WS28XX 驱动 |
| `tools/magic_config_tool.py` | 交互式串口配置工具 |
| `ref/` | Mai2LED、Mai2Touch、触摸算法和 Aime 参考资料 |
| `MIGRATION.md` | 电脑迁移、环境导入和当前功能基线 |
| `maimai_controller_H503.ioc` | STM32CubeMX 工程配置 |

## 构建

推荐使用 STM32CubeIDE 1.19 或兼容版本：

1. 在 STM32CubeIDE 中选择 **File > Import > Existing Projects into Workspace**。
2. 选择本仓库目录并导入 `maimai_controller_H503`。
3. 选择 `Debug` 或 `Release` 配置后构建工程。

目标芯片为 STM32H503CBT6。链接脚本为 `STM32H503CBTX_FLASH.ld`，程序区使用前 120 KiB Flash，最后 8 KiB 保留给配置存储。

## Magic 配置工具

安装依赖：

```powershell
python -m pip install -r tools/requirements.txt
```

启动交互式工具：

```powershell
python tools/magic_config_tool.py
```

工具支持串口选择、灯光配置、BTN8..BTN10 键值配置、原始 magic 请求和进入系统 DFU。

## Magic 协议

Magic 序列：

```text
91 3E ED 20 7C 99 58 AC
```

请求：

```text
magic_seq + [module, cmd, param, len, payload..., sum]
```

响应：

```text
[AC, status, module, cmd, param, len, payload..., sum]
```

模块编号：

- `0x00`：global
- `0x10`：touch
- `0x20`：light
- `0x30`：reader
- `0x40`：keyboard

## Flash 配置

内部 Flash 最后 8 KiB 被划分为四个独立的 2 KiB 配置槽：

- touch
- light
- reader
- keyboard

Magic `WRITE` 只修改 RAM 中的当前配置，只有 `SAVE` 或 `SAVE_ALL` 才会写入 Flash。

## 第三方组件

STM32 HAL、CMSIS、TinyUSB 和 WS28XX 驱动遵循各自目录中附带的许可证。

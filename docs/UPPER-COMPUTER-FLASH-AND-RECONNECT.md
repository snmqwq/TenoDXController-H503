# TenoDX 上位机刷写与 USB 重连实现文档

本文面向编写 Windows 上位机软件的开发者，说明如何在不手动插拔 USB 的前提下完成：

1. 让设备进入 DFU、下载固件、下载结束后自动退出 DFU 并运行新固件。
2. 在设备 USB 配置变化后，由软件触发重新枚举，并自动等待新端口出现。

本文描述的固件接口以当前仓库为基础：Magic 协议运行在 CDC3 `TenoDX Debug Port`，设备 VID/PID 目前由固件描述符决定，USB 重连使用 H503 内部 D+ 上拉控制。

## 1. 结论与职责划分

### 1.1 “刷完自动重启”应由上位机结束 DFU 会话

设备执行 `dfu enter` 后，应用固件不再运行，控制权交给芯片内置 ROM DFU。此时：

- 应用固件无法再接收 Magic 命令。
- “下载完成后是否复位、是否运行应用”由主机侧的 DFU 操作决定。

因此正确流程是上位机完成下载后，显式发送 DFU 的退出/复位/启动请求。使用 `STM32_Programmer_CLI` 时，把下载、校验和复位放在同一条命令中：

```powershell
STM32_Programmer_CLI.exe `
  -c port=USB1 `
  -w firmware.hex `
  -v `
  -rst
```

其中：

- `-c port=USB1`：连接 USB DFU，而不是 ST-Link/SWD。
- `-w firmware.hex`：下载 HEX；如果使用 BIN，必须追加起始地址，例如 `-w firmware.bin 0x08000000`。
- `-v`：下载后校验。
- `-rst`：结束 DFU 并复位/启动应用。

如果上位机不使用 CLI，而是自己实现 DFU 协议，则必须实现标准的 `DFU_DNLOAD` 下载完成后，发送 `DFU_CLRSTATUS` 或等价退出流程，使设备离开 DFU 并启动应用。仅仅断开 USB 或关闭 DFU 句柄不保证新固件开始运行。

### 1.2 “不插拔就能重新枚举”由固件命令触发

固件新增 Magic 全局命令：

```text
module = 0x00
cmd    = 0x85
param  = 0x00
payload = [0xA5]
```

设备收到后会：

1. 先返回正常 Magic `ACK`；
2. 等待约 150 ms，让 ACK 写完；
3. 临时关闭 USB D+ 上拉，主机看到断开；
4. 保持约 250 ms；
5. 重新打开 D+ 上拉，主机重新枚举 CDC/HID 接口。

因此上位机不需要增加 VBUS 或 D+ 控制硬件，也不需要人工拔插。H503 的 USB 外设提供内部 D+ 上拉，固件使用 TinyUSB 的 `tud_disconnect()` / `tud_connect()` 完成该动作。

## 2. USB 重连 Magic 命令

### 2.1 请求帧

Magic 固定前导序列：

```text
91 3E ED 20 7C 99 58 AC
```

USB 重连请求：

```text
request = magic_seq + [module, cmd, param, len, payload..., sum]
        = 91 3E ED 20 7C 99 58 AC
          00 85 00 01 A5 sum
```

字段说明：

| 字段 | 值 | 说明 |
| --- | ---: | --- |
| `module` | `0x00` | global |
| `cmd` | `0x85` | USB reconnect |
| `param` | `0x00` | 保留/固定 |
| `len` | `0x01` | 1 字节 payload |
| `payload` | `0xA5` | 二次确认值 |
| `sum` | 计算值 | 前面所有字节累加取低 8 位 |

按当前定义，完整请求通常为：

```text
91 3E ED 20 7C 99 58 AC 00 85 00 01 A5 20
```

### 2.2 响应帧

收到合法请求且当前没有正在进行的重连操作时，设备返回：

```text
AC 00 00 85 00 00 31
```

字段说明：

| 字段 | 值 | 说明 |
| --- | ---: | --- |
| `AC` | 同步字节 | Magic response sync |
| `00` | 状态 | 成功 |
| `00` | module | global |
| `85` | cmd | usb reconnect |
| `00` | param | 固定 |
| `00` | len | 无 payload |
| `sum` | 计算值 | 响应前面所有字节累加取低 8 位 |

### 2.3 失败情况

| 条件 | 返回 |
| --- | --- |
| payload 缺失、长度不是 1、确认值不是 `0xA5` | `PARAM_ERROR` (`0x04`) |
| 已有重连操作未完成，又收到新请求 | `IO_ERROR` (`0x06`) |
| cmd/module 不支持 | `CMD_ERROR` / `MODULE_ERROR` |

上位机不应把 `USB_RECONNECT` 当作周期性命令反复发送。一次配置变更最多触发一次，并等待设备重新出现后再继续操作。

## 3. 上位机推荐的 USB 重连状态机

推荐使用以下状态：

```text
IDLE
  -> SEND_RECONNECT
  -> WAIT_ACK
  -> CLOSE_SERIAL
  -> WAIT_PORT_DISAPPEAR
  -> WAIT_PORT_APPEAR
  -> VERIFY_MAGIC_PORT
  -> READY
```

### 3.1 连接设备并确认是 Debug Port

不要只按 COM 号判断。Windows 会为同一设备在不同 USB 拓扑下分配不同 COM 号。推荐通过以下组合识别：

1. USB 厂商/产品字符串或设备实例路径；
2. 打开串口后发送一次 `light info` 或 `touch info`，确认返回合法 Magic 响应；
3. 记录该设备的 USB 序列号/实例路径，重连后优先匹配同一物理设备。

如果只按 `COMx`，Windows 重新分配端口后上位机会连错旧端口。

### 3.2 发送重连命令

1. 发送 `module=0x00, cmd=0x85, param=0x00, payload=[0xA5]`。
2. 等待正常响应 `AC 00 00 85 00 00 xx`。
3. 收到响应后立即关闭当前串口句柄。

如果未收到响应：

- 检查 Magic 前缀和校验和；
- 检查是否连接到了错误的 CDC 接口；
- 检查设备是否已经处于退出/重连状态；
- 不要盲目重试，最多重试一次并记录日志。

### 3.3 等待端口消失与重现

设备 ACK 后约 150 ms 才会断开，约 250 ms 后重新连接。Windows 枚举需要额外时间，实际重现可能在 0.5 秒到数秒之间。

推荐参数：

| 参数 | 建议值 |
| --- | ---: |
| 端口轮询间隔 | 50 ms |
| 等待端口消失超时 | 2 s |
| 等待端口重现超时 | 10 s |
| 重新打开后 Magic 验证超时 | 1 s |

伪代码：

```text
send_reconnect()
wait_ack(timeout=1s)
close_serial()

wait_until(port_not_present, timeout=2s)
port = wait_until(port_present_and_matches_device, timeout=10s)

open(port)
verify_touch_or_light_info(timeout=1s)
ready()
```

如果端口在断开前就被重新打开，Windows 可能仍指向旧句柄，导致后续请求一直超时。因此必须先观察断开，再重新打开，不要只做固定延时。

### 3.4 改配置后自动重连的推荐调用顺序

如果上位机修改了会影响 USB 描述符或端口行为的配置：

1. 发送配置写入命令；
2. 如需持久化，发送保存命令；
3. 等配置命令 ACK；
4. 发送 `USB_RECONNECT`；
5. 等 `USB_RECONNECT` ACK 后关闭串口；
6. 按 3.3 的流程等待并重新连接。

如果只改了触摸映射、键值等不影响 USB 描述的配置，不必强制重连；重连只用于需要 Windows 重新识别设备接口的场景。

## 4. DFU 自动刷写与自动启动

> **实测说明（2026-09-30，STM32H503CBE / Windows）**
>
> 当前仓库的 `dfu enter` 命令会返回正常 Magic ACK，但在本机实测中，
> 设备应用固件跳转后 Windows **没有稳定枚举出 `STM32_Programmer_CLI -l usb`
> 可识别的 DFU 设备**。因此本章描述的是上位机应采用的正确刷写模型，
> 以及当前固件 DFU 跳转链路仍需修正/用 ST-Link 兜底的现实情况。
>
> 在 DFU 跳转修正完成前，建议上位机提供两条刷写路径：
>
> 1. **ST-Link/SWD 路径（当前最可靠）**：直接调用
>    `STM32_Programmer_CLI -c port=SWD mode=UR -w firmware.hex -v -rst`。
> 2. **DFU 路径（待固件修正）**：只在确认 DFU 设备已出现后调用
>    `-c port=USB1 -w ... -v -rst`。

### 4.1 进入 DFU

先通过普通 Debug Port 发送现有 DFU 命令：

```text
module = 0x00
cmd    = 0x84
param  = 0x00
payload = [0xA5]
```

设备回应成功后，目标行为是应用固件：

1. 立即返回 ACK；
2. 约 100 ms 后关闭应用 USB；
3. 跳转到 H503 系统 Bootloader 的 USB DFU。

上位机应关闭当前 Debug Port 句柄，然后枚举 DFU 设备。DFU 模式下的 VID/PID 不一定与正常应用相同，不要复用应用端的 PID 判断。

**当前状态**：实测 ACK 能收到，但 DFU 设备未稳定出现，因此上位机在实现自动
DFU 前必须先完成一次“从应用跳转后能在设备管理器中看到 DFU/STM32 BOOTLOADER”
的固件侧验证；否则应自动回退到 ST-Link/SWD 刷写。

### 4.2 下载并自动启动

确认 DFU 设备已经出现后，使用 `STM32_Programmer_CLI` 的推荐命令：

```powershell
STM32_Programmer_CLI.exe -c port=USB1 -w firmware.hex -v -rst
```

若使用 BIN：

```powershell
STM32_Programmer_CLI.exe -c port=USB1 -w firmware.bin 0x08000000 -v -rst
```

关键点：

- 不要在下载后只关闭程序就结束；`-rst` 或等价 DFU 退出动作负责让新固件运行。
- 不要把 `-rst` 放在下载之前。
- 下载完成并在 `-rst` 发出后，等待正常应用端口重新出现。

如果 `-c port=USB1` 连接失败，不要盲目重试几十次；应改用 ST-Link/SWD：

```powershell
STM32_Programmer_CLI.exe -c port=SWD mode=UR -w firmware.hex -v -rst
```

这条路径当前已在 H503 实机验证通过：刷写后自动复位，应用端口重新枚举。

### 4.3 多设备环境

如果同时连接多个 STM32 DFU 设备，必须使用序列号或明确 USB 路径区分目标：

```powershell
STM32_Programmer_CLI.exe -c "port=USB1 sn=<目标序列号>" -w firmware.hex -v -rst
```

不要只写 `port=USB1`，否则可能刷错设备。

### 4.4 下载后验证

最可靠的验证不是只看 CLI 退出码，而是：

1. 等待正常应用的 CDC/HID 重新枚举；
2. 打开 Debug Port；
3. 发送 `light info` 或 `touch info`；
4. 校验返回的 Magic 版本/能力字段符合新固件。

如果无法重新打开 Debug Port：

- 检查是否遗漏 `-rst`；
- 检查设备是否仍停留在 DFU；
- 检查是否被 Windows 分配到了新的 COM 号；
- 检查新固件是否启动失败并保持错误状态。

## 5. 固件实现位置

为了让上位机开发时可对照实现，涉及的文件如下：

| 文件 | 职责 |
| --- | --- |
| `App/config/usb_control.c/.h` | USB 软件重连状态机，调用 `tud_disconnect()` / `tud_connect()` |
| `App/config/magic_config.c/.h` | 增加全局 Magic 命令 `0x85` |
| `App/config/config_app.c` | 在主循环中运行 USB 重连任务 |
| `Core/Src/usb_descriptors.c` | CDC/HID 接口与 VID/PID 描述 |
| `tools/magic_config_tool.py` | 配置工具中的 `usb reconnect` 命令示例 |

固件不负责在 DFU 下载完成后自行重启，因为应用固件在 DFU 期间没有运行权；该步骤必须由上位机的 DFU 客户端完成。

## 6. 上位机伪代码

### 6.1 自动重连

```python
def reconnect_device(client, device_identity):
    response = client.request(
        module=0x00,
        command=0x85,
        param=0x00,
        payload=b"\xA5",
    )
    if not response.ok:
        raise RuntimeError(response.status_text)

    client.close()

    wait_until(
        lambda: not find_port(device_identity),
        timeout=2.0,
        interval=0.05,
    )

    port = wait_until(
        lambda: find_port(device_identity),
        timeout=10.0,
        interval=0.05,
    )

    client = open_magic_port(port, timeout=1.0)
    verify_magic_device(client)
    return client
```

### 6.2 自动刷写

```python
def flash_firmware(client, firmware_path, device_serial=None):
    response = client.request(
        module=0x00,
        command=0x84,
        param=0x00,
        payload=b"\xA5",
    )
    if not response.ok:
        raise RuntimeError(response.status_text)

    client.close()
    dfu_device = wait_for_dfu_device(timeout=10.0)

    connection = "port=USB1"
    if device_serial:
        connection += f" sn={device_serial}"
    command = [
        "STM32_Programmer_CLI.exe",
        "-c",
        connection,
    ]
    command += ["-w", str(firmware_path), "-v", "-rst"]

    run_checked(command)

    # 新应用启动后重新连接 Debug Port。
    port = wait_for_tenodx_debug_port(timeout=15.0)
    client = open_magic_port(port, timeout=1.0)
    verify_magic_device(client)
    return client
```

## 7. 关键注意事项

1. **不要用固定 `COM` 号代表设备身份。** 重枚举后 Windows 可能分配新的 COM 号。
2. **重连后必须重新做 Magic 校验。** 仅“端口存在”不代表它是 TenoDX Debug Port。
3. **DFU 的复位动作必须在主机侧完成。** 应用固件在 DFU 模式下不运行。
4. **下载结束后不要立即假设设备已回来。** 给 Windows 枚举和新固件启动留时间，建议总超时 10～15 秒。
5. **多设备场景必须按序列号或 USB 实例路径选目标。**
6. **普通配置写入不一定要重连。** 只有需要 Windows 重新识别接口时才发送 `USB_RECONNECT`。

## 8. 测试建议

建议上位机至少覆盖以下场景：

| 场景 | 期望 |
| --- | --- |
| 发送合法 `USB_RECONNECT` | 收到 OK；设备断开后重新枚举；Magic 验证成功 |
| 发送错误确认值 | 返回 `PARAM_ERROR`；设备不重连 |
| 重连期间再发一条 | 返回 `IO_ERROR` 或保持原连接 |
| 重连后 COM 号变化 | 上位机能按设备身份重新找到端口 |
| 正常模式下 `dfu enter` | 设备进入 DFU；应用端口消失 |
| DFU 下载并执行 `-rst` | 新固件自动运行，Debug Port 重新出现 |
| 多设备 DFU | 只刷指定序列号的设备，其他设备不受影响 |

## 9. 相关 Magic 命令摘要

| 命令 | module | cmd | payload | 用途 |
| --- | ---: | ---: | --- | --- |
| DFU enter | `0x00` | `0x84` | `A5` | 跳转 ROM DFU |
| USB reconnect | `0x00` | `0x85` | `A5` | 不断电重枚举 USB |

上位机应当把这两条命令实现为显式的、可等待、可超时、可记录日志的操作，而不是隐藏在一次普通配置请求里。

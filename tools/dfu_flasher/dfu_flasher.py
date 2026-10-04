#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TenoDX DFU 自动刷写工具.

流程:
    1. 选择固件;
    2. 选择目标控制器 (TenoDX Debug Port / CDC3 对应的 COM 口);
    3. 通过 Magic 0x84 命令让固件跳转到 STM32 ROM DFU;
    4. 按提示拔插一次 USB, 让 Windows 重新枚举 DFU 设备;
    5. 用 STM32_Programmer_CLI 连接 DFU, 写入固件, 校验并复位;
    6. 等待 TenoDX Debug Port 重新出现并做一次 Magic 校验, 给出成功/失败提示.

设计约束:
    * 正常刷写不依赖 ST-Link, 全程走 USB DFU;
    * 多设备时必须由用户明确选择目标控制器, 并优先按 USB 序列号区分;
    * DFU 设备多于一个时同样要求用户明确选择, 不按顺序猜设备.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

APP_TITLE = "TenoDX DFU 自动刷写工具"
APP_NAME = "TenoDXDFUFlasher"

# ---------------------------------------------------------------------------
# Magic 配置协议 (与 App/config/magic_config.c 保持一致)
# ---------------------------------------------------------------------------

MAGIC_SEQUENCE = bytes((0x91, 0x3E, 0xED, 0x20, 0x7C, 0x99, 0x58, 0xAC))
MAGIC_RESPONSE_SYNC = 0xAC
MAGIC_MODULE_GLOBAL = 0x00
MAGIC_MODULE_TOUCH = 0x10
MAGIC_CMD_GET_INFO = 0x05
MAGIC_CMD_ENTER_DFU = 0x84
MAGIC_DFU_CONFIRM = 0xA5
MAGIC_MAX_PAYLOAD = 248

MAGIC_STATUS_TEXT = {
    0x00: "成功",
    0x01: "校验和错误",
    0x02: "模块不支持",
    0x03: "命令不支持",
    0x04: "参数错误",
    0x05: "长度错误",
    0x06: "IO 错误",
}

# ---------------------------------------------------------------------------
# TenoDX USB 标识 (见 Core/Src/usb_descriptors.c)
# ---------------------------------------------------------------------------

TENODX_VID = 0x0483
TENODX_PID = 0x5740
TENODX_DEBUG_MI = 6  # CDC3 -> 接口号 6

# ---------------------------------------------------------------------------
# 固件与工具搜索路径
# ---------------------------------------------------------------------------

FIRMWARE_FILETYPES = (
    ("固件文件", "*.hex *.bin *.elf *.srec *.s19"),
    ("Intel HEX", "*.hex"),
    ("Binary", "*.bin"),
    ("ELF", "*.elf"),
    ("所有文件", "*.*"),
)
VALID_SUFFIXES = {".hex", ".bin", ".elf", ".srec", ".s19"}

NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0

PROGRAMMER_ROOTS = (
    r"D:\Programs\STM32CubeCLT_1.19.0\STM32CubeProgrammer",
    r"D:\Programs\STM32CubeProgrammer",
    r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer",
    r"C:\Program Files (x86)\STMicroelectronics\STM32Cube\STM32CubeProgrammer",
)

DFU_DRIVER_INF_PATHS = (
    r"D:\Programs\STM32CubeProgrammer\Drivers\DFU_Driver\Driver\STM32Bootloader.inf",
    r"D:\Files\TenoDX2\tools\DFU-driver\STM32Bootloader.inf",
    r"D:\Files\TenoDX2\synchronous\tools\DFU-driver\STM32Bootloader.inf",
)

try:  # pyserial 在打包时会一起带上
    import serial
    from serial.tools import list_ports as serial_list_ports
except Exception:  # pragma: no cover - 只有在未安装 pyserial 时才会走到
    serial = None
    serial_list_ports = None


# ---------------------------------------------------------------------------
# 数据结构
# ---------------------------------------------------------------------------


@dataclass
class MagicResponse:
    status: int
    module: int
    command: int
    param: int
    payload: bytes

    @property
    def ok(self) -> bool:
        return self.status == 0x00

    @property
    def status_text(self) -> str:
        return MAGIC_STATUS_TEXT.get(self.status, f"未知状态 0x{self.status:02X}")

    def summary(self) -> str:
        return (
            f"status={self.status_text} module=0x{self.module:02X} "
            f"cmd=0x{self.command:02X} param=0x{self.param:02X} "
            f"len={len(self.payload)}"
        )


@dataclass
class TenoDxPort:
    device: str
    serial: str
    description: str
    hwid: str
    interface: int | None = None

    @property
    def label(self) -> str:
        serial = self.serial or "无序列号"
        return f"{self.device}  |  SN={serial}  |  {self.description or 'TenoDX Debug Port'}"


@dataclass
class DfuDevice:
    index: str = ""
    serial: str = ""
    name: str = ""
    device_id: str = ""

    @property
    def label(self) -> str:
        parts = [self.name or "STM32 DFU"]
        if self.device_id:
            parts.append(f"ID={self.device_id}")
        if self.serial:
            parts.append(f"SN={self.serial}")
        if self.index:
            parts.append(f"index={self.index}")
        return "  |  ".join(parts)


class FlashError(RuntimeError):
    """工具可预期失败, 用于显示给用户."""


# ---------------------------------------------------------------------------
# 基础工具函数
# ---------------------------------------------------------------------------


def is_admin() -> bool:
    if sys.platform != "win32":
        return False
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def has_console() -> bool:
    """判断当前进程是否附着控制台 (windowed 打包运行时通常为 False)."""
    if sys.platform != "win32":
        return True
    # 控制台版 exe 由构建脚本命名为 *-CLI, 一定有控制台.
    if getattr(sys, "frozen", False) and Path(sys.executable).stem.upper().endswith("-CLI"):
        return True
    try:
        return bool(ctypes.windll.kernel32.GetConsoleWindow())
    except Exception:
        return True


def elevate() -> bool:
    """以管理员身份重新启动当前程序."""
    if sys.platform != "win32":
        return False
    try:
        if getattr(sys, "frozen", False):
            target = sys.executable
            args = list(sys.argv[1:])
        else:
            target = sys.executable
            args = [str(Path(__file__).resolve()), *sys.argv[1:]]
        params = " ".join(f'"{item}"' for item in args)
        result = ctypes.windll.shell32.ShellExecuteW(
            None, "runas", target, params, None, 1
        )
        return int(result) > 32
    except Exception:
        return False


def resource_root() -> Path:
    """打包后的解压目录, 开发时是脚本所在目录."""
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
    return Path(__file__).resolve().parent


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def human_size(size: int) -> str:
    if size < 1024:
        return f"{size} B"
    if size < 1024 * 1024:
        return f"{size / 1024:.1f} KiB"
    return f"{size / (1024 * 1024):.2f} MiB"


def find_programmer_cli(explicit: str | None = None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))

    found = shutil.which("STM32_Programmer_CLI")
    if found:
        candidates.append(Path(found))

    for root in PROGRAMMER_ROOTS:
        candidates.append(Path(root) / "bin" / "STM32_Programmer_CLI.exe")

    bundled = resource_root() / "bin" / "STM32_Programmer_CLI.exe"
    candidates.append(bundled)

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    raise FlashError(
        "找不到 STM32_Programmer_CLI.exe。\n"
        "请安装 STM32CubeProgrammer / STM32CubeCLT, "
        "或使用打包脚本把 CLI 一起打进 exe。"
    )


def programmer_workdir(cli: Path) -> Path:
    """CLI 需要能相对找到 Data_Base, 所以用它的上一级目录做工作目录."""
    return cli.resolve().parent.parent


def run_cli(
    command: list[str],
    *,
    cwd: Path | None = None,
    timeout: float = 300.0,
    log=None,
    echo: bool = True,
) -> tuple[int, str]:
    if echo and log is not None:
        log("> " + " ".join(f'"{item}"' if " " in item else item for item in command))

    try:
        process = subprocess.Popen(
            command,
            cwd=str(cwd) if cwd else None,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            creationflags=NO_WINDOW,
        )
    except OSError as exc:
        raise FlashError(f"无法启动 STM32_Programmer_CLI: {exc}") from exc

    lines: list[str] = []
    deadline = time.monotonic() + timeout
    assert process.stdout is not None

    try:
        for raw in process.stdout:
            line = raw.rstrip("\r\n")
            if line.strip():
                lines.append(line)
                if echo and log is not None:
                    log(line)
            if time.monotonic() > deadline:
                process.kill()
                raise FlashError("STM32_Programmer_CLI 执行超时。")
        process.wait(timeout=15)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)

    return int(process.returncode or 0), "\n".join(lines)


# ---------------------------------------------------------------------------
# Magic 协议 (CDC3 Debug Port)
# ---------------------------------------------------------------------------


def build_magic_request(module: int, command: int, param: int = 0, payload: bytes = b"") -> bytes:
    if len(payload) > MAGIC_MAX_PAYLOAD:
        raise ValueError(f"payload 过长: {len(payload)}")
    header = bytes((module & 0xFF, command & 0xFF, param & 0xFF, len(payload)))
    checksum = (sum(header) + sum(payload)) & 0xFF
    return MAGIC_SEQUENCE + header + payload + bytes((checksum,))


def _read_exact(handle, length: int, deadline: float) -> bytes:
    data = bytearray()
    while len(data) < length and time.monotonic() < deadline:
        chunk = handle.read(length - len(data))
        if chunk:
            data.extend(chunk)
    if len(data) != length:
        raise TimeoutError(f"读取 {length} 字节超时, 只收到 {len(data)} 字节")
    return bytes(data)


def read_magic_response(handle, timeout: float) -> MagicResponse:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        first = handle.read(1)
        if not first or first[0] != MAGIC_RESPONSE_SYNC:
            continue
        tail = _read_exact(handle, 5, deadline)
        header = bytes((MAGIC_RESPONSE_SYNC,)) + tail
        payload_length = header[5]
        payload_and_sum = _read_exact(handle, payload_length + 1, deadline)
        payload = payload_and_sum[:-1]
        checksum = payload_and_sum[-1]
        expected = (sum(header) + sum(payload)) & 0xFF
        if checksum != expected:
            raise FlashError(
                f"Magic 响应校验和错误: got 0x{checksum:02X}, expected 0x{expected:02X}"
            )
        return MagicResponse(
            status=header[1],
            module=header[2],
            command=header[3],
            param=header[4],
            payload=payload,
        )
    raise TimeoutError("等待 Magic 响应超时")


def magic_request(
    port: str,
    module: int,
    command: int,
    param: int = 0,
    payload: bytes = b"",
    timeout: float = 1.5,
) -> MagicResponse:
    if serial is None:
        raise FlashError("缺少 pyserial, 无法访问 Debug Port。请先 pip install pyserial。")

    frame = build_magic_request(module, command, param, payload)
    try:
        with serial.Serial(port, baudrate=115200, timeout=0.05, write_timeout=timeout) as handle:
            handle.reset_input_buffer()
            handle.write(frame)
            handle.flush()
            return read_magic_response(handle, timeout)
    except FlashError:
        raise
    except TimeoutError as exc:
        raise FlashError(f"{port} 没有返回 Magic 响应: {exc}") from exc
    except Exception as exc:
        raise FlashError(f"打开 {port} 失败: {exc}") from exc


def magic_probe(port: str, timeout: float = 1.0) -> bool:
    try:
        response = magic_request(port, MAGIC_MODULE_TOUCH, MAGIC_CMD_GET_INFO, timeout=timeout)
    except Exception:
        return False
    return response.module == MAGIC_MODULE_TOUCH and response.command == MAGIC_CMD_GET_INFO


def magic_enter_dfu(port: str, timeout: float = 1.5) -> MagicResponse:
    return magic_request(
        port,
        MAGIC_MODULE_GLOBAL,
        MAGIC_CMD_ENTER_DFU,
        payload=bytes((MAGIC_DFU_CONFIRM,)),
        timeout=timeout,
    )


# ---------------------------------------------------------------------------
# 串口 / 设备枚举
# ---------------------------------------------------------------------------


def _hwid_is_tenodx(hwid: str) -> bool:
    upper = hwid.upper().replace(" ", "")
    return (
        ("VID_0483" in upper and "PID_5740" in upper)
        or f"VID:PID={TENODX_VID:04X}:{TENODX_PID:04X}" in upper
    )


def _hwid_interface(hwid: str) -> int | None:
    match = re.search(r"MI_([0-9A-F]{2})", hwid, re.IGNORECASE)
    if match:
        try:
            return int(match.group(1), 16)
        except ValueError:
            return None
    # 有些 Windows 版本只给 LOCATION=1-2:1.6 或 LOCATION=1-1:x.6,
    # 末尾是接口号 (可能是十进制, 也可能是 x.十六进制 形式).
    match = re.search(r"LOCATION=[^:]+:(.+)$", hwid, re.IGNORECASE)
    if match:
        tail = match.group(1).strip()
        segment = tail.split(".")[-1] if "." in tail else tail
        segment = segment.lstrip("xX")
        if segment:
            try:
                return int(segment, 16)
            except ValueError:
                return None
    return None


def list_tenodx_ports(probe: bool = False) -> list[TenoDxPort]:
    """列出 TenoDX 控制器, 每个控制器返回它的 Debug Port (CDC3)."""
    if serial_list_ports is None:
        raise FlashError("缺少 pyserial, 无法枚举串口。请先 pip install pyserial。")

    candidates: list[TenoDxPort] = []
    for port in serial_list_ports.comports():
        hwid = getattr(port, "hwid", "") or ""
        if not _hwid_is_tenodx(hwid):
            continue
        interface = _hwid_interface(hwid)
        candidates.append(
            TenoDxPort(
                device=port.device,
                serial=getattr(port, "serial_number", "") or "",
                description=getattr(port, "description", "") or "",
                hwid=hwid,
                interface=interface,
            )
        )

    debug_ports = [item for item in candidates if item.interface == TENODX_DEBUG_MI]
    if not debug_ports:
        debug_ports = candidates

    if not probe:
        return debug_ports

    verified = [item for item in debug_ports if magic_probe(item.device)]
    return verified or debug_ports


def _dedupe_key(device: DfuDevice) -> tuple[str, str, str, str]:
    return (device.index, device.serial, device.name, device.device_id)


def parse_dfu_devices(text: str) -> list[DfuDevice]:
    """解析 STM32_Programmer_CLI -l usb 输出中的 DFU 设备.

    真实输出形如::

        =====  DFU Interface   =====
        Total number of available STM32 device in DFU mode: 1
          Device Index           : 0
          Product ID             : 0xDF11
          Serial number          : 2030FFFFFFFE
          Device ID              : 0x0474
    """
    if not text:
        return []

    lowered = text.lower()
    if "no stm32 device in dfu mode connected" in lowered and "device index" not in lowered:
        return []

    devices: list[DfuDevice] = []
    current: dict[str, str] | None = None

    def flush() -> None:
        nonlocal current
        if current is not None and current.get("index"):
            devices.append(
                DfuDevice(
                    index=current.get("index", ""),
                    serial=current.get("serial", ""),
                    name=current.get("product_id", ""),
                    device_id=current.get("device_id", ""),
                )
            )
        current = None

    for raw in text.splitlines():
        line = raw.strip()
        if not line or ":" not in line:
            continue

        key, _, value = line.partition(":")
        key = key.strip().lower()
        value = value.strip()

        if key.startswith("device index"):
            flush()
            current = {"index": value}
            continue

        if current is None:
            continue

        if key.startswith("serial"):
            if current is None:
                current = {}
            current["serial"] = value
        elif key.startswith("product id"):
            current["product_id"] = value
        elif key.startswith("device id"):
            current["device_id"] = value

    flush()

    unique: list[DfuDevice] = []
    seen: set[tuple[str, str, str, str]] = set()
    for device in devices:
        key = _dedupe_key(device)
        if key in seen:
            continue
        seen.add(key)
        unique.append(device)
    return unique


def list_dfu_devices(cli: Path) -> tuple[list[DfuDevice], str]:
    _, output = run_cli(
        [str(cli), "-l", "usb"],
        cwd=programmer_workdir(cli),
        timeout=60,
        log=None,
        echo=False,
    )
    return parse_dfu_devices(output), output


# ---------------------------------------------------------------------------
# 刷写
# ---------------------------------------------------------------------------


def build_flash_command(
    cli: Path,
    firmware: Path,
    *,
    serial: str | None = None,
    address: str = "0x08000000",
) -> list[str]:
    command = [str(cli), "-c", "port=USB1"]
    if serial:
        command.append(f"sn={serial}")
    command += ["-w", str(firmware)]
    if firmware.suffix.lower() == ".bin":
        command.append(address)
    # USB DFU 不支持 -rst (那只适用于 JTAG/SWD), ROM DFU 用 -detach 退出下载会话
    # 并让设备自行复位运行新固件.
    command += ["-v", "-detach"]
    return command


def flash_firmware(
    cli: Path,
    firmware: Path,
    *,
    serial: str | None = None,
    address: str = "0x08000000",
    log,
) -> bool:
    """写入并校验固件.

    返回 True 表示固件已确认写入成功, 且退出/运行命令也正常返回;
    返回 False 表示固件已确认写入成功, 但退出下载会话时 CLI 报了错
    (固件已经在芯片里, 只是无法确认复位/运行).

    校验通过之前遇到的任何问题都会抛出 FlashError.
    """
    if not firmware.is_file():
        raise FlashError(f"固件文件不存在: {firmware}")
    if firmware.suffix.lower() not in VALID_SUFFIXES:
        raise FlashError("不支持的固件格式, 请选择 .hex/.bin/.elf/.srec/.s19。")
    if firmware.suffix.lower() == ".bin" and not address:
        raise FlashError("刷写 .bin 必须指定起始地址, 例如 0x08000000。")

    command = build_flash_command(cli, firmware, serial=serial, address=address)
    code, output = run_cli(
        command,
        cwd=programmer_workdir(cli),
        timeout=600,
        log=log,
    )

    lowered = output.lower()
    flashed = ("download verified successfully" in lowered or "verified successfully" in lowered)

    if not flashed:
        if code != 0:
            raise FlashError(f"STM32_Programmer_CLI 返回错误码 {code}。")
        raise FlashError("没有看到下载校验成功的结果, 请检查上面的日志。")

    # 固件已确认写入并校验通过. 之后的退出/复位失败不应把整体判定为刷写失败,
    # 但要让调用方知道设备复位状态未知.
    return code == 0


# ---------------------------------------------------------------------------
# DFU 驱动安装
# ---------------------------------------------------------------------------


def find_dfu_driver_inf(explicit: str | None = None) -> Path | None:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.append(resource_root() / "drivers" / "dfu" / "STM32Bootloader.inf")
    here = Path(__file__).resolve().parent
    candidates += [here / "DFU-driver" / "STM32Bootloader.inf"]
    candidates += [Path(item) for item in DFU_DRIVER_INF_PATHS]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def install_dfu_driver(inf: Path, log) -> bool:
    if not is_admin():
        raise FlashError("安装 DFU 驱动需要管理员权限, 请以管理员身份重新运行本工具。")
    if not inf.is_file():
        raise FlashError(f"找不到 DFU 驱动 INF 文件: {inf}")

    code, output = run_cli(
        ["pnputil.exe", "/add-driver", str(inf), "/install"],
        timeout=180,
        log=log,
    )
    if code != 0:
        raise FlashError(f"pnputil 返回错误码 {code}, 驱动可能未安装成功。")
    return True


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------


def run_gui() -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    class DfuFlasherApp:
        def __init__(self, root: "tk.Tk") -> None:
            self.root = root
            self.root.title(APP_TITLE)
            self.root.geometry("860x720")
            self.root.minsize(760, 620)

            self.busy = False
            self.cancel = threading.Event()
            self.programmer: Path | None = None
            self.programmer_error = ""
            self.ports: list[TenoDxPort] = []
            self.dfu_devices: list[DfuDevice] = []
            self.wait_seconds = 180
            self.closing = False

            style = ttk.Style()
            try:
                style.theme_use("vista")
            except Exception:
                pass

            self._build_ui()
            self.root.protocol("WM_DELETE_WINDOW", self.on_close)
            self.root.after(150, self._startup)

        # ---------------- UI ----------------

        def _build_ui(self) -> None:
            import tkinter as tk
            from tkinter import ttk
            from tkinter.scrolledtext import ScrolledText

            outer = ttk.Frame(self.root, padding=12)
            outer.pack(fill="both", expand=True)

            ttk.Label(
                outer,
                text="TenoDX DFU 自动刷写工具",
                font=("Microsoft YaHei UI", 15, "bold"),
            ).pack(anchor="w")
            ttk.Label(
                outer,
                text="选择控制器后发送 DFU 命令, 按提示拔插 USB, 工具会自动检测 DFU 设备并刷写。全程不需要 ST-Link。",
                foreground="#555555",
            ).pack(anchor="w", pady=(2, 10))

            # 固件
            fw_frame = ttk.LabelFrame(outer, text="1. 固件", padding=8)
            fw_frame.pack(fill="x", pady=(0, 8))

            self.fw_var = tk.StringVar()
            self.address_var = tk.StringVar(value="0x08000000")
            self.fw_info_var = tk.StringVar(value="尚未选择固件")

            ttk.Entry(fw_frame, textvariable=self.fw_var).grid(
                row=0, column=0, sticky="ew", padx=(0, 6)
            )
            ttk.Button(fw_frame, text="浏览...", width=10, command=self.choose_firmware).grid(
                row=0, column=1
            )
            ttk.Label(fw_frame, textvariable=self.fw_info_var, foreground="#666666").grid(
                row=1, column=0, columnspan=2, sticky="w", pady=(6, 0)
            )

            addr_frame = ttk.Frame(fw_frame)
            addr_frame.grid(row=2, column=0, columnspan=2, sticky="w", pady=(6, 0))
            ttk.Label(addr_frame, text="BIN 起始地址:").pack(side="left")
            ttk.Entry(addr_frame, textvariable=self.address_var, width=18).pack(
                side="left", padx=(6, 0)
            )
            ttk.Label(
                addr_frame,
                text="(只有 .bin 使用, .hex/.elf 会忽略)",
                foreground="#888888",
            ).pack(side="left", padx=(8, 0))

            fw_frame.columnconfigure(0, weight=1)

            # 设备
            dev_frame = ttk.LabelFrame(outer, text="2. 目标控制器 (TenoDX Debug Port)", padding=8)
            dev_frame.pack(fill="x", pady=(0, 8))

            self.port_var = tk.StringVar()
            self.port_combo = ttk.Combobox(
                dev_frame, textvariable=self.port_var, state="readonly", width=80
            )
            self.port_combo.grid(row=0, column=0, sticky="ew", padx=(0, 6))
            ttk.Button(dev_frame, text="刷新设备", width=12, command=self.refresh_ports).grid(
                row=0, column=1
            )

            self.port_hint_var = tk.StringVar(value="正在检测...")
            ttk.Label(dev_frame, textvariable=self.port_hint_var, foreground="#666666").grid(
                row=1, column=0, columnspan=2, sticky="w", pady=(6, 0)
            )
            dev_frame.columnconfigure(0, weight=1)

            # 操作
            act_frame = ttk.LabelFrame(outer, text="3. 刷写", padding=8)
            act_frame.pack(fill="x", pady=(0, 8))

            self.verify_var = tk.BooleanVar(value=True)
            ttk.Checkbutton(
                act_frame,
                text="刷写完成后等待 TenoDX Debug Port 重新出现并做 Magic 校验",
                variable=self.verify_var,
            ).grid(row=0, column=0, columnspan=4, sticky="w")

            self.btn_full = ttk.Button(
                act_frame,
                text="一键: 进入 DFU 并自动刷写",
                width=26,
                command=self.start_full_flow,
            )
            self.btn_full.grid(row=1, column=0, columnspan=2, sticky="w", pady=(8, 0))

            self.btn_enter = ttk.Button(
                act_frame, text="仅发送 DFU 命令", width=18, command=self.send_dfu_command
            )
            self.btn_enter.grid(row=1, column=2, sticky="w", padx=(6, 0), pady=(8, 0))

            self.btn_flash = ttk.Button(
                act_frame, text="刷写已就绪的 DFU 设备", width=22, command=self.flash_only
            )
            self.btn_flash.grid(row=1, column=3, sticky="w", padx=(6, 0), pady=(8, 0))

            self.btn_cancel = ttk.Button(
                act_frame, text="停止等待", width=12, command=self.request_cancel, state="disabled"
            )
            self.btn_cancel.grid(row=1, column=4, sticky="w", padx=(6, 0), pady=(8, 0))

            info_frame = ttk.Frame(act_frame)
            info_frame.grid(row=2, column=0, columnspan=5, sticky="ew", pady=(8, 0))
            self.dfu_hint_var = tk.StringVar(value="DFU 设备: 未检测")
            ttk.Label(info_frame, textvariable=self.dfu_hint_var, foreground="#666666").pack(
                side="left"
            )
            ttk.Button(info_frame, text="检测 DFU 设备", width=14, command=self.refresh_dfu).pack(
                side="right"
            )
            act_frame.columnconfigure(2, weight=1)

            # 日志
            log_frame = ttk.LabelFrame(outer, text="4. 日志", padding=8)
            log_frame.pack(fill="both", expand=True)

            self.log_box = ScrolledText(log_frame, wrap="word", height=16)
            self.log_box.pack(fill="both", expand=True)
            self.log_box.configure(state="disabled")

            # 状态
            status_frame = ttk.Frame(outer)
            status_frame.pack(fill="x", pady=(8, 0))
            self.status_var = tk.StringVar(value="准备就绪")
            ttk.Label(
                status_frame, textvariable=self.status_var, foreground="#1a5fb4"
            ).pack(side="left")
            self.progress = ttk.Progressbar(status_frame, mode="indeterminate", length=220)
            self.progress.pack(side="right")

            buttons = ttk.Frame(outer)
            buttons.pack(fill="x", pady=(6, 0))
            self.admin_button = ttk.Button(
                buttons, text="以管理员身份重启", command=self.restart_as_admin
            )
            self.admin_button.pack(side="left")
            ttk.Button(buttons, text="安装 DFU 驱动", command=self.install_driver_gui).pack(
                side="left", padx=(6, 0)
            )
            ttk.Button(buttons, text="复制日志", command=self.copy_log).pack(side="left", padx=(6, 0))
            ttk.Button(buttons, text="清空日志", command=self.clear_log).pack(side="left", padx=(6, 0))

        # ---------------- 线程安全 UI 辅助 ----------------

        def ui(self, func, *args) -> None:
            if self.closing:
                return
            try:
                self.root.after(0, lambda: func(*args))
            except Exception:
                # 窗口可能正在销毁, 后台线程不应该因此崩溃
                pass

        def on_close(self) -> None:
            self.closing = True
            self.cancel.set()
            try:
                self.root.destroy()
            except Exception:
                pass

        def log(self, message: str) -> None:
            def append() -> None:
                self.log_box.configure(state="normal")
                self.log_box.insert("end", message + "\n")
                self.log_box.see("end")
                self.log_box.configure(state="disabled")

            self.ui(append)

        def set_status(self, text: str) -> None:
            self.ui(self.status_var.set, text)

        def set_controls_enabled(self, enabled: bool) -> None:
            state = "normal" if enabled else "disabled"
            for widget in (self.btn_full, self.btn_enter, self.btn_flash):
                widget.configure(state=state)
            self.btn_cancel.configure(state="disabled" if enabled else "normal")

        def clear_log(self) -> None:
            self.log_box.configure(state="normal")
            self.log_box.delete("1.0", "end")
            self.log_box.configure(state="disabled")

        def copy_log(self) -> None:
            from tkinter import messagebox

            text = self.log_box.get("1.0", "end").strip()
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            messagebox.showinfo("复制日志", "日志已复制到剪贴板。")

        def _show_error(self, title: str, message: str) -> None:
            from tkinter import messagebox

            messagebox.showerror(title, message)

        def _show_info(self, title: str, message: str) -> None:
            from tkinter import messagebox

            messagebox.showinfo(title, message)

        # ---------------- 启动 ----------------

        def _startup(self) -> None:
            self._load_programmer()
            if not is_admin():
                self.log("提示: 当前不是管理员权限, 安装 DFU 驱动会失败, 但刷写本身通常可以继续。")
            self.refresh_devices()

        def _load_programmer(self) -> None:
            try:
                self.programmer = find_programmer_cli()
                self.programmer_error = ""
                self.log(f"STM32_Programmer_CLI: {self.programmer}")
            except FlashError as exc:
                self.programmer = None
                self.programmer_error = str(exc)
                self.log(f"错误: {self.programmer_error}")

        def restart_as_admin(self) -> None:
            if is_admin():
                self._show_info("权限", "当前已经是管理员权限。")
                return
            if not elevate():
                self._show_error("提权失败", "UAC 请求被取消或失败, 请右键选择“以管理员身份运行”。")
            else:
                self.root.destroy()

        def install_driver_gui(self) -> None:
            from tkinter import messagebox

            inf = find_dfu_driver_inf()
            if inf is None:
                self._show_error(
                    "找不到驱动",
                    "没有找到 STM32Bootloader.inf。\n"
                    "请把 STM32CubeProgrammer 的 DFU 驱动目录放到工具旁边, 或先安装 CubeProgrammer。",
                )
                return
            if not is_admin():
                if messagebox.askyesno("需要管理员权限", "安装 DFU 驱动需要管理员权限。\n现在提权重启工具吗?"):
                    if elevate():
                        self.root.destroy()
                return

            def work() -> None:
                self.log(f"安装 DFU 驱动: {inf}")
                install_dfu_driver(inf, self.log)
                self.log("DFU 驱动安装命令已完成。")
                self.set_status("DFU 驱动已安装")
                self.ui(
                    messagebox.showinfo,
                    "驱动安装",
                    "驱动安装命令已完成。\n如果设备管理器里已经有 STM32 Bootloader, 可以继续刷写。",
                )

            self._run_worker(work, "安装 DFU 驱动")

        # ---------------- 刷新设备 ----------------

        def choose_firmware(self) -> None:
            from tkinter import filedialog

            path = filedialog.askopenfilename(title="选择要刷入的固件", filetypes=FIRMWARE_FILETYPES)
            if not path:
                return
            self.fw_var.set(path)
            self._update_firmware_info()

        def _update_firmware_info(self) -> None:
            raw = self.fw_var.get().strip()
            path = Path(raw) if raw else None
            if path is None or not path.is_file():
                self.fw_info_var.set("文件不存在")
                return
            try:
                digest = sha256_file(path)
            except Exception as exc:
                self.fw_info_var.set(f"读取失败: {exc}")
                return
            self.fw_info_var.set(
                f"{path.name}  |  {human_size(path.stat().st_size)}  |  SHA256 {digest[:16]}..."
            )

        def refresh_ports(self) -> None:
            self.refresh_devices()

        def refresh_dfu(self) -> None:
            self.refresh_devices()

        def refresh_devices(self) -> None:
            """后台刷新控制器和 DFU 设备, 不阻塞 GUI 线程."""
            if self.busy:
                return
            self.port_hint_var.set("正在检测设备...")

            def work() -> None:
                ports: list[TenoDxPort] = []
                devices: list[DfuDevice] = []
                port_error = ""
                dfu_error = ""

                try:
                    ports = list_tenodx_ports()
                except Exception as exc:
                    port_error = str(exc)

                if self.programmer is None:
                    dfu_error = self.programmer_error or "未找到 STM32_Programmer_CLI"
                else:
                    try:
                        devices, _ = list_dfu_devices(self.programmer)
                    except Exception as exc:
                        dfu_error = str(exc)

                def apply() -> None:
                    self.ports = ports
                    labels = [item.label for item in ports]
                    self.port_combo["values"] = labels
                    if labels and self.port_var.get() not in labels:
                        self.port_var.set(labels[0])
                    if not labels:
                        self.port_var.set("")
                    if labels:
                        self.port_hint_var.set(f"检测到 {len(labels)} 个控制器 Debug Port。")
                    elif port_error:
                        self.port_hint_var.set(port_error)
                    else:
                        self.port_hint_var.set(
                            "未检测到 TenoDX Debug Port (VID 0483 / PID 5740 / MI_06)。"
                        )

                    self.dfu_devices = devices
                    if devices:
                        self.dfu_hint_var.set(
                            f"DFU 设备: {len(devices)} 个 -> "
                            + "; ".join(device.label for device in devices)
                        )
                    elif dfu_error:
                        self.dfu_hint_var.set(dfu_error)
                    else:
                        self.dfu_hint_var.set("DFU 设备: 未检测到")

                self.ui(apply)

            threading.Thread(target=work, name="refresh-devices", daemon=True).start()

        def selected_port(self) -> TenoDxPort | None:
            label = self.port_var.get().strip()
            for item in self.ports:
                if item.label == label:
                    return item
            return None

        def _validate_inputs(self) -> tuple[Path, TenoDxPort]:
            raw = self.fw_var.get().strip()
            if not raw:
                raise FlashError("请先选择固件文件。")
            firmware = Path(raw)
            if not firmware.is_file():
                raise FlashError(f"固件文件不存在: {firmware}")
            if firmware.suffix.lower() not in VALID_SUFFIXES:
                raise FlashError("固件格式不支持, 请选择 .hex/.bin/.elf/.srec/.s19。")

            port = self.selected_port()
            if port is None:
                raise FlashError("请选择目标控制器 Debug Port。")
            if self.programmer is None:
                raise FlashError(self.programmer_error or "找不到 STM32_Programmer_CLI。")
            return firmware, port

        # ---------------- 后台任务 ----------------

        def _run_worker(self, target, name: str) -> None:
            if self.busy:
                return
            self.busy = True
            self.cancel.clear()
            self.ui(self.set_controls_enabled, False)
            self.ui(self.progress.start, 12)
            self.set_status(f"{name}...")

            def wrapper() -> None:
                error: Exception | None = None
                try:
                    target()
                except FlashError as exc:
                    error = exc
                except Exception as exc:  # pragma: no cover - 兜底
                    error = exc
                finally:
                    self.busy = False
                    self.ui(self.progress.stop)
                    self.ui(self.set_controls_enabled, True)

                if error is not None:
                    self.log(f"失败: {error}")
                    self.set_status(f"{name}失败")
                    self.ui(self._show_error, f"{name}失败", str(error))

            threading.Thread(target=wrapper, name=name, daemon=True).start()

        def request_cancel(self) -> None:
            self.cancel.set()
            self.log("已请求停止等待。")
            self.set_status("正在停止...")

        # ---------------- 业务动作 ----------------

        def send_dfu_command(self) -> None:
            try:
                _, port = self._validate_inputs()
            except FlashError as exc:
                self._show_error("参数错误", str(exc))
                return

            def work() -> None:
                self.log("=" * 64)
                self.log(f"目标控制器: {port.label}")
                self.log("发送 Magic DFU 命令 (module=0x00 cmd=0x84 payload=[A5])...")
                response = magic_enter_dfu(port.device)
                self.log("响应: " + response.summary())
                if not response.ok:
                    raise FlashError(f"设备拒绝 DFU 命令: {response.status_text}")
                self.log("DFU 命令已被接受。设备会在约 100 ms 后跳转, CDC 串口即将断开。")
                self.set_status("DFU 命令已接受")
                self.ui(
                    self._show_info,
                    "已进入 DFU",
                    "设备已接受 DFU 命令。\n\n请拔下 USB 再插回, 让 Windows 重新枚举 DFU 设备。\n"
                    "然后可以点击“刷写已就绪的 DFU 设备”。",
                )

            self._run_worker(work, "发送 DFU 命令")

        def wait_for_dfu(self, timeout: float) -> list[DfuDevice]:
            assert self.programmer is not None
            deadline = time.monotonic() + timeout
            last_report = 0.0
            while time.monotonic() < deadline:
                if self.cancel.is_set():
                    raise FlashError("用户取消了等待。")
                devices, _ = list_dfu_devices(self.programmer)
                if devices:
                    self.log(f"检测到 {len(devices)} 个 DFU 设备。")
                    for device in devices:
                        self.log("  " + device.label)
                    return devices

                now = time.monotonic()
                if now - last_report >= 5.0:
                    remaining = int(deadline - now)
                    self.set_status(f"等待 DFU 设备出现... 剩余 {remaining}s, 请拔插 USB")
                    self.log(f"仍在等待 DFU 设备 (剩余 {remaining}s)...")
                    last_report = now
                time.sleep(1.0)
            raise FlashError(
                "等待 DFU 设备超时。\n\n"
                "请确认:\n"
                "1) 设备确实收到了 DFU 命令;\n"
                "2) 已拔插一次 USB;\n"
                "3) Windows 设备管理器里出现了 STM32 Bootloader / DFU 设备;\n"
                "4) 已安装 STM32 DFU (WinUSB) 驱动。"
            )

        def choose_dfu_device(self, devices: list[DfuDevice]) -> DfuDevice:
            if len(devices) == 1:
                return devices[0]

            from tkinter import simpledialog

            choices = [f"{index + 1}. {device.label}" for index, device in enumerate(devices)]
            answer = simpledialog.askinteger(
                "选择 DFU 设备",
                "检测到多个 DFU 设备, 请输入要刷写的编号:\n\n" + "\n".join(choices),
                parent=self.root,
                minvalue=1,
                maxvalue=len(devices),
            )
            if answer is None:
                raise FlashError("没有选择 DFU 设备。")
            return devices[answer - 1]

        def wait_for_app_port(self, serial_number: str, timeout: float) -> TenoDxPort | None:
            deadline = time.monotonic() + timeout
            target = serial_number.lower()
            while time.monotonic() < deadline:
                if self.cancel.is_set():
                    return None
                try:
                    ports = list_tenodx_ports()
                except Exception:
                    ports = []
                for item in ports:
                    if target and item.serial.lower() == target:
                        return item
                if ports and not target:
                    return ports[0]
                time.sleep(1.0)
            return None

        def start_full_flow(self) -> None:
            try:
                firmware, port = self._validate_inputs()
            except FlashError as exc:
                self._show_error("参数错误", str(exc))
                return

            address = self.address_var.get().strip() or "0x08000000"
            verify_after = bool(self.verify_var.get())
            selected_serial = port.serial

            def work() -> None:
                self.log("=" * 64)
                self.log(f"固件: {firmware}")
                self.log(f"目标控制器: {port.label}")

                self.set_status("发送 DFU 命令...")
                self.log("发送 Magic DFU 命令 (module=0x00 cmd=0x84 payload=[A5])...")
                response = magic_enter_dfu(port.device)
                self.log("响应: " + response.summary())
                if not response.ok:
                    raise FlashError(f"设备拒绝 DFU 命令: {response.status_text}")
                self.log("DFU 命令已被接受。")

                self.set_status("等待用户拔插 USB...")
                self.ui(
                    self._show_info,
                    "请拔插 USB",
                    "设备已接受 DFU 命令, CDC 串口会断开。\n\n"
                    "请拔下 USB 再插回, 让 Windows 重新枚举 DFU 设备。\n"
                    "工具会在后台等待 DFU 设备出现。",
                )

                devices = self.wait_for_dfu(self.wait_seconds)
                target = self.choose_dfu_device(devices)
                self.log(f"选中 DFU 设备: {target.label}")

                self.set_status("正在刷写固件...")
                detach_ok = flash_firmware(
                    self.programmer,
                    firmware,
                    serial=target.serial or None,
                    address=address,
                    log=self.log,
                )
                if detach_ok:
                    self.log("固件写入并校验成功, 已发送 DFU detach。")
                else:
                    self.log("固件写入并校验成功; DFU detach 命令报了错, 设备可能仍停在 DFU。")

                verified = False
                if verify_after:
                    self.log(
                        "Windows 下 USB DFU 无法由 CLI 自动复位设备, "
                        "需要拔插一次 USB (或断电重上电) 才能运行新固件。"
                    )
                    self.set_status("请拔插 USB 让设备运行新固件...")
                    self.ui(
                        self._show_info,
                        "刷写完成, 请拔插 USB",
                        "固件已写入并校验通过。\n\n"
                        "Windows 下 USB DFU 不能自动复位设备, 现在请拔下 USB 再插回 "
                        "(或给设备断电重新上电)。\n\n"
                        "工具会继续等待 TenoDX Debug Port 重新出现。",
                    )
                    app_port = self.wait_for_app_port(selected_serial, 120.0)
                    if app_port is None:
                        self.log("120 秒内没有等待到 TenoDX Debug Port 重新出现。")
                    else:
                        self.log(f"设备已重新枚举: {app_port.label}")
                        if magic_probe(app_port.device):
                            verified = True
                            self.log("Magic 校验成功, 设备已运行新固件。")
                        else:
                            self.log("警告: 端口出现了, 但 Magic 校验没有通过。")

                if verified:
                    self.set_status("刷写成功")
                    self.ui(
                        self._show_info,
                        "刷写成功",
                        "固件已写入并校验通过, 设备已重新枚举并运行新固件。",
                    )
                elif not verify_after:
                    self.set_status("刷写成功")
                    self.ui(
                        self._show_info,
                        "刷写成功",
                        "固件已写入并校验通过。\n\n"
                        "请拔插一次 USB (或断电重上电) 让设备运行新固件。",
                    )
                else:
                    self.set_status("固件已写入, 但未确认设备已恢复")
                    self.ui(
                        self._show_info,
                        "固件已写入",
                        "固件已经写入并校验通过, 但工具没有等到 TenoDX Debug Port 重新出现。\n\n"
                        "请拔插一次 USB 或给设备断电重上电。\n"
                        "如果仍然看不到设备, 请检查设备管理器里是否还停留在 DFU 模式。",
                    )

            self._run_worker(work, "一键刷写")

        def flash_only(self) -> None:
            raw = self.fw_var.get().strip()
            if not raw:
                self._show_error("参数错误", "请先选择固件文件。")
                return
            firmware = Path(raw)
            if not firmware.is_file():
                self._show_error("参数错误", f"固件文件不存在: {firmware}")
                return
            if firmware.suffix.lower() not in VALID_SUFFIXES:
                self._show_error("参数错误", "固件格式不支持, 请选择 .hex/.bin/.elf/.srec/.s19。")
                return
            if self.programmer is None:
                self._show_error("缺少工具", self.programmer_error or "找不到 STM32_Programmer_CLI。")
                return

            address = self.address_var.get().strip() or "0x08000000"

            def work() -> None:
                self.log("=" * 64)
                self.set_status("检测 DFU 设备...")
                devices, _ = list_dfu_devices(self.programmer)
                if not devices:
                    raise FlashError(
                        "没有检测到 DFU 设备。\n"
                        "请确认设备已经进入 DFU 模式, 并已安装 STM32 DFU 驱动。"
                    )
                for device in devices:
                    self.log("  " + device.label)
                target = self.choose_dfu_device(devices)

                self.set_status("正在刷写固件...")
                detach_ok = flash_firmware(
                    self.programmer,
                    firmware,
                    serial=target.serial or None,
                    address=address,
                    log=self.log,
                )
                if detach_ok:
                    self.set_status("刷写成功")
                    self.ui(
                        self._show_info,
                        "刷写成功",
                        "固件已写入并校验通过。\n\n"
                        "Windows 下 USB DFU 不能自动复位设备, 请拔插一次 USB "
                        "(或断电重上电) 让设备运行新固件。",
                    )
                else:
                    self.set_status("固件已写入, 请拔插 USB")
                    self.ui(
                        self._show_info,
                        "刷写完成",
                        "固件已写入并校验通过。\n\n"
                        "请拔插一次 USB 或给设备断电重上电, 让设备运行新固件。",
                    )

            self._run_worker(work, "DFU 刷写")

    root = tk.Tk()
    app = DfuFlasherApp(root)
    root.mainloop()
    return 0


# ---------------------------------------------------------------------------
# 命令行
# ---------------------------------------------------------------------------


def _print_ports(ports: list[TenoDxPort]) -> None:
    if not ports:
        print("未检测到 TenoDX Debug Port。")
        return
    for index, port in enumerate(ports, 1):
        print(f"{index}. {port.label}")
        print(f"   HWID: {port.hwid}")


def _print_dfu(devices: list[DfuDevice], raw: str) -> None:
    if not devices:
        print("未检测到 DFU 设备。")
        if "--verbose" in sys.argv:
            print(raw)
        return
    for index, device in enumerate(devices, 1):
        print(f"{index}. {device.label}")


def _pick_port(port_arg: str | None, serial_arg: str | None) -> TenoDxPort:
    ports = list_tenodx_ports()
    if port_arg:
        for item in ports:
            if item.device.lower() == port_arg.lower():
                return item
        # 允许直接传一个没被枚举出来的 COM 口
        return TenoDxPort(device=port_arg, serial=serial_arg or "", description="", hwid="")
    if serial_arg:
        lowered = serial_arg.lower()
        for item in ports:
            if item.serial.lower() == lowered or lowered in item.serial.lower():
                return item
        raise FlashError(f"没有找到序列号匹配 '{serial_arg}' 的控制器。")
    if len(ports) == 1:
        return ports[0]
    if not ports:
        raise FlashError("未检测到 TenoDX Debug Port。")
    raise FlashError(
        "检测到多个控制器, 请用 --serial 或 --port 指定目标。\n"
        + "\n".join(f"  {item.label}" for item in ports)
    )


def run_cli_mode(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=APP_TITLE)
    parser.add_argument("--list", action="store_true", help="列出 TenoDX Debug Port")
    parser.add_argument("--list-dfu", action="store_true", help="列出 DFU 设备")
    parser.add_argument("--enter-dfu", action="store_true", help="只发送 DFU 命令")
    parser.add_argument("--flash", action="store_true", help="刷写当前 DFU 设备")
    parser.add_argument("--port", help="目标 Debug Port 的 COM 号")
    parser.add_argument("--serial", help="目标控制器 USB 序列号")
    parser.add_argument("--firmware", help="要刷入的固件")
    parser.add_argument("--address", default="0x08000000", help="BIN 起始地址")
    parser.add_argument("--wait", type=float, default=180.0, help="等待 DFU 出现的秒数")
    parser.add_argument("--programmer", help="STM32_Programmer_CLI.exe 路径")
    parser.add_argument("--install-driver", action="store_true", help="安装 STM32 DFU 驱动")
    parser.add_argument("--driver-inf", help="STM32Bootloader.inf 路径")
    parser.add_argument("--no-verify-app", action="store_true", help="刷写后不等待设备回连")
    args = parser.parse_args(argv)

    try:
        if args.list:
            _print_ports(list_tenodx_ports())
            return 0

        if args.install_driver:
            inf = find_dfu_driver_inf(args.driver_inf)
            if inf is None:
                raise FlashError("找不到 STM32Bootloader.inf。")
            install_dfu_driver(inf, print)
            print("DFU 驱动安装完成。")
            return 0

        programmer = find_programmer_cli(args.programmer)

        if args.list_dfu:
            devices, raw = list_dfu_devices(programmer)
            _print_dfu(devices, raw)
            return 0

        if args.enter_dfu:
            port = _pick_port(args.port, args.serial)
            print(f"目标控制器: {port.label}")
            response = magic_enter_dfu(port.device)
            print("响应: " + response.summary())
            if not response.ok:
                raise FlashError(f"设备拒绝 DFU 命令: {response.status_text}")
            print("DFU 命令已接受, 请拔插 USB 让设备重新枚举。")
            return 0

        if args.flash:
            if not args.firmware:
                raise FlashError("--flash 需要配合 --firmware 使用。")
            firmware = Path(args.firmware)
            if not firmware.is_file():
                raise FlashError(f"固件不存在: {firmware}")

            selected_serial = ""
            if args.port or args.serial:
                port = _pick_port(args.port, args.serial)
                selected_serial = port.serial
                print(f"目标控制器: {port.label}")
                response = magic_enter_dfu(port.device)
                print("DFU 响应: " + response.summary())
                if not response.ok:
                    raise FlashError(f"设备拒绝 DFU 命令: {response.status_text}")

            deadline = time.monotonic() + args.wait
            devices: list[DfuDevice] = []
            while time.monotonic() < deadline:
                devices, _ = list_dfu_devices(programmer)
                if devices:
                    break
                remaining = int(deadline - time.monotonic())
                print(f"等待 DFU 设备... 剩余 {remaining}s")
                time.sleep(1.0)
            if not devices:
                raise FlashError("等待 DFU 设备超时。")
            for device in devices:
                print("  " + device.label)
            target = devices[0]
            if len(devices) > 1:
                raise FlashError("检测到多个 DFU 设备, 请只连接目标设备后重试。")

            detach_ok = flash_firmware(
                programmer,
                firmware,
                serial=target.serial or None,
                address=args.address,
                log=print,
            )
            if detach_ok:
                print("刷写成功。")
                print("Windows 下 USB DFU 不能自动复位, 请拔插一次 USB 让设备运行新固件。")
            else:
                print("固件已写入并校验通过, 请拔插一次 USB 或重新上电让设备运行新固件。")

            if not args.no_verify_app and detach_ok:
                for _ in range(20):
                    ports = list_tenodx_ports()
                    match = next(
                        (
                            item
                            for item in ports
                            if not selected_serial
                            or item.serial.lower() == selected_serial.lower()
                        ),
                        None,
                    )
                    if match:
                        if magic_probe(match.device):
                            print(f"设备已回连并通过 Magic 校验: {match.label}")
                        else:
                            print(f"设备已回连, 但 Magic 校验失败: {match.label}")
                        break
                    time.sleep(1.0)
            return 0

        parser.print_help()
        return 2

    except FlashError as exc:
        print(f"失败: {exc}", file=sys.stderr)
        return 1


def main() -> int:
    argv = sys.argv[1:]

    # 打包成 --windowed 且没有控制台时, print/stderr 无处可写, 带参数运行时
    # 把输出重定向到日志文件. 有控制台时 (控制台版 exe 或开发模式) 正常输出.
    if argv and sys.platform == "win32" and getattr(sys, "frozen", False) and not has_console():
        log_path = Path(os.environ.get("TEMP", str(Path.home()))) / "TenoDXDFUFlasher.log"
        try:
            stream = log_path.open("w", encoding="utf-8", errors="replace")
            sys.stdout = stream
            sys.stderr = stream
        except Exception:
            pass

        # 看门狗: 命令行模式下如果因为外部工具卡死, 最长 20 分钟后强制退出.
        def watchdog() -> None:
            time.sleep(1200)
            os._exit(2)

        threading.Thread(target=watchdog, daemon=True).start()

    if sys.platform == "win32":
        try:
            ctypes.windll.kernel32.SetConsoleOutputCP(65001)
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    if not argv:
        if sys.platform == "win32":
            try:
                return run_gui()
            except Exception as exc:  # pragma: no cover
                print(f"图形界面启动失败: {exc}", file=sys.stderr)
                return 1
        print(APP_TITLE)
        print("在非 Windows 环境下请使用命令行参数启动, 例如 --list。")
        return 2

    try:
        return run_cli_mode(argv)
    except Exception as exc:  # pragma: no cover - 兜底, 避免 windowed exe 静默挂起
        try:
            print(f"失败: {exc}", file=sys.stderr)
        except Exception:
            pass
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

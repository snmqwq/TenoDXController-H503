#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Serial test tool for Sucareto/Arduino-Aime-Reader compatible devices."""

from __future__ import annotations

import ctypes
import re
import sys
import time
import tkinter as tk
from ctypes import wintypes
from dataclasses import dataclass
from tkinter import messagebox, ttk
from typing import Iterable

import serial
from serial.tools import list_ports


SYNC = 0xE0
ESCAPE = 0xD0
DEFAULT_ADDRESS = 0x00

CMD_GET_FW_VERSION = 0x30
CMD_GET_HW_VERSION = 0x32
CMD_START_POLLING = 0x40
CMD_STOP_POLLING = 0x41
CMD_CARD_DETECT = 0x42

STATUS_OK = 0x00
STATUS_NAMES = {
    0x00: "成功",
    0x01: "卡片错误",
    0x02: "不接受",
    0x03: "无效命令",
    0x04: "无效数据",
    0x05: "校验错误",
    0x06: "内部错误",
    0x07: "无效固件数据",
    0x08: "固件更新成功",
    0x10: "兼容状态 837-15286",
    0x20: "兼容状态 837-15396",
}

BAUDRATES = (115200, 38400)
SERIAL_OPEN_DELAY_SECONDS = 1.5
SERIAL_TIMEOUT_SECONDS = 0.7
CARD_DETECT_TIMEOUT_SECONDS = 1.5
SCAN_INTERVAL_MS = 220
MAX_FRAME_LENGTH = 128


class AimeProtocolError(RuntimeError):
    """The reader returned an invalid or unsuccessful protocol response."""


class AimeResponseTimeout(AimeProtocolError):
    """No matching response arrived before the deadline."""


@dataclass(frozen=True)
class AimeResponse:
    frame_length: int
    address: int
    sequence: int
    command: int
    status: int
    payload: bytes


@dataclass(frozen=True)
class CardInfo:
    present: bool
    card_type: str = ""
    identifier: bytes = b""
    pmm: bytes = b""
    raw_type: int = 0


def _escape_frame_bytes(data: Iterable[int]) -> bytes:
    encoded = bytearray()
    for value in data:
        if value in (SYNC, ESCAPE):
            encoded.extend((ESCAPE, (value - 1) & 0xFF))
        else:
            encoded.append(value)
    return bytes(encoded)


def build_request(
    command: int,
    payload: bytes = b"",
    sequence: int = 0,
    address: int = DEFAULT_ADDRESS,
) -> bytes:
    """Build the request format consumed by Aime_Reader.h::packet_read."""

    if not 0 <= command <= 0xFF:
        raise ValueError("command must be a byte")
    if not 0 <= sequence <= 0xFF:
        raise ValueError("sequence must be a byte")
    if not 0 <= address <= 0xFF:
        raise ValueError("address must be a byte")
    if len(payload) > MAX_FRAME_LENGTH - 5:
        raise ValueError("payload is too long")

    frame_length = 5 + len(payload)
    body = bytes(
        (
            frame_length,
            address,
            sequence,
            command,
            len(payload),
        )
    ) + payload
    checksum = sum(body) & 0xFF
    return bytes((SYNC,)) + _escape_frame_bytes(
        body + bytes((checksum,))
    )


class AimeResponseParser:
    """Streaming parser for escaped Arduino-Aime-Reader response frames."""

    def __init__(self) -> None:
        self.buffer = bytearray()

    def reset(self) -> None:
        self.buffer.clear()

    def feed(self, data: bytes) -> list[AimeResponse]:
        self.buffer.extend(data)
        responses: list[AimeResponse] = []

        while True:
            try:
                sync_index = self.buffer.index(SYNC)
            except ValueError:
                self.buffer.clear()
                return responses

            if sync_index:
                del self.buffer[:sync_index]

            decoded = bytearray()
            raw_index = 1
            expected_decoded_length: int | None = None
            restart = False

            while raw_index < len(self.buffer):
                value = self.buffer[raw_index]
                if value == SYNC:
                    del self.buffer[:raw_index]
                    restart = True
                    break

                if value == ESCAPE:
                    if raw_index + 1 >= len(self.buffer):
                        return responses
                    value = (self.buffer[raw_index + 1] + 1) & 0xFF
                    raw_index += 2
                else:
                    raw_index += 1

                decoded.append(value)
                if len(decoded) == 1:
                    frame_length = decoded[0]
                    if not 6 <= frame_length <= MAX_FRAME_LENGTH:
                        del self.buffer[0]
                        raise AimeProtocolError(
                            f"响应长度无效：{frame_length}"
                        )
                    expected_decoded_length = frame_length + 1

                if (
                    expected_decoded_length is not None
                    and len(decoded) == expected_decoded_length
                ):
                    del self.buffer[:raw_index]
                    responses.append(self._decode(bytes(decoded)))
                    break
            else:
                return responses

            if restart:
                continue

    @staticmethod
    def _decode(decoded: bytes) -> AimeResponse:
        frame_length = decoded[0]
        body = decoded[:-1]
        checksum = decoded[-1]
        if len(body) != frame_length:
            raise AimeProtocolError("响应声明长度与实际长度不一致")
        if (sum(body) & 0xFF) != checksum:
            raise AimeProtocolError("响应校验和错误")
        if frame_length < 6:
            raise AimeProtocolError("响应头不完整")

        payload_length = body[5]
        if frame_length != 6 + payload_length:
            raise AimeProtocolError("响应负载长度无效")
        return AimeResponse(
            frame_length=frame_length,
            address=body[1],
            sequence=body[2],
            command=body[3],
            status=body[4],
            payload=body[6:],
        )


def _status_error(response: AimeResponse) -> AimeProtocolError:
    name = STATUS_NAMES.get(response.status, "未知状态")
    return AimeProtocolError(
        f"命令 0x{response.command:02X} 失败："
        f"{name}（0x{response.status:02X}）"
    )


def parse_card_response(response: AimeResponse) -> CardInfo:
    if response.command != CMD_CARD_DETECT:
        raise AimeProtocolError("收到的不是卡片检测响应")
    if response.status != STATUS_OK:
        raise _status_error(response)
    if not response.payload:
        raise AimeProtocolError("卡片检测响应缺少数量字段")

    count = response.payload[0]
    if count == 0:
        return CardInfo(present=False)
    if len(response.payload) < 3:
        raise AimeProtocolError("卡片检测响应头不完整")

    card_type = response.payload[1]
    identifier_length = response.payload[2]
    card_data = response.payload[3:]

    if card_type == 0x10:
        if not 1 <= identifier_length <= 7:
            raise AimeProtocolError(
                f"MIFARE UID 长度无效：{identifier_length}"
            )
        if len(card_data) < identifier_length:
            raise AimeProtocolError("MIFARE UID 数据不完整")
        return CardInfo(
            present=True,
            card_type="MIFARE",
            identifier=card_data[:identifier_length],
            raw_type=card_type,
        )

    if card_type == 0x20:
        if identifier_length != 0x10 or len(card_data) < 16:
            raise AimeProtocolError("FeliCa IDm/PMm 数据不完整")
        return CardInfo(
            present=True,
            card_type="FeliCa",
            identifier=card_data[:8],
            pmm=card_data[8:16],
            raw_type=card_type,
        )

    if len(card_data) < identifier_length:
        raise AimeProtocolError("未知卡片标识数据不完整")
    return CardInfo(
        present=True,
        card_type=f"未知类型 0x{card_type:02X}",
        identifier=card_data[:identifier_length],
        raw_type=card_type,
    )


class AimeReaderController:
    def __init__(self, port: str, baudrate: int) -> None:
        if baudrate not in BAUDRATES:
            raise ValueError("不支持的波特率")
        try:
            self.serial = serial.Serial(
                port=port,
                baudrate=baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.02,
                write_timeout=0.5,
            )
            # The upstream project notes that some Arduino boards need DTR
            # and RTS asserted before the game-side serial session.
            self.serial.dtr = True
            self.serial.rts = True
        except (serial.SerialException, OSError) as error:
            device = getattr(self, "serial", None)
            if device is not None:
                try:
                    device.close()
                except (serial.SerialException, OSError):
                    pass
            raise AimeProtocolError(
                f"无法打开读卡器串口：{error}"
            ) from error

        self.port = port
        self.baudrate = baudrate
        self.sequence = 0
        self.parser = AimeResponseParser()
        time.sleep(SERIAL_OPEN_DELAY_SECONDS)
        try:
            self.serial.reset_input_buffer()
        except (serial.SerialException, OSError) as error:
            self.close()
            raise AimeProtocolError(
                f"初始化读卡器串口失败：{error}"
            ) from error

    def close(self) -> None:
        try:
            if self.serial.is_open:
                self.serial.close()
        except (serial.SerialException, OSError):
            pass

    def command(
        self,
        command: int,
        payload: bytes = b"",
        timeout: float = SERIAL_TIMEOUT_SECONDS,
    ) -> AimeResponse:
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFF
        frame = build_request(
            command,
            payload,
            sequence=sequence,
        )
        self.parser.reset()

        try:
            self.serial.reset_input_buffer()
            written = self.serial.write(frame)
            self.serial.flush()
        except (serial.SerialException, OSError) as error:
            raise AimeProtocolError(
                f"写入读卡器失败：{error}"
            ) from error
        if written != len(frame):
            raise AimeProtocolError(
                f"串口短写：应发送 {len(frame)} 字节，实际 {written} 字节"
            )

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                waiting = self.serial.in_waiting
                chunk = self.serial.read(waiting if waiting else 1)
            except (serial.SerialException, OSError) as error:
                raise AimeProtocolError(
                    f"读取读卡器失败：{error}"
                ) from error
            if not chunk:
                continue
            for response in self.parser.feed(chunk):
                if (
                    response.command == command
                    and response.sequence == sequence
                ):
                    return response

        raise AimeResponseTimeout(
            f"命令 0x{command:02X} 响应超时"
        )

    def command_ok(
        self,
        command: int,
        payload: bytes = b"",
        timeout: float = SERIAL_TIMEOUT_SECONDS,
    ) -> AimeResponse:
        response = self.command(command, payload, timeout)
        if response.status != STATUS_OK:
            raise _status_error(response)
        return response

    def probe(self) -> tuple[bytes, bytes]:
        firmware = self.command_ok(CMD_GET_FW_VERSION).payload
        hardware = self.command_ok(CMD_GET_HW_VERSION).payload
        if not firmware or not hardware:
            raise AimeProtocolError("版本响应为空")
        return firmware, hardware

    def start_polling(self) -> None:
        self.command_ok(CMD_START_POLLING)

    def stop_polling(self) -> None:
        self.command_ok(CMD_STOP_POLLING)

    def detect_card(self) -> CardInfo:
        response = self.command(
            CMD_CARD_DETECT,
            timeout=CARD_DETECT_TIMEOUT_SECONDS,
        )
        return parse_card_response(response)


if sys.platform == "win32":
    ULONG_PTR = wintypes.WPARAM

    class GUID(ctypes.Structure):
        _fields_ = (
            ("Data1", wintypes.DWORD),
            ("Data2", wintypes.WORD),
            ("Data3", wintypes.WORD),
            ("Data4", wintypes.BYTE * 8),
        )

    class DEVPROPKEY(ctypes.Structure):
        _fields_ = (
            ("fmtid", GUID),
            ("pid", wintypes.DWORD),
        )

    class SP_DEVINFO_DATA(ctypes.Structure):
        _fields_ = (
            ("cbSize", wintypes.DWORD),
            ("ClassGuid", GUID),
            ("DevInst", wintypes.DWORD),
            ("Reserved", ULONG_PTR),
        )

    _CFGMGR32 = ctypes.WinDLL("cfgmgr32", use_last_error=True)
    _SETUPAPI = ctypes.WinDLL("setupapi", use_last_error=True)

    _CFGMGR32.CM_Get_Parent.argtypes = (
        ctypes.POINTER(wintypes.ULONG),
        wintypes.ULONG,
        wintypes.ULONG,
    )
    _CFGMGR32.CM_Get_Parent.restype = wintypes.ULONG
    _CFGMGR32.CM_Get_DevNode_PropertyW.argtypes = (
        wintypes.ULONG,
        ctypes.POINTER(DEVPROPKEY),
        ctypes.POINTER(wintypes.ULONG),
        wintypes.LPVOID,
        ctypes.POINTER(wintypes.ULONG),
        wintypes.ULONG,
    )
    _CFGMGR32.CM_Get_DevNode_PropertyW.restype = wintypes.ULONG
    _SETUPAPI.SetupDiGetClassDevsW.argtypes = (
        ctypes.POINTER(GUID),
        wintypes.LPCWSTR,
        wintypes.HWND,
        wintypes.DWORD,
    )
    _SETUPAPI.SetupDiGetClassDevsW.restype = wintypes.HANDLE
    _SETUPAPI.SetupDiEnumDeviceInfo.argtypes = (
        wintypes.HANDLE,
        wintypes.DWORD,
        ctypes.POINTER(SP_DEVINFO_DATA),
    )
    _SETUPAPI.SetupDiEnumDeviceInfo.restype = wintypes.BOOL
    _SETUPAPI.SetupDiGetDeviceRegistryPropertyW.argtypes = (
        wintypes.HANDLE,
        ctypes.POINTER(SP_DEVINFO_DATA),
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.LPVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    )
    _SETUPAPI.SetupDiGetDeviceRegistryPropertyW.restype = wintypes.BOOL
    _SETUPAPI.SetupDiDestroyDeviceInfoList.argtypes = (wintypes.HANDLE,)
    _SETUPAPI.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL

    _DEVPKEY_BUS_REPORTED_DESC = DEVPROPKEY(
        GUID(
            0x540B947E,
            0x8B40,
            0x45BC,
            (wintypes.BYTE * 8)(
                0xA8,
                0xA2,
                0x6A,
                0x0B,
                0x89,
                0x4C,
                0xBD,
                0xA2,
            ),
        ),
        4,
    )
    _GUID_DEVCLASS_PORTS = GUID(
        0x4D36E978,
        0xE325,
        0x11CE,
        (wintypes.BYTE * 8)(
            0xBF,
            0xC1,
            0x08,
            0x00,
            0x2B,
            0xE1,
            0x03,
            0x18,
        ),
    )


CR_SUCCESS = 0
CR_BUFFER_SMALL = 0x1A
DEVPROP_TYPE_STRING = 0x12
DIGCF_PRESENT = 0x02
SPDRP_FRIENDLYNAME = 0x0C
ERROR_NO_MORE_ITEMS = 259


def _devnode_string_property(devnode: int, key: object) -> str | None:
    if sys.platform != "win32":
        return None
    property_type = wintypes.ULONG(0)
    size = wintypes.ULONG(0)
    result = _CFGMGR32.CM_Get_DevNode_PropertyW(
        devnode,
        ctypes.byref(key),
        ctypes.byref(property_type),
        None,
        ctypes.byref(size),
        0,
    )
    if result not in (CR_SUCCESS, CR_BUFFER_SMALL) or size.value < 2:
        return None
    buffer = ctypes.create_unicode_buffer(
        (size.value // ctypes.sizeof(ctypes.c_wchar)) + 1
    )
    result = _CFGMGR32.CM_Get_DevNode_PropertyW(
        devnode,
        ctypes.byref(key),
        ctypes.byref(property_type),
        buffer,
        ctypes.byref(size),
        0,
    )
    if result != CR_SUCCESS or property_type.value != DEVPROP_TYPE_STRING:
        return None
    return buffer.value.strip() or None


def _bus_description(devnode: int) -> str | None:
    if sys.platform != "win32":
        return None
    current = wintypes.ULONG(devnode)
    for _depth in range(2):
        description = _devnode_string_property(
            current.value,
            _DEVPKEY_BUS_REPORTED_DESC,
        )
        if description:
            return description
        parent = wintypes.ULONG(0)
        if (
            _CFGMGR32.CM_Get_Parent(
                ctypes.byref(parent),
                current.value,
                0,
            )
            != CR_SUCCESS
        ):
            break
        current = parent
    return None


def _registry_friendly_name(
    device_info_set: int,
    device_info: object,
) -> str | None:
    if sys.platform != "win32":
        return None
    buffer = ctypes.create_unicode_buffer(512)
    property_type = wintypes.DWORD(0)
    required = wintypes.DWORD(0)
    success = _SETUPAPI.SetupDiGetDeviceRegistryPropertyW(
        device_info_set,
        ctypes.byref(device_info),
        SPDRP_FRIENDLYNAME,
        ctypes.byref(property_type),
        buffer,
        ctypes.sizeof(buffer),
        ctypes.byref(required),
    )
    return buffer.value.strip() if success else None


def list_serial_bus_descriptions() -> dict[str, str]:
    if sys.platform != "win32":
        return {}
    device_info_set = _SETUPAPI.SetupDiGetClassDevsW(
        ctypes.byref(_GUID_DEVCLASS_PORTS),
        None,
        None,
        DIGCF_PRESENT,
    )
    if not device_info_set or device_info_set == ctypes.c_void_p(-1).value:
        return {}

    descriptions: dict[str, str] = {}
    try:
        index = 0
        while True:
            device_info = SP_DEVINFO_DATA(
                cbSize=ctypes.sizeof(SP_DEVINFO_DATA)
            )
            success = _SETUPAPI.SetupDiEnumDeviceInfo(
                device_info_set,
                index,
                ctypes.byref(device_info),
            )
            if not success:
                if ctypes.get_last_error() == ERROR_NO_MORE_ITEMS:
                    break
                index += 1
                continue
            index += 1

            friendly_name = _registry_friendly_name(
                device_info_set,
                device_info,
            )
            if not friendly_name:
                continue
            ports = re.findall(
                r"\bCOM\d+\b",
                friendly_name,
                re.IGNORECASE,
            )
            description = _bus_description(device_info.DevInst)
            if not description:
                continue
            for port in ports:
                descriptions[port.casefold()] = description
    finally:
        _SETUPAPI.SetupDiDestroyDeviceInfoList(device_info_set)
    return descriptions


def serial_port_label(port: object, bus_description: str | None) -> str:
    device = str(getattr(port, "device", ""))
    description = str(getattr(port, "description", "") or "串口设备")
    reported = bus_description or "无总线报告描述"
    vid = getattr(port, "vid", None)
    pid = getattr(port, "pid", None)
    identity = ""
    if vid is not None and pid is not None:
        identity = f" — VID {vid:04X} / PID {pid:04X}"
    return f"{device} — {reported} — {description}{identity}"


def _hex_bytes(data: bytes) -> str:
    return " ".join(f"{value:02X}" for value in data) if data else "—"


def _version_text(data: bytes) -> str:
    if data and all(0x20 <= value <= 0x7E for value in data):
        return data.decode("ascii")
    return _hex_bytes(data)


class AimeReaderTestTool:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.controller: AimeReaderController | None = None
        self.port_by_label: dict[str, object] = {}
        self.scan_after_id: str | None = None
        self.scanning = False
        self.closing = False

        self.port_var = tk.StringVar()
        self.baudrate_var = tk.StringVar(value="115200")
        self.connection_var = tk.StringVar(value="● 未连接")
        self.firmware_var = tk.StringVar(value="—")
        self.hardware_var = tk.StringVar(value="—")
        self.card_status_var = tk.StringVar(value="请连接读卡器")
        self.card_type_var = tk.StringVar(value="—")
        self.identifier_var = tk.StringVar(value="—")
        self.pmm_var = tk.StringVar(value="—")

        self._build_ui()
        self.refresh_ports()
        self._set_connected(False)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _build_ui(self) -> None:
        self.root.title("Aime 读卡器测试工具")
        self.root.resizable(False, False)
        self.root.option_add("*Font", ("Microsoft YaHei UI", 10))

        outer = ttk.Frame(self.root, padding=14)
        outer.grid(row=0, column=0, sticky="nsew")

        connection = ttk.LabelFrame(outer, text="读卡器连接", padding=10)
        connection.grid(row=0, column=0, sticky="ew")
        connection.columnconfigure(1, weight=1)

        ttk.Label(connection, text="串口").grid(
            row=0, column=0, padx=(0, 8), sticky="e"
        )
        self.port_combo = ttk.Combobox(
            connection,
            textvariable=self.port_var,
            state="readonly",
            width=64,
        )
        self.port_combo.grid(
            row=0,
            column=1,
            columnspan=2,
            padx=(0, 8),
            sticky="ew",
        )
        self.refresh_button = ttk.Button(
            connection,
            text="刷新",
            width=8,
            command=self.refresh_ports,
        )
        self.refresh_button.grid(row=0, column=3)

        ttk.Label(connection, text="波特率").grid(
            row=1, column=0, padx=(0, 8), pady=(8, 0), sticky="e"
        )
        self.baudrate_combo = ttk.Combobox(
            connection,
            textvariable=self.baudrate_var,
            values=[str(value) for value in BAUDRATES],
            state="readonly",
            width=14,
        )
        self.baudrate_combo.grid(
            row=1, column=1, pady=(8, 0), sticky="w"
        )
        self.connect_button = ttk.Button(
            connection,
            text="连接",
            width=10,
            command=self.toggle_connection,
        )
        self.connect_button.grid(
            row=1, column=2, padx=(8, 8), pady=(8, 0)
        )
        self.connection_label = tk.Label(
            connection,
            textvariable=self.connection_var,
            foreground="#C62828",
            background=self.root.cget("background"),
            anchor="w",
            width=18,
        )
        self.connection_label.grid(
            row=1, column=3, pady=(8, 0), sticky="w"
        )

        versions = ttk.LabelFrame(outer, text="设备信息", padding=10)
        versions.grid(row=1, column=0, pady=(12, 0), sticky="ew")
        versions.columnconfigure(1, weight=1)
        ttk.Label(versions, text="固件版本").grid(
            row=0, column=0, padx=(0, 10), sticky="e"
        )
        ttk.Label(
            versions,
            textvariable=self.firmware_var,
            foreground="#333333",
        ).grid(row=0, column=1, sticky="w")
        ttk.Label(versions, text="硬件版本").grid(
            row=1, column=0, padx=(0, 10), pady=(7, 0), sticky="e"
        )
        ttk.Label(
            versions,
            textvariable=self.hardware_var,
            foreground="#333333",
        ).grid(row=1, column=1, pady=(7, 0), sticky="w")

        card = ttk.LabelFrame(outer, text="读卡测试", padding=14)
        card.grid(row=2, column=0, pady=(12, 0), sticky="ew")
        card.columnconfigure(1, weight=1)

        self.card_status_label = tk.Label(
            card,
            textvariable=self.card_status_var,
            font=("Microsoft YaHei UI", 18, "bold"),
            foreground="#555555",
            background=self.root.cget("background"),
            anchor="center",
            pady=16,
        )
        self.card_status_label.grid(
            row=0, column=0, columnspan=2, sticky="ew"
        )

        for row, (caption, variable) in enumerate(
            (
                ("卡片类型", self.card_type_var),
                ("UID / IDm", self.identifier_var),
                ("PMm", self.pmm_var),
            ),
            start=1,
        ):
            ttk.Label(card, text=caption).grid(
                row=row,
                column=0,
                padx=(0, 10),
                pady=5,
                sticky="e",
            )
            entry = ttk.Entry(
                card,
                textvariable=variable,
                state="readonly",
                width=55,
            )
            entry.grid(row=row, column=1, pady=5, sticky="ew")

        controls = ttk.Frame(card)
        controls.grid(
            row=4,
            column=0,
            columnspan=2,
            pady=(15, 0),
        )
        self.start_button = ttk.Button(
            controls,
            text="开始读卡",
            width=14,
            command=self.start_scanning,
        )
        self.start_button.grid(row=0, column=0, padx=5)
        self.stop_button = ttk.Button(
            controls,
            text="停止读卡",
            width=14,
            command=self.stop_scanning,
        )
        self.stop_button.grid(row=0, column=1, padx=5)
        self.clear_button = ttk.Button(
            controls,
            text="清除结果",
            width=14,
            command=self.clear_result,
        )
        self.clear_button.grid(row=0, column=2, padx=5)

        ttk.Label(
            outer,
            text=(
                "只读取卡片类型与标识，不执行写卡；"
                "115200 对应 837-15396，38400 对应 TN32MSEC003S。"
            ),
            foreground="#666666",
        ).grid(row=3, column=0, pady=(10, 0))

    def refresh_ports(self) -> None:
        current = self.port_by_label.get(self.port_var.get())
        current_device = getattr(current, "device", None)
        ports = sorted(
            list(list_ports.comports()),
            key=lambda port: port.device.casefold(),
        )
        bus_descriptions = list_serial_bus_descriptions()

        labels: list[str] = []
        self.port_by_label.clear()
        selected_label: str | None = None
        for port in ports:
            label = serial_port_label(
                port,
                bus_descriptions.get(port.device.casefold()),
            )
            labels.append(label)
            self.port_by_label[label] = port
            if port.device == current_device:
                selected_label = label

        self.port_combo.configure(values=["", *labels])
        self.port_var.set(selected_label or "")

    def _set_connected(self, connected: bool) -> None:
        if connected:
            self.connection_var.set("● 已连接")
            self.connection_label.configure(foreground="#2E7D32")
            self.connect_button.configure(text="断开")
            self.port_combo.configure(state="disabled")
            self.baudrate_combo.configure(state="disabled")
            self.refresh_button.configure(state="disabled")
            self.clear_button.configure(state="normal")
        else:
            self.connection_var.set("● 未连接")
            self.connection_label.configure(foreground="#C62828")
            self.connect_button.configure(text="连接")
            self.port_combo.configure(state="readonly")
            self.baudrate_combo.configure(state="readonly")
            self.refresh_button.configure(state="normal")
            self.clear_button.configure(state="disabled")
        self._update_scan_buttons()

    def _update_scan_buttons(self) -> None:
        connected = self.controller is not None
        self.start_button.configure(
            state="normal" if connected and not self.scanning else "disabled"
        )
        self.stop_button.configure(
            state="normal" if connected and self.scanning else "disabled"
        )

    def toggle_connection(self) -> None:
        if self.controller is None:
            self.connect()
        else:
            self.disconnect()

    def connect(self) -> None:
        selected = self.port_by_label.get(self.port_var.get())
        port = getattr(selected, "device", None)
        if not port:
            messagebox.showwarning(
                "未选择串口",
                "请刷新并选择读卡器串口。",
            )
            return
        try:
            baudrate = int(self.baudrate_var.get(), 10)
        except ValueError:
            messagebox.showwarning("波特率无效", "请选择有效波特率。")
            return

        controller: AimeReaderController | None = None
        try:
            controller = AimeReaderController(port, baudrate)
            firmware, hardware = controller.probe()
        except (AimeProtocolError, ValueError) as error:
            if controller is not None:
                controller.close()
            messagebox.showerror("连接失败", str(error))
            return

        self.controller = controller
        self.firmware_var.set(_version_text(firmware))
        self.hardware_var.set(_version_text(hardware))
        self._set_connected(True)
        self.start_scanning()

    def _cancel_scan_timer(self) -> None:
        if self.scan_after_id is not None:
            try:
                self.root.after_cancel(self.scan_after_id)
            except tk.TclError:
                pass
            self.scan_after_id = None

    def _schedule_scan(self) -> None:
        self._cancel_scan_timer()
        if self.controller is not None and self.scanning:
            self.scan_after_id = self.root.after(
                SCAN_INTERVAL_MS,
                self._scan_once,
            )

    def start_scanning(self) -> None:
        controller = self.controller
        if controller is None or self.scanning:
            return
        try:
            controller.start_polling()
        except AimeProtocolError as error:
            self._communication_failed(error)
            return
        self.scanning = True
        self.card_status_var.set("等待刷卡…")
        self.card_status_label.configure(foreground="#1565C0")
        self._update_scan_buttons()
        self._schedule_scan()

    def stop_scanning(self) -> None:
        self._stop_scanning_internal(send_command=True)

    def _stop_scanning_internal(self, send_command: bool) -> None:
        self._cancel_scan_timer()
        was_scanning = self.scanning
        self.scanning = False
        controller = self.controller
        if send_command and was_scanning and controller is not None:
            try:
                controller.stop_polling()
            except AimeProtocolError as error:
                self._communication_failed(error)
                return
        if controller is not None:
            self.card_status_var.set("读卡已停止")
            self.card_status_label.configure(foreground="#555555")
        self._update_scan_buttons()

    def _scan_once(self) -> None:
        self.scan_after_id = None
        controller = self.controller
        if controller is None or not self.scanning:
            return
        try:
            card = controller.detect_card()
        except AimeProtocolError as error:
            self._communication_failed(error)
            return

        if card.present:
            self.card_status_var.set(f"读取成功：{card.card_type}")
            self.card_status_label.configure(foreground="#2E7D32")
            self.card_type_var.set(card.card_type)
            self.identifier_var.set(_hex_bytes(card.identifier))
            self.pmm_var.set(_hex_bytes(card.pmm))
        else:
            self.card_status_var.set("等待刷卡…")
            self.card_status_label.configure(foreground="#1565C0")
        self._schedule_scan()

    def clear_result(self) -> None:
        self.card_type_var.set("—")
        self.identifier_var.set("—")
        self.pmm_var.set("—")
        if self.scanning:
            self.card_status_var.set("等待刷卡…")
            self.card_status_label.configure(foreground="#1565C0")
        elif self.controller is not None:
            self.card_status_var.set("读卡已停止")
            self.card_status_label.configure(foreground="#555555")

    def _communication_failed(self, error: Exception) -> None:
        self._disconnect_internal(refresh=False)
        if not self.closing:
            messagebox.showerror("通信失败", str(error))

    def _disconnect_internal(self, refresh: bool) -> None:
        self._cancel_scan_timer()
        controller = self.controller
        self.controller = None
        was_scanning = self.scanning
        self.scanning = False
        if controller is not None:
            if was_scanning:
                try:
                    controller.stop_polling()
                except AimeProtocolError:
                    pass
            controller.close()

        self.firmware_var.set("—")
        self.hardware_var.set("—")
        self.card_status_var.set("请连接读卡器")
        self.card_status_label.configure(foreground="#555555")
        self._set_connected(False)
        if refresh:
            self.refresh_ports()

    def disconnect(self) -> None:
        self._disconnect_internal(refresh=True)

    def close(self) -> None:
        self.closing = True
        self._disconnect_internal(refresh=False)
        self.root.destroy()


def _build_test_response(
    command: int,
    payload: bytes,
    sequence: int = 0,
    status: int = STATUS_OK,
) -> bytes:
    frame_length = 6 + len(payload)
    body = bytes(
        (
            frame_length,
            DEFAULT_ADDRESS,
            sequence,
            command,
            status,
            len(payload),
        )
    ) + payload
    checksum = sum(body) & 0xFF
    return bytes((SYNC,)) + _escape_frame_bytes(
        body + bytes((checksum,))
    )


def self_test() -> None:
    assert build_request(
        CMD_GET_FW_VERSION,
        sequence=9,
    ) == bytes.fromhex("E0 05 00 09 30 00 3E")
    assert build_request(
        0x50,
        bytes((0xD0, 0xE0)),
        sequence=1,
    ) == bytes.fromhex("E0 07 00 01 50 02 D0 CF D0 DF 0A")

    parser = AimeResponseParser()
    firmware_frame = _build_test_response(
        CMD_GET_FW_VERSION,
        b"TN32MSEC003S F/W Ver1.2",
        sequence=9,
    )
    assert parser.feed(firmware_frame[:5]) == []
    responses = parser.feed(firmware_frame[5:])
    assert len(responses) == 1
    assert responses[0].sequence == 9
    assert responses[0].payload == b"TN32MSEC003S F/W Ver1.2"
    escaped_response = _build_test_response(
        CMD_GET_FW_VERSION,
        bytes((0x94, 0xE0, 0xD0)),
        sequence=10,
    )
    escaped_parsed = parser.feed(escaped_response)
    assert escaped_parsed[0].payload == bytes((0x94, 0xE0, 0xD0))

    mifare_payload = bytes((1, 0x10, 4, 0x04, 0xA1, 0xB2, 0xC3))
    mifare_response = AimeResponse(
        frame_length=6 + len(mifare_payload),
        address=0,
        sequence=1,
        command=CMD_CARD_DETECT,
        status=STATUS_OK,
        payload=mifare_payload,
    )
    mifare = parse_card_response(mifare_response)
    assert mifare.card_type == "MIFARE"
    assert mifare.identifier == bytes.fromhex("04 A1 B2 C3")
    assert not mifare.pmm

    felica_data = bytes.fromhex(
        "01 20 10 "
        "01 02 03 04 05 06 07 08 "
        "11 12 13 14 15 16 17 18"
    )
    felica = parse_card_response(
        AimeResponse(
            frame_length=6 + len(felica_data),
            address=0,
            sequence=2,
            command=CMD_CARD_DETECT,
            status=STATUS_OK,
            payload=felica_data,
        )
    )
    assert felica.card_type == "FeliCa"
    assert felica.identifier == bytes.fromhex(
        "01 02 03 04 05 06 07 08"
    )
    assert felica.pmm == bytes.fromhex(
        "11 12 13 14 15 16 17 18"
    )

    no_card = parse_card_response(
        AimeResponse(
            frame_length=7,
            address=0,
            sequence=3,
            command=CMD_CARD_DETECT,
            status=STATUS_OK,
            payload=b"\x00",
        )
    )
    assert not no_card.present
    print("aime_reader_test_tool self-test: OK")


def ui_smoke_test() -> None:
    root = tk.Tk()
    root.withdraw()
    app = AimeReaderTestTool(root)
    root.update_idletasks()
    assert app.controller is None
    assert not app.scanning
    assert app.port_var.get() == ""
    assert app.baudrate_var.get() == "115200"
    assert app.card_type_var.get() == "—"
    root.destroy()
    print("aime_reader_test_tool UI smoke test: OK")


def main() -> int:
    if "--self-test" in sys.argv:
        self_test()
        return 0
    if "--ui-smoke-test" in sys.argv:
        ui_smoke_test()
        return 0

    root = tk.Tk()
    AimeReaderTestTool(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

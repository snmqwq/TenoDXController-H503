#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Combined LED and main-button test tool for the STM32F072 controller."""

from __future__ import annotations

import ctypes
import math
import os
import queue
import re
import sys
import threading
import time
import tkinter as tk
from ctypes import wintypes
from pathlib import Path
from tkinter import messagebox, ttk
from typing import Callable, Iterable

import serial
from PIL import Image, ImageDraw, ImageFont, ImageTk
from serial.tools import list_ports

from aime_reader_test_tool import (
    BAUDRATES as AIME_BAUDRATES,
    SCAN_INTERVAL_MS as AIME_SCAN_INTERVAL_MS,
    AimeProtocolError,
    AimeReaderController,
    CardInfo,
    _hex_bytes as _aime_hex_bytes,
    _version_text as _aime_version_text,
)
from light_test_tool import (
    BLACK,
    CHASE_HOLD_MS,
    FADE_DURATION_MS,
    FADE_HOLD_MS,
    FADE_VISUAL_STEPS,
    LOGICAL_LIGHT_COUNT,
    RGBW_HOLD_MS,
    Color,
    LightController,
    ProtocolError,
    _validate_color,
)
from touch_test_tool import (
    DEVICE_START_DELAY_SECONDS,
    POLL_INTERVAL_MS as TOUCH_POLL_INTERVAL_MS,
    RSET_COMMAND,
    SERIAL_BAUDRATE as TOUCH_SERIAL_BAUDRATE,
    STAT_COMMAND,
    TOUCH_TIMEOUT_SECONDS,
    VALID_TOUCH_BITS,
    VALID_TOUCH_MASK,
    SensorRenderer,
    TouchFrameParser,
    _encode_test_frame,
)


BUTTON_COUNT = 8
BUTTON_CANVAS_SIZE = 440
TOUCH_DISPLAY_SIZE = 560
RAW_INPUT_POLL_MS = 12

# Main-button mappings taken from Core/Src/app_config.c.  Scan codes are used
# so the 2P keypad mapping is independent of the host's Num Lock state.
BUTTON_SCANCODES: dict[str, tuple[int, ...]] = {
    "1P": (
        0x11,  # W
        0x12,  # E
        0x20,  # D
        0x2E,  # C
        0x2D,  # X
        0x2C,  # Z
        0x1E,  # A
        0x10,  # Q
    ),
    "2P": (
        0x48,  # Keypad 8
        0x49,  # Keypad 9
        0x4D,  # Keypad 6
        0x51,  # Keypad 3
        0x50,  # Keypad 2
        0x4F,  # Keypad 1
        0x4B,  # Keypad 4
        0x47,  # Keypad 7
    ),
}

BUTTON_MAPPING_TEXT = {
    "1P": "BTN1–8：W / E / D / C / X / Z / A / Q",
    "2P": "BTN1–8：小键盘 8 / 9 / 6 / 3 / 2 / 1 / 4 / 7",
}

TestAction = Callable[[int], None]


def resource_path(*parts: str) -> Path:
    """Return a path that works in source and PyInstaller one-file builds."""

    bundle_root = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    return bundle_root.joinpath(*parts)


def _program_background_rgb(root: tk.Tk) -> tuple[int, int, int]:
    red, green, blue = root.winfo_rgb(root.cget("background"))
    return red // 257, green // 257, blue // 257


class ButtonRingRenderer:
    """Compose eight rotated button assets onto the program background."""

    def __init__(
        self,
        asset_directory: Path,
        background: tuple[int, int, int],
        size: int = BUTTON_CANVAS_SIZE,
    ) -> None:
        self.size = size
        self.background = background

        off_source = Image.open(asset_directory / "button_off.png").convert(
            "RGBA"
        )
        on_source = Image.open(asset_directory / "button_on.png").convert(
            "RGBA"
        )
        if off_source.size != on_source.size:
            raise ValueError("button_on.png 和 button_off.png 的尺寸必须一致")
        if off_source.width != off_source.height:
            raise ValueError("按键素材必须是正方形")

        self.off_layers = self._make_layers(off_source)
        self.on_layers = self._make_layers(on_source)
        self.base = Image.new("RGBA", (size, size), (*background, 255))
        for layer in self.off_layers:
            self.base.alpha_composite(layer)

        self.label_font = self._load_font(max(12, round(size * 0.029)))

    def _make_layers(self, source: Image.Image) -> list[Image.Image]:
        layers: list[Image.Image] = []
        for index in range(BUTTON_COUNT):
            rotated = source.rotate(
                -45 * index,
                resample=Image.Resampling.BICUBIC,
                expand=False,
            )
            layers.append(
                rotated.resize(
                    (self.size, self.size),
                    Image.Resampling.LANCZOS,
                )
            )
        return layers

    @staticmethod
    def _load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
        for name in ("segoeui.ttf", "arial.ttf"):
            try:
                return ImageFont.truetype(name, size)
            except OSError:
                continue
        return ImageFont.load_default()

    def render(self, pressed_mask: int) -> Image.Image:
        image = self.base.copy()
        for index, layer in enumerate(self.on_layers):
            if pressed_mask & (1 << index):
                image.alpha_composite(layer)

        draw = ImageDraw.Draw(image)
        center = self.size / 2
        radius = self.size * 0.365
        label_fill = (85, 85, 85, 255)
        for index in range(BUTTON_COUNT):
            # BTN1 is the upper-right asset supplied by the user.  The rest
            # proceed clockwise in 45-degree increments.
            angle = math.radians(-67.5 + (45 * index))
            x = center + (radius * math.cos(angle))
            y = center + (radius * math.sin(angle))
            text = f"BTN{index + 1}"
            box = draw.textbbox((0, 0), text, font=self.label_font)
            draw.text(
                (x - ((box[2] - box[0]) / 2), y - ((box[3] - box[1]) / 2)),
                text,
                fill=label_fill,
                font=self.label_font,
            )
        return image.convert("RGB")


if sys.platform == "win32":
    ULONG_PTR = wintypes.WPARAM
    LRESULT = ctypes.c_ssize_t
    WNDPROC = ctypes.WINFUNCTYPE(
        LRESULT,
        wintypes.HWND,
        wintypes.UINT,
        wintypes.WPARAM,
        wintypes.LPARAM,
    )

    class RAWINPUTDEVICE(ctypes.Structure):
        _fields_ = (
            ("usUsagePage", wintypes.USHORT),
            ("usUsage", wintypes.USHORT),
            ("dwFlags", wintypes.DWORD),
            ("hwndTarget", wintypes.HWND),
        )

    class RAWINPUTDEVICELIST(ctypes.Structure):
        _fields_ = (
            ("hDevice", wintypes.HANDLE),
            ("dwType", wintypes.DWORD),
        )

    class RAWINPUTHEADER(ctypes.Structure):
        _fields_ = (
            ("dwType", wintypes.DWORD),
            ("dwSize", wintypes.DWORD),
            ("hDevice", wintypes.HANDLE),
            ("wParam", ULONG_PTR),
        )

    class RAWKEYBOARD(ctypes.Structure):
        _fields_ = (
            ("MakeCode", wintypes.USHORT),
            ("Flags", wintypes.USHORT),
            ("Reserved", wintypes.USHORT),
            ("VKey", wintypes.USHORT),
            ("Message", wintypes.UINT),
            ("ExtraInformation", wintypes.ULONG),
        )

    class RAWINPUTUNION(ctypes.Union):
        _fields_ = (("keyboard", RAWKEYBOARD),)

    class RAWINPUT(ctypes.Structure):
        _anonymous_ = ("data",)
        _fields_ = (
            ("header", RAWINPUTHEADER),
            ("data", RAWINPUTUNION),
        )

    class WNDCLASSEXW(ctypes.Structure):
        _fields_ = (
            ("cbSize", wintypes.UINT),
            ("style", wintypes.UINT),
            ("lpfnWndProc", WNDPROC),
            ("cbClsExtra", ctypes.c_int),
            ("cbWndExtra", ctypes.c_int),
            ("hInstance", wintypes.HINSTANCE),
            ("hIcon", wintypes.HANDLE),
            ("hCursor", wintypes.HANDLE),
            ("hbrBackground", wintypes.HANDLE),
            ("lpszMenuName", wintypes.LPCWSTR),
            ("lpszClassName", wintypes.LPCWSTR),
            ("hIconSm", wintypes.HANDLE),
        )

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

    _USER32 = ctypes.WinDLL("user32", use_last_error=True)
    _KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _CFGMGR32 = ctypes.WinDLL("cfgmgr32", use_last_error=True)
    _SETUPAPI = ctypes.WinDLL("setupapi", use_last_error=True)

    _USER32.GetRawInputDeviceList.argtypes = (
        ctypes.POINTER(RAWINPUTDEVICELIST),
        ctypes.POINTER(wintypes.UINT),
        wintypes.UINT,
    )
    _USER32.GetRawInputDeviceList.restype = wintypes.UINT
    _USER32.GetRawInputDeviceInfoW.argtypes = (
        wintypes.HANDLE,
        wintypes.UINT,
        wintypes.LPVOID,
        ctypes.POINTER(wintypes.UINT),
    )
    _USER32.GetRawInputDeviceInfoW.restype = wintypes.UINT
    _USER32.GetRawInputData.argtypes = (
        wintypes.HANDLE,
        wintypes.UINT,
        wintypes.LPVOID,
        ctypes.POINTER(wintypes.UINT),
        wintypes.UINT,
    )
    _USER32.GetRawInputData.restype = wintypes.UINT
    _USER32.RegisterRawInputDevices.argtypes = (
        ctypes.POINTER(RAWINPUTDEVICE),
        wintypes.UINT,
        wintypes.UINT,
    )
    _USER32.RegisterRawInputDevices.restype = wintypes.BOOL
    _USER32.RegisterClassExW.argtypes = (ctypes.POINTER(WNDCLASSEXW),)
    _USER32.RegisterClassExW.restype = wintypes.ATOM
    _USER32.CreateWindowExW.argtypes = (
        wintypes.DWORD,
        wintypes.LPCWSTR,
        wintypes.LPCWSTR,
        wintypes.DWORD,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.HWND,
        wintypes.HMENU,
        wintypes.HINSTANCE,
        wintypes.LPVOID,
    )
    _USER32.CreateWindowExW.restype = wintypes.HWND
    _USER32.DefWindowProcW.argtypes = (
        wintypes.HWND,
        wintypes.UINT,
        wintypes.WPARAM,
        wintypes.LPARAM,
    )
    _USER32.DefWindowProcW.restype = LRESULT
    _USER32.PostMessageW.argtypes = (
        wintypes.HWND,
        wintypes.UINT,
        wintypes.WPARAM,
        wintypes.LPARAM,
    )
    _USER32.PostMessageW.restype = wintypes.BOOL
    _USER32.DestroyWindow.argtypes = (wintypes.HWND,)
    _USER32.DestroyWindow.restype = wintypes.BOOL
    _USER32.GetMessageW.argtypes = (
        ctypes.POINTER(wintypes.MSG),
        wintypes.HWND,
        wintypes.UINT,
        wintypes.UINT,
    )
    _USER32.GetMessageW.restype = wintypes.BOOL
    _USER32.TranslateMessage.argtypes = (ctypes.POINTER(wintypes.MSG),)
    _USER32.TranslateMessage.restype = wintypes.BOOL
    _USER32.DispatchMessageW.argtypes = (ctypes.POINTER(wintypes.MSG),)
    _USER32.DispatchMessageW.restype = LRESULT
    _KERNEL32.GetModuleHandleW.argtypes = (wintypes.LPCWSTR,)
    _KERNEL32.GetModuleHandleW.restype = wintypes.HINSTANCE
    _CFGMGR32.CM_Locate_DevNodeW.argtypes = (
        ctypes.POINTER(wintypes.ULONG),
        wintypes.LPWSTR,
        wintypes.ULONG,
    )
    _CFGMGR32.CM_Locate_DevNodeW.restype = wintypes.ULONG
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


RIM_TYPEKEYBOARD = 1
RIDI_DEVICENAME = 0x20000007
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100
RIDEV_DEVNOTIFY = 0x00002000
WM_INPUT = 0x00FF
WM_INPUT_DEVICE_CHANGE = 0x00FE
WM_CLOSE = 0x0010
WM_DESTROY = 0x0002
RI_KEY_BREAK = 0x0001
RI_KEY_E0 = 0x0002
UINT_ERROR = 0xFFFFFFFF
CR_SUCCESS = 0x00000000
CR_BUFFER_SMALL = 0x0000001A
DEVPROP_TYPE_STRING = 0x00000012
DIGCF_PRESENT = 0x00000002
SPDRP_FRIENDLYNAME = 0x0000000C
ERROR_NO_MORE_ITEMS = 259

if sys.platform == "win32":
    _DEVPKEY_DEVICE_BUS_REPORTED_DESC = DEVPROPKEY(
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


def _raw_device_name(device: int) -> str:
    if sys.platform != "win32":
        return ""
    length = wintypes.UINT(0)
    result = _USER32.GetRawInputDeviceInfoW(
        device, RIDI_DEVICENAME, None, ctypes.byref(length)
    )
    if result == UINT_ERROR or length.value == 0:
        return ""
    buffer = ctypes.create_unicode_buffer(length.value + 1)
    result = _USER32.GetRawInputDeviceInfoW(
        device,
        RIDI_DEVICENAME,
        buffer,
        ctypes.byref(length),
    )
    if result == UINT_ERROR:
        return ""
    return buffer.value


def list_raw_keyboard_names() -> list[str]:
    """Enumerate Windows Raw Input keyboard device paths."""

    if sys.platform != "win32":
        return []
    count = wintypes.UINT(0)
    result = _USER32.GetRawInputDeviceList(
        None,
        ctypes.byref(count),
        ctypes.sizeof(RAWINPUTDEVICELIST),
    )
    if result == UINT_ERROR or count.value == 0:
        return []

    devices = (RAWINPUTDEVICELIST * count.value)()
    result = _USER32.GetRawInputDeviceList(
        devices,
        ctypes.byref(count),
        ctypes.sizeof(RAWINPUTDEVICELIST),
    )
    if result == UINT_ERROR:
        return []

    names: list[str] = []
    for item in devices[: result]:
        if item.dwType != RIM_TYPEKEYBOARD:
            continue
        name = _raw_device_name(item.hDevice)
        if name and name not in names:
            names.append(name)
    return names


def _raw_path_to_instance_id(name: str) -> str | None:
    path = name
    if path.startswith("\\\\?\\"):
        path = path[4:]
    segments = path.split("#")
    if len(segments) < 3:
        return None
    return "\\".join(segments[:3])


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

    character_count = (size.value // ctypes.sizeof(ctypes.c_wchar)) + 1
    buffer = ctypes.create_unicode_buffer(character_count)
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
    value = buffer.value.strip()
    return value or None


def _bus_description_from_devnode(
    devnode: int,
    search_depth: int,
) -> str | None:
    if sys.platform != "win32":
        return None
    current = wintypes.ULONG(devnode)
    for _depth in range(search_depth):
        description = _devnode_string_property(
            current.value,
            _DEVPKEY_DEVICE_BUS_REPORTED_DESC,
        )
        if description:
            return description
        parent = wintypes.ULONG(0)
        result = _CFGMGR32.CM_Get_Parent(
            ctypes.byref(parent),
            current.value,
            0,
        )
        if result != CR_SUCCESS:
            break
        current = parent
    return None


def bus_reported_device_description(name: str) -> str | None:
    """Return the first bus-reported description in the device ancestry."""

    if sys.platform != "win32":
        return None
    instance_id = _raw_path_to_instance_id(name)
    if not instance_id:
        return None

    devnode = wintypes.ULONG(0)
    result = _CFGMGR32.CM_Locate_DevNodeW(
        ctypes.byref(devnode),
        instance_id,
        0,
    )
    if result != CR_SUCCESS:
        return None

    # HID keyboard class nodes usually have no value themselves.  Their
    # immediate USB/Bluetooth/virtual-HID parent carries the descriptor
    # reported by the bus.  Do not walk farther into generic host bridges.
    search_depth = 2 if instance_id.upper().startswith("HID\\") else 1
    return _bus_description_from_devnode(devnode.value, search_depth)


def _setupapi_registry_string(
    device_info_set: int,
    device_info: object,
    property_code: int,
) -> str | None:
    if sys.platform != "win32":
        return None
    buffer = ctypes.create_unicode_buffer(512)
    property_type = wintypes.DWORD(0)
    required = wintypes.DWORD(0)
    success = _SETUPAPI.SetupDiGetDeviceRegistryPropertyW(
        device_info_set,
        ctypes.byref(device_info),
        property_code,
        ctypes.byref(property_type),
        buffer,
        ctypes.sizeof(buffer),
        ctypes.byref(required),
    )
    if not success:
        return None
    value = buffer.value.strip()
    return value or None


def list_serial_bus_descriptions() -> dict[str, str]:
    """Map COM port names to their bus-reported device descriptions."""

    if sys.platform != "win32":
        return {}
    device_info_set = _SETUPAPI.SetupDiGetClassDevsW(
        ctypes.byref(_GUID_DEVCLASS_PORTS),
        None,
        None,
        DIGCF_PRESENT,
    )
    invalid_handle = ctypes.c_void_p(-1).value
    if not device_info_set or device_info_set == invalid_handle:
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

            friendly_name = _setupapi_registry_string(
                device_info_set,
                device_info,
                SPDRP_FRIENDLYNAME,
            )
            if not friendly_name:
                continue
            ports = re.findall(r"\bCOM\d+\b", friendly_name, re.IGNORECASE)
            if not ports:
                continue
            description = _bus_description_from_devnode(
                device_info.DevInst,
                2,
            )
            if not description:
                continue
            for port in ports:
                descriptions[port.casefold()] = description
    finally:
        _SETUPAPI.SetupDiDestroyDeviceInfoList(device_info_set)
    return descriptions


def serial_port_label(
    device: str,
    description: str,
    vid: int | None,
    pid: int | None,
    bus_description: str | None,
) -> str:
    reported = bus_description or "无总线报告描述"
    identity = ""
    if vid is not None and pid is not None:
        identity = f" — VID {vid:04X} / PID {pid:04X}"
    return f"{device} — {reported} — {description}{identity}"


def keyboard_device_label(
    name: str,
    index: int,
    bus_description: str | None,
) -> str:
    """Build a compact, stable label for a Raw Input keyboard path."""

    upper = name.upper()
    vid_pid = re.search(r"VID_([0-9A-F]{4}).*?PID_([0-9A-F]{4})", upper)
    if vid_pid:
        identity = f"VID {vid_pid.group(1)} / PID {vid_pid.group(2)}"
    else:
        segments = [segment for segment in name.split("#") if segment]
        identity = segments[0].replace("\\\\?\\", "") if segments else name

    segments = name.split("#")
    instance = segments[2] if len(segments) > 2 else name
    if len(instance) > 28:
        instance = f"{instance[:25]}..."
    description = bus_description or "无总线报告描述"
    return f"键盘 {index} — {description} — {identity} — {instance}"


class RawKeyboardMonitor:
    """Receive device-specific keyboard events through Windows Raw Input."""

    def __init__(self) -> None:
        self.events: queue.SimpleQueue[tuple[str, int, bool]] = (
            queue.SimpleQueue()
        )
        self._target_names: set[str] = set()
        self._target_lock = threading.Lock()
        self._device_name_cache: dict[int, str] = {}
        self._ready = threading.Event()
        self._error: str | None = None
        self._window: int | None = None
        self._thread = threading.Thread(
            target=self._thread_main,
            name="Mai2ButtonRawInput",
            daemon=True,
        )

        if sys.platform != "win32":
            self._error = "按键直读目前仅支持 Windows"
            self._ready.set()
            return

        self._thread.start()
        if not self._ready.wait(2.0):
            self._error = "初始化按键输入监听超时"

    @property
    def error(self) -> str | None:
        return self._error

    def set_targets(self, names: set[str]) -> None:
        with self._target_lock:
            self._target_names = {name.casefold() for name in names}
        self.clear_events()

    def clear_targets(self) -> None:
        with self._target_lock:
            self._target_names.clear()
        self.clear_events()

    def clear_events(self) -> None:
        while True:
            try:
                self.events.get_nowait()
            except queue.Empty:
                return

    def target_is_present(self) -> bool:
        current = {name.casefold() for name in list_raw_keyboard_names()}
        with self._target_lock:
            return bool(self._target_names) and self._target_names <= current

    def close(self) -> None:
        window = self._window
        if sys.platform == "win32" and window:
            _USER32.PostMessageW(window, WM_CLOSE, 0, 0)
        if self._thread.is_alive():
            self._thread.join(timeout=1.0)

    def _thread_main(self) -> None:
        try:
            instance = _KERNEL32.GetModuleHandleW(None)
            class_name = (
                f"Mai2ButtonRawInput_{os.getpid()}_{id(self):x}"
            )
            self._wndproc = WNDPROC(self._window_proc)
            window_class = WNDCLASSEXW(
                cbSize=ctypes.sizeof(WNDCLASSEXW),
                style=0,
                lpfnWndProc=self._wndproc,
                cbClsExtra=0,
                cbWndExtra=0,
                hInstance=instance,
                hIcon=None,
                hCursor=None,
                hbrBackground=None,
                lpszMenuName=None,
                lpszClassName=class_name,
                hIconSm=None,
            )
            if not _USER32.RegisterClassExW(ctypes.byref(window_class)):
                raise ctypes.WinError(ctypes.get_last_error())

            window = _USER32.CreateWindowExW(
                0,
                class_name,
                class_name,
                0,
                0,
                0,
                0,
                0,
                None,
                None,
                instance,
                None,
            )
            if not window:
                raise ctypes.WinError(ctypes.get_last_error())
            self._window = window

            registration = RAWINPUTDEVICE(
                usUsagePage=0x01,
                usUsage=0x06,
                dwFlags=RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                hwndTarget=window,
            )
            if not _USER32.RegisterRawInputDevices(
                ctypes.byref(registration),
                1,
                ctypes.sizeof(RAWINPUTDEVICE),
            ):
                raise ctypes.WinError(ctypes.get_last_error())

            self._ready.set()
            message = wintypes.MSG()
            while _USER32.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
                _USER32.TranslateMessage(ctypes.byref(message))
                _USER32.DispatchMessageW(ctypes.byref(message))
        except Exception as error:  # thread boundary: preserve a useful error
            self._error = f"初始化按键输入失败：{error}"
            self._ready.set()
        finally:
            self._window = None

    def _window_proc(
        self,
        hwnd: int,
        message: int,
        wparam: int,
        lparam: int,
    ) -> int:
        try:
            if message == WM_INPUT:
                self._handle_raw_input(lparam)
                return 0
            if message == WM_INPUT_DEVICE_CHANGE:
                self._device_name_cache.clear()
                self.events.put(("device-change", 0, False))
                return 0
            if message == WM_CLOSE:
                _USER32.DestroyWindow(hwnd)
                return 0
            if message == WM_DESTROY:
                _USER32.PostQuitMessage(0)
                return 0
        except Exception:
            # A callback exception must not unwind through the Win32 window
            # procedure.  A future report can continue normally.
            pass
        return _USER32.DefWindowProcW(hwnd, message, wparam, lparam)

    def _handle_raw_input(self, raw_input_handle: int) -> None:
        size = wintypes.UINT(0)
        result = _USER32.GetRawInputData(
            raw_input_handle,
            RID_INPUT,
            None,
            ctypes.byref(size),
            ctypes.sizeof(RAWINPUTHEADER),
        )
        if result == UINT_ERROR or size.value < ctypes.sizeof(RAWINPUT):
            return

        buffer = ctypes.create_string_buffer(size.value)
        result = _USER32.GetRawInputData(
            raw_input_handle,
            RID_INPUT,
            buffer,
            ctypes.byref(size),
            ctypes.sizeof(RAWINPUTHEADER),
        )
        if result == UINT_ERROR:
            return

        raw = ctypes.cast(buffer, ctypes.POINTER(RAWINPUT)).contents
        if raw.header.dwType != RIM_TYPEKEYBOARD:
            return

        handle = int(raw.header.hDevice or 0)
        name = self._device_name_cache.get(handle)
        if name is None:
            name = _raw_device_name(raw.header.hDevice)
            self._device_name_cache[handle] = name

        with self._target_lock:
            if name.casefold() not in self._target_names:
                return

        # No configured main key uses an E0-extended scan code.  Keeping the
        # flag in the event lets the UI reject unrelated extended keys.
        is_extended = bool(raw.keyboard.Flags & RI_KEY_E0)
        is_pressed = not bool(raw.keyboard.Flags & RI_KEY_BREAK)
        self.events.put(
            (
                "key-e0" if is_extended else "key",
                int(raw.keyboard.MakeCode),
                is_pressed,
            )
        )


class ControllerTestTool:
    def __init__(self, root: tk.Tk, start_monitor: bool = True) -> None:
        self.root = root
        self.connected = False
        self.controller: LightController | None = None
        self.touch_serial: serial.Serial | None = None
        self.reader_controller: AimeReaderController | None = None
        self.touch_parser = TouchFrameParser()
        self.monitor = RawKeyboardMonitor() if start_monitor else None
        self.port_by_label: dict[str, object] = {}
        self.touch_port_by_label: dict[str, object] = {}
        self.reader_port_by_label: dict[str, object] = {}
        self.keyboard_by_label: dict[str, str] = {}

        self.sequence_token = 0
        self.sequence_after_id: str | None = None
        self.raw_poll_after_id: str | None = None
        self.touch_poll_after_id: str | None = None
        self.reader_event_after_id: str | None = None
        self.reader_events: queue.SimpleQueue[
            tuple[int, str, object]
        ] = queue.SimpleQueue()
        self.reader_thread: threading.Thread | None = None
        self.reader_stop_event: threading.Event | None = None
        self.reader_abort_event: threading.Event | None = None
        self.reader_generation = 0
        self.last_valid_touch_frame_at = 0.0
        self.current_touch_bits = -1
        self.pressed_scancodes: set[int] = set()
        self.pressed_mask = 0

        self.port_var = tk.StringVar()
        self.touch_port_var = tk.StringVar()
        self.reader_port_var = tk.StringVar()
        self.reader_baudrate_var = tk.StringVar(value="115200")
        self.keyboard_var = tk.StringVar()
        self.player_var = tk.StringVar(value="1P")
        self.mapping_var = tk.StringVar(value=BUTTON_MAPPING_TEXT["1P"])
        self.red_var = tk.StringVar(value="255")
        self.green_var = tk.StringVar(value="0")
        self.blue_var = tk.StringVar(value="0")
        self.reader_firmware_var = tk.StringVar(value="—")
        self.reader_hardware_var = tk.StringVar(value="—")
        self.reader_status_var = tk.StringVar(value="请连接读卡器")
        self.reader_card_type_var = tk.StringVar(value="—")
        self.reader_identifier_var = tk.StringVar(value="—")
        self.reader_pmm_var = tk.StringVar(value="—")

        self.light_blocks: list[tk.Label] = []
        self.test_widgets: list[tk.Widget] = []
        self.rgb_entries: list[ttk.Entry] = []

        self.button_renderer = ButtonRingRenderer(
            resource_path("images"),
            _program_background_rgb(root),
        )
        self.button_photo: ImageTk.PhotoImage | None = None
        touch_background = (*_program_background_rgb(root), 255)
        self.touch_renderer = SensorRenderer(
            resource_path("images"),
            display_size=TOUCH_DISPLAY_SIZE,
            background_color=touch_background,
        )
        self.touch_photo: ImageTk.PhotoImage | None = None

        self._build_ui()
        self._bind_events()
        self.refresh_devices()
        self._set_connected(False)
        self._render_buttons()
        self._show_touch_bits(0)
        if self.monitor is not None:
            self.raw_poll_after_id = self.root.after(
                RAW_INPUT_POLL_MS, self._poll_raw_input
            )
        self.reader_event_after_id = self.root.after(
            30, self._poll_reader_events
        )

    def _build_ui(self) -> None:
        self.root.title("Maimai 综合测试工具")
        self.root.resizable(False, False)
        self.root.option_add("*Font", ("Microsoft YaHei UI", 10))

        outer = ttk.Frame(self.root, padding=14)
        outer.grid(row=0, column=0, sticky="nsew")

        connection = ttk.LabelFrame(outer, text="控制器连接", padding=10)
        connection.grid(row=0, column=0, sticky="ew")
        connection.columnconfigure(1, weight=1)

        ttk.Label(connection, text="灯光串口").grid(
            row=0, column=0, padx=(0, 8), sticky="e"
        )
        self.port_combo = ttk.Combobox(
            connection,
            textvariable=self.port_var,
            width=52,
            state="readonly",
        )
        self.port_combo.grid(row=0, column=1, padx=(0, 8), sticky="ew")

        ttk.Label(connection, text="按键设备").grid(
            row=1, column=0, padx=(0, 8), pady=(8, 0), sticky="e"
        )
        self.keyboard_combo = ttk.Combobox(
            connection,
            textvariable=self.keyboard_var,
            width=52,
            state="readonly",
        )
        self.keyboard_combo.grid(
            row=1,
            column=1,
            padx=(0, 8),
            pady=(8, 0),
            sticky="ew",
        )

        ttk.Label(connection, text="触摸串口").grid(
            row=2, column=0, padx=(0, 8), pady=(8, 0), sticky="e"
        )
        self.touch_port_combo = ttk.Combobox(
            connection,
            textvariable=self.touch_port_var,
            width=52,
            state="readonly",
        )
        self.touch_port_combo.grid(
            row=2,
            column=1,
            padx=(0, 8),
            pady=(8, 0),
            sticky="ew",
        )

        ttk.Label(connection, text="读卡器串口").grid(
            row=3, column=0, padx=(0, 8), pady=(8, 0), sticky="e"
        )
        self.reader_port_combo = ttk.Combobox(
            connection,
            textvariable=self.reader_port_var,
            width=52,
            state="readonly",
        )
        self.reader_port_combo.grid(
            row=3,
            column=1,
            padx=(0, 8),
            pady=(8, 0),
            sticky="ew",
        )
        self.reader_baudrate_combo = ttk.Combobox(
            connection,
            textvariable=self.reader_baudrate_var,
            values=[str(value) for value in AIME_BAUDRATES],
            width=10,
            state="readonly",
        )
        self.reader_baudrate_combo.grid(
            row=3,
            column=2,
            padx=(0, 8),
            pady=(8, 0),
        )

        self.refresh_button = ttk.Button(
            connection, text="刷新", width=8, command=self.refresh_devices
        )
        self.refresh_button.grid(row=0, column=3, padx=(0, 8), rowspan=4)

        self.connect_button = ttk.Button(
            connection, text="连接", width=10, command=self.toggle_connection
        )
        self.connect_button.grid(row=0, column=4, padx=(0, 12), rowspan=4)

        self.connection_status = tk.Label(
            connection,
            text="● 未连接",
            foreground="#C62828",
            background=self.root.cget("background"),
            anchor="w",
            width=18,
        )
        self.connection_status.grid(
            row=0, column=5, rowspan=4, sticky="w"
        )

        self.notebook = ttk.Notebook(outer)
        self.notebook.grid(row=1, column=0, pady=(12, 0), sticky="nsew")

        content = ttk.Frame(self.notebook, padding=8)
        touch_tab = ttk.Frame(self.notebook, padding=8)
        reader_tab = ttk.Frame(self.notebook, padding=8)
        self.notebook.add(content, text="灯光与按键")
        self.notebook.add(touch_tab, text="触摸")
        self.notebook.add(reader_tab, text="读卡器")

        buttons = ttk.LabelFrame(content, text="八键测试", padding=10)
        buttons.grid(row=0, column=0, padx=(0, 12), sticky="ns")

        selector = ttk.Frame(buttons)
        selector.grid(row=0, column=0, sticky="ew")
        ttk.Label(selector, text="键位：").grid(row=0, column=0)
        ttk.Radiobutton(
            selector,
            text="1P",
            variable=self.player_var,
            value="1P",
            command=self._player_changed,
        ).grid(row=0, column=1, padx=(2, 8))
        ttk.Radiobutton(
            selector,
            text="2P",
            variable=self.player_var,
            value="2P",
            command=self._player_changed,
        ).grid(row=0, column=2)

        ttk.Label(
            buttons,
            textvariable=self.mapping_var,
            foreground="#555555",
            anchor="w",
        ).grid(row=1, column=0, pady=(7, 2), sticky="ew")

        self.button_image_label = tk.Label(
            buttons,
            width=BUTTON_CANVAS_SIZE,
            height=BUTTON_CANVAS_SIZE,
            background=self.root.cget("background"),
            borderwidth=0,
            highlightthickness=0,
        )
        self.button_image_label.grid(row=2, column=0)

        right = ttk.Frame(content)
        right.grid(row=0, column=1, sticky="n")

        lights = ttk.LabelFrame(right, text="八路灯光", padding=(10, 12))
        lights.grid(row=0, column=0, sticky="ew")

        for index in range(LOGICAL_LIGHT_COUNT):
            item = ttk.Frame(lights)
            item.grid(row=0, column=index, padx=4)

            ttk.Label(item, text=f"BTN{index + 1}").grid(row=0, column=0)
            block = tk.Label(
                item,
                width=4,
                height=2,
                background="#000000",
                relief="sunken",
                borderwidth=2,
            )
            block.grid(row=1, column=0, pady=(5, 0))
            self.light_blocks.append(block)

        color_frame = ttk.LabelFrame(
            right, text="测试颜色（0～255）", padding=10
        )
        color_frame.grid(row=1, column=0, pady=(12, 0), sticky="ew")

        for column, (name, variable) in enumerate(
            (
                ("R", self.red_var),
                ("G", self.green_var),
                ("B", self.blue_var),
            )
        ):
            ttk.Label(color_frame, text=name).grid(
                row=0, column=column * 2, padx=(0, 4)
            )
            entry = ttk.Entry(
                color_frame, textvariable=variable, width=7, justify="center"
            )
            entry.grid(row=0, column=column * 2 + 1, padx=(0, 12))
            self.rgb_entries.append(entry)
            self.test_widgets.append(entry)

        self.color_swatch = tk.Label(
            color_frame,
            width=5,
            height=1,
            background="#FF0000",
            relief="sunken",
            borderwidth=2,
        )
        self.color_swatch.grid(row=0, column=6, padx=(0, 10))

        show_color = ttk.Button(
            color_frame,
            text="显示测试颜色",
            command=self.show_test_color,
        )
        show_color.grid(row=0, column=7)
        self.test_widgets.append(show_color)

        tests = ttk.LabelFrame(right, text="灯光测试", padding=10)
        tests.grid(row=2, column=0, pady=(12, 0), sticky="ew")

        buttons_and_commands = (
            ("RGBW 四色测试", self.start_rgbw_test),
            ("逐灯追踪", self.start_chase_test),
            ("单色淡入淡出", self.start_fade_test),
            ("停止测试", self.stop_test),
        )
        for row, (text, command) in enumerate(buttons_and_commands):
            button = ttk.Button(tests, text=text, command=command, width=22)
            button.grid(row=row, column=0, pady=5, padx=5, ipadx=8)
            self.test_widgets.append(button)

        ttk.Label(
            right,
            text="按键按下时，左侧对应区域显示为橙色。",
            foreground="#666666",
        ).grid(row=3, column=0, pady=(16, 0))

        touch_tab.columnconfigure(0, weight=1)
        sensor_frame = ttk.LabelFrame(
            touch_tab,
            text="实时触摸状态",
            padding=8,
        )
        sensor_frame.grid(row=0, column=0)

        self.touch_canvas = tk.Canvas(
            sensor_frame,
            width=TOUCH_DISPLAY_SIZE,
            height=TOUCH_DISPLAY_SIZE,
            background=self.root.cget("background"),
            highlightthickness=0,
            borderwidth=0,
        )
        self.touch_canvas.grid(row=0, column=0)
        self.touch_image_id = self.touch_canvas.create_image(
            0,
            0,
            anchor="nw",
        )

        touch_legend = ttk.Frame(touch_tab)
        touch_legend.grid(row=1, column=0, pady=(10, 0))
        tk.Label(
            touch_legend,
            width=3,
            height=1,
            background="#D6D6D6",
            relief="solid",
            borderwidth=1,
        ).grid(row=0, column=0, padx=(0, 5))
        ttk.Label(touch_legend, text="未按下").grid(
            row=0, column=1, padx=(0, 18)
        )
        tk.Label(
            touch_legend,
            width=3,
            height=1,
            background="#FF9E5E",
            relief="solid",
            borderwidth=1,
        ).grid(row=0, column=2, padx=(0, 5))
        ttk.Label(touch_legend, text="按下").grid(row=0, column=3)

        ttk.Label(
            touch_tab,
            text="A/B/D/E 各八区、C 两区；仅实时显示，不统计 34 区覆盖率。",
            foreground="#666666",
        ).grid(row=2, column=0, pady=(8, 0))

        reader_tab.columnconfigure(0, weight=1)
        reader_info = ttk.LabelFrame(
            reader_tab,
            text="读卡器信息",
            padding=10,
        )
        reader_info.grid(row=0, column=0, sticky="ew")
        reader_info.columnconfigure(1, weight=1)
        ttk.Label(reader_info, text="固件版本").grid(
            row=0, column=0, padx=(0, 10), sticky="e"
        )
        ttk.Label(
            reader_info,
            textvariable=self.reader_firmware_var,
        ).grid(row=0, column=1, sticky="w")
        ttk.Label(reader_info, text="硬件版本").grid(
            row=1, column=0, padx=(0, 10), pady=(7, 0), sticky="e"
        )
        ttk.Label(
            reader_info,
            textvariable=self.reader_hardware_var,
        ).grid(row=1, column=1, pady=(7, 0), sticky="w")

        reader_card = ttk.LabelFrame(
            reader_tab,
            text="读卡测试",
            padding=14,
        )
        reader_card.grid(row=1, column=0, pady=(12, 0), sticky="ew")
        reader_card.columnconfigure(1, weight=1)

        self.reader_status_label = tk.Label(
            reader_card,
            textvariable=self.reader_status_var,
            font=("Microsoft YaHei UI", 18, "bold"),
            foreground="#555555",
            background=self.root.cget("background"),
            anchor="center",
            pady=16,
        )
        self.reader_status_label.grid(
            row=0, column=0, columnspan=2, sticky="ew"
        )

        for row, (caption, variable) in enumerate(
            (
                ("卡片类型", self.reader_card_type_var),
                ("UID / IDm", self.reader_identifier_var),
                ("PMm", self.reader_pmm_var),
            ),
            start=1,
        ):
            ttk.Label(reader_card, text=caption).grid(
                row=row,
                column=0,
                padx=(0, 10),
                pady=5,
                sticky="e",
            )
            ttk.Entry(
                reader_card,
                textvariable=variable,
                state="readonly",
                width=55,
            ).grid(row=row, column=1, pady=5, sticky="ew")

        reader_controls = ttk.Frame(reader_card)
        reader_controls.grid(
            row=4,
            column=0,
            columnspan=2,
            pady=(15, 0),
        )
        self.reader_start_button = ttk.Button(
            reader_controls,
            text="开始读卡",
            width=14,
            command=self.start_reader_scanning,
        )
        self.reader_start_button.grid(row=0, column=0, padx=5)
        self.reader_stop_button = ttk.Button(
            reader_controls,
            text="停止读卡",
            width=14,
            command=self.stop_reader_scanning,
        )
        self.reader_stop_button.grid(row=0, column=1, padx=5)
        self.reader_clear_button = ttk.Button(
            reader_controls,
            text="清除结果",
            width=14,
            command=self.clear_reader_result,
        )
        self.reader_clear_button.grid(row=0, column=2, padx=5)

        ttk.Label(
            reader_tab,
            text=(
                "只读取卡片类型与标识，不执行写卡；"
                "115200 对应 837-15396，38400 对应 TN32MSEC003S。"
            ),
            foreground="#666666",
        ).grid(row=2, column=0, pady=(10, 0))

    def _bind_events(self) -> None:
        for variable in (self.red_var, self.green_var, self.blue_var):
            variable.trace_add("write", self._update_color_swatch)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def refresh_devices(self) -> None:
        self.refresh_ports()
        self.refresh_keyboards()

    def refresh_ports(self) -> None:
        current = self.port_by_label.get(self.port_var.get())
        current_device = getattr(current, "device", None)
        current_touch = self.touch_port_by_label.get(
            self.touch_port_var.get()
        )
        current_touch_device = getattr(current_touch, "device", None)
        current_reader = self.reader_port_by_label.get(
            self.reader_port_var.get()
        )
        current_reader_device = getattr(current_reader, "device", None)
        ports = sorted(
            list(list_ports.comports()),
            key=lambda port: port.device.casefold(),
        )

        labels: list[str] = []
        self.port_by_label.clear()
        self.touch_port_by_label.clear()
        self.reader_port_by_label.clear()
        light_label: str | None = None
        touch_label: str | None = None
        reader_label: str | None = None
        bus_descriptions = list_serial_bus_descriptions()

        for port in ports:
            description = port.description or "串口设备"
            label = serial_port_label(
                port.device,
                description,
                port.vid,
                port.pid,
                bus_descriptions.get(port.device.casefold()),
            )
            labels.append(label)
            self.port_by_label[label] = port
            self.touch_port_by_label[label] = port
            self.reader_port_by_label[label] = port
            if port.device == current_device:
                light_label = label
            if port.device == current_touch_device:
                touch_label = label
            if port.device == current_reader_device:
                reader_label = label

        values = ["", *labels]

        self.port_combo.configure(values=values)
        self.port_var.set(light_label or "")

        self.touch_port_combo.configure(values=values)
        self.touch_port_var.set(touch_label or "")

        self.reader_port_combo.configure(values=values)
        self.reader_port_var.set(reader_label or "")

    def refresh_keyboards(self) -> None:
        current_name = self.keyboard_by_label.get(self.keyboard_var.get())
        names = list_raw_keyboard_names()

        labels: list[str] = []
        self.keyboard_by_label.clear()
        current_label: str | None = None
        for index, name in enumerate(names, start=1):
            label = keyboard_device_label(
                name,
                index,
                bus_reported_device_description(name),
            )
            labels.append(label)
            self.keyboard_by_label[label] = name
            if name == current_name:
                current_label = label

        self.keyboard_combo.configure(values=labels)
        if current_label is not None:
            self.keyboard_var.set(current_label)
        elif len(labels) == 1:
            self.keyboard_var.set(labels[0])
        else:
            # With more than one keyboard, require an explicit choice so the
            # PC's built-in keyboard is not silently mistaken for the cabinet.
            self.keyboard_var.set("")

    def _set_connected(self, connected: bool, port: str = "") -> None:
        self.connected = connected
        if connected:
            self.connection_status.configure(
                text="● 已连接",
                foreground="#2E7D32",
            )
            self.connect_button.configure(text="断开")
            self.port_combo.configure(state="disabled")
            self.keyboard_combo.configure(state="disabled")
            self.touch_port_combo.configure(state="disabled")
            self.reader_port_combo.configure(state="disabled")
            self.reader_baudrate_combo.configure(state="disabled")
            self.refresh_button.configure(state="disabled")
            self.reader_clear_button.configure(state="normal")
            for widget in self.test_widgets:
                widget.configure(
                    state=(
                        "normal"
                        if self.controller is not None
                        else "disabled"
                    )
                )
        else:
            self.connection_status.configure(
                text="● 未连接",
                foreground="#C62828",
            )
            self.connect_button.configure(text="连接")
            self.port_combo.configure(state="readonly")
            self.keyboard_combo.configure(state="readonly")
            self.touch_port_combo.configure(state="readonly")
            self.reader_port_combo.configure(state="readonly")
            self.reader_baudrate_combo.configure(state="readonly")
            self.refresh_button.configure(state="normal")
            self.reader_clear_button.configure(state="disabled")
            for widget in self.test_widgets:
                widget.configure(state="disabled")
        self._update_reader_buttons()

    def toggle_connection(self) -> None:
        if not self.connected:
            self.connect()
        else:
            self.disconnect()

    def connect(self) -> None:
        selected = self.port_by_label.get(self.port_var.get())
        port = getattr(selected, "device", None)
        selected_touch = self.touch_port_by_label.get(
            self.touch_port_var.get()
        )
        touch_port = getattr(selected_touch, "device", None)
        selected_reader = self.reader_port_by_label.get(
            self.reader_port_var.get()
        )
        reader_port = getattr(selected_reader, "device", None)
        selected_ports = [
            value.casefold()
            for value in (port, touch_port, reader_port)
            if value
        ]
        if len(selected_ports) != len(set(selected_ports)):
            messagebox.showwarning(
                "串口选择重复",
                "灯光、触摸和读卡器中已选择的串口不能重复。",
            )
            return
        reader_baudrate: int | None = None
        if reader_port:
            try:
                reader_baudrate = int(
                    self.reader_baudrate_var.get(),
                    10,
                )
            except ValueError:
                messagebox.showwarning(
                    "读卡器波特率无效",
                    "请选择有效的读卡器波特率。",
                )
                return
        keyboard_name = self.keyboard_by_label.get(self.keyboard_var.get())
        if not keyboard_name:
            messagebox.showwarning(
                "未选择按键设备",
                "请选择控制器实际使用的键盘 HID 设备。",
            )
            return
        if self.monitor is None:
            messagebox.showerror("按键监听不可用", "按键监听没有启动。")
            return
        if self.monitor.error:
            messagebox.showerror("按键监听不可用", self.monitor.error)
            return
        current_keyboards = {
            name.casefold() for name in list_raw_keyboard_names()
        }
        if keyboard_name.casefold() not in current_keyboards:
            messagebox.showerror(
                "按键设备已离线",
                "所选按键设备已不存在，请刷新后重新选择。",
            )
            return

        controller: LightController | None = None
        touch_device: serial.Serial | None = None
        reader_device: AimeReaderController | None = None
        reader_firmware: bytes | None = None
        reader_hardware: bytes | None = None
        try:
            if port:
                controller = LightController(port)
                controller.probe()
            if touch_port:
                touch_device = serial.Serial(
                    port=touch_port,
                    baudrate=TOUCH_SERIAL_BAUDRATE,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE,
                    timeout=0,
                    write_timeout=0.5,
                )
                touch_device.reset_input_buffer()
                touch_device.write(RSET_COMMAND)
                touch_device.flush()
                time.sleep(DEVICE_START_DELAY_SECONDS)
                touch_device.write(STAT_COMMAND)
                touch_device.flush()
            if reader_port and reader_baudrate is not None:
                reader_device = AimeReaderController(
                    reader_port,
                    reader_baudrate,
                )
                reader_firmware, reader_hardware = reader_device.probe()
        except (
            AimeProtocolError,
            ProtocolError,
            ValueError,
            serial.SerialException,
            OSError,
        ) as error:
            if controller is not None:
                controller.close()
            if touch_device is not None:
                try:
                    touch_device.close()
                except (serial.SerialException, OSError):
                    pass
            if reader_device is not None:
                reader_device.close()
            messagebox.showerror("连接失败", str(error))
            return

        self.controller = controller
        self.touch_serial = touch_device
        self.reader_controller = reader_device
        if reader_firmware is not None and reader_hardware is not None:
            self.reader_firmware_var.set(
                _aime_version_text(reader_firmware)
            )
            self.reader_hardware_var.set(
                _aime_version_text(reader_hardware)
            )
        else:
            self.reader_firmware_var.set("—")
            self.reader_hardware_var.set("—")
        self.touch_parser.reset()
        self.last_valid_touch_frame_at = time.monotonic()
        self._show_touch_bits(0)
        self.monitor.set_targets({keyboard_name})
        self._clear_button_state()
        self._set_connected(True)
        self._schedule_touch_poll()
        self.start_reader_scanning()

    def _cancel_touch_poll(self) -> None:
        if self.touch_poll_after_id is not None:
            try:
                self.root.after_cancel(self.touch_poll_after_id)
            except tk.TclError:
                pass
            self.touch_poll_after_id = None

    def _schedule_touch_poll(self) -> None:
        self._cancel_touch_poll()
        if self.touch_serial is not None:
            self.touch_poll_after_id = self.root.after(
                TOUCH_POLL_INTERVAL_MS,
                self._poll_touch_serial,
            )

    def _poll_touch_serial(self) -> None:
        self.touch_poll_after_id = None
        device = self.touch_serial
        if device is None:
            return

        try:
            waiting = device.in_waiting
            data = device.read(waiting) if waiting else b""
        except (serial.SerialException, OSError) as error:
            self._handle_connection_lost(f"触摸设备通信失败：{error}")
            return

        if data:
            for touch_bits in self.touch_parser.feed(data):
                self.last_valid_touch_frame_at = time.monotonic()
                self._show_touch_bits(touch_bits)

        if (
            time.monotonic() - self.last_valid_touch_frame_at
            > TOUCH_TIMEOUT_SECONDS
        ):
            self._show_touch_bits(0)
        self._schedule_touch_poll()

    def _show_touch_bits(self, touch_bits: int) -> None:
        visible_bits = touch_bits & VALID_TOUCH_MASK
        if visible_bits == self.current_touch_bits:
            return
        self.current_touch_bits = visible_bits
        rendered = self.touch_renderer.render(visible_bits)
        self.touch_photo = ImageTk.PhotoImage(rendered)
        self.touch_canvas.itemconfigure(
            self.touch_image_id,
            image=self.touch_photo,
        )

    def _close_touch_device(self) -> None:
        self._cancel_touch_poll()
        device = self.touch_serial
        self.touch_serial = None
        if device is not None:
            try:
                device.close()
            except (serial.SerialException, OSError):
                pass
        self.touch_parser.reset()
        self._show_touch_bits(0)

    def _update_reader_buttons(self) -> None:
        connected = self.reader_controller is not None
        active = (
            self.reader_thread is not None
            and self.reader_thread.is_alive()
        )
        stopping = (
            self.reader_stop_event is not None
            and self.reader_stop_event.is_set()
        )
        self.reader_start_button.configure(
            state="normal" if connected and not active else "disabled"
        )
        self.reader_stop_button.configure(
            state=(
                "normal"
                if connected and active and not stopping
                else "disabled"
            )
        )

    def start_reader_scanning(self) -> None:
        controller = self.reader_controller
        if controller is None:
            return
        if self.reader_thread is not None and self.reader_thread.is_alive():
            return
        try:
            controller.start_polling()
        except AimeProtocolError as error:
            self._handle_connection_lost(
                f"读卡器启动寻卡失败：{error}"
            )
            return

        self.reader_generation += 1
        generation = self.reader_generation
        stop_event = threading.Event()
        abort_event = threading.Event()
        worker = threading.Thread(
            target=self._reader_worker,
            args=(
                generation,
                controller,
                stop_event,
                abort_event,
            ),
            name="AimeReaderPolling",
            daemon=True,
        )
        self.reader_stop_event = stop_event
        self.reader_abort_event = abort_event
        self.reader_thread = worker
        self.reader_status_var.set("等待刷卡…")
        self.reader_status_label.configure(foreground="#1565C0")
        worker.start()
        self._update_reader_buttons()

    def _reader_worker(
        self,
        generation: int,
        controller: AimeReaderController,
        stop_event: threading.Event,
        abort_event: threading.Event,
    ) -> None:
        try:
            while not stop_event.is_set():
                card = controller.detect_card()
                if stop_event.is_set():
                    break
                self.reader_events.put(
                    (generation, "card", card)
                )
                if stop_event.wait(AIME_SCAN_INTERVAL_MS / 1000):
                    break

            if not abort_event.is_set():
                controller.stop_polling()
                self.reader_events.put(
                    (generation, "stopped", None)
                )
        except AimeProtocolError as error:
            if not abort_event.is_set():
                self.reader_events.put(
                    (generation, "error", error)
                )

    def stop_reader_scanning(self) -> None:
        worker = self.reader_thread
        stop_event = self.reader_stop_event
        if (
            worker is None
            or not worker.is_alive()
            or stop_event is None
        ):
            return
        stop_event.set()
        self.reader_status_var.set("正在停止读卡…")
        self.reader_status_label.configure(foreground="#555555")
        self._update_reader_buttons()

    def _poll_reader_events(self) -> None:
        self.reader_event_after_id = None
        while True:
            try:
                generation, kind, payload = (
                    self.reader_events.get_nowait()
                )
            except queue.Empty:
                break
            if generation != self.reader_generation:
                continue

            if kind == "card" and isinstance(payload, CardInfo):
                stop_event = self.reader_stop_event
                if stop_event is None or not stop_event.is_set():
                    self._show_reader_card(payload)
            elif kind == "stopped":
                self.reader_thread = None
                self.reader_stop_event = None
                self.reader_abort_event = None
                if self.reader_controller is not None:
                    self.reader_status_var.set("读卡已停止")
                    self.reader_status_label.configure(
                        foreground="#555555"
                    )
                self._update_reader_buttons()
            elif kind == "error" and isinstance(
                payload, AimeProtocolError
            ):
                self._handle_connection_lost(
                    f"读卡器通信失败：{payload}"
                )
                break

        self.reader_event_after_id = self.root.after(
            30, self._poll_reader_events
        )

    def _show_reader_card(self, card: CardInfo) -> None:
        if card.present:
            self.reader_status_var.set(
                f"读取成功：{card.card_type}"
            )
            self.reader_status_label.configure(foreground="#2E7D32")
            self.reader_card_type_var.set(card.card_type)
            self.reader_identifier_var.set(
                _aime_hex_bytes(card.identifier)
            )
            self.reader_pmm_var.set(_aime_hex_bytes(card.pmm))
        else:
            self.reader_status_var.set("等待刷卡…")
            self.reader_status_label.configure(foreground="#1565C0")

    def clear_reader_result(self) -> None:
        self.reader_card_type_var.set("—")
        self.reader_identifier_var.set("—")
        self.reader_pmm_var.set("—")
        active = (
            self.reader_thread is not None
            and self.reader_thread.is_alive()
        )
        if active:
            self.reader_status_var.set("等待刷卡…")
            self.reader_status_label.configure(foreground="#1565C0")
        elif self.reader_controller is not None:
            self.reader_status_var.set("读卡已停止")
            self.reader_status_label.configure(foreground="#555555")

    def _close_reader_device(self) -> None:
        self.reader_generation += 1
        stop_event = self.reader_stop_event
        abort_event = self.reader_abort_event
        if abort_event is not None:
            abort_event.set()
        if stop_event is not None:
            stop_event.set()

        controller = self.reader_controller
        self.reader_controller = None
        if controller is not None:
            controller.close()

        worker = self.reader_thread
        if worker is not None and worker.is_alive():
            worker.join(timeout=0.25)
        self.reader_thread = None
        self.reader_stop_event = None
        self.reader_abort_event = None
        self.reader_firmware_var.set("—")
        self.reader_hardware_var.set("—")
        self.reader_status_var.set("请连接读卡器")
        self.reader_status_label.configure(foreground="#555555")
        self._update_reader_buttons()

    def disconnect(self, refresh: bool = True) -> None:
        self._cancel_sequence()
        self._close_touch_device()
        self._close_reader_device()
        if self.monitor is not None:
            self.monitor.clear_targets()
        self._clear_button_state()

        controller = self.controller
        self.controller = None
        if controller is not None:
            try:
                controller.set_all(BLACK)
            except (ProtocolError, ValueError):
                pass
            controller.close()

        self._set_blocks([BLACK] * LOGICAL_LIGHT_COUNT)
        self._set_connected(False)
        if refresh:
            self.refresh_devices()

    def _player_changed(self) -> None:
        mode = self.player_var.get()
        self.mapping_var.set(BUTTON_MAPPING_TEXT.get(mode, ""))
        self._clear_button_state()

    def _poll_raw_input(self) -> None:
        monitor = self.monitor
        if monitor is None:
            return

        device_changed = False
        changed = False
        while True:
            try:
                kind, scan_code, is_pressed = monitor.events.get_nowait()
            except queue.Empty:
                break
            if kind == "device-change":
                device_changed = True
                continue
            if kind != "key":
                continue
            mapping = BUTTON_SCANCODES[self.player_var.get()]
            if scan_code not in mapping:
                continue
            if is_pressed:
                if scan_code not in self.pressed_scancodes:
                    self.pressed_scancodes.add(scan_code)
                    changed = True
            elif scan_code in self.pressed_scancodes:
                self.pressed_scancodes.remove(scan_code)
                changed = True

        if changed:
            self._update_pressed_mask()

        if (
            device_changed
            and self.connected
            and not monitor.target_is_present()
        ):
            self._handle_connection_lost("控制器的按键接口已断开。")

        self.raw_poll_after_id = self.root.after(
            RAW_INPUT_POLL_MS, self._poll_raw_input
        )

    def _update_pressed_mask(self) -> None:
        mapping = BUTTON_SCANCODES[self.player_var.get()]
        mask = 0
        for index, scan_code in enumerate(mapping):
            if scan_code in self.pressed_scancodes:
                mask |= 1 << index
        if mask != self.pressed_mask:
            self.pressed_mask = mask
            self._render_buttons()

    def _clear_button_state(self) -> None:
        self.pressed_scancodes.clear()
        if self.pressed_mask:
            self.pressed_mask = 0
            self._render_buttons()

    def _render_buttons(self) -> None:
        image = self.button_renderer.render(self.pressed_mask)
        self.button_photo = ImageTk.PhotoImage(image)
        self.button_image_label.configure(image=self.button_photo)

    def _parse_test_color(self) -> Color | None:
        variables = (self.red_var, self.green_var, self.blue_var)
        names = ("R", "G", "B")
        values: list[int] = []

        for index, (name, variable) in enumerate(zip(names, variables)):
            text = variable.get().strip()
            try:
                value = int(text, 10)
            except ValueError:
                messagebox.showwarning(
                    "RGB 输入无效", f"{name} 必须是 0～255 的整数。"
                )
                self.rgb_entries[index].focus_set()
                return None
            if not 0 <= value <= 255:
                messagebox.showwarning(
                    "RGB 输入无效", f"{name} 必须在 0～255 范围内。"
                )
                self.rgb_entries[index].focus_set()
                return None
            values.append(value)
        return values[0], values[1], values[2]

    def _update_color_swatch(self, *_args: object) -> None:
        try:
            values = tuple(
                int(variable.get().strip(), 10)
                for variable in (
                    self.red_var,
                    self.green_var,
                    self.blue_var,
                )
            )
            color = _validate_color(values)
        except (ValueError, TypeError):
            return
        self.color_swatch.configure(background=self._color_hex(color))

    @staticmethod
    def _color_hex(color: Color) -> str:
        red, green, blue = _validate_color(color)
        return f"#{red:02X}{green:02X}{blue:02X}"

    def _set_blocks(self, colors: Iterable[Color]) -> None:
        values = list(colors)
        if len(values) != LOGICAL_LIGHT_COUNT:
            raise ValueError("必须提供八路灯光颜色")
        for block, color in zip(self.light_blocks, values):
            block.configure(background=self._color_hex(color))

    def _set_all_blocks(self, color: Color) -> None:
        self._set_blocks([color] * LOGICAL_LIGHT_COUNT)

    def _cancel_sequence(self) -> None:
        self.sequence_token += 1
        if self.sequence_after_id is not None:
            try:
                self.root.after_cancel(self.sequence_after_id)
            except tk.TclError:
                pass
            self.sequence_after_id = None

    def _handle_connection_lost(self, reason: str) -> None:
        self._cancel_sequence()
        self._close_touch_device()
        self._close_reader_device()
        if self.monitor is not None:
            self.monitor.clear_targets()
        self._clear_button_state()
        controller = self.controller
        self.controller = None
        if controller is not None:
            controller.close()
        self._set_all_blocks(BLACK)
        self._set_connected(False)
        messagebox.showerror("连接已断开", reason)

    def _handle_communication_error(self, error: Exception) -> None:
        self._handle_connection_lost(str(error))

    def _send_all(self, color: Color) -> bool:
        if self.controller is None:
            return False
        try:
            self.controller.set_all(color)
        except (ProtocolError, ValueError) as error:
            self._handle_communication_error(error)
            return False
        self._set_all_blocks(color)
        return True

    def _prepare_new_test(self) -> bool:
        if self.controller is None:
            return False
        self._cancel_sequence()
        return self._send_all(BLACK)

    def show_test_color(self) -> None:
        color = self._parse_test_color()
        if color is None or not self._prepare_new_test():
            return
        self._send_all(color)

    def _start_loop(self, actions: list[tuple[TestAction, int]]) -> None:
        if not actions or self.controller is None:
            return
        token = self.sequence_token

        def run_step(index: int) -> None:
            if token != self.sequence_token or self.controller is None:
                return
            action, delay_ms = actions[index]
            action(token)
            if token != self.sequence_token or self.controller is None:
                return
            self.sequence_after_id = self.root.after(
                delay_ms,
                run_step,
                (index + 1) % len(actions),
            )

        run_step(0)

    def start_rgbw_test(self) -> None:
        if not self._prepare_new_test():
            return
        colors: tuple[Color, ...] = (
            (255, 0, 0),
            (0, 255, 0),
            (0, 0, 255),
            (255, 255, 255),
        )

        def make_action(color: Color) -> TestAction:
            def action(_token: int) -> None:
                self._send_all(color)

            return action

        self._start_loop(
            [(make_action(color), RGBW_HOLD_MS) for color in colors]
        )

    def start_chase_test(self) -> None:
        color = self._parse_test_color()
        if color is None or not self._prepare_new_test():
            return

        def make_action(index: int) -> TestAction:
            def action(_token: int) -> None:
                controller = self.controller
                if controller is None:
                    return
                try:
                    controller.set_chase_frame(index, color)
                except (ProtocolError, ValueError) as error:
                    self._handle_communication_error(error)
                    return
                colors = [BLACK] * LOGICAL_LIGHT_COUNT
                colors[index] = color
                self._set_blocks(colors)

            return action

        self._start_loop(
            [
                (make_action(index), CHASE_HOLD_MS)
                for index in range(LOGICAL_LIGHT_COUNT)
            ]
        )

    def _animate_blocks(
        self,
        token: int,
        start_color: Color,
        end_color: Color,
        duration_ms: int,
    ) -> None:
        for step in range(1, FADE_VISUAL_STEPS + 1):
            amount = step / FADE_VISUAL_STEPS
            color = tuple(
                round(start + ((end - start) * amount))
                for start, end in zip(start_color, end_color)
            )

            def update(
                visual_color: Color = color,
                expected_token: int = token,
            ) -> None:
                if expected_token == self.sequence_token:
                    self._set_all_blocks(visual_color)

            self.root.after(round(duration_ms * amount), update)

    def _fade_action(
        self,
        start_color: Color,
        end_color: Color,
    ) -> TestAction:
        def action(token: int) -> None:
            controller = self.controller
            if controller is None:
                return
            try:
                controller.fade_all(
                    start_color,
                    end_color,
                    FADE_DURATION_MS,
                )
            except (ProtocolError, ValueError) as error:
                self._handle_communication_error(error)
                return
            self._set_all_blocks(start_color)
            self._animate_blocks(
                token,
                start_color,
                end_color,
                FADE_DURATION_MS,
            )

        return action

    def start_fade_test(self) -> None:
        target_color = self._parse_test_color()
        if target_color is None or not self._prepare_new_test():
            return
        if not self._send_all(target_color):
            return

        def hold_target(_token: int) -> None:
            self._set_all_blocks(target_color)

        self._start_loop(
            [
                (hold_target, FADE_HOLD_MS),
                (
                    self._fade_action(target_color, BLACK),
                    FADE_DURATION_MS,
                ),
                (
                    self._fade_action(BLACK, target_color),
                    FADE_DURATION_MS,
                ),
            ]
        )

    def stop_test(self) -> None:
        self._cancel_sequence()
        self._send_all(BLACK)

    def close(self) -> None:
        if self.raw_poll_after_id is not None:
            try:
                self.root.after_cancel(self.raw_poll_after_id)
            except tk.TclError:
                pass
            self.raw_poll_after_id = None
        if self.reader_event_after_id is not None:
            try:
                self.root.after_cancel(self.reader_event_after_id)
            except tk.TclError:
                pass
            self.reader_event_after_id = None
        self.disconnect(refresh=False)
        if self.monitor is not None:
            self.monitor.close()
        self.root.destroy()


def self_test() -> None:
    assert BUTTON_SCANCODES["1P"] == (
        0x11,
        0x12,
        0x20,
        0x2E,
        0x2D,
        0x2C,
        0x1E,
        0x10,
    )
    assert BUTTON_SCANCODES["2P"] == (
        0x48,
        0x49,
        0x4D,
        0x51,
        0x50,
        0x4F,
        0x4B,
        0x47,
    )
    assert AIME_BAUDRATES == (115200, 38400)
    assert serial_port_label(
        "COM21",
        "USB Serial Device (COM21)",
        0x0CA3,
        0x0021,
        "Mai2LED CDC",
    ) == (
        "COM21 — Mai2LED CDC — USB Serial Device (COM21)"
        " — VID 0CA3 / PID 0021"
    )
    assert serial_port_label(
        "COM1",
        "Communications Port (COM1)",
        None,
        None,
        None,
    ).startswith("COM1 — 无总线报告描述")

    name_a = (
        r"\\?\HID#VID_0CA3&PID_0021&MI_03#SERIAL_A"
        r"#{00001124-0000-1000-8000-00805f9b34fb}"
    )
    name_b = (
        r"\\?\HID#VID_1234&PID_5678&MI_00#SERIAL_B"
        r"#{00001124-0000-1000-8000-00805f9b34fb}"
    )
    assert keyboard_device_label(name_a, 1, "13KRO Keyboard").startswith(
        "键盘 1 — 13KRO Keyboard — VID 0CA3 / PID 0021"
    )
    assert keyboard_device_label(name_b, 2, None).startswith(
        "键盘 2 — 无总线报告描述 — VID 1234 / PID 5678"
    )

    renderer = ButtonRingRenderer(
        resource_path("images"),
        (240, 240, 240),
        size=360,
    )
    off = renderer.render(0)
    pressed = renderer.render((1 << 0) | (1 << 3))
    assert off.size == (360, 360)
    assert pressed.size == off.size
    assert off.tobytes() != pressed.tobytes()
    assert off.getpixel((180, 180)) == (240, 240, 240)

    expected_touch_bits = (1 << 0) | (1 << 16) | (1 << 33)
    touch_parser = TouchFrameParser()
    assert touch_parser.feed(
        _encode_test_frame(expected_touch_bits)
    ) == [expected_touch_bits]
    touch_renderer = SensorRenderer(
        resource_path("images"),
        display_size=320,
        background_color=(240, 240, 240, 255),
    )
    assert len(touch_renderer.zone_overlays) == VALID_TOUCH_BITS
    assert touch_renderer.render(0).getpixel((0, 0)) == (
        240,
        240,
        240,
        255,
    )
    assert (
        touch_renderer.render(expected_touch_bits).tobytes()
        != touch_renderer.render(0).tobytes()
    )
    print("controller_test_tool self-test: OK")


def ui_smoke_test() -> None:
    root = tk.Tk()
    root.withdraw()
    app = ControllerTestTool(root, start_monitor=False)
    root.update_idletasks()
    assert len(app.light_blocks) == LOGICAL_LIGHT_COUNT
    assert len(app.rgb_entries) == 3
    assert app.player_var.get() == "1P"
    assert not app.connected
    assert app.controller is None
    assert app.touch_serial is None
    assert app.reader_controller is None
    assert app.port_var.get() == ""
    assert app.touch_port_var.get() == ""
    assert app.reader_port_var.get() == ""
    assert app.current_touch_bits == 0
    assert len(app.touch_renderer.zone_overlays) == VALID_TOUCH_BITS
    app._show_reader_card(
        CardInfo(
            present=True,
            card_type="MIFARE",
            identifier=bytes.fromhex("04 A1 B2 C3"),
        )
    )
    assert app.reader_card_type_var.get() == "MIFARE"
    assert app.reader_identifier_var.get() == "04 A1 B2 C3"
    assert len(app.notebook.tabs()) == 3

    class SmokeMonitor:
        error: str | None = None

        def set_targets(self, names: set[str]) -> None:
            self.targets = names

        def clear_targets(self) -> None:
            self.targets = set()

    keyboard_name = r"\\?\HID#VID_1234&PID_5678#SMOKE"
    app.monitor = SmokeMonitor()  # type: ignore[assignment]
    app.keyboard_by_label["smoke keyboard"] = keyboard_name
    app.keyboard_var.set("smoke keyboard")
    original_list_raw_keyboard_names = list_raw_keyboard_names
    try:
        globals()["list_raw_keyboard_names"] = lambda: [keyboard_name]
        app.connect()
    finally:
        globals()["list_raw_keyboard_names"] = (
            original_list_raw_keyboard_names
        )
    assert app.connected
    assert app.controller is None
    assert app.touch_serial is None
    assert app.reader_controller is None
    assert all(
        str(widget.cget("state")) == "disabled"
        for widget in app.test_widgets
    )
    app.disconnect(refresh=False)
    assert not app.connected
    root.destroy()
    print("controller_test_tool UI smoke test: OK")


def main() -> int:
    if "--self-test" in sys.argv:
        self_test()
        return 0
    if "--ui-smoke-test" in sys.argv:
        ui_smoke_test()
        return 0

    root = tk.Tk()
    ControllerTestTool(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

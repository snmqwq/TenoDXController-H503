#!/usr/bin/env python3
"""Interactive magic-config console using the TenoDX Aime CDC port."""

from __future__ import annotations

import argparse
import dataclasses
import shlex
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


MAGIC_SEQUENCE = bytes([0x91, 0x3E, 0xED, 0x20, 0x7C, 0x99, 0x58, 0xAC])
MAGIC_RESPONSE_SYNC = 0xAC
MAX_PAYLOAD = 248

MODULES = {
    "global": 0x00,
    "touch": 0x10,
    "light": 0x20,
    "keyboard": 0x40,
}

COMMANDS = {
    "read": 0x01,
    "write": 0x02,
    "save": 0x03,
    "defaults": 0x04,
    "info": 0x05,
    "read-all": 0x81,
    "write-all": 0x82,
    "save-all": 0x83,
    "enter-dfu": 0x84,
    "dfu": 0x84,
}

STATUS_TEXT = {
    0x00: "OK",
    0x01: "SUM_ERROR",
    0x02: "MODULE_ERROR",
    0x03: "CMD_ERROR",
    0x04: "PARAM_ERROR",
    0x05: "LENGTH_ERROR",
    0x06: "IO_ERROR",
}

LIGHT_PARAM_LED_PER_BIT = 0x01
LIGHT_PARAM_RAINBOW_ENABLE = 0x02
LIGHT_INFO_PARAMS = bytes([LIGHT_PARAM_LED_PER_BIT, LIGHT_PARAM_RAINBOW_ENABLE])
CONNECTION_CHECK_INTERVAL_SECONDS = 1.0

TOUCH_MODULE = 0x10
TOUCH_PARAM_FULL_MAP = 0x01
TOUCH_PARAM_MODE = 0x02
TOUCH_PARAM_BATCH_MAP = 0x03
TOUCH_PARAM_PSOC_STATUS = 0x04
TOUCH_STATUS_VERSION = 1
TOUCH_STATUS_LENGTH = 16
TOUCH_STATUS_DEVICE_LENGTH = 6
TOUCH_STATUS_POLL_SECONDS = 0.5
TOUCH_CHANNEL_COUNT = 34
TOUCH_ZONE_COUNT = 34
TOUCH_MAP_ENTRY_SIZE = 2
TOUCH_BATCH_RECORD_SIZE = 1 + TOUCH_MAP_ENTRY_SIZE
TOUCH_MODE_RAW = 0
TOUCH_MODE_MAI2TOUCH = 1
TOUCH_MODE_VALUES = {
    "raw": TOUCH_MODE_RAW,
    "mai2touch": TOUCH_MODE_MAI2TOUCH,
}
TOUCH_MODE_NAMES = {value: name for name, value in TOUCH_MODE_VALUES.items()}
TOUCH_STATE_NAMES = {
    0: "等待发现 PSoC",
    1: "准备 PSoC",
    2: "等待 PSoC 重启",
    3: "验证 PSoC 重启",
    4: "写入 PSoC 配置",
    5: "等待校准",
    6: "运行中",
    7: "排空 I2C 后重初始化",
}
PSOC_STATUS_NAMES = {
    0x00: "等待配置",
    0x01: "开始初始化",
    0x02: "运行中",
    0x11: "启动 CapSense",
    0x12: "应用参数",
    0x13: "读取 Cp",
    0x14: "SAR 校准",
    0x15: "初始化基线",
    0xAD: "收到软复位命令",
    0xFF: "未知",
}

TOUCH_GLOBAL_FLAG_REINIT_REQUESTED = 0x01
TOUCH_GLOBAL_FLAG_READ_INFLIGHT = 0x02
PSOC_FLAG_CONNECTED = 0x01
PSOC_FLAG_OPERATIONAL = 0x02
PSOC_FLAG_UNAVAILABLE = 0x04
PSOC_FLAG_STATUS_VALID = 0x08
PSOC_FLAG_SOFT_RESET_SUPPORTED = 0x10
PSOC_FLAG_LEGACY_FIRMWARE = 0x20
PSOC_FLAG_POWER_CYCLE_REQUIRED = 0x40
TOUCH_ZONE_NAMES = tuple(
    [f"A{number}" for number in range(1, 9)]
    + [f"B{number}" for number in range(1, 9)]
    + [f"C{number}" for number in range(1, 3)]
    + [f"D{number}" for number in range(1, 9)]
    + [f"E{number}" for number in range(1, 9)]
)
TOUCH_ZONE_INDICES = {name: index for index, name in enumerate(TOUCH_ZONE_NAMES)}

GLOBAL_MODULE = 0x00
GLOBAL_PARAM_ALL = 0x00
GLOBAL_CMD_ENTER_DFU = 0x84
GLOBAL_DFU_CONFIRM = 0xA5

KEYBOARD_CONFIG_KEY_FIRST = 8
KEYBOARD_CONFIG_KEY_COUNT = 4
KEYBOARD_PARAM_CONFIG_KEYS = 0x80
KEYBOARD_PARAM_MAIN_LAYOUT = 0x81
KEYBOARD_TOTAL_KEYS = 12
KEYBOARD_BUTTON_NAMES = tuple(
    [f"BTN{index}" for index in range(1, 9)]
    + [f"EK_{index}" for index in range(1, 5)]
)
KEYBOARD_BUTTON_INDICES = {
    name.lower(): index for index, name in enumerate(KEYBOARD_BUTTON_NAMES)
}
KEYBOARD_MAIN_LAYOUTS = {
    "1p": 0,
    "2p": 1,
}
KEYBOARD_MAIN_LAYOUT_NAMES = {
    value: name.upper() for name, value in KEYBOARD_MAIN_LAYOUTS.items()
}


HID_KEYS: dict[str, int] = {"none": 0x00}
HID_KEY_NAMES: dict[int, str] = {0x00: "none"}


def add_hid(name: str, value: int, *aliases: str) -> None:
    for item in (name, *aliases):
        HID_KEYS[item.lower()] = value
    HID_KEY_NAMES.setdefault(value, name.lower())


for offset, letter in enumerate("abcdefghijklmnopqrstuvwxyz"):
    add_hid(letter, 0x04 + offset, f"hid_key_{letter}")

for digit, value in zip("1234567890", range(0x1E, 0x28)):
    add_hid(digit, value, f"hid_key_{digit}")

for index in range(1, 13):
    add_hid(f"f{index}", 0x39 + index, f"hid_key_f{index}")

for index in range(13, 25):
    add_hid(f"f{index}", 0x68 + index - 13, f"hid_key_f{index}")

add_hid("enter", 0x28, "return")
add_hid("escape", 0x29, "esc")
add_hid("backspace", 0x2A, "bs")
add_hid("tab", 0x2B)
add_hid("space", 0x2C)
add_hid("minus", 0x2D, "-")
add_hid("equal", 0x2E, "=")
add_hid("bracket_left", 0x2F, "[")
add_hid("bracket_right", 0x30, "]")
add_hid("backslash", 0x31, "\\")
add_hid("semicolon", 0x33, ";")
add_hid("apostrophe", 0x34, "'")
add_hid("grave", 0x35, "`")
add_hid("comma", 0x36, ",")
add_hid("period", 0x37, ".")
add_hid("slash", 0x38, "/")
add_hid("caps_lock", 0x39)
add_hid("insert", 0x49)
add_hid("home", 0x4A)
add_hid("page_up", 0x4B)
add_hid("delete", 0x4C, "del")
add_hid("end", 0x4D)
add_hid("page_down", 0x4E)
add_hid("arrow_right", 0x4F, "right")
add_hid("arrow_left", 0x50, "left")
add_hid("arrow_down", 0x51, "down")
add_hid("arrow_up", 0x52, "up")
add_hid("num_lock", 0x53)
add_hid("keypad_divide", 0x54, "kp_divide", "kp_slash")
add_hid("keypad_multiply", 0x55, "kp_multiply", "kp_mul", "*")
add_hid("keypad_subtract", 0x56, "kp_subtract", "kp_minus")
add_hid("keypad_add", 0x57, "kp_add", "kp_plus")
add_hid("keypad_enter", 0x58, "kp_enter")

for digit, value in zip("1234567890", range(0x59, 0x63)):
    add_hid(f"keypad_{digit}", value, f"kp_{digit}")

add_hid("keypad_decimal", 0x63, "kp_decimal", "kp_dot")


@dataclasses.dataclass(frozen=True)
class MagicResponse:
    status: int
    module: int
    command: int
    param: int
    payload: bytes

    @property
    def ok(self) -> bool:
        return self.status == 0

    @property
    def status_text(self) -> str:
        return STATUS_TEXT.get(self.status, f"UNKNOWN_0x{self.status:02X}")


@dataclasses.dataclass(frozen=True)
class TouchMapEntry:
    zone: str

    def __post_init__(self) -> None:
        if not isinstance(self.zone, str):
            raise TypeError("touch zone must be a string")
        zone = self.zone.strip().upper()
        if zone not in TOUCH_ZONE_INDICES:
            raise ValueError(
                f"unknown touch zone {self.zone!r}; use A1..A8, B1..B8, "
                "C1..C2, D1..D8, or E1..E8"
            )
        object.__setattr__(self, "zone", zone)

    @property
    def region(self) -> int:
        return TOUCH_ZONE_INDICES[self.zone]

    @property
    def block(self) -> str:
        return self.zone[0]


@dataclasses.dataclass(frozen=True)
class PsocRuntimeStatus:
    address: int
    raw_status: int
    flags: int
    consecutive_failures: int
    status_age_ms: int


@dataclasses.dataclass(frozen=True)
class TouchRuntimeStatus:
    state: int
    flags: int
    devices: tuple[PsocRuntimeStatus, PsocRuntimeStatus]


def parse_touch_channel(text: str) -> int:
    try:
        channel = int(text, 0)
    except ValueError as exc:
        raise ValueError("channel must be 0..33") from exc
    if not 0 <= channel < TOUCH_CHANNEL_COUNT:
        raise ValueError("channel must be 0..33")
    return channel


def parse_touch_zone(text: str) -> str:
    return TouchMapEntry(text).zone


def validate_touch_map_entry(entry: TouchMapEntry) -> TouchMapEntry:
    if not isinstance(entry, TouchMapEntry):
        raise TypeError("touch map entry must be TouchMapEntry")
    return entry


def encode_touch_map_entry(entry: TouchMapEntry) -> bytes:
    validate_touch_map_entry(entry)
    return bytes((entry.region, ord(entry.block)))


def decode_touch_map_entry(payload: bytes) -> TouchMapEntry:
    if len(payload) != TOUCH_MAP_ENTRY_SIZE:
        raise ValueError(
            f"touch map entry must be {TOUCH_MAP_ENTRY_SIZE} bytes, got {len(payload)}"
        )
    region = payload[0]
    if region >= TOUCH_ZONE_COUNT:
        raise ValueError(f"touch region must be 0..33, got {region}")
    entry = TouchMapEntry(TOUCH_ZONE_NAMES[region])
    if payload[1] != ord(entry.block):
        raise ValueError(
            f"touch block 0x{payload[1]:02X} does not match zone {entry.zone}"
        )
    return entry


def encode_touch_full_map(entries: list[TouchMapEntry]) -> bytes:
    if len(entries) != TOUCH_CHANNEL_COUNT:
        raise ValueError(f"full touch map must contain {TOUCH_CHANNEL_COUNT} channels")
    return b"".join(encode_touch_map_entry(entry) for entry in entries)


def decode_touch_full_map(payload: bytes) -> list[TouchMapEntry]:
    expected_length = TOUCH_CHANNEL_COUNT * TOUCH_MAP_ENTRY_SIZE
    if len(payload) != expected_length:
        raise ValueError(f"full touch map must be {expected_length} bytes, got {len(payload)}")
    return [
        decode_touch_map_entry(payload[offset:offset + TOUCH_MAP_ENTRY_SIZE])
        for offset in range(0, len(payload), TOUCH_MAP_ENTRY_SIZE)
    ]


def encode_touch_batch(records: list[tuple[int, TouchMapEntry]]) -> bytes:
    if not 1 <= len(records) <= TOUCH_CHANNEL_COUNT:
        raise ValueError(f"touch batch must contain 1..{TOUCH_CHANNEL_COUNT} channels")

    channels: set[int] = set()
    payload = bytearray()
    for channel, entry in records:
        if not 0 <= channel < TOUCH_CHANNEL_COUNT:
            raise ValueError("channel must be 0..33")
        if channel in channels:
            raise ValueError(f"channel {channel} is repeated in the same batch")
        channels.add(channel)
        payload.append(channel)
        payload.extend(encode_touch_map_entry(entry))
    return bytes(payload)


def decode_touch_batch(payload: bytes) -> list[tuple[int, TouchMapEntry]]:
    if not payload or len(payload) % TOUCH_BATCH_RECORD_SIZE != 0:
        raise ValueError(
            f"touch batch length must be a non-zero multiple of {TOUCH_BATCH_RECORD_SIZE}"
        )
    record_count = len(payload) // TOUCH_BATCH_RECORD_SIZE
    if record_count > TOUCH_CHANNEL_COUNT:
        raise ValueError(f"touch batch must contain at most {TOUCH_CHANNEL_COUNT} channels")

    records: list[tuple[int, TouchMapEntry]] = []
    channels: set[int] = set()
    for offset in range(0, len(payload), TOUCH_BATCH_RECORD_SIZE):
        channel = payload[offset]
        if channel >= TOUCH_CHANNEL_COUNT:
            raise ValueError(f"channel must be 0..33, got {channel}")
        if channel in channels:
            raise ValueError(f"channel {channel} is repeated in the same batch")
        channels.add(channel)
        entry_start = offset + 1
        records.append(
            (
                channel,
                decode_touch_map_entry(
                    payload[entry_start:entry_start + TOUCH_MAP_ENTRY_SIZE]
                ),
            )
        )
    return records


def parse_touch_batch_args(argv: list[str]) -> list[tuple[int, TouchMapEntry]]:
    if not argv or len(argv) % 2 != 0:
        raise ValueError(
            "set-many requires one or more groups: <channel> <zone>"
        )
    records = []
    for offset in range(0, len(argv), 2):
        records.append(
            (
                parse_touch_channel(argv[offset]),
                TouchMapEntry(parse_touch_zone(argv[offset + 1])),
            )
        )
    # Encoding performs the final record-count and duplicate-channel validation.
    encode_touch_batch(records)
    return records


def parse_touch_mode(text: str) -> int:
    mode = text.strip().lower()
    if mode not in TOUCH_MODE_VALUES:
        raise ValueError("touch mode must be raw or mai2touch")
    return TOUCH_MODE_VALUES[mode]


def decode_touch_mode(payload: bytes) -> int:
    if len(payload) != 1 or payload[0] not in TOUCH_MODE_NAMES:
        raise ValueError("touch mode response must be one byte: 0=raw or 1=mai2touch")
    return payload[0]


def decode_touch_status(payload: bytes) -> TouchRuntimeStatus:
    if len(payload) != TOUCH_STATUS_LENGTH:
        raise ValueError(
            f"touch status must be {TOUCH_STATUS_LENGTH} bytes, got {len(payload)}"
        )
    if payload[0] != TOUCH_STATUS_VERSION:
        raise ValueError(f"unsupported touch status version {payload[0]}")
    if payload[3] != 2:
        raise ValueError(f"touch status device count must be 2, got {payload[3]}")

    devices = []
    for index in range(2):
        offset = 4 + index * TOUCH_STATUS_DEVICE_LENGTH
        devices.append(
            PsocRuntimeStatus(
                address=payload[offset],
                raw_status=payload[offset + 1],
                flags=payload[offset + 2],
                consecutive_failures=payload[offset + 3],
                status_age_ms=payload[offset + 4] | (payload[offset + 5] << 8),
            )
        )
    return TouchRuntimeStatus(
        state=payload[1],
        flags=payload[2],
        devices=(devices[0], devices[1]),
    )


class MagicConfigClient:
    def __init__(self, port: str, baudrate: int, timeout: float) -> None:
        if serial is None:
            raise RuntimeError("pyserial is not installed. Run: python -m pip install pyserial")

        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self._io_lock = threading.Lock()
        self.serial = serial.Serial(
            port=port,
            baudrate=baudrate,
            timeout=0.05,
            write_timeout=timeout,
        )

    def close(self) -> None:
        with self._io_lock:
            if self.serial.is_open:
                self.serial.close()

    def request(self, module: int, command: int, param: int = 0, payload: bytes = b"") -> MagicResponse:
        if len(payload) > MAX_PAYLOAD:
            raise ValueError(f"payload too long: {len(payload)} > {MAX_PAYLOAD}")

        header = bytes([module & 0xFF, command & 0xFF, param & 0xFF, len(payload)])
        checksum = (sum(header) + sum(payload)) & 0xFF
        frame = MAGIC_SEQUENCE + header + payload + bytes([checksum])

        with self._io_lock:
            if not self.serial.is_open:
                raise ConnectionError(f"serial port is closed: {self.port}")

            self.serial.reset_input_buffer()
            self.serial.write(frame)
            self.serial.flush()

            response = self.read_response()
            if response.module != (module & 0xFF) or response.command != (command & 0xFF):
                raise RuntimeError(
                    "response mismatch: "
                    f"got module=0x{response.module:02X} cmd=0x{response.command:02X}"
                )

            return response

    def read_exact(self, length: int) -> bytes:
        deadline = time.monotonic() + self.timeout
        data = bytearray()

        while len(data) < length and time.monotonic() < deadline:
            chunk = self.serial.read(length - len(data))
            if chunk:
                data.extend(chunk)

        if len(data) != length:
            raise TimeoutError(f"timeout while reading {length} bytes, got {len(data)}")

        return bytes(data)

    def read_response(self) -> MagicResponse:
        deadline = time.monotonic() + self.timeout

        while time.monotonic() < deadline:
            first = self.serial.read(1)
            if not first or first[0] != MAGIC_RESPONSE_SYNC:
                continue

            tail = self.read_exact(5)
            header = bytes([MAGIC_RESPONSE_SYNC]) + tail
            payload_length = header[5]
            payload_and_sum = self.read_exact(payload_length + 1)
            payload = payload_and_sum[:-1]
            checksum = payload_and_sum[-1]
            expected = (sum(header) + sum(payload)) & 0xFF

            if checksum != expected:
                raise RuntimeError(f"bad response checksum: got 0x{checksum:02X}, expected 0x{expected:02X}")

            return MagicResponse(
                status=header[1],
                module=header[2],
                command=header[3],
                param=header[4],
                payload=payload,
            )

        raise TimeoutError("timeout while waiting for magic response")


def parse_u8(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value <= 0xFF:
        raise ValueError("value must be 0..255")
    return value


def parse_bool(text: str) -> bool:
    lowered = text.lower()
    if lowered in ("1", "y", "yes", "true", "on", "enable", "enabled"):
        return True
    if lowered in ("0", "n", "no", "false", "off", "disable", "disabled"):
        return False
    raise ValueError("please enter on/off")


def parse_module(text: str) -> int:
    lowered = text.lower()
    if lowered in MODULES:
        return MODULES[lowered]
    return parse_u8(text)


def parse_command(text: str) -> int:
    lowered = text.lower()
    if lowered in COMMANDS:
        return COMMANDS[lowered]
    return parse_u8(text)


def parse_key(text: str) -> int:
    lowered = text.lower().replace("-", "_")
    lowered = lowered.removeprefix("hid_key_")
    if lowered in HID_KEYS:
        return HID_KEYS[lowered]
    return parse_u8(text)


def parse_keyboard_layout(text: str) -> int:
    lowered = text.lower()
    if lowered in KEYBOARD_MAIN_LAYOUTS:
        return KEYBOARD_MAIN_LAYOUTS[lowered]

    value = parse_u8(text)
    if value not in KEYBOARD_MAIN_LAYOUT_NAMES:
        raise ValueError("layout must be 1p or 2p")
    return value


def parse_keyboard_button(text: str) -> int:
    normalized = text.strip().lower().replace("-", "_")
    if normalized in KEYBOARD_BUTTON_INDICES:
        return KEYBOARD_BUTTON_INDICES[normalized]

    error = "button must be BTN1..BTN8, EK_1..EK_4, or protocol index 0..11"
    try:
        index = parse_u8(text)
    except ValueError as exc:
        raise ValueError(error) from exc

    if index >= KEYBOARD_TOTAL_KEYS:
        raise ValueError(error)
    return index


def key_name(value: int) -> str:
    return HID_KEY_NAMES.get(value, f"0x{value:02X}")


def keyboard_layout_name(value: int) -> str:
    return KEYBOARD_MAIN_LAYOUT_NAMES.get(value, f"0x{value:02X}")


def keyboard_button_name(index: int) -> str:
    if 0 <= index < len(KEYBOARD_BUTTON_NAMES):
        return KEYBOARD_BUTTON_NAMES[index]
    return f"key{index}"


def hex_bytes(data: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in data)


def response_line(response: MagicResponse) -> str:
    return (
        f"status={response.status_text} "
        f"module=0x{response.module:02X} "
        f"cmd=0x{response.command:02X} "
        f"param=0x{response.param:02X} "
        f"payload=[{hex_bytes(response.payload)}]"
    )


def require_ok(response: MagicResponse) -> None:
    if not response.ok:
        raise RuntimeError(response_line(response))


def pause() -> None:
    input("\n按 Enter 继续...")


def ask(prompt: str, default: str | None = None) -> str:
    suffix = f" [{default}]" if default is not None else ""
    value = input(f"{prompt}{suffix}: ").strip()
    if value == "" and default is not None:
        return default
    return value


def ask_u8(prompt: str, default: int | None = None) -> int:
    while True:
        try:
            default_text = None if default is None else f"0x{default:02X}"
            return parse_u8(ask(prompt, default_text))
        except ValueError as exc:
            print(f"Invalid value: {exc}")


def ask_bool(prompt: str, default: bool | None = None) -> bool:
    while True:
        try:
            default_text = None if default is None else ("on" if default else "off")
            return parse_bool(ask(prompt, default_text))
        except ValueError as exc:
            print(f"Invalid value: {exc}")


def ask_key(prompt: str, default: int | None = None) -> int:
    while True:
        try:
            default_text = None if default is None else key_name(default)
            return parse_key(ask(prompt, default_text))
        except ValueError as exc:
            print(f"Invalid key: {exc}")


def safe_request(client: MagicConfigClient, module: int, command: int, param: int = 0, payload: bytes = b"") -> MagicResponse | None:
    try:
        response = client.request(module, command, param, payload)
        print(response_line(response))
        return response
    except Exception as exc:
        print(f"Error: {exc}")
        return None


def list_serial_ports() -> list[str]:
    if list_ports is None:
        raise RuntimeError("pyserial is not installed. Run: python -m pip install pyserial")

    ports = list(list_ports.comports())
    if not ports:
        print("未发现串口。")
        return []

    print("\n串口列表:")
    for index, port in enumerate(ports, 1):
        print(f"  {index}. {port.device}  {port.description}")

    return [port.device for port in ports]


def choose_port(default: str | None) -> str:
    ports = list_serial_ports()

    if default:
        selected = ask("串口", default)
        return selected

    if not ports:
        return ask("串口")

    while True:
        value = ask("选择串口编号或名称", "1")
        if value.isdigit():
            index = int(value)
            if 1 <= index <= len(ports):
                return ports[index - 1]
        elif value:
            return value
        print("串口选择无效。")


def connect(default_port: str | None, baudrate: int, timeout: float) -> MagicConfigClient:
    while True:
        try:
            port = choose_port(default_port)
            client = MagicConfigClient(port, baudrate, timeout)
            print(f"已连接: {port}")
            return client
        except Exception as exc:
            print(f"连接失败: {exc}")
            default_port = None
            if ask("重试? y/n", "y").lower() not in ("y", "yes"):
                raise


def read_light(client: MagicConfigClient) -> tuple[int | None, bool | None]:
    led_per_bit = safe_request(client, MODULES["light"], COMMANDS["read"], LIGHT_PARAM_LED_PER_BIT)
    rainbow = safe_request(client, MODULES["light"], COMMANDS["read"], LIGHT_PARAM_RAINBOW_ENABLE)

    led_value = led_per_bit.payload[0] if led_per_bit and led_per_bit.ok and led_per_bit.payload else None
    rainbow_value = bool(rainbow.payload[0]) if rainbow and rainbow.ok and rainbow.payload else None
    return led_value, rainbow_value


def read_keyboard_key(client: MagicConfigClient, index: int) -> int | None:
    response = safe_request(client, MODULES["keyboard"], COMMANDS["read"], index)
    if response and response.ok and response.payload:
        return response.payload[0]
    return None


def read_keyboard_layout(client: MagicConfigClient) -> int | None:
    response = safe_request(
        client,
        MODULES["keyboard"],
        COMMANDS["read"],
        KEYBOARD_PARAM_MAIN_LAYOUT,
    )
    if response and response.ok and response.payload:
        return response.payload[0]
    return None


def show_keyboard(client: MagicConfigClient) -> None:
    print("\nKeyboard keys")
    layout = read_keyboard_layout(client)
    if layout is not None:
        print(f"  main layout: {keyboard_layout_name(layout)}")
    for index in range(KEYBOARD_TOTAL_KEYS):
        value = read_keyboard_key(client, index)
        if value is None:
            continue
        key_type = " main layout" if index < KEYBOARD_CONFIG_KEY_FIRST else " configurable"
        print(f"  {keyboard_button_name(index)}: 0x{value:02X} ({key_name(value)}){key_type}")


def read_touch_map(client: MagicConfigClient) -> list[TouchMapEntry] | None:
    response = safe_request(
        client,
        TOUCH_MODULE,
        COMMANDS["read"],
        TOUCH_PARAM_FULL_MAP,
    )
    if not response or not response.ok:
        return None
    try:
        return decode_touch_full_map(response.payload)
    except ValueError as exc:
        print(f"Error: invalid touch map response: {exc}")
        return None


def read_touch_mode(client: MagicConfigClient) -> int | None:
    response = safe_request(
        client,
        TOUCH_MODULE,
        COMMANDS["read"],
        TOUCH_PARAM_MODE,
    )
    if not response or not response.ok:
        return None
    try:
        return decode_touch_mode(response.payload)
    except ValueError as exc:
        print(f"Error: invalid touch mode response: {exc}")
        return None


def read_touch_status(client: MagicConfigClient) -> TouchRuntimeStatus:
    response = client.request(
        TOUCH_MODULE,
        COMMANDS["read"],
        TOUCH_PARAM_PSOC_STATUS,
    )
    require_ok(response)
    return decode_touch_status(response.payload)


def show_touch_status(status: TouchRuntimeStatus) -> None:
    state_name = TOUCH_STATE_NAMES.get(status.state, "未知状态")
    reinit = "是" if status.flags & TOUCH_GLOBAL_FLAG_REINIT_REQUESTED else "否"
    reading = "是" if status.flags & TOUCH_GLOBAL_FLAG_READ_INFLIGHT else "否"
    print(
        f"STM32 Touch 状态机: {status.state} ({state_name})  "
        f"重初始化={reinit}  I2C读取中={reading}"
    )

    for index, device in enumerate(status.devices):
        connected = "已连接" if device.flags & PSOC_FLAG_CONNECTED else "未连接"
        operational = "运行" if device.flags & PSOC_FLAG_OPERATIONAL else "未运行"
        unavailable = "，恢复中" if device.flags & PSOC_FLAG_UNAVAILABLE else ""
        if device.flags & PSOC_FLAG_SOFT_RESET_SUPPORTED:
            generation = "新版（支持软复位）"
        elif device.flags & PSOC_FLAG_LEGACY_FIRMWARE:
            generation = "旧版（不支持软复位）"
        else:
            generation = "未知"

        if device.flags & PSOC_FLAG_STATUS_VALID:
            status_text = PSOC_STATUS_NAMES.get(device.raw_status, "未知值")
            raw_status = f"0x{device.raw_status:02X} ({status_text})"
            age = f"{device.status_age_ms} ms"
        else:
            raw_status = "无有效数据"
            age = "未知"

        config_state = (
            "需要断电重连后应用"
            if device.flags & PSOC_FLAG_POWER_CYCLE_REQUIRED
            else "当前无需断电"
        )
        print(
            f"PSoC{index}  地址=0x{device.address:02X}  "
            f"{connected}/{operational}{unavailable}\n"
            f"  状态={raw_status}  固件={generation}\n"
            f"  连续I2C失败={device.consecutive_failures}  "
            f"状态年龄={age}  配置={config_state}"
        )


def show_touch_map(client: MagicConfigClient) -> None:
    entries = read_touch_map(client)
    if entries is None:
        return
    print("\nTouch channel map")
    for channel, entry in enumerate(entries):
        print(
            f"  channel {channel:2}: zone={entry.zone:<3} "
            f"block={entry.block}"
        )


def print_general_help() -> None:
    print(
        """
可用命令:
  help [type]                 显示帮助；type 可为 led/touch/keyboard/dfu/raw
  led <command> [args]        灯光配置
  keyboard <command> [args]   键盘配置
  touch <command> [args]      触摸通道映射和协议输出模式配置
  dfu enter                   进入 DFU
  raw <module> <cmd> <param> [payload bytes...]
  exit                        断开当前串口并返回串口连接层

示例:
  led get
  led set led-per-bit 2
  led set rainbow on
  keyboard get
  keyboard set EK_1 a
  keyboard set-all 3 kp_multiply 8 9
  keyboard layout 2p
  touch set 0 A1
  touch set-many 0 A1 1 A1
  touch mode mai2touch
  raw light read 0x01
""".strip()
    )


def print_connection_help() -> None:
    print(
        """
串口连接层命令:
  ports                       列出串口
  list                        同 ports
  refresh                     刷新串口列表
  connect <编号|COMx>         连接串口
  <编号>                      直接连接列表中的串口编号
  help                        显示本说明
  exit                        退出工具
""".strip()
    )


def print_led_help() -> None:
    print(
        """
led 命令:
  led get                     读取 LED_PER_BIT 和 rainbow
  led set led-per-bit <0..255>
                              设置每个逻辑灯位对应的实际灯珠数量
  led set rainbow <on|off>    设置空闲彩虹灯效
  led save                    保存灯光配置到 Flash
  led defaults                恢复灯光默认配置到 RAM
  led info                    读取固件暴露的灯光参数列表
""".strip()
    )


def print_keyboard_help() -> None:
    print(
        """
keyboard 命令:
  keyboard get                读取主按键布局和全部 12 个 HID 键位
  keyboard get <button>       读取 BTN1..BTN8 或 EK_1..EK_4 指定键位
                              兼容协议索引 0..11
  keyboard layout [1p|2p]     读取或选择 BTN1..BTN8 主按键布局
  keyboard player [1p|2p]     layout 的兼容别名
  keyboard set <EK_1..EK_4> <hid-key|byte>
                              设置副按键键值，兼容协议索引 8..11
  keyboard set-all <EK_1-key> <EK_2-key> <EK_3-key> <EK_4-key>
                              按 EK_1..EK_4 顺序一次设置四个副按键
  keyboard save               保存键盘配置到 Flash
  keyboard defaults           恢复键盘默认配置到 RAM
  keyboard keys               列出可用 HID key 名称
  keyboard info               读取固件暴露的键盘参数信息
""".strip()
    )


def print_touch_help() -> None:
    print(
        """
touch 命令:
  touch get                   读取全部 34 个物理通道的区域映射和共用 block
  touch set <channel> <zone>  修改一个通道；block 由 zone 自动确定
  touch set-many <channel> <zone> [...]
                              按二元组一次修改多个通道，同批通道不可重复
  touch mode                  读取 CDC0 输出模式
  touch mode <raw|mai2touch>  设置 CDC0 输出模式
  touch save                  保存当前 Touch 配置到 Flash
  touch defaults              恢复 Touch 默认配置到 RAM
  touch info                  读取固件暴露的 Touch 参数列表
  touch status                显示 STM32 与两个 PSoC 的当前状态
  touch watch                 每 500 ms 刷新一次状态，按 Ctrl+C 停止

区域:
  A1..A8、B1..B8、C1..C2、D1..D8、E1..E8。
  每个物理通道必须且只能映射一个区域；多个物理通道可映射同一区域。
  set/set-many 只修改 RAM；需要执行 touch save 才会持久化。

示例:
  touch set 0 A1
  touch set 1 D4
  touch set-many 0 A1 1 A1 12 B4
  touch mode mai2touch
""".strip()
    )


def print_dfu_help() -> None:
    print(
        """
dfu 命令:
  dfu enter                   发送 DFU 进入命令

参数:
  执行 dfu enter 后需要输入 DFU 进行二次确认。
  设备接受后 USB 会断开并重新枚举到 DFU。
""".strip()
    )


def print_raw_help() -> None:
    print(
        """
raw 命令:
  raw <module> <command> <param> [payload bytes...]

module:
  global, touch, light, keyboard，或 0x00..0xFF

command:
  read, write, save, defaults, info, read-all, write-all, save-all, enter-dfu
  或 0x00..0xFF

示例:
  raw light read 0x01
  raw keyboard write 8 0x04
  raw global enter-dfu 0 0xA5
""".strip()
    )


def print_help(topic: str | None = None) -> None:
    if topic is None:
        print_general_help()
        return

    lowered = topic.lower()
    if lowered == "led":
        print_led_help()
    elif lowered == "keyboard":
        print_keyboard_help()
    elif lowered == "touch":
        print_touch_help()
    elif lowered == "dfu":
        print_dfu_help()
    elif lowered == "raw":
        print_raw_help()
    else:
        print(f"未知帮助主题: {topic}")


def command_error(message: str, topic: str | None = None) -> None:
    print(f"错误: {message}")
    if topic:
        print(f"输入 help {topic} 查看用法。")


def print_hid_keys() -> None:
    names = sorted(set(HID_KEY_NAMES.values()))
    print("可用 HID key 名称:")
    for index in range(0, len(names), 10):
        print("  " + "  ".join(names[index:index + 10]))
    print("也可以直接输入 0x00..0xFF。")


def cmd_led(client: MagicConfigClient, argv: list[str]) -> None:
    if not argv or argv[0].lower() in ("help", "-h", "--help"):
        print_led_help()
        return

    command = argv[0].lower()
    if command in ("get", "show"):
        led_per_bit, rainbow = read_light(client)
        if led_per_bit is not None:
            print(f"LED_PER_BIT = {led_per_bit}")
        if rainbow is not None:
            print(f"Rainbow = {'on' if rainbow else 'off'}")
        return

    if command == "set":
        if len(argv) != 3:
            command_error("led set 需要参数: <led-per-bit|rainbow> <value>", "led")
            return
        field = argv[1].lower().replace("_", "-")
        try:
            if field in ("led-per-bit", "led-perbit", "per-bit"):
                value = parse_u8(argv[2])
                safe_request(client, MODULES["light"], COMMANDS["write"], LIGHT_PARAM_LED_PER_BIT, bytes([value]))
            elif field == "rainbow":
                enabled = parse_bool(argv[2])
                safe_request(client, MODULES["light"], COMMANDS["write"], LIGHT_PARAM_RAINBOW_ENABLE, bytes([1 if enabled else 0]))
            else:
                command_error(f"未知 led 参数: {argv[1]}", "led")
        except ValueError as exc:
            command_error(str(exc), "led")
        return

    if command == "save":
        safe_request(client, MODULES["light"], COMMANDS["save"])
        return

    if command in ("defaults", "default"):
        safe_request(client, MODULES["light"], COMMANDS["defaults"])
        return

    if command == "info":
        safe_request(client, MODULES["light"], COMMANDS["info"])
        return

    command_error(f"未知 led 命令: {argv[0]}", "led")


def cmd_keyboard(client: MagicConfigClient, argv: list[str]) -> None:
    if not argv or argv[0].lower() in ("help", "-h", "--help"):
        print_keyboard_help()
        return

    command = argv[0].lower()
    if command in ("layout", "player"):
        if len(argv) == 1:
            layout = read_keyboard_layout(client)
            if layout is not None:
                print(f"Main layout = {keyboard_layout_name(layout)}")
            return
        if len(argv) != 2:
            command_error("keyboard layout 接受 0 或 1 个参数: [1p|2p]", "keyboard")
            return
        try:
            layout = parse_keyboard_layout(argv[1])
        except ValueError as exc:
            command_error(str(exc), "keyboard")
            return
        response = safe_request(
            client,
            MODULES["keyboard"],
            COMMANDS["write"],
            KEYBOARD_PARAM_MAIN_LAYOUT,
            bytes([layout]),
        )
        if response and response.ok:
            print(f"Main layout = {keyboard_layout_name(layout)}")
        return

    if command in ("get", "show", "list"):
        if len(argv) == 1:
            show_keyboard(client)
            return
        if len(argv) != 2:
            command_error("keyboard get 只接受 0 或 1 个键位参数。", "keyboard")
            return
        try:
            index = parse_keyboard_button(argv[1])
        except ValueError as exc:
            command_error(str(exc), "keyboard")
            return
        value = read_keyboard_key(client, index)
        if value is not None:
            key_type = "main layout" if index < KEYBOARD_CONFIG_KEY_FIRST else "configurable"
            print(
                f"{keyboard_button_name(index)}: "
                f"0x{value:02X} ({key_name(value)}) {key_type}"
            )
        return

    if command == "set":
        if len(argv) != 3:
            command_error("keyboard set 需要参数: <EK_1..EK_4> <hid-key|byte>", "keyboard")
            return
        try:
            index = parse_keyboard_button(argv[1])
            value = parse_key(argv[2])
        except ValueError as exc:
            command_error(str(exc), "keyboard")
            return
        if not KEYBOARD_CONFIG_KEY_FIRST <= index < KEYBOARD_CONFIG_KEY_FIRST + KEYBOARD_CONFIG_KEY_COUNT:
            command_error("只有 EK_1、EK_2、EK_3、EK_4 可配置。", "keyboard")
            return
        safe_request(client, MODULES["keyboard"], COMMANDS["write"], index, bytes([value]))
        return

    if command == "set-all":
        if len(argv) != 5:
            command_error(
                "keyboard set-all 需要参数: "
                "<EK_1-key> <EK_2-key> <EK_3-key> <EK_4-key>",
                "keyboard",
            )
            return
        try:
            values = bytes(parse_key(item) for item in argv[1:])
        except ValueError as exc:
            command_error(str(exc), "keyboard")
            return
        safe_request(client, MODULES["keyboard"], COMMANDS["write"], KEYBOARD_PARAM_CONFIG_KEYS, values)
        return

    if command == "save":
        safe_request(client, MODULES["keyboard"], COMMANDS["save"])
        return

    if command in ("defaults", "default"):
        safe_request(client, MODULES["keyboard"], COMMANDS["defaults"])
        return

    if command == "info":
        safe_request(client, MODULES["keyboard"], COMMANDS["info"])
        return

    if command == "keys":
        print_hid_keys()
        return

    command_error(f"未知 keyboard 命令: {argv[0]}", "keyboard")


def cmd_touch(client: MagicConfigClient, argv: list[str]) -> None:
    if not argv or argv[0].lower() in ("help", "-h", "--help"):
        print_touch_help()
        return

    command = argv[0].lower()
    if command in ("get", "show"):
        if len(argv) != 1:
            command_error("touch get 不接受参数。", "touch")
            return
        show_touch_map(client)
        return

    if command == "set":
        if len(argv) != 3:
            command_error(
                "touch set 需要参数: <channel> <zone>",
                "touch",
            )
            return
        try:
            records = parse_touch_batch_args(argv[1:])
            payload = encode_touch_batch(records)
        except ValueError as exc:
            command_error(str(exc), "touch")
            return
        response = safe_request(
            client,
            TOUCH_MODULE,
            COMMANDS["write"],
            TOUCH_PARAM_BATCH_MAP,
            payload,
        )
        if response and response.ok:
            channel, entry = records[0]
            print(
                f"channel {channel}: zone={entry.zone} "
                f"block={entry.block}"
            )
        return

    if command == "set-many":
        try:
            records = parse_touch_batch_args(argv[1:])
            payload = encode_touch_batch(records)
        except ValueError as exc:
            command_error(str(exc), "touch")
            return
        response = safe_request(
            client,
            TOUCH_MODULE,
            COMMANDS["write"],
            TOUCH_PARAM_BATCH_MAP,
            payload,
        )
        if response and response.ok:
            print(f"已一次性修改 {len(records)} 个通道。")
        return

    if command == "mode":
        if len(argv) == 1:
            mode = read_touch_mode(client)
            if mode is not None:
                print(f"Touch mode = {TOUCH_MODE_NAMES[mode]}")
            return
        if len(argv) != 2:
            command_error("touch mode 接受 0 或 1 个参数: [raw|mai2touch]", "touch")
            return
        try:
            mode = parse_touch_mode(argv[1])
        except ValueError as exc:
            command_error(str(exc), "touch")
            return
        response = safe_request(
            client,
            TOUCH_MODULE,
            COMMANDS["write"],
            TOUCH_PARAM_MODE,
            bytes([mode]),
        )
        if response and response.ok:
            print(f"Touch mode = {TOUCH_MODE_NAMES[mode]}")
        return

    if command in ("status", "watch"):
        if len(argv) != 1:
            command_error(f"touch {command} 不接受参数。", "touch")
            return

        try:
            if command == "status":
                show_touch_status(read_touch_status(client))
                return

            while True:
                print("\033[2J\033[H", end="")
                show_touch_status(read_touch_status(client))
                print("\n每 500 ms 刷新；按 Ctrl+C 停止。")
                time.sleep(TOUCH_STATUS_POLL_SECONDS)
        except KeyboardInterrupt:
            print("\n已停止 PSoC 状态监控。")
        except Exception as exc:
            print(f"Error: {exc}")
        return

    if command == "save":
        if len(argv) != 1:
            command_error("touch save 不接受参数。", "touch")
            return
        safe_request(client, TOUCH_MODULE, COMMANDS["save"])
        return

    if command in ("defaults", "default"):
        if len(argv) != 1:
            command_error("touch defaults 不接受参数。", "touch")
            return
        safe_request(client, TOUCH_MODULE, COMMANDS["defaults"])
        return

    if command == "info":
        if len(argv) != 1:
            command_error("touch info 不接受参数。", "touch")
            return
        safe_request(client, TOUCH_MODULE, COMMANDS["info"])
        return

    command_error(f"未知 touch 命令: {argv[0]}", "touch")


def cmd_dfu(client: MagicConfigClient, argv: list[str]) -> None:
    if not argv or argv[0].lower() in ("help", "-h", "--help"):
        print_dfu_help()
        return

    if argv[0].lower() != "enter" or len(argv) != 1:
        command_error("dfu 目前只支持 enter。", "dfu")
        return

    print("将发送 DFU 进入命令: module=0x00 cmd=0x84 param=0x00 payload=[0xA5]")
    confirm = ask("输入 DFU 确认", "")
    if confirm != "DFU":
        print("已取消。")
        return

    response = safe_request(
        client,
        GLOBAL_MODULE,
        GLOBAL_CMD_ENTER_DFU,
        GLOBAL_PARAM_ALL,
        bytes([GLOBAL_DFU_CONFIRM]),
    )
    if response and response.ok:
        print("DFU 命令已接受，请等待 USB 重新枚举。")
    else:
        print("DFU 命令未被当前固件接受。")


def cmd_raw(client: MagicConfigClient, argv: list[str]) -> None:
    if not argv or argv[0].lower() in ("help", "-h", "--help"):
        print_raw_help()
        return
    if len(argv) < 3:
        command_error("raw 需要参数: <module> <command> <param> [payload bytes...]", "raw")
        return

    try:
        module = parse_module(argv[0])
        command = parse_command(argv[1])
        param = parse_u8(argv[2])
        payload = bytes(parse_u8(item) for item in argv[3:])
    except ValueError as exc:
        command_error(str(exc), "raw")
        return

    safe_request(client, module, command, param, payload)


def connect_to_port(port: str, baudrate: int, timeout: float) -> MagicConfigClient:
    client = MagicConfigClient(port, baudrate, timeout)
    try:
        verify_magic_port(client)
    except Exception:
        client.close()
        raise

    print(f"已连接并验证为 TenoDX Aime/Magic 配置端口: {port}")
    return client


def verify_magic_port(client: MagicConfigClient) -> None:
    """Verify that an open serial port is the firmware's CDC2 Aime/Magic endpoint."""
    response = client.request(MODULES["light"], COMMANDS["info"])

    if not response.ok:
        raise RuntimeError(
            "端口响应了 Magic 协议，但灯光模块未就绪："
            f"{response_line(response)}"
        )

    if not set(LIGHT_INFO_PARAMS).issubset(response.payload):
        raise RuntimeError(
            "端口响应了 Magic 协议，但不是兼容的 TenoDX 灯光配置端口："
            f"{response_line(response)}"
        )


class ConnectionMonitor:
    """Periodically verify that an open CDC2 Aime/Magic port is still alive."""

    def __init__(self, client: MagicConfigClient, interval_seconds: float) -> None:
        self.client = client
        self.interval_seconds = interval_seconds
        self._stop_event = threading.Event()
        self._disconnected_event = threading.Event()
        self._error: str | None = None
        self._thread = threading.Thread(target=self._run, name="tenodx-connection-monitor", daemon=True)

    @property
    def disconnected(self) -> bool:
        return self._disconnected_event.is_set()

    @property
    def error(self) -> str:
        return self._error or "连接已断开。"

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        self._thread.join(timeout=self.client.timeout + self.interval_seconds)

    def _run(self) -> None:
        while not self._stop_event.wait(self.interval_seconds):
            try:
                verify_magic_port(self.client)
            except Exception as exc:
                self._error = str(exc)
                self._disconnected_event.set()
                self.client.close()
                print(f"\n连接已断开：{self._error}\n按 Enter 返回串口连接层。")
                return


def resolve_port_selection(selection: str, ports: list[str]) -> str:
    if selection.isdigit():
        index = int(selection)
        if 1 <= index <= len(ports):
            return ports[index - 1]
        raise ValueError("串口编号超出范围。")

    if not selection:
        raise ValueError("缺少串口编号或名称。")

    return selection


def command_loop(client: MagicConfigClient, args: argparse.Namespace) -> None:
    monitor = ConnectionMonitor(client, CONNECTION_CHECK_INTERVAL_SECONDS)
    monitor.start()
    print("\n已进入配置命令层。输入 help 查看命令，输入 exit 断开并返回串口连接层。")

    while not monitor.disconnected:
        try:
            line = input(f"tenodx:{client.port}> ").strip()
        except EOFError:
            print()
            monitor.stop()
            return

        if monitor.disconnected:
            monitor.stop()
            return

        if not line:
            continue

        try:
            parts = shlex.split(line)
        except ValueError as exc:
            print(f"命令解析失败: {exc}")
            continue

        if not parts:
            continue

        root = parts[0].lower()
        argv = parts[1:]

        if root in ("exit", "quit"):
            monitor.stop()
            return

        if root == "help":
            print_help(argv[0] if argv else None)
            continue

        if root == "led":
            cmd_led(client, argv)
        elif root == "keyboard":
            cmd_keyboard(client, argv)
        elif root == "touch":
            cmd_touch(client, argv)
        elif root == "dfu":
            cmd_dfu(client, argv)
        elif root == "raw":
            cmd_raw(client, argv)
        else:
            print(f"未知命令类型: {parts[0]}")
            print("输入 help 查看可用命令。")

    monitor.stop()


def connection_loop(args: argparse.Namespace) -> None:
    print("TenoDX 配置工具")
    print("当前位于串口连接层。输入 help 查看命令。")
    ports = list_serial_ports()

    if args.port:
        print(f"\n默认串口: {args.port}")
        print("输入 connect 直接连接默认串口，或输入其他编号/COM 名称。")

    while True:
        try:
            line = input("tenodx:connect> ").strip()
        except EOFError:
            print()
            return

        if not line:
            continue

        try:
            parts = shlex.split(line)
        except ValueError as exc:
            print(f"命令解析失败: {exc}")
            continue

        if not parts:
            continue

        root = parts[0].lower()
        argv = parts[1:]

        if root in ("exit", "quit"):
            return

        if root in ("help", "-h", "--help"):
            print_connection_help()
            continue

        if root in ("ports", "list", "refresh"):
            ports = list_serial_ports()
            continue

        if root == "connect":
            selection = argv[0] if argv else (args.port or "")
        elif len(parts) == 1:
            selection = parts[0]
        else:
            print(f"未知串口连接层命令: {parts[0]}")
            print("输入 help 查看用法。")
            continue

        try:
            port = resolve_port_selection(selection, ports)
            client = connect_to_port(port, args.baudrate, args.timeout)
        except Exception as exc:
            print(f"连接失败: {exc}")
            continue

        try:
            command_loop(client, args)
        finally:
            client.close()
            print(f"已断开: {port}")

        ports = list_serial_ports()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="TenoDX magic-config console for the Aime CDC port."
    )
    parser.add_argument("-p", "--port", help="默认串口，例如 COM7；启动时仍会列出串口")
    parser.add_argument("--baudrate", type=int, default=115200, help="CDC 波特率占位，默认 115200")
    parser.add_argument("--timeout", type=float, default=1.0, help="串口响应超时时间，单位秒，默认 1.0")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if serial is None:
        print("pyserial is not installed. Run: python -m pip install pyserial", file=sys.stderr)
        return 1

    try:
        connection_loop(args)
        return 0
    except KeyboardInterrupt:
        print("\n已退出。")
        return 0
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Interactive magic-config console for maimai_controller_H503."""

from __future__ import annotations

import argparse
import dataclasses
import shlex
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


MAGIC_SEQUENCE = bytes([0x91, 0x3E, 0xED, 0x20, 0x7C, 0x99, 0x58, 0xAC])
MAGIC_RESPONSE_SYNC = 0xAC
MAX_PAYLOAD = 192

MODULES = {
    "global": 0x00,
    "touch": 0x10,
    "light": 0x20,
    "reader": 0x30,
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

GLOBAL_MODULE = 0x00
GLOBAL_PARAM_ALL = 0x00
GLOBAL_CMD_ENTER_DFU = 0x84
GLOBAL_DFU_CONFIRM = 0xA5

KEYBOARD_CONFIG_KEY_FIRST = 8
KEYBOARD_CONFIG_KEY_COUNT = 3
KEYBOARD_PARAM_CONFIG_KEYS = 0x80
KEYBOARD_TOTAL_KEYS = 11


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


class MagicConfigClient:
    def __init__(self, port: str, baudrate: int, timeout: float) -> None:
        if serial is None:
            raise RuntimeError("pyserial is not installed. Run: python -m pip install pyserial")

        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial = serial.Serial(
            port=port,
            baudrate=baudrate,
            timeout=0.05,
            write_timeout=timeout,
        )

    def close(self) -> None:
        if self.serial.is_open:
            self.serial.close()

    def request(self, module: int, command: int, param: int = 0, payload: bytes = b"") -> MagicResponse:
        if len(payload) > MAX_PAYLOAD:
            raise ValueError(f"payload too long: {len(payload)} > {MAX_PAYLOAD}")

        header = bytes([module & 0xFF, command & 0xFF, param & 0xFF, len(payload)])
        checksum = (sum(header) + sum(payload)) & 0xFF
        frame = MAGIC_SEQUENCE + header + payload + bytes([checksum])

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


def key_name(value: int) -> str:
    return HID_KEY_NAMES.get(value, f"0x{value:02X}")


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


def show_keyboard(client: MagicConfigClient) -> None:
    print("\nKeyboard keys")
    for index in range(KEYBOARD_TOTAL_KEYS):
        value = read_keyboard_key(client, index)
        if value is None:
            continue
        fixed = " fixed" if index < KEYBOARD_CONFIG_KEY_FIRST else " configurable"
        print(f"  key{index}: 0x{value:02X} ({key_name(value)}){fixed}")


def print_general_help() -> None:
    print(
        """
可用命令:
  help [type]                 显示帮助；type 可为 led/touch/aime/keyboard/dfu/raw
  led <command> [args]        灯光配置
  keyboard <command> [args]   键盘配置
  touch help                  触摸配置占位，当前暂不完善
  aime help                   Aime 读卡配置占位，当前暂不完善
  dfu enter                   进入 DFU
  raw <module> <cmd> <param> [payload bytes...]
  exit                        断开当前串口并返回串口连接层

示例:
  led get
  led set led-per-bit 2
  led set rainbow on
  keyboard get
  keyboard set 8 a
  keyboard set-all 3 kp_multiply 9
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
  keyboard get                读取全部 11 个 HID 键位
  keyboard get <0..10>        读取指定键位
  keyboard set <8|9|10> <hid-key|byte>
                              设置可配置键位；0..7 为固定键
  keyboard set-all <key8> <key9> <key10>
                              一次设置 key8/key9/key10
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
  touch help                  显示本说明

说明:
  当前先不完善 touch 配置命令，不会向设备发送 touch 配置请求。
  后续触摸阈值等参数需要先确认固件端协议和参数含义后再接入。
""".strip()
    )


def print_aime_help() -> None:
    print(
        """
aime 命令:
  aime help                   显示本说明

说明:
  当前先不完善 Aime 读卡配置命令，不会向设备发送 aime/reader 配置请求。
  后续读卡器参数需要先确认固件端协议和参数含义后再接入。
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
  global, touch, light, reader, keyboard，或 0x00..0xFF

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
    elif lowered in ("aime", "reader"):
        print_aime_help()
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
    if command in ("get", "show", "list"):
        if len(argv) == 1:
            show_keyboard(client)
            return
        if len(argv) != 2:
            command_error("keyboard get 只接受 0 或 1 个键位参数。", "keyboard")
            return
        try:
            index = parse_u8(argv[1])
        except ValueError as exc:
            command_error(str(exc), "keyboard")
            return
        if index >= KEYBOARD_TOTAL_KEYS:
            command_error("键位范围必须是 0..10。", "keyboard")
            return
        value = read_keyboard_key(client, index)
        if value is not None:
            fixed = "fixed" if index < KEYBOARD_CONFIG_KEY_FIRST else "configurable"
            print(f"key{index}: 0x{value:02X} ({key_name(value)}) {fixed}")
        return

    if command == "set":
        if len(argv) != 3:
            command_error("keyboard set 需要参数: <8|9|10> <hid-key|byte>", "keyboard")
            return
        try:
            index = parse_u8(argv[1])
            value = parse_key(argv[2])
        except ValueError as exc:
            command_error(str(exc), "keyboard")
            return
        if not KEYBOARD_CONFIG_KEY_FIRST <= index < KEYBOARD_CONFIG_KEY_FIRST + KEYBOARD_CONFIG_KEY_COUNT:
            command_error("只有 key8、key9、key10 可配置。", "keyboard")
            return
        safe_request(client, MODULES["keyboard"], COMMANDS["write"], index, bytes([value]))
        return

    if command == "set-all":
        if len(argv) != 4:
            command_error("keyboard set-all 需要参数: <key8> <key9> <key10>", "keyboard")
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


def cmd_touch(argv: list[str]) -> None:
    if not argv or argv[0].lower() in ("help", "-h", "--help"):
        print_touch_help()
        return
    print_touch_help()
    print(f"\n未执行: touch {argv[0]} 当前未实现。")


def cmd_aime(argv: list[str]) -> None:
    if not argv or argv[0].lower() in ("help", "-h", "--help"):
        print_aime_help()
        return
    print_aime_help()
    print(f"\n未执行: aime {argv[0]} 当前未实现。")


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
    print(f"已连接: {port}")
    return client


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
    print("\n已进入配置命令层。输入 help 查看命令，输入 exit 断开并返回串口连接层。")

    while True:
        try:
            line = input(f"tenodx:{client.port}> ").strip()
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

        if root == "help":
            print_help(argv[0] if argv else None)
            continue

        if root == "led":
            cmd_led(client, argv)
        elif root == "keyboard":
            cmd_keyboard(client, argv)
        elif root == "touch":
            cmd_touch(argv)
        elif root in ("aime", "reader"):
            cmd_aime(argv)
        elif root == "dfu":
            cmd_dfu(client, argv)
        elif root == "raw":
            cmd_raw(client, argv)
        else:
            print(f"未知命令类型: {parts[0]}")
            print("输入 help 查看可用命令。")


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
    parser = argparse.ArgumentParser(description="TenoDX command-style magic-config console.")
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

#!/usr/bin/env python3
"""Interactive magic-config console for maimai_controller_H503."""

from __future__ import annotations

import argparse
import dataclasses
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
    input("\nPress Enter to continue...")


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
        print("No serial ports found.")
        return []

    print("\nSerial ports:")
    for index, port in enumerate(ports, 1):
        print(f"  {index}. {port.device}  {port.description}")

    return [port.device for port in ports]


def choose_port(default: str | None) -> str:
    ports = list_serial_ports()

    if default:
        selected = ask("Port", default)
        return selected

    if not ports:
        return ask("Port")

    while True:
        value = ask("Select port number or name", "1")
        if value.isdigit():
            index = int(value)
            if 1 <= index <= len(ports):
                return ports[index - 1]
        elif value:
            return value
        print("Invalid port selection.")


def connect(default_port: str | None, baudrate: int, timeout: float) -> MagicConfigClient:
    while True:
        try:
            port = choose_port(default_port)
            client = MagicConfigClient(port, baudrate, timeout)
            print(f"Connected: {port}")
            return client
        except Exception as exc:
            print(f"Connect failed: {exc}")
            default_port = None
            if ask("Try again? y/n", "y").lower() not in ("y", "yes"):
                raise


def read_light(client: MagicConfigClient) -> tuple[int | None, bool | None]:
    led_per_bit = safe_request(client, MODULES["light"], COMMANDS["read"], LIGHT_PARAM_LED_PER_BIT)
    rainbow = safe_request(client, MODULES["light"], COMMANDS["read"], LIGHT_PARAM_RAINBOW_ENABLE)

    led_value = led_per_bit.payload[0] if led_per_bit and led_per_bit.ok and led_per_bit.payload else None
    rainbow_value = bool(rainbow.payload[0]) if rainbow and rainbow.ok and rainbow.payload else None
    return led_value, rainbow_value


def light_menu(client: MagicConfigClient) -> None:
    while True:
        print("\nLight config")
        print("  1. Show current config")
        print("  2. Set LED_PER_BIT")
        print("  3. Set rainbow mode")
        print("  4. Save to flash")
        print("  5. Load defaults in RAM")
        print("  0. Back")

        choice = ask("Choice", "1")
        if choice == "0":
            return

        if choice == "1":
            led_per_bit, rainbow = read_light(client)
            if led_per_bit is not None:
                print(f"LED_PER_BIT = {led_per_bit}")
            if rainbow is not None:
                print(f"Rainbow = {'on' if rainbow else 'off'}")
            pause()
        elif choice == "2":
            current, _ = read_light(client)
            value = ask_u8("LED_PER_BIT", current if current is not None else 2)
            safe_request(client, MODULES["light"], COMMANDS["write"], LIGHT_PARAM_LED_PER_BIT, bytes([value]))
        elif choice == "3":
            _, current = read_light(client)
            enabled = ask_bool("Rainbow", current if current is not None else False)
            safe_request(client, MODULES["light"], COMMANDS["write"], LIGHT_PARAM_RAINBOW_ENABLE, bytes([1 if enabled else 0]))
        elif choice == "4":
            safe_request(client, MODULES["light"], COMMANDS["save"])
        elif choice == "5":
            safe_request(client, MODULES["light"], COMMANDS["defaults"])
        else:
            print("Unknown choice.")


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


def keyboard_menu(client: MagicConfigClient) -> None:
    while True:
        print("\nKeyboard config")
        print("  1. Show keys")
        print("  2. Set one key (8..10)")
        print("  3. Set key8/key9/key10 together")
        print("  4. Save to flash")
        print("  5. Load defaults in RAM")
        print("  0. Back")

        choice = ask("Choice", "1")
        if choice == "0":
            return

        if choice == "1":
            show_keyboard(client)
            pause()
        elif choice == "2":
            index = ask_u8("Key index 8..10", KEYBOARD_CONFIG_KEY_FIRST)
            if not KEYBOARD_CONFIG_KEY_FIRST <= index < KEYBOARD_CONFIG_KEY_FIRST + KEYBOARD_CONFIG_KEY_COUNT:
                print("Only key8, key9 and key10 are configurable.")
                continue
            current = read_keyboard_key(client, index)
            value = ask_key("HID key name or byte", current)
            safe_request(client, MODULES["keyboard"], COMMANDS["write"], index, bytes([value]))
        elif choice == "3":
            current = []
            for index in range(KEYBOARD_CONFIG_KEY_FIRST, KEYBOARD_CONFIG_KEY_FIRST + KEYBOARD_CONFIG_KEY_COUNT):
                current.append(read_keyboard_key(client, index))
            values = []
            for offset in range(KEYBOARD_CONFIG_KEY_COUNT):
                default = current[offset] if current[offset] is not None else None
                values.append(ask_key(f"key{KEYBOARD_CONFIG_KEY_FIRST + offset}", default))
            safe_request(client, MODULES["keyboard"], COMMANDS["write"], KEYBOARD_PARAM_CONFIG_KEYS, bytes(values))
        elif choice == "4":
            safe_request(client, MODULES["keyboard"], COMMANDS["save"])
        elif choice == "5":
            safe_request(client, MODULES["keyboard"], COMMANDS["defaults"])
        else:
            print("Unknown choice.")


def raw_menu(client: MagicConfigClient) -> None:
    print("\nRaw magic request")
    try:
        module = parse_module(ask("Module global/touch/light/reader/keyboard or byte", "light"))
        command = parse_command(ask("Command read/write/save/defaults/info/enter-dfu or byte", "info"))
        param = ask_u8("Param", 0)
        payload_text = ask("Payload bytes separated by spaces", "")
        payload = bytes(parse_u8(item) for item in payload_text.split()) if payload_text else b""
    except ValueError as exc:
        print(f"Invalid input: {exc}")
        pause()
        return

    safe_request(client, module, command, param, payload)
    pause()


def dfu_menu(client: MagicConfigClient) -> None:
    print("\nEnter DFU")
    print("This sends: module=0x00 cmd=0x84 param=0x00 payload=[0xA5].")
    print("If the device accepts it, USB will disconnect after about 100 ms.")

    confirm = ask("Type DFU to continue", "")
    if confirm != "DFU":
        print("Canceled.")
        pause()
        return

    response = safe_request(
        client,
        GLOBAL_MODULE,
        GLOBAL_CMD_ENTER_DFU,
        GLOBAL_PARAM_ALL,
        bytes([GLOBAL_DFU_CONFIRM]),
    )

    if response and response.ok:
        print("DFU accepted. Wait for USB re-enumeration.")
    else:
        print("DFU was not accepted by the current firmware.")

    pause()


def main_menu(client: MagicConfigClient, args: argparse.Namespace) -> None:
    while True:
        print("\nMaimai Controller Config")
        print(f"  Port: {client.port}")
        print("  1. Light config")
        print("  2. Keyboard config")
        print("  3. Raw magic request")
        print("  4. Enter DFU")
        print("  5. Reconnect")
        print("  6. List serial ports")
        print("  0. Exit")

        choice = ask("Choice", "1")
        if choice == "0":
            return
        if choice == "1":
            light_menu(client)
        elif choice == "2":
            keyboard_menu(client)
        elif choice == "3":
            raw_menu(client)
        elif choice == "4":
            dfu_menu(client)
        elif choice == "5":
            client.close()
            client = connect(None, args.baudrate, args.timeout)
        elif choice == "6":
            try:
                list_serial_ports()
            except Exception as exc:
                print(f"Error: {exc}")
            pause()
        else:
            print("Unknown choice.")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Interactive magic-config console.")
    parser.add_argument("-p", "--port", help="serial port of light CDC, for example COM7")
    parser.add_argument("--baudrate", type=int, default=115200, help="CDC baudrate placeholder, default 115200")
    parser.add_argument("--timeout", type=float, default=1.0, help="serial timeout seconds, default 1.0")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if serial is None:
        print("pyserial is not installed. Run: python -m pip install pyserial", file=sys.stderr)
        return 1

    client = None
    try:
        client = connect(args.port, args.baudrate, args.timeout)
        main_menu(client, args)
        return 0
    except KeyboardInterrupt:
        print("\nBye.")
        return 0
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        if client is not None:
            client.close()


if __name__ == "__main__":
    raise SystemExit(main())

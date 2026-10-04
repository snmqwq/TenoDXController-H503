#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本机自检: 不访问硬件, 只验证 DFU 刷写工具的纯逻辑."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "dfu_flasher" / "dfu_flasher.py"


def load_module():
    spec = importlib.util.spec_from_file_location("dfu_flasher_under_test", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load dfu_flasher module")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    module = load_module()

    # Magic DFU 请求帧: 8 字节前导 + 5 字节头 + 1 字节载荷 + 1 字节校验
    frame = module.build_magic_request(0x00, 0x84, 0x00, bytes((0xA5,)))
    prefix = bytes((0x91, 0x3E, 0xED, 0x20, 0x7C, 0x99, 0x58, 0xAC))
    body = bytes((0x00, 0x84, 0x00, 0x01, 0xA5))
    check(frame == prefix + body + bytes((sum(body) & 0xFF,)), "DFU request mismatch")

    # -l usb 输出解析
    sample = """
=====  DFU Interface   =====
Total number of available STM32 device in DFU mode: 1
  Device Index           : 0
  Product ID             : 0xDF11
  Serial number          : 2030FFFFFFFE
  Device ID              : 0x0474
"""
    devices = module.parse_dfu_devices(sample)
    check(len(devices) == 1, "DFU device count mismatch")
    check(devices[0].serial == "2030FFFFFFFE", "DFU serial mismatch")
    check(devices[0].device_id == "0x0474", "DFU device id mismatch")
    check(devices[0].name == "0xDF11", "DFU product id mismatch")

    empty = module.parse_dfu_devices("No STM32 device in DFU mode connected")
    check(empty == [], "empty DFU output should parse to no devices")

    # 刷写命令拼装
    command = module.build_flash_command(
        cli=Path("STM32_Programmer_CLI.exe"),
        firmware=Path("firmware.bin"),
        serial="ABC123",
        address="0x08000000",
    )
    check("-c" in command and "port=USB1" in command, "missing usb connection")
    check("sn=ABC123" in command, "missing DFU serial selector")
    check("0x08000000" in command, "missing bin start address")
    check("-v" in command, "missing verify")
    check("-detach" in command, "USB DFU must exit with -detach, not -rst")
    check("-rst" not in command, "USB DFU does not support -rst")

    hex_command = module.build_flash_command(
        cli=Path("STM32_Programmer_CLI.exe"),
        firmware=Path("firmware.hex"),
    )
    check("0x08000000" not in hex_command, "hex should not carry a bin address")

    # flash_firmware 的返回值语义: 校验成功但 detach 报错时不应整体失败.
    def fake_run_cli(command, **kwargs):
        return 1, "Download verified successfully\nError: something"

    original = module.run_cli
    module.run_cli = fake_run_cli
    try:
        firmware = Path("firmware.hex")
        original_is_file = Path.is_file
        Path.is_file = lambda self: True  # type: ignore[method-assign]
        try:
            result = module.flash_firmware(
                Path("STM32_Programmer_CLI.exe"), firmware, log=lambda _line: None
            )
        finally:
            Path.is_file = original_is_file  # type: ignore[method-assign]
    finally:
        module.run_cli = original

    check(result is False, "verified-but-detach-failed should report False, not raise")

    def fake_run_cli_ok(command, **kwargs):
        return 0, "Download verified successfully\nDetach command executed and finished with success"

    module.run_cli = fake_run_cli_ok
    try:
        Path.is_file = lambda self: True  # type: ignore[method-assign]
        try:
            result_ok = module.flash_firmware(
                Path("STM32_Programmer_CLI.exe"), Path("firmware.hex"), log=lambda _line: None
            )
        finally:
            Path.is_file = original_is_file  # type: ignore[method-assign]
    finally:
        module.run_cli = original

    check(result_ok is True, "clean verify+detach should report True")

    print("dfu_flasher self-test: OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"dfu_flasher self-test failed: {exc}", file=sys.stderr)
        raise SystemExit(1)

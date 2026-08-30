#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Mai2LED light test tool for the STM32F072 controller."""

from __future__ import annotations

import sys
import time
import tkinter as tk
from dataclasses import dataclass
from tkinter import messagebox, ttk
from typing import Callable, Iterable

import serial
from serial.tools import list_ports


SYNC = 0xE0
MARKER = 0xD0

DEVICE_VID = 0x0CA3
DEVICE_PID = 0x0021
DEFAULT_DST_NODE = 0x11
DEFAULT_SRC_NODE = 0x01

SET_LED_GS_8BIT = 0x31
SET_LED_GS_8BIT_MULTI = 0x32
SET_LED_GS_8BIT_MULTI_FADE = 0x33
SET_LED_GS_UPDATE = 0x3C
GET_BOARD_INFO = 0xF0

ACK_STATUS_OK = 0x01
ACK_REPORT_OK = 0x01

LOGICAL_LIGHT_COUNT = 8
BLACK = (0, 0, 0)

# Fixed test timings chosen for easy visual inspection.
RGBW_HOLD_MS = 800
CHASE_HOLD_MS = 300
FADE_HOLD_MS = 300
FADE_DURATION_MS = 600
FADE_VISUAL_STEPS = 12

Color = tuple[int, int, int]
TestAction = Callable[[int], None]


class ProtocolError(RuntimeError):
    """The controller returned a malformed or unsuccessful response."""


class ResponseTimeoutError(ProtocolError):
    """No complete acknowledgement was received before the deadline."""


@dataclass(frozen=True)
class Ack:
    dst_node: int
    src_node: int
    status: int
    command: int
    report: int
    payload: bytes


def _validate_color(color: Iterable[int]) -> Color:
    values = tuple(color)
    if len(values) != 3 or any(
        not isinstance(value, int) or not 0 <= value <= 255
        for value in values
    ):
        raise ValueError("RGB values must be integers from 0 to 255")
    return values  # type: ignore[return-value]


def _escape_body(body: bytes) -> bytes:
    encoded = bytearray()
    for value in body:
        if value in (SYNC, MARKER):
            encoded.extend((MARKER, (value - 1) & 0xFF))
        else:
            encoded.append(value)
    return bytes(encoded)


def build_request(
    command: int,
    payload: bytes = b"",
    dst_node: int = DEFAULT_DST_NODE,
    src_node: int = DEFAULT_SRC_NODE,
) -> bytes:
    """Build a framed 837-15070 request.

    The firmware treats an unescaped checksum equal to E0/D0 as framing data.
    Node IDs are ignored by this controller, so use another source node only
    for those two checksum values.
    """

    if not 0 <= command <= 255:
        raise ValueError("command must be a byte")
    if len(payload) > 34:
        raise ValueError("payload is too long")

    candidate_nodes = [src_node]
    candidate_nodes.extend(
        value for value in range(1, 0x10) if value != src_node
    )

    for candidate_src in candidate_nodes:
        body = bytes(
            (dst_node, candidate_src, len(payload) + 1, command)
        ) + payload
        checksum = sum(body) & 0xFF
        if checksum not in (SYNC, MARKER):
            return bytes((SYNC,)) + _escape_body(body) + bytes((checksum,))

    raise ProtocolError("unable to build a frame with a safe checksum")


def _try_extract_ack(buffer: bytearray) -> Ack | None:
    """Extract one length-aware acknowledgement from a streaming buffer."""

    while True:
        try:
            sync_index = buffer.index(SYNC)
        except ValueError:
            buffer.clear()
            return None

        if sync_index:
            del buffer[:sync_index]

        decoded = bytearray()
        raw_index = 1
        expected_body_length: int | None = None
        restart = False

        while raw_index < len(buffer):
            # The checksum itself is not escaped by the firmware. Once the
            # declared body is complete, E0/D0 must therefore be accepted as
            # an ordinary checksum byte.
            if (
                expected_body_length is not None
                and len(decoded) == expected_body_length
            ):
                checksum = buffer[raw_index]
                consumed = raw_index + 1
                body = bytes(decoded)
                del buffer[:consumed]

                if (sum(body) & 0xFF) != checksum:
                    raise ProtocolError("响应校验和错误")
                if len(body) < 6 or body[2] < 3:
                    raise ProtocolError("响应长度无效")

                return Ack(
                    dst_node=body[0],
                    src_node=body[1],
                    status=body[3],
                    command=body[4],
                    report=body[5],
                    payload=body[6:],
                )

            value = buffer[raw_index]
            if value == SYNC:
                del buffer[:raw_index]
                restart = True
                break

            if value == MARKER:
                if raw_index + 1 >= len(buffer):
                    return None
                value = (buffer[raw_index + 1] + 1) & 0xFF
                raw_index += 2
            else:
                raw_index += 1

            decoded.append(value)
            if len(decoded) == 3:
                expected_body_length = decoded[2] + 3
                if not 6 <= expected_body_length <= 64:
                    del buffer[0]
                    raise ProtocolError("响应声明长度超出范围")
            elif (
                expected_body_length is not None
                and len(decoded) > expected_body_length
            ):
                del buffer[0]
                raise ProtocolError("响应数据超过声明长度")

        if restart:
            continue
        return None


def _fade_speed_for_duration(duration_ms: int) -> int:
    if duration_ms <= 0:
        raise ValueError("fade duration must be positive")
    return max(1, min(255, round((4095 * 8) / duration_ms)))


class LightController:
    def __init__(self, port: str) -> None:
        try:
            self.serial = serial.Serial(
                port=port,
                baudrate=115200,
                timeout=0.02,
                write_timeout=0.5,
            )
        except (serial.SerialException, OSError) as error:
            raise ProtocolError(f"无法打开串口：{error}") from error

        self.port = port
        self._rx_buffer = bytearray()
        time.sleep(0.05)

    def close(self) -> None:
        try:
            if self.serial.is_open:
                self.serial.close()
        except (serial.SerialException, OSError):
            pass

    def _read_ack(self, timeout: float = 0.5) -> Ack:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ack = _try_extract_ack(self._rx_buffer)
            if ack is not None:
                return ack

            try:
                waiting = self.serial.in_waiting
                chunk = self.serial.read(waiting if waiting else 1)
            except (serial.SerialException, OSError) as error:
                raise ProtocolError(f"读取串口失败：{error}") from error

            if chunk:
                self._rx_buffer.extend(chunk)

        raise ResponseTimeoutError("设备响应超时")

    def command(self, command: int, payload: bytes = b"") -> Ack:
        frame = build_request(command, payload)
        self._rx_buffer.clear()

        try:
            self.serial.reset_input_buffer()
            written = self.serial.write(frame)
            self.serial.flush()
        except (serial.SerialException, OSError) as error:
            raise ProtocolError(f"写入串口失败：{error}") from error

        if written != len(frame):
            raise ProtocolError(
                f"串口短写：应发送 {len(frame)} 字节，实际 {written} 字节"
            )

        ack = self._read_ack()
        if ack.command != command:
            raise ProtocolError(
                f"响应命令不匹配：发送 0x{command:02X}，"
                f"收到 0x{ack.command:02X}"
            )
        if ack.status != ACK_STATUS_OK or ack.report != ACK_REPORT_OK:
            raise ProtocolError(
                f"设备拒绝命令：status=0x{ack.status:02X}，"
                f"report=0x{ack.report:02X}"
            )
        return ack

    def probe(self) -> tuple[str, int]:
        ack = self.command(GET_BOARD_INFO)
        try:
            terminator = ack.payload.index(0xFF)
        except ValueError as error:
            raise ProtocolError("设备信息缺少板号结束标记") from error

        if terminator == 0 or terminator + 1 >= len(ack.payload):
            raise ProtocolError("设备信息长度无效")

        board_number = ack.payload[:terminator].decode(
            "ascii", errors="replace"
        )
        revision = ack.payload[terminator + 1]
        if board_number != "15070-04":
            raise ProtocolError(f"串口设备不是受支持的灯板：{board_number}")
        return board_number, revision

    def set_all(self, color: Color) -> None:
        red, green, blue = _validate_color(color)
        self.command(
            SET_LED_GS_8BIT_MULTI,
            bytes((0, LOGICAL_LIGHT_COUNT, 0, red, green, blue, 0)),
        )
        self.command(SET_LED_GS_UPDATE)

    def set_one(self, index: int, color: Color) -> None:
        if not 0 <= index < LOGICAL_LIGHT_COUNT:
            raise ValueError("light index is out of range")
        red, green, blue = _validate_color(color)
        self.command(
            SET_LED_GS_8BIT,
            bytes((index, red, green, blue)),
        )
        # SetLedGs8Bit only changes the staging data. SetLedGsUpdate commits it.
        self.command(SET_LED_GS_UPDATE)

    def set_chase_frame(self, index: int, color: Color) -> None:
        """Stage one lit logical light and submit exactly one display frame."""

        if not 0 <= index < LOGICAL_LIGHT_COUNT:
            raise ValueError("light index is out of range")
        red, green, blue = _validate_color(color)

        self.command(
            SET_LED_GS_8BIT_MULTI,
            bytes((0, LOGICAL_LIGHT_COUNT, 0, 0, 0, 0, 0)),
        )
        self.command(
            SET_LED_GS_8BIT_MULTI,
            bytes((index, index + 1, 0, red, green, blue, 0)),
        )
        self.command(SET_LED_GS_UPDATE)

    def fade_all(
        self,
        start_color: Color,
        end_color: Color,
        duration_ms: int,
    ) -> None:
        start_red, start_green, start_blue = _validate_color(start_color)
        end_red, end_green, end_blue = _validate_color(end_color)
        speed = _fade_speed_for_duration(duration_ms)

        self.command(
            SET_LED_GS_8BIT_MULTI,
            bytes(
                (
                    0,
                    LOGICAL_LIGHT_COUNT,
                    0,
                    start_red,
                    start_green,
                    start_blue,
                    0,
                )
            ),
        )
        self.command(
            SET_LED_GS_8BIT_MULTI_FADE,
            bytes(
                (
                    0,
                    LOGICAL_LIGHT_COUNT,
                    0,
                    end_red,
                    end_green,
                    end_blue,
                    speed,
                )
            ),
        )
        self.command(SET_LED_GS_UPDATE)


class LightTestTool:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.controller: LightController | None = None
        self.port_by_label: dict[str, str] = {}

        self.sequence_token = 0
        self.sequence_after_id: str | None = None

        self.port_var = tk.StringVar()
        self.red_var = tk.StringVar(value="255")
        self.green_var = tk.StringVar(value="0")
        self.blue_var = tk.StringVar(value="0")

        self.light_blocks: list[tk.Label] = []
        self.test_widgets: list[tk.Widget] = []
        self.rgb_entries: list[ttk.Entry] = []

        self._build_ui()
        self._bind_events()
        self.refresh_ports()
        self._set_connected(False)

    def _build_ui(self) -> None:
        self.root.title("Mai2LED 灯光测试工具")
        self.root.resizable(False, False)
        self.root.option_add("*Font", ("Microsoft YaHei UI", 10))

        outer = ttk.Frame(self.root, padding=14)
        outer.grid(row=0, column=0, sticky="nsew")

        connection = ttk.LabelFrame(outer, text="串口连接", padding=10)
        connection.grid(row=0, column=0, sticky="ew")
        connection.columnconfigure(0, weight=1)

        self.port_combo = ttk.Combobox(
            connection,
            textvariable=self.port_var,
            width=48,
            state="readonly",
        )
        self.port_combo.grid(row=0, column=0, padx=(0, 8), sticky="ew")

        self.refresh_button = ttk.Button(
            connection, text="刷新", width=8, command=self.refresh_ports
        )
        self.refresh_button.grid(row=0, column=1, padx=(0, 8))

        self.connect_button = ttk.Button(
            connection, text="连接", width=10, command=self.toggle_connection
        )
        self.connect_button.grid(row=0, column=2, padx=(0, 12))

        self.connection_status = tk.Label(
            connection,
            text="● 未连接",
            foreground="#C62828",
            background=self.root.cget("background"),
            anchor="w",
            width=18,
        )
        self.connection_status.grid(row=0, column=3, sticky="w")

        lights = ttk.LabelFrame(outer, text="八路灯光", padding=(10, 12))
        lights.grid(row=1, column=0, pady=(12, 0), sticky="ew")

        for index in range(LOGICAL_LIGHT_COUNT):
            item = ttk.Frame(lights)
            item.grid(row=0, column=index, padx=5)

            ttk.Label(item, text=f"BTN{index + 1}").grid(row=0, column=0)
            block = tk.Label(
                item,
                width=7,
                height=3,
                background="#000000",
                relief="sunken",
                borderwidth=2,
            )
            block.grid(row=1, column=0, pady=(5, 0))
            self.light_blocks.append(block)

        color_frame = ttk.LabelFrame(
            outer, text="测试颜色（0～255）", padding=10
        )
        color_frame.grid(row=2, column=0, pady=(12, 0), sticky="ew")

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
            entry.grid(
                row=0,
                column=column * 2 + 1,
                padx=(0, 14),
            )
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
        self.color_swatch.grid(row=0, column=6, padx=(0, 12))

        show_color = ttk.Button(
            color_frame,
            text="显示测试颜色",
            command=self.show_test_color,
        )
        show_color.grid(row=0, column=7)
        self.test_widgets.append(show_color)

        tests = ttk.LabelFrame(outer, text="灯光测试", padding=10)
        tests.grid(row=3, column=0, pady=(12, 0), sticky="ew")

        rgbw_button = ttk.Button(
            tests, text="RGBW 四色测试", command=self.start_rgbw_test
        )
        chase_button = ttk.Button(
            tests, text="逐灯追踪", command=self.start_chase_test
        )
        fade_button = ttk.Button(
            tests, text="单色淡入淡出", command=self.start_fade_test
        )
        stop_button = ttk.Button(
            tests, text="停止测试", command=self.stop_test
        )

        for column, button in enumerate(
            (rgbw_button, chase_button, fade_button, stop_button)
        ):
            button.grid(row=0, column=column, padx=6, ipadx=8)
            self.test_widgets.append(button)

    def _bind_events(self) -> None:
        for variable in (self.red_var, self.green_var, self.blue_var):
            variable.trace_add("write", self._update_color_swatch)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def refresh_ports(self) -> None:
        current_device = self.port_by_label.get(self.port_var.get())
        ports = sorted(
            list(list_ports.comports()),
            key=lambda port: port.device.casefold(),
        )

        labels: list[str] = []
        self.port_by_label.clear()
        selected_label: str | None = None

        for port in ports:
            description = port.description or "串口设备"
            label = f"{port.device} — {description}"
            labels.append(label)
            self.port_by_label[label] = port.device

            if port.device == current_device:
                selected_label = label

        self.port_combo.configure(values=["", *labels])
        self.port_var.set(selected_label or "")

    def _set_connected(self, connected: bool, port: str = "") -> None:
        if connected:
            self.connection_status.configure(
                text=f"● 已连接 {port}",
                foreground="#2E7D32",
            )
            self.connect_button.configure(text="断开")
            self.port_combo.configure(state="disabled")
            self.refresh_button.configure(state="disabled")
            for widget in self.test_widgets:
                widget.configure(state="normal")
        else:
            self.connection_status.configure(
                text="● 未连接",
                foreground="#C62828",
            )
            self.connect_button.configure(text="连接")
            self.port_combo.configure(state="readonly")
            self.refresh_button.configure(state="normal")
            for widget in self.test_widgets:
                widget.configure(state="disabled")

    def toggle_connection(self) -> None:
        if self.controller is None:
            self.connect()
        else:
            self.disconnect()

    def connect(self) -> None:
        label = self.port_var.get()
        port = self.port_by_label.get(label)
        if not port:
            messagebox.showwarning("未选择串口", "请先刷新并选择一个串口。")
            return

        controller: LightController | None = None
        try:
            controller = LightController(port)
            controller.probe()
        except (ProtocolError, ValueError) as error:
            if controller is not None:
                controller.close()
            messagebox.showerror("连接失败", str(error))
            return

        self.controller = controller
        self._set_connected(True, port)

    def disconnect(self) -> None:
        self._cancel_sequence()
        controller = self.controller
        if controller is not None:
            try:
                controller.set_all(BLACK)
            except (ProtocolError, ValueError):
                pass
            controller.close()
        self.controller = None
        self._set_blocks([BLACK] * LOGICAL_LIGHT_COUNT)
        self._set_connected(False)
        self.refresh_ports()

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
            raise ValueError("exactly eight block colors are required")
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

    def _handle_communication_error(self, error: Exception) -> None:
        self._cancel_sequence()
        controller = self.controller
        self.controller = None
        if controller is not None:
            controller.close()
        self._set_all_blocks(BLACK)
        self._set_connected(False)
        messagebox.showerror("通信失败", str(error))

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

    def _start_loop(
        self,
        actions: list[tuple[TestAction, int]],
    ) -> None:
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
                visual_color: Color = color, expected_token: int = token
            ) -> None:
                if expected_token == self.sequence_token:
                    self._set_all_blocks(visual_color)

            self.root.after(
                round(duration_ms * amount),
                update,
            )

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
        self.disconnect()
        self.root.destroy()


def _decode_request_body(frame: bytes) -> tuple[bytes, int]:
    if not frame or frame[0] != SYNC:
        raise AssertionError("missing sync")

    decoded = bytearray()
    index = 1
    expected: int | None = None
    while index < len(frame):
        if expected is not None and len(decoded) == expected:
            return bytes(decoded), frame[index]
        value = frame[index]
        if value == MARKER:
            index += 1
            value = (frame[index] + 1) & 0xFF
        decoded.append(value)
        index += 1
        if len(decoded) == 3:
            expected = decoded[2] + 3
    raise AssertionError("incomplete frame")


def self_test() -> None:
    assert build_request(GET_BOARD_INFO) == bytes.fromhex(
        "E0 11 01 01 F0 03"
    )
    assert build_request(
        SET_LED_GS_8BIT_MULTI,
        bytes((0, 8, 0, 250, 150, 0, 0)),
    ) == bytes.fromhex(
        "E0 11 01 08 32 00 08 00 FA 96 00 00 E4"
    )

    escaped = build_request(0x39, bytes((0xD0, 0xD0, 0xFF)))
    assert escaped == bytes.fromhex(
        "E0 11 01 04 39 D0 CF D0 CF FF EE"
    )

    ack_buffer = bytearray(
        bytes.fromhex("E0 01 11 03 01 31 01 48")
    )
    ack = _try_extract_ack(ack_buffer)
    assert ack is not None
    assert ack.command == SET_LED_GS_8BIT
    assert ack.status == ACK_STATUS_OK
    assert ack.report == ACK_REPORT_OK
    assert not ack.payload
    assert not ack_buffer

    board_buffer = bytearray(
        bytes.fromhex(
            "E0 01 11 0D 01 F0 01 "
            "31 35 30 37 30 2D 30 34 FF 90 2E"
        )
    )
    board_ack = _try_extract_ack(board_buffer)
    assert board_ack is not None
    assert board_ack.payload == b"15070-04\xFF\x90"

    # With source node 01 this request would have checksum E0. The builder
    # must transparently choose another ignored source-node value.
    safe_frame = build_request(
        SET_LED_GS_8BIT,
        bytes((0, 0x98, 0, 0)),
    )
    safe_body, safe_checksum = _decode_request_body(safe_frame)
    assert safe_body[1] != DEFAULT_SRC_NODE
    assert safe_checksum not in (SYNC, MARKER)
    assert (sum(safe_body) & 0xFF) == safe_checksum

    recorded_commands: list[tuple[int, bytes]] = []
    controller = object.__new__(LightController)

    def record_command(command: int, payload: bytes = b"") -> Ack:
        recorded_commands.append((command, payload))
        return Ack(0, 0, ACK_STATUS_OK, command, ACK_REPORT_OK, b"")

    controller.command = record_command  # type: ignore[method-assign]
    controller.set_chase_frame(3, (12, 34, 56))
    assert recorded_commands == [
        (
            SET_LED_GS_8BIT_MULTI,
            bytes((0, LOGICAL_LIGHT_COUNT, 0, 0, 0, 0, 0)),
        ),
        (
            SET_LED_GS_8BIT_MULTI,
            bytes((3, 4, 0, 12, 34, 56, 0)),
        ),
        (SET_LED_GS_UPDATE, b""),
    ]

    assert _fade_speed_for_duration(FADE_DURATION_MS) == 55
    print("light_test_tool self-test: OK")


def ui_smoke_test() -> None:
    root = tk.Tk()
    root.withdraw()
    app = LightTestTool(root)
    root.update_idletasks()
    assert len(app.light_blocks) == LOGICAL_LIGHT_COUNT
    assert len(app.rgb_entries) == 3
    assert app.controller is None
    assert app.port_var.get() == ""
    root.destroy()
    print("light_test_tool UI smoke test: OK")


def main() -> int:
    if "--self-test" in sys.argv:
        self_test()
        return 0
    if "--ui-smoke-test" in sys.argv:
        ui_smoke_test()
        return 0

    root = tk.Tk()
    LightTestTool(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

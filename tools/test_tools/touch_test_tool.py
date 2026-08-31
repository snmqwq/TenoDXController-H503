#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Real-time Mai2Touch sensor monitor."""

from __future__ import annotations

import sys
import time
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

import serial
from PIL import Image, ImageTk
from serial.tools import list_ports


SERIAL_BAUDRATE = 9600
POLL_INTERVAL_MS = 10
TOUCH_TIMEOUT_SECONDS = 0.5
DEVICE_START_DELAY_SECONDS = 0.1

FRAME_START = 0x28  # (
FRAME_END = 0x29  # )
FRAME_DATA_LENGTH = 7
FRAME_LENGTH = 9
FRAME_DATA_MASK = 0x1F

VALID_TOUCH_BITS = 34
VALID_TOUCH_MASK = (1 << VALID_TOUCH_BITS) - 1

RSET_COMMAND = b"{RSET}"
STAT_COMMAND = b"{STAT}"

DISPLAY_SIZE = 700
DEFAULT_DISPLAY_BACKGROUND = (240, 240, 240, 255)

ZONE_NAMES = (
    *(f"A{index}" for index in range(1, 9)),
    *(f"B{index}" for index in range(1, 9)),
    "C1",
    "C2",
    *(f"D{index}" for index in range(1, 9)),
    *(f"E{index}" for index in range(1, 9)),
)


class TouchProtocolError(RuntimeError):
    """Mai2Touch serial communication failed."""


class TouchFrameParser:
    """Parse binary `(xxxxxxx)` frames emitted by Mai2Touch.ino."""

    def __init__(self) -> None:
        self.buffer = bytearray()

    def reset(self) -> None:
        self.buffer.clear()

    def feed(self, data: bytes) -> list[int]:
        self.buffer.extend(data)
        states: list[int] = []

        while True:
            try:
                start_index = self.buffer.index(FRAME_START)
            except ValueError:
                self.buffer.clear()
                break

            if start_index:
                del self.buffer[:start_index]

            if len(self.buffer) < FRAME_LENGTH:
                break

            candidate = self.buffer[:FRAME_LENGTH]
            frame_data = candidate[1:-1]
            if (
                candidate[-1] != FRAME_END
                or any(value > FRAME_DATA_MASK for value in frame_data)
            ):
                del self.buffer[0]
                continue

            del self.buffer[:FRAME_LENGTH]

            # Mai2Touch.ino writes TouchData & 0x1F first and then shifts
            # TouchData right by five bits. The seven data bytes are therefore
            # little-endian 5-bit chunks.
            touch_bits = 0
            for chunk_index, value in enumerate(frame_data):
                touch_bits |= value << (chunk_index * 5)
            states.append(touch_bits & VALID_TOUCH_MASK)

        return states


def _asset_directory() -> Path:
    bundle_root = getattr(sys, "_MEIPASS", None)
    if bundle_root is not None:
        return Path(bundle_root) / "images"
    return Path(__file__).resolve().parent / "images"


class SensorRenderer:
    """Place the gray sensor image over the program-colored background."""

    def __init__(
        self,
        asset_directory: Path,
        display_size: int = DISPLAY_SIZE,
        background_color: tuple[int, int, int, int] = (
            DEFAULT_DISPLAY_BACKGROUND
        ),
    ) -> None:
        self.display_size = display_size
        self.asset_directory = asset_directory
        self.background_color = background_color
        self.base_image = self._load_base()
        self.zone_overlays = self._load_zone_overlays()

        if len(self.zone_overlays) != VALID_TOUCH_BITS:
            raise ValueError("触摸区域素材数量不是 34")

    def _open_rgba(self, filename: str) -> Image.Image:
        path = self.asset_directory / filename
        if not path.is_file():
            raise FileNotFoundError(f"缺少触摸素材：{path}")
        return Image.open(path).convert("RGBA")

    def _resize(self, image: Image.Image) -> Image.Image:
        return image.resize(
            (self.display_size, self.display_size),
            Image.Resampling.LANCZOS,
        )

    def _load_base(self) -> Image.Image:
        background = Image.new(
            "RGBA",
            (self.display_size, self.display_size),
            self.background_color,
        )
        sensor = self._resize(self._open_rgba("sensor.png"))
        background.alpha_composite(sensor)
        return background

    def _rotated_group(
        self,
        group_name: str,
        count: int,
        step_degrees: int,
    ) -> list[Image.Image]:
        source = self._open_rgba(f"canvas{group_name}.png")
        overlays: list[Image.Image] = []
        for index in range(count):
            # Pillow uses positive angles for counter-clockwise rotation.
            # Zone numbers increase clockwise, so use a negative angle.
            angle = -(index * step_degrees)
            if angle:
                rotated = source.rotate(
                    angle,
                    resample=Image.Resampling.BICUBIC,
                    expand=False,
                    fillcolor=(0, 0, 0, 0),
                )
            else:
                rotated = source.copy()
            overlays.append(self._resize(rotated))
        return overlays

    def _load_zone_overlays(self) -> tuple[Image.Image, ...]:
        overlays: list[Image.Image] = []
        overlays.extend(self._rotated_group("A", 8, 45))
        overlays.extend(self._rotated_group("B", 8, 45))
        overlays.extend(self._rotated_group("C", 2, 180))
        overlays.extend(self._rotated_group("D", 8, 45))
        overlays.extend(self._rotated_group("E", 8, 45))
        return tuple(overlays)

    def render(self, touch_bits: int) -> Image.Image:
        image = self.base_image.copy()
        visible_bits = touch_bits & VALID_TOUCH_MASK
        for bit_index, overlay in enumerate(self.zone_overlays):
            if visible_bits & (1 << bit_index):
                image.alpha_composite(overlay)
        return image


class TouchTestTool:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.device_serial: serial.Serial | None = None
        self.parser = TouchFrameParser()
        self.program_background = self.root.cget("background")
        background_rgb16 = self.root.winfo_rgb(self.program_background)
        background_rgba = tuple(
            round(value / 257) for value in background_rgb16
        ) + (255,)
        self.renderer = SensorRenderer(
            _asset_directory(),
            background_color=background_rgba,
        )

        self.port_by_label: dict[str, str] = {}
        self.port_var = tk.StringVar()
        self.poll_after_id: str | None = None
        self.last_valid_frame_at = 0.0
        self.current_touch_bits = -1
        self.sensor_photo: ImageTk.PhotoImage | None = None
        self.closing = False

        self._build_ui()
        self.refresh_ports()
        self._set_connected(False)
        self._show_touch_bits(0)

    def _build_ui(self) -> None:
        self.root.title("Mai2Touch 触摸测试工具")
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
            width=50,
            state="readonly",
        )
        self.port_combo.grid(row=0, column=0, padx=(0, 8), sticky="ew")

        self.refresh_button = ttk.Button(
            connection,
            text="刷新",
            width=8,
            command=self.refresh_ports,
        )
        self.refresh_button.grid(row=0, column=1, padx=(0, 8))

        self.connect_button = ttk.Button(
            connection,
            text="连接",
            width=10,
            command=self.toggle_connection,
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

        sensor_frame = ttk.LabelFrame(
            outer,
            text="实时触摸状态",
            padding=8,
        )
        sensor_frame.grid(row=1, column=0, pady=(12, 0))

        self.sensor_canvas = tk.Canvas(
            sensor_frame,
            width=DISPLAY_SIZE,
            height=DISPLAY_SIZE,
            background=self.program_background,
            highlightthickness=0,
            borderwidth=0,
        )
        self.sensor_canvas.grid(row=0, column=0)
        self.sensor_image_id = self.sensor_canvas.create_image(
            0,
            0,
            anchor="nw",
        )

        legend = ttk.Frame(outer)
        legend.grid(row=2, column=0, pady=(10, 0))
        tk.Label(
            legend,
            width=3,
            height=1,
            background="#D6D6D6",
            relief="solid",
            borderwidth=1,
        ).grid(row=0, column=0, padx=(0, 5))
        ttk.Label(legend, text="未按下").grid(row=0, column=1, padx=(0, 18))
        tk.Label(
            legend,
            width=3,
            height=1,
            background="#FF9E5E",
            relief="solid",
            borderwidth=1,
        ).grid(row=0, column=2, padx=(0, 5))
        ttk.Label(legend, text="按下").grid(row=0, column=3)

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
        else:
            self.connection_status.configure(
                text="● 未连接",
                foreground="#C62828",
            )
            self.connect_button.configure(text="连接")
            self.port_combo.configure(state="readonly")
            self.refresh_button.configure(state="normal")

    def toggle_connection(self) -> None:
        if self.device_serial is None:
            self.connect()
        else:
            self.disconnect()

    def connect(self) -> None:
        port = self.port_by_label.get(self.port_var.get())
        if not port:
            messagebox.showwarning("未选择串口", "请先刷新并选择一个串口。")
            return

        device: serial.Serial | None = None
        try:
            device = serial.Serial(
                port=port,
                baudrate=SERIAL_BAUDRATE,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0,
                write_timeout=0.5,
            )
            device.reset_input_buffer()
            device.write(RSET_COMMAND)
            device.flush()
            time.sleep(DEVICE_START_DELAY_SECONDS)
            device.write(STAT_COMMAND)
            device.flush()
        except (serial.SerialException, OSError) as error:
            if device is not None:
                try:
                    device.close()
                except (serial.SerialException, OSError):
                    pass
            messagebox.showerror("连接失败", f"无法初始化触摸设备：{error}")
            return

        self.device_serial = device
        self.parser.reset()
        self.last_valid_frame_at = time.monotonic()
        self._show_touch_bits(0)
        self._set_connected(True, port)
        self._schedule_poll()

    def _cancel_poll(self) -> None:
        if self.poll_after_id is not None:
            try:
                self.root.after_cancel(self.poll_after_id)
            except tk.TclError:
                pass
            self.poll_after_id = None

    def _schedule_poll(self) -> None:
        self._cancel_poll()
        if self.device_serial is not None:
            self.poll_after_id = self.root.after(
                POLL_INTERVAL_MS,
                self._poll_serial,
            )

    def _poll_serial(self) -> None:
        self.poll_after_id = None
        device = self.device_serial
        if device is None:
            return

        try:
            waiting = device.in_waiting
            data = device.read(waiting) if waiting else b""
        except (serial.SerialException, OSError) as error:
            self._communication_failed(error)
            return

        if data:
            for touch_bits in self.parser.feed(data):
                self.last_valid_frame_at = time.monotonic()
                self._show_touch_bits(touch_bits)

        if (
            time.monotonic() - self.last_valid_frame_at
            > TOUCH_TIMEOUT_SECONDS
        ):
            self._show_touch_bits(0)

        self._schedule_poll()

    def _communication_failed(self, error: Exception) -> None:
        self._disconnect_internal(refresh=False)
        if not self.closing:
            messagebox.showerror("通信失败", f"触摸设备连接已断开：{error}")

    def _show_touch_bits(self, touch_bits: int) -> None:
        visible_bits = touch_bits & VALID_TOUCH_MASK
        if visible_bits == self.current_touch_bits:
            return

        self.current_touch_bits = visible_bits
        rendered = self.renderer.render(visible_bits)
        self.sensor_photo = ImageTk.PhotoImage(rendered)
        self.sensor_canvas.itemconfigure(
            self.sensor_image_id,
            image=self.sensor_photo,
        )

    def _disconnect_internal(self, refresh: bool) -> None:
        self._cancel_poll()
        device = self.device_serial
        self.device_serial = None
        if device is not None:
            try:
                device.close()
            except (serial.SerialException, OSError):
                pass

        self.parser.reset()
        self._show_touch_bits(0)
        self._set_connected(False)
        if refresh:
            self.refresh_ports()

    def disconnect(self) -> None:
        self._disconnect_internal(refresh=True)

    def close(self) -> None:
        self.closing = True
        self._disconnect_internal(refresh=False)
        self.root.destroy()


def _encode_test_frame(touch_bits: int) -> bytes:
    data = bytearray((FRAME_START,))
    remaining = touch_bits
    for _ in range(FRAME_DATA_LENGTH):
        data.append(remaining & FRAME_DATA_MASK)
        remaining >>= 5
    data.append(FRAME_END)
    return bytes(data)


def self_test() -> None:
    assert len(ZONE_NAMES) == VALID_TOUCH_BITS
    assert ZONE_NAMES[0] == "A1"
    assert ZONE_NAMES[7] == "A8"
    assert ZONE_NAMES[8] == "B1"
    assert ZONE_NAMES[15] == "B8"
    assert ZONE_NAMES[16:18] == ("C1", "C2")
    assert ZONE_NAMES[18] == "D1"
    assert ZONE_NAMES[25] == "D8"
    assert ZONE_NAMES[26] == "E1"
    assert ZONE_NAMES[33] == "E8"

    expected_bits = (
        (1 << 0)
        | (1 << 7)
        | (1 << 8)
        | (1 << 16)
        | (1 << 17)
        | (1 << 18)
        | (1 << 25)
        | (1 << 26)
        | (1 << 33)
    )
    frame = _encode_test_frame(expected_bits)
    assert frame[1] & 0x01  # A1 is in the first 5-bit data byte.

    parser = TouchFrameParser()
    assert parser.feed(b"noise" + frame[:4]) == []
    assert parser.feed(frame[4:]) == [expected_bits]

    assert parser.feed(
        _encode_test_frame(1 << 34)
    ) == [0]  # Reserved bit is ignored.

    two_frames = (
        _encode_test_frame(1 << 0)
        + _encode_test_frame(1 << 33)
    )
    assert parser.feed(two_frames) == [1 << 0, 1 << 33]

    malformed = bytes(
        (FRAME_START, 0, 0, 0x20, 0, 0, 0, 0, FRAME_END)
    )
    assert parser.feed(malformed + _encode_test_frame(1 << 16)) == [
        1 << 16
    ]

    renderer = SensorRenderer(_asset_directory(), display_size=320)
    assert len(renderer.zone_overlays) == VALID_TOUCH_BITS
    assert renderer.render(0).size == (320, 320)
    assert renderer.render(0).getpixel((0, 0)) == (
        DEFAULT_DISPLAY_BACKGROUND
    )
    plain_background = Image.new(
        "RGBA",
        (320, 320),
        DEFAULT_DISPLAY_BACKGROUND,
    )
    assert renderer.render(0).tobytes() != plain_background.tobytes()
    assert renderer.render(1 << 0).tobytes() != renderer.render(0).tobytes()

    def overlay_center(index: int) -> tuple[float, float]:
        bbox = renderer.zone_overlays[index].getchannel("A").getbbox()
        assert bbox is not None
        left, top, right, bottom = bbox
        return (left + right) / 2, (top + bottom) / 2

    center = renderer.display_size / 2
    a1_x, a1_y = overlay_center(0)
    a2_x, _a2_y = overlay_center(1)
    c1_x, _c1_y = overlay_center(16)
    c2_x, _c2_y = overlay_center(17)
    d1_x, d1_y = overlay_center(18)

    assert a1_x > center and a1_y < center
    assert a2_x > a1_x  # Clockwise from upper-right toward the right.
    assert c1_x > center and c2_x < center
    assert abs(d1_x - center) < renderer.display_size * 0.1
    assert d1_y < center

    print("touch_test_tool self-test: OK")


def ui_smoke_test() -> None:
    root = tk.Tk()
    root.withdraw()
    app = TouchTestTool(root)
    root.update_idletasks()
    assert app.device_serial is None
    assert app.port_var.get() == ""
    assert len(app.renderer.zone_overlays) == VALID_TOUCH_BITS
    assert app.current_touch_bits == 0
    expected_background = tuple(
        round(value / 257)
        for value in root.winfo_rgb(app.program_background)
    ) + (255,)
    assert app.renderer.render(0).getpixel((0, 0)) == expected_background
    app.close()
    print("touch_test_tool UI smoke test: OK")


def main() -> int:
    if "--self-test" in sys.argv:
        self_test()
        return 0
    if "--ui-smoke-test" in sys.argv:
        ui_smoke_test()
        return 0

    root = tk.Tk()
    TouchTestTool(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

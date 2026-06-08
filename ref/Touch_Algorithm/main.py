import os
# 强制 pyqtgraph 使用 PyQt5 后端
os.environ["PYQTGRAPH_QT_LIB"] = "PyQt5"

import sys
import time
import csv
from datetime import datetime
import serial
import serial.tools.list_ports
import pyqtgraph as pg
from collections import deque
from PyQt5.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                             QHBoxLayout, QPushButton, QComboBox, QLabel, QMessageBox, QSplitter)
from PyQt5.QtCore import QTimer, Qt

# 导入你新建的判定类
from detector import ButtonDetector

class TouchOscilloscope(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("CapSense 实时波形与数据监视器 (PSoC 直连版)")
        self.resize(1300, 800)

        # ---------------- 自定义判定波形高度配置区 ----------------
        # 请根据你的实际 Diff 和 Deriv 的数值范围修改这里的值
        self.trigger_high_diff = 4000     # Diff 图中判定为 True 时的高度
        self.trigger_low_diff = 0        # Diff 图中判定为 False 时的高度
        
        self.trigger_high_deriv = 2000    # 导数图中判定为 True 时的高度
        self.trigger_low_deriv = -50     # 导数图中判定为 False 时的高度
        # ----------------------------------------------------------

        # ---------------- 核心数据结构 ----------------
        self.serial_port = None
        self.window_size = 3000  
        self.diff_buffer = deque([0] * self.window_size, maxlen=self.window_size)
        self.deriv_buffer = deque([0] * self.window_size, maxlen=self.window_size)
        
        # 用于存储判定状态的两个 Buffer，确保与源数据完全同频同帧
        self.trigger_buffer_diff = deque([self.trigger_low_diff] * self.window_size, maxlen=self.window_size)
        self.trigger_buffer_deriv = deque([self.trigger_low_deriv] * self.window_size, maxlen=self.window_size)
        
        self.buffer_3s = deque()

        # 算法需要的数据结构
        self.detector = ButtonDetector()           
        self.history_16 = deque([0]*16, maxlen=16) 
        self.setup_buffer = []                     
        self.setup_raw = 0.0                       
        self.is_setup_done = False                 

        self.is_running = False
        self.current_channel = 0  
        
        # 统计用变量
        self.hz_counter = 0
        self.last_time = time.time()

        # ---------------- 日志系统变量 ----------------
        self.is_logging = False
        self.log_file = None
        self.csv_writer = None

        # ---------------- UI 布局搭建 ----------------
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_hbox = QHBoxLayout(central_widget)
        left_vbox = QVBoxLayout()
        control_layout = QHBoxLayout()
        
        self.port_combo = QComboBox()
        self.refresh_ports()
        control_layout.addWidget(QLabel("串口号:"))
        control_layout.addWidget(self.port_combo)

        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "256000", "460800", "921600"])
        self.baud_combo.setCurrentText("115200") 
        control_layout.addWidget(QLabel("波特率:"))
        control_layout.addWidget(self.baud_combo)

        self.btn_scan = QPushButton("刷新串口")
        self.btn_scan.clicked.connect(self.refresh_ports)
        control_layout.addWidget(self.btn_scan)

        self.channel_combo = QComboBox()
        self.channel_combo.addItems([f"Channel {i}" for i in range(17)])
        self.channel_combo.currentIndexChanged.connect(self.on_config_changed)
        control_layout.addWidget(QLabel("选择通道:"))
        control_layout.addWidget(self.channel_combo)

        self.btn_start = QPushButton("打开串口并绘图")
        self.btn_start.clicked.connect(self.toggle_capture)
        control_layout.addWidget(self.btn_start)

        # --- 新增：日志记录按钮 ---
        self.btn_log = QPushButton("开始记录日志 (CSV)")
        self.btn_log.setCheckable(True)
        self.btn_log.clicked.connect(self.toggle_logging)
        control_layout.addWidget(self.btn_log)

        left_vbox.addLayout(control_layout)

        pg.setConfigOption('background', '#1e1e1e') 
        pg.setConfigOption('foreground', '#d4d4d4')
        
        splitter = QSplitter(Qt.Vertical)
        
        # Diff 波形图设置
        self.plot_diff = pg.PlotWidget()
        self.plot_diff.addLegend() 
        self.update_plot_title()
        self.plot_diff.setLabel('left', 'Diff Value')
        self.plot_diff.showGrid(x=True, y=True, alpha=0.3)
        self.curve_diff = self.plot_diff.plot(pen=pg.mkPen(color='#00ff00', width=2), name="Diff数据")
        self.curve_trigger_diff = self.plot_diff.plot(pen=pg.mkPen(color='#ff3333', width=1.5, style=Qt.DashLine), name="判定状态")
        
        # 导数波形图设置
        self.plot_deriv = pg.PlotWidget()
        self.plot_deriv.addLegend() 
        self.plot_deriv.setTitle("Raw 导数波形 (1阶差分)", size="12pt")
        self.plot_deriv.setLabel('left', 'Derivative')
        self.plot_deriv.setLabel('bottom', 'Samples')
        self.plot_deriv.showGrid(x=True, y=True, alpha=0.3)
        self.curve_deriv = self.plot_deriv.plot(pen=pg.mkPen(color='#ff00ff', width=2), name="1阶差分")
        self.curve_trigger_deriv = self.plot_deriv.plot(pen=pg.mkPen(color='#ff3333', width=1.5, style=Qt.DashLine), name="判定状态")
        
        self.plot_deriv.setXLink(self.plot_diff)

        splitter.addWidget(self.plot_diff)
        splitter.addWidget(self.plot_deriv)
        
        left_vbox.addWidget(splitter)
        main_hbox.addLayout(left_vbox, stretch=4)
        
        right_vbox = QVBoxLayout()
        right_vbox.setAlignment(Qt.AlignTop)
        
        def create_dashboard_label(title, color="#ffffff", height=75):
            lbl = QLabel(title)
            lbl.setFixedHeight(height)
            lbl.setStyleSheet(f"""
                QLabel {{
                    background-color: #2d2d2d;
                    color: {color};
                    font-size: 16px;
                    font-family: Consolas, monospace;
                    font-weight: bold;
                    border: 2px solid #3d3d3d;
                    border-radius: 8px;
                    padding: 8px;
                }}
            """)
            lbl.setAlignment(Qt.AlignCenter)
            return lbl

        self.lbl_sync_status = create_dashboard_label("串口同步状态\n等待数据", color="#aaaaaa")
        self.lbl_raw = create_dashboard_label("当前 Raw\n---", color="#00ff00")
        
        self.lbl_diff = create_dashboard_label("Diff\n(Raw-SetupRaw)", color="#ffaa00")
        self.lbl_button = create_dashboard_label("按键判定\n未按下", color="#aaaaaa")
        self.lbl_3s_fluc = create_dashboard_label("3s 平均波动\n---", color="#ff55ff")
        self.lbl_hz = create_dashboard_label("有效回报率\n--- Hz", color="#00aaff")

        right_vbox.addWidget(QLabel("<b>实时数据面板</b>"))
        right_vbox.addWidget(self.lbl_sync_status)
        right_vbox.addWidget(self.lbl_raw)
        right_vbox.addWidget(self.lbl_diff)
        right_vbox.addWidget(self.lbl_button) 
        right_vbox.addWidget(self.lbl_3s_fluc)
        right_vbox.addWidget(self.lbl_hz)
        
        main_hbox.addLayout(right_vbox, stretch=1)

        self.timer = QTimer()
        self.timer.timeout.connect(self.update_data)

    def refresh_ports(self):
        self.port_combo.clear()
        ports = serial.tools.list_ports.comports()
        for port in ports:
            self.port_combo.addItem(port.device)

    def update_plot_title(self):
        self.plot_diff.setTitle(f"Channel {self.current_channel} 实时电容 Diff 值", size="12pt")

    # --- 新增：日志控制函数 ---
    def toggle_logging(self):
        if self.btn_log.isChecked():
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"capsense_log_ch{self.current_channel}_{timestamp}.csv"
            try:
                self.log_file = open(filename, 'w', newline='', encoding='utf-8')
                self.csv_writer = csv.writer(self.log_file)
                self.csv_writer.writerow(["Timestamp", "Channel", "Raw", "SetupRaw", "Diff", "Deriv", "IsPressed"])
                
                self.is_logging = True
                self.btn_log.setText("停止记录日志")
                self.btn_log.setStyleSheet("background-color: #006400; color: white;")
            except Exception as e:
                QMessageBox.critical(self, "日志错误", f"无法创建日志文件:\n{str(e)}")
                self.btn_log.setChecked(False)
        else:
            self.is_logging = False
            if self.log_file:
                self.log_file.close()
                self.log_file = None
            self.btn_log.setText("开始记录日志 (CSV)")
            self.btn_log.setStyleSheet("")

    def on_config_changed(self):
        # 切换通道前自动停止记录日志，防止数据串流
        if self.is_logging:
            self.btn_log.setChecked(False)
            self.toggle_logging()

        self.current_channel = self.channel_combo.currentIndex()
        self.update_plot_title()
        
        self.diff_buffer = deque([0] * self.window_size, maxlen=self.window_size)
        self.deriv_buffer = deque([0] * self.window_size, maxlen=self.window_size)
        
        self.trigger_buffer_diff = deque([self.trigger_low_diff] * self.window_size, maxlen=self.window_size)
        self.trigger_buffer_deriv = deque([self.trigger_low_deriv] * self.window_size, maxlen=self.window_size)
        
        self.buffer_3s.clear()
        self.history_16 = deque([0]*16, maxlen=16)
        
        self.setup_buffer.clear()
        self.setup_raw = 0.0
        self.is_setup_done = False
        self.lbl_button.setText("系统校准中...\n等待30帧")
        self.lbl_button.setStyleSheet("QLabel { background-color: #2d2d2d; color: #ffaa00; font-size: 16px; font-weight: bold; border: 2px solid #3d3d3d; border-radius: 8px; padding: 8px; }")

    def toggle_capture(self):
        if not self.is_running:
            port = self.port_combo.currentText()
            baud = int(self.baud_combo.currentText())
            if not port: return
            try:
                self.serial_port = serial.Serial(port, baud, timeout=0)
                self.serial_port.reset_input_buffer()
                self.is_running = True
                
                self.hz_counter = 0
                self.last_time = time.time()
                self.on_config_changed()
                
                self.btn_start.setText("停止并关闭串口")
                self.btn_start.setStyleSheet("background-color: #8b0000; color: white;")
                self.timer.start(5) 
            except Exception as e:
                QMessageBox.critical(self, "串口错误", f"无法打开串口 {port}:\n{str(e)}")
        else:
            self.timer.stop()
            self.is_running = False
            if self.serial_port and self.serial_port.is_open:
                self.serial_port.close()
            self.btn_start.setText("打开串口并绘图")
            self.btn_start.setStyleSheet("")

    def update_data(self):
        if not self.serial_port or not self.serial_port.is_open: return
        try:
            has_new_data = False
            current_time = time.time()
            
            while self.serial_port.in_waiting >= 35:
                head = self.serial_port.read(1)
                
                if head[0] == 0x01:
                    payload = self.serial_port.read(34)
                    
                    if len(payload) == 34:
                        ch = self.current_channel
                        high_idx = ch * 2
                        low_idx = ch * 2 + 1
                        val = (payload[high_idx] << 8) | payload[low_idx]
                        
                        # ---------------- 核心数据计算与判定 ----------------
                        diff = 0
                        deriv = 0
                        is_pressed = False  

                        if not self.is_setup_done:
                            self.setup_buffer.append(val)
                            
                            if len(self.setup_buffer) == 30:
                                self.setup_raw = sum(self.setup_buffer) / 30.0
                                self.is_setup_done = True
                                
                                self.lbl_button.setText("按键判定\n未按下")
                                self.lbl_button.setStyleSheet("QLabel { background-color: #2d2d2d; color: #aaaaaa; font-size: 16px; font-weight: bold; border: 2px solid #3d3d3d; border-radius: 8px; padding: 8px; }")

                        else:
                            deriv = val - self.history_16[0]
                            diff = val - self.setup_raw
                            
                            is_pressed = self.detector.process_frame(
                                channel=ch,
                                current_val=val,
                                history_16=list(self.history_16),
                                diff=diff,
                                setup_raw=self.setup_raw,
                                deriv=deriv
                            )

                            if is_pressed:
                                self.lbl_button.setText("按键判定\n!! 按下 !!")
                                self.lbl_button.setStyleSheet("QLabel { background-color: #8b0000; color: #ffffff; font-size: 16px; font-weight: bold; border: 2px solid #ff5555; border-radius: 8px; padding: 8px; }")
                            else:
                                self.lbl_button.setText("按键判定\n未按下")
                                self.lbl_button.setStyleSheet("QLabel { background-color: #2d2d2d; color: #aaaaaa; font-size: 16px; font-weight: bold; border: 2px solid #3d3d3d; border-radius: 8px; padding: 8px; }")

                        # --- 新增：向 CSV 写入当前帧数据 ---
                        if self.is_logging and self.csv_writer:
                            self.csv_writer.writerow([
                                f"{current_time:.6f}",  # 时间戳
                                ch,                     # 通道号
                                val,                    # 当前 Raw 值
                                f"{self.setup_raw:.2f}",# 基准值 SetupRaw
                                f"{diff:.2f}",          # 差分 Diff
                                f"{deriv:.2f}",         # 导数 Deriv
                                int(is_pressed)         # 判定结果 (1或0)
                            ])
                        # -----------------------------------

                        # ================= 完全同步更新所有 Buffer =================
                        diff_status_val = self.trigger_high_diff if is_pressed else self.trigger_low_diff
                        deriv_status_val = self.trigger_high_deriv if is_pressed else self.trigger_low_deriv

                        self.trigger_buffer_diff.append(diff_status_val)
                        self.trigger_buffer_deriv.append(deriv_status_val)
                        self.history_16.appendleft(val) 
                        self.deriv_buffer.append(deriv)
                        self.diff_buffer.append(diff)  
                        # ==========================================================
                        
                        self.buffer_3s.append((current_time, val))
                        
                        self.hz_counter += 1
                        has_new_data = True
                        
                        self.lbl_sync_status.setText("状态\n数据同步良好")
                        self.lbl_sync_status.setStyleSheet("QLabel { background-color: #2d2d2d; color: #00ff00; font-size: 16px; font-weight: bold; border: 2px solid #3d3d3d; border-radius: 8px; padding: 8px; }")
                        
                        self.lbl_raw.setText(f"当前 Raw\n{val}")
                        self.lbl_diff.setText(f"Diff\n{diff:.1f}")

                else:
                    self.lbl_sync_status.setText("状态\n错位,寻轨中...")
                    self.lbl_sync_status.setStyleSheet("QLabel { background-color: #2d2d2d; color: #ffaa00; font-size: 16px; font-weight: bold; border: 2px solid #3d3d3d; border-radius: 8px; padding: 8px; }")

            # ---------------- 界面刷新逻辑 ----------------
            if has_new_data:
                self.curve_diff.setData(list(self.diff_buffer))
                self.curve_deriv.setData(list(self.deriv_buffer))
                self.curve_trigger_diff.setData(list(self.trigger_buffer_diff))
                self.curve_trigger_deriv.setData(list(self.trigger_buffer_deriv))
                
                while self.buffer_3s and (current_time - self.buffer_3s[0][0] > 3.0):
                    self.buffer_3s.popleft()
                
                if len(self.buffer_3s) > 1:
                    vals = [v for t, v in self.buffer_3s]
                    mean_val = sum(vals) / len(vals)
                    avg_fluctuation = sum(abs(v - mean_val) for v in vals) / len(vals)
                    self.lbl_3s_fluc.setText(f"3s 平均波动\n{avg_fluctuation:.1f}")
                else:
                    self.lbl_3s_fluc.setText("3s 平均波动\n0.0")

            dt = current_time - self.last_time
            if dt >= 1.0:
                hz = self.hz_counter / dt
                self.lbl_hz.setText(f"有效回报率\n{hz:.1f} Hz")
                self.hz_counter = 0
                self.last_time = current_time

        except Exception as e:
            print(f"数据处理报错: {e}")

    def closeEvent(self, event):
        # 退出前安全关闭日志
        if self.is_logging:
            self.btn_log.setChecked(False)
            self.toggle_logging()

        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        event.accept()

if __name__ == '__main__':
    app = pg.mkQApp("CapSense Oscilloscope")
    window = TouchOscilloscope()
    window.show()
    try:
        app.exec_()
    except SystemExit:
        print("波形上位机已关闭。")
import serial
import time
import threading

# ==========================================
# 配置区
# ==========================================
PN532_PORT = 'COM105'  # 替换为你的 PN532 串口
GAME_PORT = 'COM11'    # 替换为连接游戏的串口
BAUDRATE = 115200

# ==========================================
# 共享状态管理器
# ==========================================
class CardState:
    def __init__(self):
        self.lock = threading.Lock()
        self.is_present = False
        self.block_data = bytes(16)
        self.last_read_time = 0.0

    def update_card(self, block_data):
        with self.lock:
            self.is_present = True
            self.block_data = block_data
            self.last_read_time = time.time()

    def check_presence(self, timeout=2.5):
        """检查卡片是否在线，防抖时间匹配物理读取频率"""
        with self.lock:
            if self.is_present and (time.time() - self.last_read_time > timeout):
                self.is_present = False
            return self.is_present

    def get_block_data(self):
        with self.lock:
            return self.block_data

# ==========================================
# 模块 1: PN532 读卡器控制器
# ==========================================
class PN532Controller:
    def __init__(self, port, baudrate, shared_state):
        self.shared_state = shared_state
        self.tx_lock = threading.Lock() # 【新增】发送锁，防止多线程同时写入串口导致包损坏
        
        try:
            self.serial = serial.Serial(port, baudrate, timeout=0.1)
            print(f"[PN532] 成功打开串口: {port}")
        except Exception as e:
            print(f"[PN532] 打开串口失败: {e}")
            self.running = False
            return
            
        self.running = True
        self.read_state = "IDLE"  
        self.state_timestamp = time.time() # 【新增】状态时间戳，用于超时恢复
        self.current_uid = []
        self.last_card_time = 0   

    def set_state(self, new_state):
        """【新增】统一管理状态切换，记录时间戳"""
        self.read_state = new_state
        self.state_timestamp = time.time()

    def calculate_checksum(self, data_bytes):
        return (256 - (sum(data_bytes) % 256)) & 0xFF

    def send_data(self, payload, silent=True):
        length = len(payload)
        length_checksum = self.calculate_checksum([length])
        payload_checksum = self.calculate_checksum(payload)
        
        packet = [0xFF, length, length_checksum] + payload + [payload_checksum]
        packet = [0x00, 0x00] + packet 
        
        # 【核心修复】使用线程锁确保完整的数据包被一次性写入
        if hasattr(self, 'serial') and self.serial.is_open:
            with self.tx_lock:
                self.serial.write(bytes(packet))
            if not silent:
                print(f"[PN532 TX] {' '.join([f'{x:02X}' for x in packet])}")

    def receive_loop(self):
        buffer = []
        while self.running:
            if hasattr(self, 'serial') and self.serial.in_waiting:
                data = self.serial.read(self.serial.in_waiting)
                buffer.extend(data)
                
                while len(buffer) >= 4:
                    if buffer[0] != 0xFF:
                        buffer.pop(0)
                        continue
                    
                    length = buffer[1]
                    length_chk = buffer[2]
                    
                    if (length + length_chk) & 0xFF != 0:
                        buffer.pop(0)
                        continue
                    
                    total_packet_len = 3 + length + 1 
                    if len(buffer) < total_packet_len:
                        break 
                        
                    payload = buffer[3 : 3 + length]
                    payload_chk = buffer[3 + length]
                    
                    if (sum(payload) + payload_chk) & 0xFF != 0:
                        buffer.pop(0)
                        continue
                        
                    if len(payload) > 0:
                        if payload == [0xD5, 0x4B, 0x00]:
                            pass
                            
                        elif payload[0] == 0xD5 and payload[1] == 0x4B:
                            target_count = payload[2] if len(payload) > 2 else 0
                            
                            if target_count > 0 and (time.time() - self.last_card_time > 2.0): 
                                if len(payload) > 10 and payload[4] == 0x14:
                                    # FeliCa 原生卡
                                    self.last_card_time = time.time()
                                    idm = payload[6:14]
                                    idm_hex = ''.join([f'{x:02X}' for x in idm])
                                    access_code = str(int(idm_hex, 16)).zfill(20)
                                    
                                    print(f"[PN532] 读取原生卡片 -> 卡号: {access_code}")
                                    self.shared_state.update_card(bytes.fromhex(f"000000000000{access_code}"))
                                    self.set_state("IDLE") # 确保复位
                                    
                                else:
                                    # Mifare 兼容卡
                                    uid_length = payload[7] if len(payload) > 7 else 0
                                    if len(payload) >= 8 + uid_length:
                                        self.current_uid = payload[8 : 8 + uid_length]
                                        self.set_state("AUTHING")
                                        auth_cmd = [0xD4, 0x40, 0x01, 0x60, 0x02, 0x57, 0x43, 0x43, 0x46, 0x76, 0x32] + self.current_uid
                                        self.send_data(auth_cmd)
                                        
                        elif payload[0] == 0xD5 and payload[1] == 0x41:
                            status = payload[2] if len(payload) > 2 else 0xFF
                            
                            if self.read_state == "AUTHING":
                                if status == 0x00:
                                    self.set_state("READING")
                                    read_cmd = [0xD4, 0x40, 0x01, 0x30, 0x02]
                                    self.send_data(read_cmd)
                                else:
                                    print("[PN532] ❌ 认证失败!")
                                    self.set_state("IDLE")
                                    self.last_card_time = time.time()
                                    
                            elif self.read_state == "READING":
                                if status == 0x00 and len(payload) >= 19: 
                                    block_data = payload[3:19]
                                    access_code = ''.join([f'{x:02X}' for x in block_data])[-20:]
                                    self.last_card_time = time.time()
                                    
                                    print(f"[PN532] 读取 Aime 兼容卡 -> 卡号: {access_code}")
                                    self.shared_state.update_card(bytes(block_data))
                                else:
                                    print("[PN532] ❌ 读取 Block 2 失败!")
                                    
                                self.set_state("IDLE")

                    buffer = buffer[total_packet_len:]
            time.sleep(0.005)

    def start(self):
        if hasattr(self, 'running') and self.running:
            threading.Thread(target=self.receive_loop, daemon=True).start()

    def stop(self):
        self.running = False
        if hasattr(self, 'serial') and self.serial.is_open:
            self.serial.close()


# ==========================================
# 模块 2: 游戏通讯模拟器
# ==========================================
class GameProtocolHandler:
    def __init__(self, port, baudrate, shared_state):
        self.shared_state = shared_state
        self.running = False
        self.empty_block = bytes(16)
        try:
            self.ser = serial.Serial(port, baudrate, timeout=0.1)
            print(f"[游戏通讯] 成功打开串口: {port}")
            self.running = True
        except Exception as e:
            print(f"[游戏通讯] 打开串口失败: {e}")

    def read_packet(self):
        if self.ser.in_waiting == 0: return None
        while True:
            b = self.ser.read(1)
            if not b: return None
            if b[0] == 0xE0: break

        packet = bytearray()
        frame_len, checksum, escape = -1, 0, False

        while True:
            b = self.ser.read(1)
            if not b: return None
            v = b[0]

            if v == 0xE0:
                packet, frame_len, checksum, escape = bytearray(), -1, 0, False
                continue
            if v == 0xD0:
                escape = True
                continue
            if escape:
                v = (v + 1) & 0xFF
                escape = False

            if frame_len == -1:
                frame_len = v
                checksum = v
                packet.append(v)
                continue

            packet.append(v)
            if len(packet) == frame_len + 1:
                if checksum == v: return packet
                else: return None
            else:
                checksum = (checksum + v) & 0xFF

    def handle_packet(self, packet):
        if len(packet) < 5: return
        addr, seq_no, cmd, payload_len = packet[1], packet[2], packet[3], packet[4]
        payload = packet[5:5+payload_len]
        res_cmd, res_status, res_payload, send_reply = cmd, 0x00, bytearray(), True

        if cmd == 0x30:
            res_payload = b"\x94"
        elif cmd == 0x32:
            res_payload = b"837-15396"
        elif cmd == 0xf0:
            res_payload = b"000-00000\xFF\x11\x40"
        elif cmd == 0x42: 
            if self.shared_state.check_presence():
                res_payload = bytearray([1, 0x10, 4, 0x01, 0x02, 0x03, 0x04])
            else:
                res_payload = bytearray([0])
        elif cmd == 0x52: 
            if payload[4] == 2:
                current_block = self.shared_state.get_block_data()
                res_payload = current_block
                hex_data = ''.join([f'{x:02X}' for x in current_block])
                print(f"[交互] 游戏成功拉取数据 -> {hex_data[-20:]}")
            else:
                res_payload = self.empty_block
        elif cmd in (0x81, 0x82):
            send_reply = False
        elif cmd == 0x61:
            res_status = 0x20
        elif cmd == 0x64:
            res_status = 0x08

        if send_reply:
            self.send_response(addr, seq_no, res_cmd, res_status, res_payload)

    def send_response(self, addr, seq_no, cmd, status, payload):
        frame_len = 6 + len(payload)
        packet = bytearray([frame_len, addr, seq_no, cmd, status, len(payload)])
        packet.extend(payload)
        checksum = sum(packet) & 0xFF

        out = bytearray([0xE0])
        for b in packet:
            if b == 0xE0 or b == 0xD0:
                out.extend([0xD0, b - 1])
            else:
                out.append(b)

        if checksum == 0xE0 or checksum == 0xD0:
            out.extend([0xD0, checksum - 1])
        else:
            out.append(checksum)

        self.ser.write(out)

    def receive_loop(self):
        while self.running:
            packet = self.read_packet()
            if packet:
                self.handle_packet(packet)
            time.sleep(0.005)

    def start(self):
        if self.running:
            threading.Thread(target=self.receive_loop, daemon=True).start()

    def stop(self):
        self.running = False
        if hasattr(self, 'ser') and self.ser.is_open:
            self.ser.close()


# ==========================================
# 主程序入口
# ==========================================
if __name__ == '__main__':
    print("=======================================")
    print("     Aime Reader 整合通讯桥接器")
    print("=======================================")
    
    shared_state = CardState()
    pn532 = PN532Controller(PN532_PORT, BAUDRATE, shared_state)
    game_sim = GameProtocolHandler(GAME_PORT, BAUDRATE, shared_state)

    if not pn532.running or not game_sim.running:
        print("\n[错误] 串口初始化失败，退出。")
        pn532.stop()
        game_sim.stop()
        exit(1)

    pn532.start()
    game_sim.start()
    
    try:
        pn532.send_data([0xD4, 0x14, 0x01])
        time.sleep(0.1)
        pn532.send_data([0xD4, 0x14, 0x01, 0x14, 0x01])
        time.sleep(0.1)
        pn532.send_data([0xD4, 0x32, 0x05, 0xFF, 0x01, 0x01])
        time.sleep(0.1)
        pn532.send_data([0xD4, 0x32, 0x01, 0x01])
        time.sleep(0.1)
        
        print("\n[系统] 初始化完成，系统稳定运行中... (按 Ctrl+C 退出)")
        
        polling_cmd_long = [0xD4, 0x4A, 0x01, 0x01, 0x00, 0xFF, 0xFF, 0x01, 0x00]
        polling_cmd_short = [0xD4, 0x4A, 0x01, 0x00]
        counter = 0
        
        while True:
            # 【核心修复】状态机超时看门狗 (Watchdog)
            # 如果状态卡在 AUTHING 或 READING 超过 0.5 秒没有恢复（比如丢包了）
            # 强制复位状态，防止读卡器永远死锁
            if pn532.read_state != "IDLE" and (time.time() - pn532.state_timestamp > 0.5):
                print(f"[警告] PN532 状态 '{pn532.read_state}' 响应超时，强制重置状态机。")
                pn532.set_state("IDLE")

            if pn532.read_state == "IDLE":
                if counter % 6 == 0:
                    pn532.send_data(polling_cmd_short)
                else:
                    pn532.send_data(polling_cmd_long)
                counter += 1
                
            time.sleep(0.033)
            
    except KeyboardInterrupt:
        print("\n[系统] 正在关闭端口...")
    finally:
        pn532.stop()
        game_sim.stop()
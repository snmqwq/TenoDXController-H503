class ButtonDetector:
    def __init__(self):
        # 如果你未来需要记录一些长期的判定状态，可以在这里初始化
        self.is_pressed = False
        # self.raw_diff_up = 0
        # self.raw_diff_up_count = -1

        self.deriv_down_count = -1
        # self.deriv_down_h = -1

        self.up = 0; # 0:无 1:上升 -1:下降
        #self.deriv_down_diff = -1

        # 补全缺失的状态锁初始化
        self.lock_releasing = False

        

    # 关键修改：加上 self，并将通道号参数命名为 channel
    def process_frame(self, channel, current_val, history_16, diff, setup_raw, deriv):
        """
        处理单帧数据的判定逻辑
        :param ch:          通道号
        :param current_val: 当前帧的原始数据 (Raw)
        :param history_16:  历史16帧数据数组。history_16[0]是上一帧，history_16[1]是上上帧...
        :param diff:        当前帧与基准值的差值 (current_val - setup_raw)
        :param setup_raw:   启动时前30帧的平均基准值
        :param deriv:       当前帧的一阶导数 (current_val - 上一帧)
        :return: bool       True 代表判定为按下，False 代表未按下
        """
        # 继承上一帧的状态，避免闪断
        on = False

        match channel:
            case 9: #C
                if diff > 18 or deriv > 7:
                    if deriv < -8:
                        on = False
                    else:
                        on = True
                elif diff < 10:
                    on = False
            case 1 | 5 | 10 | 14: # E
                if diff > 24:
                    on = True
                if deriv < -16:
                    on = False
            case 3 | 7 | 12 | 16: # B
                if diff > 4:
                    on = True
                if deriv < -2:
                    on = False
            case 2 | 3 | 11 | 15: # D
                if diff > 3:
                    on = True
                if deriv < -4:
                    on = False


            case 0 | 4 | 8 | 13: # A 区 (大自电容区块)
                on = self.is_pressed

                on_default_diff = 550

                if diff > on_default_diff + 400 or diff < on_default_diff - 400:
                    self.up = 0
                
                on_diff = on_default_diff
                match self.up:
                    case 0:
                        on_diff = on_default_diff
                    case 1:
                        on_diff = 350  # 确认上升后，降低保持门槛
                    case -1:
                        on_diff = 700  # 确认下降时，提高判定门槛以加速断开

                last_diff = (history_16[0] - setup_raw) 
                if last_diff < on_default_diff and diff >= on_default_diff:
                    self.up = 1
                elif last_diff >= on_default_diff and diff < on_default_diff:
                    self.up = -1
                
                # [核心修改 1] 将绝对安全线从 900 提高到 1200
                # 作用：在连击的"前半段"（速度依然很快时）就提前捕获释放信号，完美避开尾部的减速期。
                absolute_safe_diff = 1200 

                # ==========================================
                # 机制 1：状态锁的动态解开条件
                # ==========================================
                if diff < 200: 
                    self.lock_releasing = False
                
                if self.lock_releasing and deriv > 150 and diff > on_diff:
                    self.lock_releasing = False

                # ==========================================
                # 机制 2：常规按下判定与尾巴拦截
                # ==========================================
                if diff > on_diff:
                    if self.deriv_down_count > 0:
                        self.deriv_down_count -= 1
                    elif self.lock_releasing:
                        pass 
                    else:
                        # [核心修改 2] 抑制远距离触发
                        # 作用：保持单指和快敲不被影响，但将悬停慢速接近的拦截线从 800 抬高到 1000，速度容忍略微放宽到 15。
                        # 这样即使是大面积在远距离悬空，只要动作缓慢，就不会触发。
                        if not self.is_pressed and deriv < 15 and diff < 1000:
                            pass 
                        else:
                            on = True
                else:
                    if self.deriv_down_count > 0:
                        self.deriv_down_count -= 1
                    
                    if self.is_pressed and diff > 200:
                        self.lock_releasing = True
                        
                    on = False
                
                # ==========================================
                # 机制 3：快抬手/连击释放判定 (修复划断与连击)
                # ==========================================
                # [核心修改 3] 配合 1200 的高阈值，稍微收紧基础跌落速度，防止滑动边缘误触释放
                deriv_down = -250 
                last3_diff = (history_16[2] - setup_raw) 
                
                if last3_diff > 2700:
                    deriv_down = -400 # 从 -350 收紧到 -400

                if deriv < deriv_down:
                    # 情形 1：断崖式下跌（物理连击抬手与防划断保护）
                    if deriv < -800:
                        # 维持原样，保护划断！滑动时 Diff 会停在 1200~1400 左右，进不来这里。
                        if diff < 1000:
                            on = False
                            self.deriv_down_count = 3
                            if diff > 500:
                                self.lock_releasing = True
                                
                    # 情形 2：正常快抬手/连击下探松开
                    # 此时连击 Diff 穿过 1200 时，Deriv 通常在 -500 到 -700，满足条件，成功释放！
                    # 而滑动穿过 1200 时，已经平缓（Deriv 仅有 -50 ~ -150），会被 deriv < -250 完美挡住！
                    elif diff < absolute_safe_diff:
                        on = False
                        self.deriv_down_count = 3
                        if diff > 200:
                            self.lock_releasing = True

        # 更新类内部状态，用于下一帧的上升/下降沿判定
        self.is_pressed = on
        return on
/*
 * 本机验证程序：直接编译真实的 touch_mode.c / cdc_manager.c / mai2touch.c，
 * 只 stub 掉硬件相关部分（TinyUSB / HAL / touch_pipeline / tenodata 少数函数）。
 *
 * 目的：在不能烧录的情况下验证
 *   1. `{RSET}` / `{HALT}` / `{STAT}` 的会话行为是否符合 docs/Mai2TouchREADME.md
 *   2. raw 特征 (0xAA 0x01) 与 Mai2Touch 特征 ({ + 4 字节 + }) 的自动切换
 *   3. {STAT} 之后"先发空白帧、不等 PSoC 数据"
 *   4. 触发切换的那条命令不会丢 (STAT 必须开始推流 / LAr2 必须回显)
 *   5. raw 帧里偶然出现 0x7B (校验和) 不会误切到 Mai2Touch
 *   6. Magic 强制模式后不再自动切换
 *
 * 构建: tests/build_native_test.ps1
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- Tusb stub 的存储 ---------- */
static uint8_t g_rx[4096];
static int g_rx_len = 0;
static uint8_t g_tx[4096];
static int g_tx_len = 0;

uint8_t *tusb_stub_rx_buffer(void) { return g_rx; }
int *tusb_stub_rx_len(void) { return &g_rx_len; }
uint8_t *tusb_stub_tx_buffer(void) { return g_tx; }
int *tusb_stub_tx_len(void) { return &g_tx_len; }

/* ---------- HAL stub ---------- */
uint32_t native_test_tick = 0;

/* ---------- touch_pipeline stub ---------- */
static bool g_pipeline_ready = false;
static int g_pipeline_init_count = 0;

void touch_pipeline_init(void) {
    g_pipeline_ready = false;
    g_pipeline_init_count++;
}
void touch_pipeline_set_device_masks(uint8_t connected_mask, uint8_t forced_mask) {
    (void)connected_mask; (void)forced_mask;
}
bool touch_pipeline_is_ready(void) { return g_pipeline_ready; }
void touch_pipeline_feed(int psoc_index, const uint8_t *raw_35) {
    (void)psoc_index; (void)raw_35;
}

/* ---------- tenodata stub (只 stub touch_mode 用到的两个) ---------- */
static int g_clear_override_count = 0;
static int g_reconfigure_count = 0;

void tenodata_clear_host_config_override(void) { g_clear_override_count++; }
void tenodata_request_reconfigure(void) { g_reconfigure_count++; }

/* ---------- 被测模块（真实源码） ---------- */
#include "cdc_manager.c"
#include "touch_mode.c"
#include "mai2touch.c"

/* ---------- 测试辅助 ---------- */
static int g_failures = 0;

static void check(bool ok, const char *what) {
    printf("  %s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) g_failures++;
}

static void rx_push(const void *data, int len) {
    if (g_rx_len + len > (int)sizeof(g_rx)) { printf("  rx 溢出\n"); return; }
    memcpy(g_rx + g_rx_len, data, (size_t)len);
    g_rx_len += len;
}

static void rx_push_byte(uint8_t b) { rx_push(&b, 1); }

static int frames_of(int size) { return g_tx_len / size; }

static void report(const char *tag) {
    printf("  %-34s mode=%-9s tx=%d 字节 (9B x %d, 6B x %d)\n",
           tag,
           cdc_manager_is_mai2touch_active() ? "MAI2TOUCH" : "RAW",
           g_tx_len, frames_of(9), frames_of(6));
}

/*
 * 复刻真实主循环的一个 tick (touch_app_task 的顺序):
 *   1. touch_mode_task()   —— 消化上一拍识别出的切换
 *   2. tenodata_task()     —— 非 Mai2Touch 模式下排空 CDC RX 并喂识别器
 *   3. mai2touch_task()    —— 仅 Mai2Touch 模式活跃; 内部自己排空 CDC RX
 * 两个消费者**按模式二选一**, 与分析结论一致 (Mai2Touch 模式独占 CDC RX)。
 */
static void tenodata_task_stub(void) {
    if (cdc_manager_is_mai2touch_active()) return;

    while (g_rx_len > 0) {
        uint8_t b = g_rx[0];
        memmove(g_rx, g_rx + 1, (size_t)(--g_rx_len));

        touch_mode_observe_byte(b);

        if (touch_mode_pending_to_mai2touch() ||
            cdc_manager_is_mai2touch_active()) {
            return;
        }
    }
}

static void run_ticks(int ticks) {
    for (int i = 0; i < ticks; i++) {
        touch_mode_task();
        tenodata_task_stub();
        mai2touch_task();
        native_test_tick += 2;
    }
}

static void push_cmd(const char *text) {
    uint8_t frame[8];
    int n = 0;
    frame[n++] = 0x7B;
    for (const char *p = text; *p && n < 7; p++) frame[n++] = (uint8_t)*p;
    frame[n++] = 0x7D;
    rx_push(frame, n);
}

/* raw 扫描参数帧: 0xAA 0x01 + 136 字节 payload + 1 字节校验和 */
static void push_raw_frame(int checksum_override) {
    uint8_t frame[139];
    int sum = 0;

    frame[0] = 0xAA;
    frame[1] = 0x01;
    /* 真实的 res/mod/sns/div 都 <= 16, 这决定了 payload 里不可能出现 '{' */
    for (int i = 0; i < 136; i++) frame[2 + i] = (uint8_t)((i % 5) + 2);
    for (int i = 0; i < 138; i++) sum += frame[i];

    frame[138] = (checksum_override < 0) ? (uint8_t)(sum & 0xFF)
                                         : (uint8_t)checksum_override;
    rx_push(frame, 139);
}

int main(void) {
    printf("=== 场景 1: 游戏接入 (raw 待机时收到 {RSET}) ===\n");

    cdc_manager_init();
    touch_mode_init();
    mai2touch_init();

    g_tx_len = 0;
    report("上电初始 (自动模式, 待机)");
    check(!cdc_manager_is_mai2touch_active(), "上电默认走 raw 流程");
    check(cdc_manager_is_auto_mode(), "上电处于自动识别模式");

    push_cmd("RSET");
    run_ticks(1);
    check(!cdc_manager_is_mai2touch_active(), "识别当拍模式还没变 (切换在下一拍)");
    check(touch_mode_pending_to_mai2touch(), "已标记待切换到 Mai2Touch");
    check(g_rx_len == 0, "整条命令被 raw 侧收下用于语法校验");

    run_ticks(1);
    report("raw 收到 {RSET} 后");
    check(cdc_manager_is_mai2touch_active(), "自动切换到 MAI2TOUCH");
    check(g_clear_override_count > 0, "切换时清除了上位机参数覆盖");
    check(g_tx_len == 0, "{RSET} 无需回复, 切换后保持静默");

    printf("\n=== 场景 2: 配置期回显 (34 条 ratio / sens 靠回显推进) ===\n");
    g_tx_len = 0;
    push_cmd("LAr2");               /* L + A1(0x41) + r + 0x32(ratio 固定值) */
    run_ticks(1);
    {   /* L + E8(0x62) + k + 0x14(默认档 sensitivity, 控制字符) */
        static const uint8_t le8k[] = {0x7B, 0x4C, 0x62, 0x6B, 0x14, 0x7D};
        rx_push(le8k, (int)sizeof(le8k));
    }
    run_ticks(1);
    push_cmd("RAr2");               /* R = 2P */
    run_ticks(1);
    report("回显 3 条配置命令");
    {
        static const uint8_t e1[] = {0x28, 0x4C, 0x41, 0x72, 0x32, 0x29};
        static const uint8_t e2[] = {0x28, 0x4C, 0x62, 0x6B, 0x14, 0x29};
        static const uint8_t e3[] = {0x28, 0x52, 0x41, 0x72, 0x32, 0x29};
        check(g_tx_len == 18, "恰好 3 条 6 字节回显 (之间无多余字节)");
        check(memcmp(g_tx, e1, 6) == 0 && memcmp(g_tx + 6, e2, 6) == 0 &&
              memcmp(g_tx + 12, e3, 6) == 0,
              "回显逐字节正确 (含控制字符的 sens 值也原样回显)");
    }

    printf("\n=== 场景 3: {STAT} 立刻发空白帧, {HALT} 立刻静默 ===\n");
    g_tx_len = 0;
    g_pipeline_ready = false;
    push_cmd("STAT");
    run_ticks(1);
    report("收到 {STAT} (管线未就绪)");
    {
        static const uint8_t blank[] = {0x28, 0, 0, 0, 0, 0, 0, 0, 0x29};
        check(g_tx_len == 9, "立即发出 1 个 9 字节帧 (不等 PSoC 数据)");
        check(g_tx_len == 9 && memcmp(g_tx, blank, 9) == 0, "内容为空白帧 (全 0)");
    }

    g_tx_len = 0;
    for (int i = 0; i < 5; i++) {
        native_test_tick += 4;
        mai2touch_task();
    }
    check(g_tx_len == 45, "未就绪期间按 4ms 持续发空白帧");

    g_tx_len = 0;
    g_pipeline_ready = true;
    /* 取 4 个跨越字节边界的通道 (全局通道号 = 摊平序号, A1 = 0):
     *   num 0  (A1) -> b0 bit0 = 0x01
     *   num 5  (A6) -> b1 bit0 = 0x01
     *   num 29 (E4) -> b5 bit4 = 0x10
     *   num 33 (E8) -> b6 bit3 = 0x08   (每字节只用低 5 位) */
    *mai2touch_get_touch_bits() = (1ULL << 0) | (1ULL << 5) |
                                  (1ULL << 29) | (1ULL << 33);
    native_test_tick += 4;
    mai2touch_task();
    printf("  数据帧:");
    for (int i = 0; i < g_tx_len; i++) printf(" %02X", g_tx[i]);
    printf("\n");
    {
        static const uint8_t expect[] =
            {0x28, 0x01, 0x01, 0x00, 0x00, 0x00, 0x10, 0x08, 0x29};
        check(g_tx_len == 9 && memcmp(g_tx, expect, 9) == 0,
              "触摸位打包正确 (34 位摊平, 每 5 位一字节)");
    }

    g_tx_len = 0;
    push_cmd("HALT");
    run_ticks(1);
    for (int i = 0; i < 20; i++) {
        native_test_tick += 4;
        mai2touch_task();
    }
    report("收到 {HALT} 后再跑 20 个发送周期");
    check(g_tx_len == 0, "{HALT} 之后完全静默 (游戏靠读超时进配置模式)");

    printf("\n=== 场景 4: 上位机发 raw 帧 -> 自动切回 raw ===\n");
    g_tx_len = 0;
    {
        uint8_t prefix[2] = {0xAA, 0x01};
        rx_push(prefix, 2);
    }
    run_ticks(1);
    report("mai2touch 读到 0xAA 0x01");
    check(touch_mode_pending_to_raw(), "已标记待切换到 raw");
    check(cdc_manager_is_mai2touch_active(), "识别当拍仍在 Mai2Touch 侧");

    {
        int clear_before = g_clear_override_count;
        int reconf_before = g_reconfigure_count;
        rx_push_byte(0x0C);         /* 帧体跟着到, 必须原样留给 raw 侧 */
        run_ticks(1);
        report("下一拍切回 raw");
        check(!cdc_manager_is_mai2touch_active(), "自动切换到 RAW");
        check(g_clear_override_count > clear_before, "切换时清除了上位机参数覆盖");
        check(g_reconfigure_count > reconf_before,
              "切换时请求重走初始化 (回到等参数状态)");
        check(g_rx_len == 0, "raw 侧接管并消化完剩余字节");
    }

    printf("\n=== 场景 5: raw 帧里的 0x7B (校验和) 不能误切 ===\n");
    check(!cdc_manager_is_mai2touch_active(), "此时处于 raw 侧");
    push_raw_frame(0x7B);           /* 校验和恰好等于 '{' 的帧 */
    run_ticks(2);
    check(!cdc_manager_is_mai2touch_active(), "校验和=0x7B 的 raw 帧不触发切换");
    check(g_rx_len == 0, "raw 帧已被消费");

    push_raw_frame(-1);             /* 紧接着一帧正常 raw 帧 */
    run_ticks(2);
    check(!cdc_manager_is_mai2touch_active(), "后续正常 raw 帧也不误切");
    check(g_rx_len == 0, "raw 帧已被消费");

    printf("\n=== 场景 6: 非法 Mai2Touch 命令体不切换 ===\n");
    {
        static const uint8_t junk[] = {0x7B, 'A', 'B', 'C', 'D', 0x7D};
        rx_push(junk, (int)sizeof(junk));
        run_ticks(2);
        check(!cdc_manager_is_mai2touch_active(), "{ABCD} 语法非法, 不切换");
    }

    printf("\n=== 场景 7: raw 待机下 {STAT} 先到 (必须立刻开始推流) ===\n");
    cdc_manager_set_auto_mode(false);
    touch_mode_init();
    g_pipeline_ready = false;
    g_tx_len = 0;
    push_cmd("STAT");
    run_ticks(1);
    check(touch_mode_pending_to_mai2touch(), "已标记待切换到 Mai2Touch");
    run_ticks(1);
    report("raw 收到 {STAT} 后");
    check(cdc_manager_is_mai2touch_active(), "自动切换到 MAI2TOUCH");
    check(g_tx_len == 9, "{STAT} 被补喂给 mai2touch, 立刻开始推流");

    printf("\n=== 场景 8: raw 待机下 {LAr2} 先到 (必须回显) ===\n");
    cdc_manager_set_auto_mode(false);
    touch_mode_init();
    g_tx_len = 0;
    push_cmd("LAr2");
    run_ticks(1);
    check(touch_mode_pending_to_mai2touch(), "已标记待切换到 Mai2Touch");
    run_ticks(1);
    report("raw 收到 {LAr2} 后");
    check(cdc_manager_is_mai2touch_active(), "自动切换到 MAI2TOUCH");
    {
        static const uint8_t echo[] = {0x28, 0x4C, 0x41, 0x72, 0x32, 0x29};
        check(g_tx_len == 6 && memcmp(g_tx, echo, 6) == 0,
              "{LAr2} 被补喂给 mai2touch, 回显正确 (游戏靠它推进配置)");
    }

    printf("\n=== 场景 9: Magic 强制模式后不再自动切换 ===\n");
    g_tx_len = 0;
    cdc_manager_set_mai2touch_active(false);        /* 强制 raw */
    check(!cdc_manager_is_auto_mode(), "已退出自动模式");
    push_cmd("STAT");
    run_ticks(8);
    report("强制 raw 下收到 {STAT}");
    check(!cdc_manager_is_mai2touch_active(), "强制设定优先, 未被自动识别改写");
    check(g_rx_len == 0, "命令被 raw 侧消费掉");

    printf("\n=== 场景 10: raw 帧头分两次到达 (0xAA .. 0x01) ===\n");
    cdc_manager_set_auto_mode(true);
    touch_mode_init();
    check(cdc_manager_is_mai2touch_active(), "已回到自动 Mai2Touch 侧");

    rx_push_byte(0xAA);
    run_ticks(1);
    check(cdc_manager_is_mai2touch_active(), "只收到 0xAA 时不切换 (等确认字节)");
    check(!touch_mode_pending_to_raw(), "0xAA 未触发切换");
    check(g_rx_len == 0, "0xAA 已被消费");

    rx_push_byte(0x01);
    run_ticks(1);
    check(touch_mode_pending_to_raw(), "收到 0x01 后标记待切换");
    check(cdc_manager_is_mai2touch_active(), "确认当拍模式未变");
    run_ticks(1);
    check(!cdc_manager_is_mai2touch_active(), "下一拍完成切换");

    printf("\n=== 场景 11: 0xAA 后接非 0x01 不算 raw 帧头 ===\n");
    cdc_manager_set_auto_mode(true);
    touch_mode_init();
    rx_push_byte(0xAA);
    run_ticks(1);
    rx_push_byte(0x00);
    run_ticks(2);
    check(cdc_manager_is_mai2touch_active(), "0xAA 0x00 不切换 (丢弃半截帧头)");
    check(g_rx_len == 0, "半截帧头被消费");

    printf("\n================ 结果 ================\n");
    if (g_failures == 0) {
        printf("全部通过\n");
        return 0;
    }
    printf("%d 项失败\n", g_failures);
    return 1;
}

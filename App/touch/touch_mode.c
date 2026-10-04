#include "touch_mode.h"

#include "cdc_manager.h"
#include "mai2touch.h"
#include "tenodata.h"
#include "touch_app.h"
#include "touch_pipeline.h"

/* raw 参数帧头 (与 tenodata.c 的 TENODATA_HOST_CFG_HEADER_A/B 一致) */
#define TOUCH_MODE_RAW_HEADER_A   0xAAU
#define TOUCH_MODE_RAW_HEADER_B   0x01U
/* Mai2Touch 命令起始符 '{' / 结束符 '}' / 触摸数据起始符 '(' */
#define TOUCH_MODE_MAI2TOUCH_CMD   0x7BU
#define TOUCH_MODE_MAI2TOUCH_END   0x7DU
#define TOUCH_MODE_MAI2TOUCH_DATA  0x28U

/* `{` + 4 字节命令体 + `}` */
#define TOUCH_MODE_MAI2TOUCH_FRAME 6U
#define TOUCH_MODE_MAI2TOUCH_BODY  4U

typedef enum
{
    OBS_IDLE = 0,
    OBS_MAI_BODY,  /* raw 侧: 已看到 '{', 正在收集 4 字节命令体 */
    OBS_RAW_A      /* Mai2Touch 侧: 已看到 0xAA, 等 0x01 确认 */
} observe_state_t;

static observe_state_t observe_state;
static bool switch_pending;
static bool switch_to_mai2touch;

/* raw 侧收集中的命令帧 */
static uint8_t mai_frame[TOUCH_MODE_MAI2TOUCH_FRAME];
static uint8_t mai_frame_len;
/* 待补喂给 mai2touch 的触发命令 */
static uint8_t replay_frame[TOUCH_MODE_MAI2TOUCH_FRAME];
static uint8_t replay_len;

/*
 * 命令体语法 (见 docs/Mai2TouchREADME.md §3/§5):
 *   {RSET} / {HALT} / {STAT}
 *   {L|R}{sensor}{r|k}{value}   —— ratio(r)/sensitivity(k)
 *      sensor: 0x41('A')..0x62('b'), A1 起连续 34 个 ASCII 码,
 *              注意 C 区只有 2 个点, 所以它不是"区字母+序号"
 *      value : ratio 恒为 0x32; sensitivity 是编码值, **可能是控制字符**
 *              (默认档 A 区是 0x28, B~E 区是 0x14), 不能要求可打印
 * 只认这几种, 别的组合一律当作噪声 (raw 帧里偶然出现的 0x7B 会在这里被挡掉)。
 */
static bool mai_body_valid(const uint8_t *body)
{
    if (body[0] == 'R' && body[1] == 'S' && body[2] == 'E' && body[3] == 'T')
    {
        return true;
    }
    if (body[0] == 'H' && body[1] == 'A' && body[2] == 'L' && body[3] == 'T')
    {
        return true;
    }
    if (body[0] == 'S' && body[1] == 'T' && body[2] == 'A' && body[3] == 'T')
    {
        return true;
    }
    if ((body[0] == 'L' || body[0] == 'R') &&
        (body[1] >= 0x41U) && (body[1] <= 0x62U) &&
        (body[2] == 'r' || body[2] == 'k'))
    {
        return true;
    }
    return false;
}

static void observe_reset(void)
{
    observe_state = OBS_IDLE;
    mai_frame_len = 0U;
}

/* 当前模式为 raw 时: 只找一个完整的 Mai2Touch 命令 */
static void observe_raw_byte(uint8_t byte)
{
    switch (observe_state)
    {
    case OBS_IDLE:
        if (byte == TOUCH_MODE_MAI2TOUCH_CMD)
        {
            observe_state = OBS_MAI_BODY;
            mai_frame_len = 0U;
            mai_frame[mai_frame_len++] = byte;
        }
        break;

    case OBS_MAI_BODY:
        if (mai_frame_len < (1U + TOUCH_MODE_MAI2TOUCH_BODY))
        {
            /* 位置相关的最小约束, 只为尽早排除噪声; 真正的合法性由
             * mai_body_valid() 在收齐 4 字节后统一判定。
             * body[3] 允许任意字节: sensitivity 的值可能是控制字符。 */
            bool shape_ok;
            if (mai_frame_len == 1U)
            {
                /* {RSET}/{HALT}/{STAT} 与 {L../R..} 的首字节 */
                shape_ok = (byte == 'L') || (byte == 'R') ||
                           (byte == 'H') || (byte == 'S');
            }
            else if (mai_frame_len == 2U)
            {
                shape_ok = (byte >= 0x20U) && (byte <= 0x7EU);
            }
            else if (mai_frame_len == 3U)
            {
                shape_ok = (byte >= 0x20U) && (byte <= 0x7EU);
            }
            else
            {
                shape_ok = (byte != TOUCH_MODE_MAI2TOUCH_CMD) &&
                           (byte != TOUCH_MODE_MAI2TOUCH_END);
            }

            if (shape_ok)
            {
                mai_frame[mai_frame_len++] = byte;
                break;
            }
        }

        if ((byte == TOUCH_MODE_MAI2TOUCH_END) &&
            (mai_frame_len == (1U + TOUCH_MODE_MAI2TOUCH_BODY)))
        {
            /* 补上结束符, 让 mai_frame 成为完整 6 字节 (补喂时要原样重放) */
            mai_frame[mai_frame_len++] = byte;

            if (mai_body_valid(&mai_frame[1]))
            {
                /* 整条命令已在手里: 切换后由 touch_mode_task() 补喂给 mai2touch */
                for (uint8_t i = 0U; i < TOUCH_MODE_MAI2TOUCH_FRAME; i++)
                {
                    replay_frame[i] = mai_frame[i];
                }
                replay_len = TOUCH_MODE_MAI2TOUCH_FRAME;
                switch_pending = true;
                switch_to_mai2touch = true;
            }
        }
        else if (byte == TOUCH_MODE_MAI2TOUCH_CMD)
        {
            /* 新的起始符: 重新开始收集 */
            mai_frame_len = 0U;
            mai_frame[mai_frame_len++] = byte;
            break;
        }

        observe_state = OBS_IDLE;
        mai_frame_len = 0U;
        break;

    default:
        observe_reset();
        break;
    }
}

/* 当前模式为 Mai2Touch 时: 只找 raw 帧头 (数据/命令流里不可能出现 0xAA) */
static void observe_mai2touch_byte(uint8_t byte)
{
    switch (observe_state)
    {
    case OBS_IDLE:
        if (byte == TOUCH_MODE_RAW_HEADER_A)
        {
            observe_state = OBS_RAW_A;
        }
        break;

    case OBS_RAW_A:
        if (byte == TOUCH_MODE_RAW_HEADER_B)
        {
            switch_pending = true;
            switch_to_mai2touch = false;
            observe_state = OBS_IDLE;
        }
        else if (byte == TOUCH_MODE_RAW_HEADER_A)
        {
            /* 还是 0xAA, 继续等确认字节 */
        }
        else
        {
            observe_state = OBS_IDLE;
        }
        break;

    default:
        observe_reset();
        break;
    }
}

void touch_mode_init(void)
{
    observe_reset();
    switch_pending = false;
    switch_to_mai2touch = false;
    replay_len = 0U;
}

void touch_mode_observe_byte(uint8_t byte)
{
    if (cdc_manager_is_io_mode())
    {
        return;
    }

    if (!cdc_manager_is_auto_mode())
    {
        /* 已被 Magic 固定模式, 不识别 */
        return;
    }

    /* 识别器始终与"当前正在跑的那条流程"配套: 谁在消费字节, 就找对方的特征 */
    if (cdc_manager_is_mai2touch_active())
    {
        observe_mai2touch_byte(byte);
    }
    else
    {
        observe_raw_byte(byte);
    }
}

bool touch_mode_pending_to_mai2touch(void)
{
    return switch_pending && switch_to_mai2touch;
}

bool touch_mode_pending_to_raw(void)
{
    return switch_pending && !switch_to_mai2touch;
}

bool touch_mode_take_replay(uint8_t *frame_out, uint8_t *length_out)
{
    if ((replay_len == 0U) || (frame_out == 0) || (length_out == 0))
    {
        return false;
    }

    for (uint8_t i = 0U; i < replay_len; i++)
    {
        frame_out[i] = replay_frame[i];
    }
    *length_out = replay_len;
    replay_len = 0U;
    return true;
}

void touch_mode_task(void)
{
    if (!switch_pending)
    {
        return;
    }

    switch_pending = false;

    if (cdc_manager_is_io_mode() || !cdc_manager_is_auto_mode())
    {
        return;
    }

    /* 模式变了, 识别器状态机也要跟着换一套 (0xAA 与 '{' 的语义完全不同) */
    observe_reset();

    cdc_manager_set_auto_detected(switch_to_mai2touch);

    /* 切换时丢弃上一模式的扫描参数覆盖 (两套参数表数值本来就相同, 只是来源不同) */
    tenodata_clear_host_config_override();

    if (switch_to_mai2touch)
    {
        /* 切到游戏: 停止推流等 {STAT}, 并重置软件基线
         * (PSoC 不动 —— 此时游戏正处在 {HALT} 之后的配置阶段,
         *  绝不能在这中间插入 1~2 秒的 PSoC 软复位+校准) */
        mai2touch_init();
        touch_pipeline_init();

        /* 触发切换的那条命令是 raw 侧替 mai2touch 收下的, 这里补喂回去。
         * {STAT} 靠它才会开始推流, {L/R..} 靠它才会回显。 */
        uint8_t frame[TOUCH_MODE_MAI2TOUCH_FRAME];
        uint8_t length = 0U;
        if (touch_mode_take_replay(frame, &length))
        {
            mai2touch_replay_command(frame, length);
        }
    }
    else
    {
        /* 切回上位机调试: 重走初始化, 让状态机回到 WAIT_HOST_CONFIG
         * 并上报 0x01, 由上位机重新下发扫描参数 */
        tenodata_request_reconfigure();
    }
}

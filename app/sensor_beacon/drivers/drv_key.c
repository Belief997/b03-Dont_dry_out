/**
 * drv_key —— 单按键驱动实现(去抖 + 单击/双击/长按)
 * ------------------------------------------------------------------
 * 分层:
 *   app_button (SDK)  —— GPIOTE 边沿中断 + 延时复检, 输出"稳定的按下/松开"
 *        ↓
 *   本文件的手势状态机 —— 把稳定跳变翻译成 单击 / 双击 / 长按
 *
 * ⚠ 上下文: app_button 的回调由 app_timer 中断触发, 本文件的手势定时器回调
 *   同样在 app_timer 中断上下文。两者同属一个中断优先级, 因此状态机内部无需
 *   临界区保护(不会互相抢占)。但对外回调也处在中断中, 应用侧须保持轻量。
 */

#include "drv_key.h"

#include "nrf_gpio.h"
#include "app_button.h"
#include "app_timer.h"
#include "app_error.h"

#define NRF_LOG_MODULE_NAME drv_key
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

/* 按键另一端接地 → 按下为低电平, 需内部上拉 */
#define KEY_ACTIVE_STATE            APP_BUTTON_ACTIVE_LOW
#define KEY_PULL_CFG                NRF_GPIO_PIN_PULLUP

/**@brief 手势状态机状态 */
typedef enum
{
    KEY_ST_IDLE = 0,        /**< 空闲, 等待第一次按下 */
    KEY_ST_PRESS_1,         /**< 第一次按下中, 等松开或长按超时 */
    KEY_ST_WAIT_SECOND,     /**< 第一次已松开, 在双击窗口内等第二次按下 */
    KEY_ST_PRESS_2,         /**< 第二次按下中, 等松开(双击)或长按超时 */
    KEY_ST_LONG_HELD        /**< 长按已上报, 等松开后归位(松手不再产生事件) */
} key_state_t;

APP_TIMER_DEF(m_gesture_timer);     /* 复用: PRESS_x 时计长按, WAIT_SECOND 时计双击窗口 */

static bool                     m_inited  = false;
static volatile key_state_t     m_state   = KEY_ST_IDLE;
static drv_key_evt_handler_t    m_handler = NULL;

/* app_button 要求配置数组为 static 长生命周期 */
static app_button_cfg_t m_buttons[] =
{
    {
        .pin_no         = DRV_KEY_PIN,
        .active_state   = KEY_ACTIVE_STATE,
        .pull_cfg       = KEY_PULL_CFG,
        .button_handler = NULL      /* 在 drv_key_init() 中填入, 见下 */
    }
};

const char * drv_key_evt_str(drv_key_evt_t evt)
{
    switch (evt)
    {
        case DRV_KEY_EVT_SINGLE_CLICK: return "SINGLE_CLICK";
        case DRV_KEY_EVT_DOUBLE_CLICK: return "DOUBLE_CLICK";
        case DRV_KEY_EVT_LONG_PRESS:   return "LONG_PRESS";
        default:                       return "UNKNOWN";
    }
}

/* 统一出口: 打印日志 + 通知应用 */
static void evt_report(drv_key_evt_t evt)
{
    NRF_LOG_INFO("KEY event: %s", drv_key_evt_str(evt));

    if (m_handler != NULL)
    {
        m_handler(evt);
    }
}

/* 重置手势定时器为指定超时。先停再启, 避免对运行中的定时器重复 start。 */
static void gesture_timer_restart(uint32_t timeout_ms)
{
    (void)app_timer_stop(m_gesture_timer);

    ret_code_t err = app_timer_start(m_gesture_timer, APP_TIMER_TICKS(timeout_ms), NULL);
    if (err != NRF_SUCCESS)
    {
        /* app_timer 操作队列满 → 本次手势判定失效, 回到空闲而不是卡在中间态 */
        NRF_LOG_WARNING("KEY timer start failed (0x%08x), gesture reset", err);
        m_state = KEY_ST_IDLE;
    }
}

static void gesture_timer_cancel(void)
{
    (void)app_timer_stop(m_gesture_timer);
}

/* 手势定时器到期: 含义取决于当前状态 */
static void gesture_timeout_handler(void * p_context)
{
    (void)p_context;

    switch (m_state)
    {
        case KEY_ST_PRESS_1:
        case KEY_ST_PRESS_2:
            /* 仍按住且已达长按阈值 → 立即上报长按, 之后等松手 */
            m_state = KEY_ST_LONG_HELD;
            evt_report(DRV_KEY_EVT_LONG_PRESS);
            break;

        case KEY_ST_WAIT_SECOND:
            /* 双击窗口内没有第二次按下 → 确认为单击 */
            m_state = KEY_ST_IDLE;
            evt_report(DRV_KEY_EVT_SINGLE_CLICK);
            break;

        default:
            /* IDLE / LONG_HELD 下不应有超时(定时器已被取消), 忽略 */
            break;
    }
}

/* app_button 回调: 已去抖的稳定跳变。action = APP_BUTTON_PUSH / APP_BUTTON_RELEASE */
static void button_evt_handler(uint8_t pin_no, uint8_t action)
{
    if (pin_no != DRV_KEY_PIN)
    {
        return;
    }

    if (action == APP_BUTTON_PUSH)
    {
        switch (m_state)
        {
            case KEY_ST_IDLE:
                m_state = KEY_ST_PRESS_1;
                gesture_timer_restart(DRV_KEY_LONG_PRESS_MS);
                break;

            case KEY_ST_WAIT_SECOND:
                /* 双击窗口内的第二次按下: 转为等它松开(或按住变长按) */
                m_state = KEY_ST_PRESS_2;
                gesture_timer_restart(DRV_KEY_LONG_PRESS_MS);
                break;

            default:
                /* PRESS_x / LONG_HELD 下再收到 PUSH 属异常(去抖已保证配对), 忽略 */
                break;
        }
    }
    else /* APP_BUTTON_RELEASE */
    {
        switch (m_state)
        {
            case KEY_ST_PRESS_1:
                /* 未达长按阈值就松开 → 可能是单击, 也可能是双击的前半:
                 * 开双击窗口等待, 超时才确认单击。 */
                m_state = KEY_ST_WAIT_SECOND;
                gesture_timer_restart(DRV_KEY_MULTI_CLICK_MS);
                break;

            case KEY_ST_PRESS_2:
                /* 第二次短按松开 → 双击成立 */
                gesture_timer_cancel();
                m_state = KEY_ST_IDLE;
                evt_report(DRV_KEY_EVT_DOUBLE_CLICK);
                break;

            case KEY_ST_LONG_HELD:
                /* 长按已上报, 松手只做归位, 不再产生事件 */
                gesture_timer_cancel();
                m_state = KEY_ST_IDLE;
                break;

            default:
                break;
        }
    }
}

ret_code_t drv_key_init(drv_key_evt_handler_t handler)
{
    if (m_inited)
    {
        m_handler = handler;        /* 允许重复调用时只更新回调 */
        return NRF_SUCCESS;
    }

    m_handler = handler;
    m_state   = KEY_ST_IDLE;

    m_buttons[0].button_handler = button_evt_handler;

    ret_code_t err = app_timer_create(&m_gesture_timer, APP_TIMER_MODE_SINGLE_SHOT,
                                      gesture_timeout_handler);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    /* app_button 内部会按需初始化 nrf_drv_gpiote, 无需在此显式 init */
    err = app_button_init(m_buttons,
                          sizeof(m_buttons) / sizeof(m_buttons[0]),
                          APP_TIMER_TICKS(DRV_KEY_DEBOUNCE_MS));
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    err = app_button_enable();
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    m_inited = true;

    NRF_LOG_INFO("KEY init: P0.%02u (active low, pull-up), debounce %ums",
                 DRV_KEY_PIN, DRV_KEY_DEBOUNCE_MS);
    NRF_LOG_INFO("KEY thresholds: long>=%ums, double-click window %ums",
                 DRV_KEY_LONG_PRESS_MS, DRV_KEY_MULTI_CLICK_MS);
    return NRF_SUCCESS;
}

bool drv_key_is_pressed(void)
{
    if (!m_inited)
    {
        return false;
    }
    return app_button_is_pushed(0);
}

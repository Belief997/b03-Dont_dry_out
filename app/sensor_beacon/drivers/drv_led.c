/**
 * drv_led —— 单路 LED 驱动实现
 * ------------------------------------------------------------------
 * 极性: 引脚接 LED 阴极 → 输出低 = 亮, 输出高 = 灭。
 *       本文件用 LED_ACTIVE_LOW 集中表达该事实, 其余代码只谈"亮/灭"。
 */

#include "drv_led.h"

#include "nrf_gpio.h"
#include "app_timer.h"
#include "app_error.h"

#define NRF_LOG_MODULE_NAME drv_led
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

/* 阴极接 MCU → 低电平点亮 */
#define LED_ACTIVE_LOW              1

#if LED_ACTIVE_LOW
  #define LED_LEVEL_ON              0
  #define LED_LEVEL_OFF             1
#else
  #define LED_LEVEL_ON              1
  #define LED_LEVEL_OFF             0
#endif

APP_TIMER_DEF(m_blink_timer);

static bool             m_inited     = false;
static drv_led_blink_t  m_blink_mode = DRV_LED_BLINK_NONE;

/* ---------- 电平原语(唯一接触极性的地方) ---------- */

static inline void led_write(bool on)
{
    if (on) { nrf_gpio_pin_write(DRV_LED_PIN, LED_LEVEL_ON);  }
    else    { nrf_gpio_pin_write(DRV_LED_PIN, LED_LEVEL_OFF); }
}

static inline bool led_read(void)
{
    /* 读输出寄存器(OUT)而非输入缓冲: 引脚配置为输出时 IN 不一定可用 */
    return (nrf_gpio_pin_out_read(DRV_LED_PIN) == (uint32_t)LED_LEVEL_ON);
}

/* ---------- 闪烁定时器 ---------- */

static void blink_timeout_handler(void * p_context)
{
    (void)p_context;
    led_write(!led_read());
}

/* 停止闪烁。keep_level=false 时顺带熄灭。
 * app_timer_stop() 对未运行的定时器同样返回 NRF_SUCCESS, 故无需先判状态。 */
static void blink_stop(bool keep_level)
{
    if (m_blink_mode != DRV_LED_BLINK_NONE)
    {
        (void)app_timer_stop(m_blink_timer);
        m_blink_mode = DRV_LED_BLINK_NONE;
    }
    if (!keep_level)
    {
        led_write(false);
    }
}

/* ---------- 公共接口 ---------- */

ret_code_t drv_led_init(void)
{
    if (m_inited)
    {
        return NRF_SUCCESS;
    }

    nrf_gpio_cfg_output(DRV_LED_PIN);
    led_write(false);                   /* 上电即熄灭, 避免复位瞬间误亮 */

    ret_code_t err = app_timer_create(&m_blink_timer, APP_TIMER_MODE_REPEATED,
                                      blink_timeout_handler);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    m_blink_mode = DRV_LED_BLINK_NONE;
    m_inited     = true;

    NRF_LOG_INFO("LED init: P0.%02u (active low, cathode to MCU)", DRV_LED_PIN);
    return NRF_SUCCESS;
}

void drv_led_on(void)
{
    blink_stop(true);
    led_write(true);
}

void drv_led_off(void)
{
    blink_stop(true);
    led_write(false);
}

void drv_led_set(bool on)
{
    blink_stop(true);
    led_write(on);
}

void drv_led_toggle(void)
{
    /* 先取当前瞬时电平, 再停闪烁 —— 顺序无关(blink_stop 保留电平),
     * 但显式先读可表明 toggle 是相对"看到的状态"翻转。 */
    bool now = led_read();
    blink_stop(true);
    led_write(!now);
}

bool drv_led_is_on(void)
{
    return led_read();
}

ret_code_t drv_led_blink(drv_led_blink_t mode)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (mode == m_blink_mode)
    {
        return NRF_SUCCESS;         /* 同模式重复设置: 不重启相位 */
    }

    if (mode == DRV_LED_BLINK_NONE)
    {
        blink_stop(false);
        return NRF_SUCCESS;
    }

    /* 切换快慢: 必须先停再启, app_timer_start 对运行中的定时器返回
     * NRF_ERROR_INVALID_STATE。 */
    blink_stop(true);

    uint32_t half_ms = (mode == DRV_LED_BLINK_FAST) ? DRV_LED_BLINK_FAST_MS
                                                    : DRV_LED_BLINK_SLOW_MS;

    ret_code_t err = app_timer_start(m_blink_timer, APP_TIMER_TICKS(half_ms), NULL);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    m_blink_mode = mode;
    led_write(true);                /* 从"亮"开始, 让闪烁立即可见 */
    return NRF_SUCCESS;
}

drv_led_blink_t drv_led_blink_get(void)
{
    return m_blink_mode;
}

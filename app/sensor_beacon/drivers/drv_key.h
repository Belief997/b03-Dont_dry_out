/**
 * drv_key —— 单按键驱动(去抖 + 单击/双击/长按识别)
 * ------------------------------------------------------------------
 * 硬件接法: 按键一端接 MCU 引脚, 另一端接地 → 按下时引脚被拉低。
 *           故引脚需使能内部上拉, 判定为 active-low。
 *
 * 去抖: 复用 SDK 的 app_button 模块(GPIOTE 边沿中断 + 延时复检),
 *       延时窗口 DRV_KEY_DEBOUNCE_MS 内的抖动被自动吞掉。
 *
 * 手势识别(在去抖后的稳定电平之上):
 *   按下并在 DRV_KEY_LONG_PRESS_MS 内松开        → 单击(需再等双击窗口)
 *   两次单击间隔小于 DRV_KEY_MULTI_CLICK_MS      → 双击
 *   按住超过 DRV_KEY_LONG_PRESS_MS(尚未松手)     → 长按(立即上报, 不等松手)
 *
 * ⚠ 单击存在 DRV_KEY_MULTI_CLICK_MS 的固有延迟: 必须等双击窗口超时,
 *   才能确定这一次不是双击的前半。这是单/双击共存时无法避免的取舍。
 *
 * ⚠ 依赖 app_timer 与 app_button, 故 drv_key_init() 必须在
 *   app_timer_init() 之后调用。
 */

#ifndef DRV_KEY_H__
#define DRV_KEY_H__

#include <stdbool.h>
#include <stdint.h>
#include "sdk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 按键引脚。P0.14 在 PCA10040 DK 上是 Button2, 在 E73-TBM 上也是按键, 两板通用。 */
#define DRV_KEY_PIN                 14

/* 去抖窗口(ms): GPIOTE 触发后延时该时长再复检电平, 一致才算有效跳变 */
#define DRV_KEY_DEBOUNCE_MS         50

/* 长按判定阈值(ms): 按住超过该时长即上报长按, 无需等待松手 */
#define DRV_KEY_LONG_PRESS_MS       1000

/* 双击窗口(ms): 上次松手后在该时长内再次按下并松开即判为双击 */
#define DRV_KEY_MULTI_CLICK_MS      300

/**@brief 按键事件 */
typedef enum
{
    DRV_KEY_EVT_SINGLE_CLICK = 0,   /**< 单击(短按一次, 双击窗口内无后续按下) */
    DRV_KEY_EVT_DOUBLE_CLICK,       /**< 双击 */
    DRV_KEY_EVT_LONG_PRESS          /**< 长按(按住达阈值时立即上报, 松手不再上报) */
} drv_key_evt_t;

/**@brief 按键事件回调。
 *
 * @note 在 app_timer 的中断上下文中调用(IRQ 优先级 = APP_TIMER_CONFIG_IRQ_PRIORITY),
 *       回调内应只做轻量操作, 不可阻塞、不可调用 nrf_delay 长延时。
 */
typedef void (*drv_key_evt_handler_t)(drv_key_evt_t evt);

/**@brief 事件名字符串(用于日志打印)。 */
const char * drv_key_evt_str(drv_key_evt_t evt);

/**@brief 初始化按键(配置引脚 + 去抖 + 手势定时器)并使能检测。
 *
 * @param handler  事件回调, 可传 NULL(此时仅打印日志, 不通知应用)。
 *
 * @note 必须在 app_timer_init() 之后调用。重复调用安全。
 *
 * @retval NRF_SUCCESS 成功, 否则为 app_button/app_timer 的错误码。
 */
ret_code_t drv_key_init(drv_key_evt_handler_t handler);

/**@brief 查询按键当前是否处于按下状态(去抖后的稳定状态)。 */
bool drv_key_is_pressed(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_KEY_H__ */

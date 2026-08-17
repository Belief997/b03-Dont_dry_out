/**
 * drv_led —— 单路 LED 驱动(亮灭 + 快慢闪烁)
 * ------------------------------------------------------------------
 * 硬件接法: MCU 引脚接 LED 阴极, 阳极经限流电阻接 VCC
 *           → 引脚输出低电平时 LED 点亮(灌电流), 高电平时熄灭。
 *           本驱动内部处理该极性, 调用者只需按"亮/灭"语义使用。
 *
 * ⚠ 依赖 app_timer: 闪烁由一个 APP_TIMER_MODE_REPEATED 定时器驱动,
 *   因此 drv_led_init() 必须在 app_timer_init() 之后调用。
 */

#ifndef DRV_LED_H__
#define DRV_LED_H__

#include <stdbool.h>
#include <stdint.h>
#include "sdk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LED 引脚。P0.17 在 PCA10040 DK 上是 LED1, 在 E73-TBM 上也是 LED, 两板通用。 */
#define DRV_LED_PIN                 17

/* 闪烁半周期(ms): 定时器每到期一次翻转一次电平, 故实际闪烁周期 = 2 倍 */
#define DRV_LED_BLINK_SLOW_MS       500     /* 慢闪: 1Hz */
#define DRV_LED_BLINK_FAST_MS       100     /* 快闪: 5Hz */

/**@brief 闪烁模式 */
typedef enum
{
    DRV_LED_BLINK_NONE = 0,     /**< 不闪烁(停止闪烁并熄灭) */
    DRV_LED_BLINK_SLOW,         /**< 慢闪 DRV_LED_BLINK_SLOW_MS */
    DRV_LED_BLINK_FAST          /**< 快闪 DRV_LED_BLINK_FAST_MS */
} drv_led_blink_t;

/**@brief 初始化 LED 引脚与闪烁定时器。初始状态为熄灭。
 *
 * @note  必须在 app_timer_init() 之后调用。重复调用安全(第二次起直接返回成功)。
 *
 * @retval NRF_SUCCESS 成功, 否则为 app_timer_create 的错误码。
 */
ret_code_t drv_led_init(void);

/**@brief 点亮 LED。会先停止正在进行的闪烁。 */
void drv_led_on(void);

/**@brief 熄灭 LED。会先停止正在进行的闪烁。 */
void drv_led_off(void);

/**@brief 按布尔值设置亮灭。会先停止正在进行的闪烁。 */
void drv_led_set(bool on);

/**@brief 翻转当前亮灭状态。会先停止正在进行的闪烁。
 *
 * @note  若正在闪烁, 闪烁被停止, 并以"当前电平的反相"作为结果静态保持,
 *        以保证调用后 LED 状态确定可查(drv_led_is_on())。
 */
void drv_led_toggle(void);

/**@brief 查询 LED 当前是否点亮(闪烁过程中返回瞬时状态)。 */
bool drv_led_is_on(void);

/**@brief 设置闪烁模式。
 *
 * @param mode  DRV_LED_BLINK_NONE 停止闪烁并熄灭; SLOW/FAST 开始对应速率闪烁。
 *              重复设置为同一模式不会重启相位。
 *
 * @retval NRF_SUCCESS 成功, 否则为 app_timer_start 的错误码。
 */
ret_code_t drv_led_blink(drv_led_blink_t mode);

/**@brief 查询当前闪烁模式。 */
drv_led_blink_t drv_led_blink_get(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_LED_H__ */

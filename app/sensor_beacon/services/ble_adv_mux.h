/**
 * ble_adv_mux —— 广播集仲裁(唯一持有 adv handle 的模块)
 * ------------------------------------------------------------------
 * 为什么需要这个模块:
 *
 *   S112 只有一个广播集(components/softdevice/s112/headers/ble_gap.h:222
 *   BLE_GAP_ADV_SET_COUNT_MAX == 1), 而"广播内容 + 广播参数"是跟着广播集的。
 *   谁第二个拿 BLE_GAP_ADV_SET_HANDLE_NOT_SET 去调 sd_ble_gap_adv_set_configure()
 *   就会收到 NRF_ERROR_NO_MEM ——
 *       "Not enough memory to configure a new advertising handle.
 *        Update an existing advertising handle instead."
 *
 *   本工程有两个想广播的模块:
 *       ble_beacon —— 不可连接的传感器数据广播(单击触发, 播固定次数就停)
 *       ble_link   —— 可连接广播 + NUS 透传(长按打开的调试/配置窗口)
 *   若两者各自持有一个 static m_adv_handle, 上面那个错误就是必然的运行期失败。
 *   所以句柄收敛到本模块独占, 两个模块都只提交"内容 + 参数", 不碰句柄。
 *
 *   SDK 自带的 eddystone 是同一思路: 句柄由 nrf_ble_es.c:59 独占, 通过指针
 *   传给 es_adv.c 使用。
 *
 * 仲裁策略(刻意做得极简):
 *   ble_adv_mux_start() 是"抢占式"的 —— 谁调用谁拿走广播集, 原持有者的广播
 *   被直接停掉。本模块【不做】优先级判断, 因为"什么时候该谁播"是应用层策略,
 *   放在这里会让两个模块的时序耦合到仲裁器里。当前工程的策略(平时静默、
 *   单击时数据广播播固定次数、长按时可连接广播占用 30 秒)由 main.c 编排,
 *   见那里的 adv_policy_* 说明。
 *
 *   ble_adv_mux_stop() 反过来是"礼让式"的: 只有当前持有者能停自己的广播,
 *   传入的 owner 与当前持有者不符时直接返回成功(视作"我的广播本来就没在播")。
 *   这样 A 模块的 stop 不会误伤 B 模块刚抢过去的广播。
 *
 * ⚠ 广播数据缓冲的所有权仍在各提供方:
 *   sd_ble_gap_adv_set_configure() 只记住指针, SoftDevice 每次发包直接读原地址,
 *   不做内部拷贝。故 p_data 指向的内存必须在整个广播期间保持有效 ——
 *   本模块不复制、也无法代管, 由调用方保证(通常是 static 缓冲)。
 *
 * ⚠ TX 功率是跟着广播集的, 且 sd_ble_gap_adv_set_configure() 之后需要重设,
 *   所以 tx_power_dbm 作为 start 的参数一并传入, 由本模块在 configure 之后
 *   立刻调用 sd_ble_gap_tx_power_set() —— 免得每个提供方各自记得这件事。
 *   (main.c:508 的既有 beacon 路径就是手工做这一步的。)
 */

#ifndef BLE_ADV_MUX_H__
#define BLE_ADV_MUX_H__

#include <stdbool.h>
#include <stdint.h>

#include "ble_gap.h"
#include "sdk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SoftDevice 连接配置标签。必须与 main.c 里 nrf_sdh_ble_default_cfg_set()
 * 使用的标签一致 —— sd_ble_gap_adv_start() 靠它找到对应的连接配置,
 * 不一致会返回 NRF_ERROR_NOT_FOUND。
 * 对不可连接广播该参数被协议栈忽略, 但统一传同一个值最省心。 */
#define BLE_ADV_MUX_CONN_CFG_TAG    1

/**@brief 广播集的持有者标识。 */
typedef enum
{
    BLE_ADV_OWNER_NONE = 0,     /**< 无人持有(当前没有广播) */
    BLE_ADV_OWNER_BEACON,       /**< ble_beacon: 不可连接数据广播 */
    BLE_ADV_OWNER_LINK,         /**< ble_link: 可连接广播 */
} ble_adv_owner_t;

/**@brief 抢占广播集并开始广播。
 *
 * 内部时序: (若正在广播) sd_ble_gap_adv_stop
 *           → sd_ble_gap_adv_set_configure(句柄, 数据, 参数)
 *           → sd_ble_gap_tx_power_set
 *           → sd_ble_gap_adv_start
 *
 * 必须先 stop 再 configure: 协议栈禁止在广播中修改广播参数, 否则
 * sd_ble_gap_adv_set_configure() 返回 NRF_ERROR_INVALID_STATE
 * ("It is invalid to provide non-NULL advertising set parameters while advertising")。
 *
 * @param owner         调用方标识, 成功后成为新的持有者。不可为 BLE_ADV_OWNER_NONE。
 * @param p_data        广播数据。⚠ 其 p_data 缓冲需在广播期间一直有效(见文件头说明)。
 * @param p_params      广播参数(properties.type / interval / duration 等)。
 * @param tx_power_dbm  发射功率, S112 支持 -40..+4 的若干档位。
 *
 * @retval NRF_SUCCESS         已开始广播, owner 成为持有者。
 * @retval NRF_ERROR_NULL      p_data 或 p_params 为 NULL。
 * @retval NRF_ERROR_INVALID_PARAM  owner 为 BLE_ADV_OWNER_NONE。
 * @retval 其它                底层 SoftDevice 错误码。失败时持有者被置为 NONE,
 *                             因为此时广播集的状态已不确定, 不宜再记在谁名下。
 */
ret_code_t ble_adv_mux_start(ble_adv_owner_t              owner,
                             ble_gap_adv_data_t   const * p_data,
                             ble_gap_adv_params_t const * p_params,
                             int8_t                       tx_power_dbm);

/**@brief 停止自己的广播并交还广播集。
 *
 * @param owner  调用方标识。若与当前持有者不符则不做任何事并返回成功 ——
 *               语义是"我的广播没在播", 目标状态已达成。
 *
 * @retval NRF_SUCCESS  已停播, 或本来就不是自己在播。
 */
ret_code_t ble_adv_mux_stop(ble_adv_owner_t owner);

/**@brief 当前广播集归谁。未在广播时仍可能返回上一个持有者 ——
 *        被连上或限时到期时广播由协议栈停止, 但归属未变(便于上层判断
 *        "刚结束的那次广播是谁的"), 需要区分请配合 ble_adv_mux_is_advertising()。 */
ble_adv_owner_t ble_adv_mux_owner(void);

/**@brief 当前是否真的在广播。 */
bool ble_adv_mux_is_advertising(void);

/**@brief 当前广播句柄, 仅供诊断/日志。未配置过时为
 *        BLE_GAP_ADV_SET_HANDLE_NOT_SET。 */
uint8_t ble_adv_mux_handle(void);

/**@brief 持有者名字, 供日志打印。 */
const char * ble_adv_owner_str(ble_adv_owner_t owner);

#ifdef __cplusplus
}
#endif

#endif /* BLE_ADV_MUX_H__ */

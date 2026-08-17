/**
 * ble_beacon —— 不可连接的传感器数据广播
 * ------------------------------------------------------------------
 * 职责: 把"把传感器数据以不可连接广播持续播出去"这件事收敛成一个模块。
 *       对上只暴露 "初始化 / 开播 / 更新数据 / 停播" 四件事。
 *
 * 与 ble_link 的关系: 两者都要用唯一的那个广播集, 但【不】直接抢 ——
 *   都通过 ble_adv_mux 申请, 由后者独占持有句柄。谁在什么时候播, 由 main.c
 *   编排(当前策略: 平时本模块播, 调试窗口期让给 ble_link, 连接期间都不播)。
 *
 * 广播内容与 main.c 原有的 beacon 路径保持一致(厂商自定义段, 无设备名):
 *   Flags(3) + 厂商段头(4) + 载荷(BLE_BEACON_PAYLOAD_LEN)
 * 网关侧解析方式不变, 所以这个模块可以直接替换 main.c 里
 * advertising_init()/advertising_start() 那套代码, 不需要改网关。
 *
 * ⚠ 为什么"不可连接": 这是需求 —— 平时的数据广播不接受连接。
 *   BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED 同时也意味着
 *   不响应扫描请求(no scan response), 手机搜到的只有广播包本身。
 *
 * ⚠ 双缓冲的必要性(不是优化, 是协议栈的硬要求):
 *   要在【不停播】的前提下更新广播数据, 必须提供一块新的缓冲 ——
 *     "In order to update advertising data while advertising,
 *      new advertising buffers must be provided."
 *   复用同一块会得到 NRF_ERROR_INVALID_STATE
 *     ("It is invalid to provide the same data buffers while advertising")。
 *   所以本模块备了两块编码缓冲交替使用。若改成 stop→configure→start,
 *   虽可省一块缓冲, 但每次更新都会产生广播空档, 与"数据广播全周期存在"相悖。
 */

#ifndef BLE_BEACON_H__
#define BLE_BEACON_H__

#include <stdbool.h>
#include <stdint.h>
#include "sdk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 广播参数。与 main.c 原 beacon 路径的取值保持一致, 便于对照。
 * duration 固定 0(不限时): 本模块的语义就是"平时一直播", 到期终止没有意义。 */
#define BLE_BEACON_ADV_INTERVAL_MS  100
#define BLE_BEACON_TX_POWER_DBM     0

/* 厂商自定义数据的公司标识。0xFFFF 为 SIG 保留(测试用), 与 main.c 一致。 */
#define BLE_BEACON_COMPANY_ID       0xFFFF

/* 载荷长度。与 main.c 的 MANUF_DATA_LEN 对齐(魔数1 + 版本1 + 设备ID2 +
 * 计数器1 + 电池2 + 3通道×3字节 = 16)。
 * ⚠ 改这个值要留意 31 字节预算: Flags(3) + 厂商段头(4) + 载荷 <= 31,
 *   即载荷上限 24 字节。超了 ble_advdata_encode() 返回 NRF_ERROR_DATA_SIZE。 */
#define BLE_BEACON_PAYLOAD_LEN      16

/**@brief 初始化(只做准备, 不开播)。
 *
 * @note 必须在 SoftDevice 已使能之后调用。重复调用安全。
 */
ret_code_t ble_beacon_init(void);

/**@brief 开始(或重新开始)数据广播, 并设置本次要播的载荷。
 *
 * 会向 ble_adv_mux 抢占广播集。若当前是 ble_link 在播, 那边会被停掉 ——
 * 是否该抢由调用方决定, 本模块不做策略判断。
 *
 * @param p_payload  载荷, 长度须为 BLE_BEACON_PAYLOAD_LEN。内部会拷贝一份,
 *                   调用方不需要保持该缓冲有效。
 *
 * @retval NRF_SUCCESS  已开始广播。
 * @retval NRF_ERROR_NULL / NRF_ERROR_INVALID_STATE / 底层错误码。
 */
ret_code_t ble_beacon_start(const uint8_t * p_payload);

/**@brief 更新载荷。
 *
 * 若当前正由本模块广播, 则【不中断广播】地换掉数据(双缓冲交替);
 * 若当前广播集不在本模块手上, 只更新内部副本并返回
 * NRF_ERROR_INVALID_STATE —— 下次 ble_beacon_start() 时会用上新数据。
 *
 * @param p_payload  新载荷, 长度须为 BLE_BEACON_PAYLOAD_LEN。
 */
ret_code_t ble_beacon_update(const uint8_t * p_payload);

/**@brief 停止数据广播, 交还广播集。未在播时返回成功。 */
ret_code_t ble_beacon_stop(void);

/**@brief 当前是否正由本模块广播。 */
bool ble_beacon_is_advertising(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_BEACON_H__ */

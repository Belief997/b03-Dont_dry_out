/**
 * ble_beacon —— 不可连接的传感器数据广播(按次触发)
 * ------------------------------------------------------------------
 * 职责: 把"把一份传感器数据以不可连接广播播出去固定几次"这件事收敛成一个
 *       模块。对上只暴露 "初始化 / 播一次(N 个广播事件) / 停播" 三件事。
 *
 * ⚠ 本模块【不是】常驻广播: 平时不播, 只在被调用时播 BLE_BEACON_ADV_EVENTS
 *   个广播事件, 播完由协议栈自动停下(见下方 max_adv_evts 说明)。这是需求 ——
 *   数据广播只在按键触发后出现, 不持续占用空气与电量。
 *
 * 与 ble_link 的关系: 两者都要用唯一的那个广播集, 但【不】直接抢 ——
 *   都通过 ble_adv_mux 申请, 由后者独占持有句柄。谁在什么时候播, 由 main.c
 *   编排(当前策略: 平时谁都不播, 单击时本模块播几次, 长按时让给 ble_link)。
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
 * ⚠ 为什么不再需要双缓冲: 上一版是"广播中不停播地更新数据", 那必须提供新缓冲
 *   ("It is invalid to provide the same data buffers while advertising")。
 *   现在改成"每次触发都是 stop → configure → start 的完整一轮", configure
 *   发生在没有广播的时刻, 复用同一块缓冲是合法的。少一块 31 字节的 RAM 是
 *   附带好处, 主要好处是"这一轮播的就是这一份数据"不再有歧义。
 */

#ifndef BLE_BEACON_H__
#define BLE_BEACON_H__

#include <stdbool.h>
#include <stdint.h>
#include "sdk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 广播参数。与 main.c 原 beacon 路径的取值保持一致, 便于对照。 */
#define BLE_BEACON_ADV_INTERVAL_MS  100

/* 一次触发要播几个广播事件。
 *
 * 用协议栈的 max_adv_evts 实现"播够就停", 而不是应用层起定时器数时间 ——
 * 后者只能保证"播了大约多久", 前者精确保证"播了几次"。播完协议栈自己产生
 * BLE_GAP_EVT_ADV_SET_TERMINATED(reason = LIMIT_REACHED)。
 *
 * ⚠ 一个"广播事件"是在 37/38/39 三个信道上各发一包(共 3 个空中包),
 *   所以 3 个广播事件 = 网关有 9 次机会收到同一份数据。
 *
 * ⚠ max_adv_evts != 0 在本 SoftDevice(S112 6.0.0)中标注为
 *   "experimental feature"(ble_gap.h:648)。实测若不可用, 退路是把它设回 0
 *   并在应用层用定时器控制窗口(见 git 历史里 ADV_WINDOW_MS 的做法)。 */
#define BLE_BEACON_ADV_EVENTS       3

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

/**@brief 播一轮数据: 把这份载荷播 BLE_BEACON_ADV_EVENTS 个广播事件后自动停。
 *
 * 三次播的是【同一份】数据 —— 载荷在本函数入口拷贝一份, 之后不再变化。
 * 需求如此: 网关收到重复包可直接去重, 不必判断哪一包更新。
 *
 * 会向 ble_adv_mux 抢占广播集。若当前是 ble_link 在播, 那边会被停掉 ——
 * 是否该抢由调用方决定, 本模块不做策略判断(main.c 的 adv_policy_* 会先判)。
 *
 * 若上一轮还没播完就再次调用, 上一轮被丢弃, 从新数据重新数 N 次
 * (不排队, 不叠加) —— 连续单击的语义是"我要最新的那份"。
 *
 * @param p_payload  载荷, 长度须为 BLE_BEACON_PAYLOAD_LEN。内部会拷贝一份,
 *                   调用方不需要保持该缓冲有效。
 *
 * @retval NRF_SUCCESS  已开始广播, 播完会自动停。
 * @retval NRF_ERROR_NULL / NRF_ERROR_INVALID_STATE / 底层错误码。
 */
ret_code_t ble_beacon_burst(const uint8_t * p_payload);

/**@brief 提前停止本轮数据广播, 交还广播集。未在播时返回成功。
 *
 * 正常情况不需要调用 —— 播够 BLE_BEACON_ADV_EVENTS 次后协议栈自己会停。
 * 本接口用于"话说到一半要让位"(例如长按要开可连接窗口)。
 */
ret_code_t ble_beacon_stop(void);

/**@brief 当前是否正由本模块广播(即本轮还没播完)。 */
bool ble_beacon_is_advertising(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_BEACON_H__ */

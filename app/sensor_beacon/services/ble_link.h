/**
 * ble_link —— 可连接广播 + 双向透传链路(后续 cmd 协议的承载层)
 * ------------------------------------------------------------------
 * 职责: 把"能被手机搜到、能连上、能收发字节"这件事收敛成一个模块,
 *       对上只暴露 "初始化 / 开播 / 发送 / 收到了数据" 四件事。
 *       后期的 cmd 协议只需实现 ble_link_rx_handler_t 并调用
 *       ble_link_send() 回包, 不必关心 GAP/GATT 细节。
 *
 * 承载方式: Nordic UART Service (NUS), 128bit 厂商自定义 UUID。
 *   - RX 特性(手机→设备): Write / Write Without Response
 *   - TX 特性(设备→手机): Notify
 *   选它是因为手机端工具链现成(nRF Connect / Web Bluetooth 都直接支持),
 *   省掉自定义 GATT 表的对接成本; 协议层完全由我们自己在字节流上定义。
 *
 * ⚠ 与 main.c 里那套 beacon 广播互斥, 不能同时开:
 *   S112 的 BLE_GAP_ADV_SET_COUNT_MAX == 1, 整个协议栈只有一个广播集。
 *   main.c 的 advertising_init()/advertising_start() 是"事件触发不可连接
 *   广播"路径, 当前运行分支并未调用它, 故无冲突。
 *
 * ==================================================================
 *  关于"不可连接的传感器数据广播 + 可连接调试广播 并存"(重要)
 * ==================================================================
 *
 * 结论: 在 S112 上【无法】让两种广播真正同时存在。硬约束有两条, 都来自
 *       协议栈本身, 不是配置能放开的:
 *
 *   1) components/softdevice/s112/headers/ble_gap.h:222
 *        #define BLE_GAP_ADV_SET_COUNT_MAX  (1)
 *      整个协议栈只有一个广播集, 也就是只有一份"广播内容 + 广播参数"。
 *      而"可连接/不可连接"是广播参数 properties.type 里的一个字段, 同一
 *      个广播集在同一时刻只能是其中一种。
 *
 *   2) 同文件 sd_ble_gap_adv_start() 文档明确写着:
 *        "Only one advertiser may be active at any time."
 *      即便硬件能排开时隙, API 层也只允许一个 advertiser 处于活动状态。
 *
 *   顺带一提: 本 SDK(15.0.0) 里 s112 / s132 / s140 三者的
 *   BLE_GAP_ADV_SET_COUNT_MAX 全都是 1, 所以"换个 SoftDevice 就能并存"
 *   这条路走不通。多广播集要到 nRF Connect SDK / 更新的协议栈才有。
 *
 * 因此"不可连接广播全周期存在"只能用下面两种方式近似实现, 二选一:
 *
 *   方案 A(推荐, 真正的"全周期不中断"): 单广播集 + 合并载荷。
 *     把传感器数据(厂商自定义段)和设备名一起放进同一个【可连接】广播包。
 *     网关照常解析厂商段拿数据, 手机照常搜到名字并连上 —— 一个广播集
 *     同时满足两个用途, 不需要切换, 也就不存在"数据广播中断"。
 *     字节预算(实测 ble_advdata_encode, 上限 31):
 *         名字 "water" + 16 字节厂商载荷 → 占 30 字节, 通过
 *         名字 "water" + 17 字节厂商载荷 → 占 31 字节, 通过(刚好用满)
 *         名字 "water" + 18 字节起        → NRF_ERROR_DATA_SIZE
 *       即当前载荷(MANUF_DATA_LEN=16)只剩 1 字节可加。要加更多字段就得
 *       缩短设备名: 名字缩到 "wtr" 可放 18 字节, 缩到 "w" 可放 20 字节。
 *     NUS 的 128bit UUID 本来就在扫描响应包里(独立 31 字节), 不占这份预算。
 *     代价: 数据广播期间设备始终可被连接 —— 若"平时不许连"是安全需求,
 *           则不能用本方案(明文透传口的鉴权问题见下方 SEC_PARAMS 处说明)。
 *
 *   方案 B(分时切换): 平时播不可连接的数据包, 需要调试时切成可连接。
 *     切换必须 stop → adv_set_configure(换 properties.type 和数据) → start,
 *     因为协议栈禁止在广播中修改广播参数(adv_set_configure 的
 *     NRF_ERROR_INVALID_STATE: "invalid to provide non-NULL advertising set
 *     parameters while advertising")。
 *     代价: 只有【可连接窗口那一段】没有数据广播(唯一的广播集被可连接包占用),
 *           窗口结束或连上之后, 数据广播都可以恢复 —— 见下方"连接期间"一条。
 *
 *   ⚠ 连接期间【可以】继续播不可连接广播(此前本注释写反了, 已更正):
 *     Peripheral(连接) 与 Broadcaster(不可连接广播) 是两个独立的 role,
 *     连接占用的是 periph_role_count, 不占广播集。sd_ble_gap_adv_start 的
 *     NRF_ERROR_CONN_COUNT 措辞限定在"connectable advertiser cannot be
 *     started" —— 只挡可连接广播, 不挡不可连接广播。
 *     实证(比文档更硬): SDK 自带 eddystone 就是干这件事的, 且随包提供了
 *     pca10040e_s112 的预编译 hex, 说明 S112 上确实跑得通:
 *       components/ble/ble_services/eddystone/es_adv.c 的 BLE_GAP_EVT_CONNECTED
 *       分支直接调 es_adv_start_non_connctable_adv(), 原注释写着
 *       "The beacon must provide these advertisements for the client to see
 *        updated values during the connection."
 *     而同文件 adv_start() 里 sd_ble_gap_adv_start 只容忍 NRF_ERROR_BUSY,
 *     其余错误(含 CONN_COUNT / RESOURCES)一律 APP_ERROR_CHECK 断言 ——
 *     若连接期间开不可连接广播真会返回 CONN_COUNT, 这个官方例程一连上就 fault。
 *
 *     故"连接期间数据广播必然断流"不成立。真正的代价只是: 连接期间广播集要在
 *     "数据帧"与"无"之间被 stop/configure/start 反复重配, 每次重配后还得重设
 *     sd_ble_gap_tx_power_set(TX 功率是跟着广播集的, 见 main.c:508 的既有做法)。
 *
 * 本模块为这两个方案都留好了钩子: ble_link_adv_stop() 让出广播集,
 * ble_link_disconnect() 主动挂断, ble_link_is_advertising() 供上层判断
 * 当前归属。真正的切换编排(谁在什么时候占用广播集)应放在上层一个
 * "广播仲裁"模块里, 而不是塞进本模块 —— 本模块只管好自己那一半。
 *
 * ⚠ 依赖(必须已完成初始化, 顺序敏感):
 *     ble_stack_init()  → SoftDevice 已使能
 *     app_timer_init()  → ble_conn_params 内部要用 app_timer
 *   即 ble_link_init() 必须排在这两者之后。
 *
 * ⚠ sdk_config.h 相关开关(缺一不可, 见该文件内注释):
 *     NRF_SDH_BLE_PERIPHERAL_LINK_COUNT = 1   (原为 0, beacon 不接受连接)
 *     NRF_SDH_BLE_VS_UUID_COUNT         >= 1  (NUS 用 128bit 厂商 UUID)
 *     NRF_SDH_BLE_GATTS_ATTR_TAB_SIZE   足够放下 NUS 属性表
 *     BLE_NUS_ENABLED / NRF_BLE_GATT_ENABLED / NRF_BLE_QWR_ENABLED
 *     NRF_BLE_CONN_PARAMS_ENABLED
 *   改这些会推高 SoftDevice 占用的 RAM, 必须同步调整工程的 RAM 起始地址,
 *   否则 nrf_sdh_ble_enable() 返回 NRF_ERROR_NO_MEM。
 */

#ifndef BLE_LINK_H__
#define BLE_LINK_H__

#include <stdbool.h>
#include <stdint.h>
#include "sdk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 广播名(Complete Local Name)。暂定 "water"。
 * ⚠ 改长了要留意广播包 31 字节预算: Flags(3) + 名字(2+strlen) 必须放得下,
 *   NUS 的 128bit UUID 已挪到扫描响应包里, 不占这 31 字节。 */
#define BLE_LINK_DEVICE_NAME        "water"

/* 广播间隔(ms)。可连接广播, 40ms 是"搜得快"与"功耗可接受"的常用折中。 */
#define BLE_LINK_ADV_INTERVAL_MS    40

/* 广播时长: 0 = 不限时, 一直播到被连上。
 * 断开后默认自动重新开播(见 ble_link.c 的 DISCONNECTED 分支); 但若调用过
 * ble_link_adv_stop(), 则断开后保持静默, 直到再次显式 ble_link_adv_start()。 */
#define BLE_LINK_ADV_DURATION_MS    0

/* 期望的连接参数。连上后由 ble_conn_params 模块向主机协商。 */
#define BLE_LINK_MIN_CONN_INTERVAL_MS   20
#define BLE_LINK_MAX_CONN_INTERVAL_MS   75
#define BLE_LINK_SLAVE_LATENCY          0
#define BLE_LINK_CONN_SUP_TIMEOUT_MS    4000

/**@brief 收到对端数据的回调。
 *
 * @param p_data  数据首地址(仅在回调内有效, 需要留存请自行拷贝)。
 * @param len     字节数, 1..ble_link_max_data_len()。
 *
 * @note ⚠ 在 SoftDevice 事件中断上下文中执行(NRF_SDH_DISPATCH_MODEL = 0
 *       即 INTERRUPT 模型, 运行于 SWI2 / APP_IRQ_PRIORITY_LOW), 不是主循环。
 *       故回调内不可阻塞、不可长延时。后续 cmd 协议若解析开销大, 应在此
 *       只做入队, 交由主循环或 app_scheduler 处理。
 */
typedef void (*ble_link_rx_handler_t)(const uint8_t * p_data, uint16_t len);

/**@brief 初始化 GAP / GATT / NUS / 连接参数协商, 并配置好广播内容。
 *
 * 本函数只做"配置", 不开始广播 —— 开播由 ble_link_adv_start() 显式触发,
 * 便于调用方控制时机(例如先把传感器跑起来再放出连接入口)。
 *
 * @param rx_handler  收到对端数据的回调, 可传 NULL(此时仅打印日志)。
 *
 * @note 必须在 ble_stack_init() 与 app_timer_init() 之后调用。重复调用安全。
 *
 * @retval NRF_SUCCESS 成功, 否则为底层 SoftDevice / SDK 模块的错误码。
 */
ret_code_t ble_link_init(ble_link_rx_handler_t rx_handler);

/**@brief 开始可连接广播。已在广播中时重复调用安全(直接返回成功)。 */
ret_code_t ble_link_adv_start(void);

/**@brief 停止可连接广播。未在广播时重复调用安全(直接返回成功)。
 *
 * 停播后设备不再可被搜到/连接, 但已建立的连接不受影响 —— 本函数只关广播,
 * 不断开连接。要彻底关掉调试口, 用 ble_link_disconnect() 再调本函数。
 *
 * ⚠ 停播后 ble_link.c 的自动重开播逻辑也随之关闭: 一旦调用过本函数,
 *   后续断开连接不会再自动开播(否则"关"就没有意义了)。要恢复请显式调用
 *   ble_link_adv_start()。
 *
 * @retval NRF_SUCCESS             已停播, 或本来就没在播。
 * @retval NRF_ERROR_INVALID_STATE 模块未初始化。
 */
ret_code_t ble_link_adv_stop(void);

/**@brief 主动断开当前连接。未连接时直接返回成功。
 *
 * 用于"收到关闭命令后主动挂断"这类场景。断开后是否自动重新开播, 取决于
 * 上一次调用的是 ble_link_adv_start() 还是 ble_link_adv_stop()。
 *
 * @note 断开是异步的: 本函数返回后还要等 BLE_GAP_EVT_DISCONNECTED 才真正断开。
 */
ret_code_t ble_link_disconnect(void);

/**@brief 当前是否正在进行可连接广播。 */
bool ble_link_is_advertising(void);

/**@brief 当前是否已建立连接。 */
bool ble_link_is_connected(void);

/**@brief 单次可发送的最大字节数(受协商后的 ATT MTU 限制)。
 *
 * @note 未连接时返回按默认 MTU 估算的值; 连上并完成 MTU 交换后数值可能变大。
 */
uint16_t ble_link_max_data_len(void);

/**@brief 向对端发送数据(NUS TX 特性的 Notify)。
 *
 * @param p_data  待发送数据。
 * @param len     字节数, 需 <= ble_link_max_data_len()。
 *
 * @retval NRF_SUCCESS                 已交给 SoftDevice。
 * @retval NRF_ERROR_INVALID_STATE     未连接, 或对端未使能 Notify。
 * @retval NRF_ERROR_RESOURCES         SoftDevice 发送缓冲已满, 稍后重试。
 * @retval NRF_ERROR_DATA_SIZE         len 超过当前 MTU 允许的上限。
 */
ret_code_t ble_link_send(const uint8_t * p_data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* BLE_LINK_H__ */

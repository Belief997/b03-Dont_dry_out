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
 * ⚠ 广播集不归本模块所有 —— 由 ble_adv_mux 独占持有:
 *   S112 的 BLE_GAP_ADV_SET_COUNT_MAX == 1, 整个协议栈只有一个广播集。
 *   本模块与 ble_beacon(不可连接数据广播) 都通过 ble_adv_mux 申请, 只提交
 *   "内容 + 参数", 不碰句柄。ble_link_adv_start() 是抢占式的, 会停掉数据广播;
 *   ble_link_adv_stop() 则把广播集交还。谁在什么时候播由 main.c 编排。
 *
 * ==================================================================
 *  关于"不可连接的传感器数据广播 + 可连接调试广播 并存"(重要)
 * ==================================================================
 *
 * ⚠ 当前工程【不需要】两者并存 —— 需求已明确为分时且都是按需触发:
 *   平时什么都不播; 单击时数据广播播 3 次(BLE_BEACON_ADV_EVENTS)就自动停;
 *   长按时可连接广播占用广播集 30 秒。三种状态互不重叠, 广播集永远只有
 *   至多一个使用者。下面的论证保留下来, 是为了记录"想并存也做不到"这个
 *   硬约束, 免得日后有人再花时间去试。
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
 *     字节预算(实测 ble_advdata_encode, 上限 31; ⚠ 下列实测值是名字为 5 字符
 *     "water" 时测的, 现在名字带了 MAC 后缀变成 10 字符, 每项要再减 5 字节):
 *         名字 "water" + 16 字节厂商载荷 → 占 30 字节, 通过
 *         名字 "water" + 17 字节厂商载荷 → 占 31 字节, 通过(刚好用满)
 *         名字 "water" + 18 字节起        → NRF_ERROR_DATA_SIZE
 *       即名字 5 字符时载荷上限 17。换成 10 字符的带后缀名字后上限只有 12 字节,
 *       【装不下当前的 MANUF_DATA_LEN=16】—— 所以方案 A 若要启用, 得先缩短名字
 *       (或去掉后缀), 不能照抄上面这组数字。
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
 * 当前归属。当前工程走的是方案 B 的一个变体 —— 数据广播不是"平时一直播",
 * 而是"单击时播 3 次", 所以三种状态(静默 / 数据 3 连播 / 可连接窗口)天然
 * 互斥, 切换编排在 main.c(见那里的 adv_policy_*)。
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

/* 广播名(Complete Local Name)的固定前缀。
 *
 * 实际播出去的名字 = 前缀 + "_" + MAC 后两字节的大写十六进制, 例如
 * MAC C3:9A:12:34:AB:CD → "water_ABCD"。完整名字用 ble_link_device_name() 取,
 * 【不要】直接拿这个宏去打印或比较 —— 那会漏掉后缀。
 *
 * ⚠ 为什么加后缀: 之前所有板子都叫 "water", 同场多台设备在手机扫描列表里无法
 *   分辨。取 MAC 而不是 FICR->DEVICEID 是因为前者就是扫描列表里显示的地址,
 *   名字后四位与地址后四位一致, 肉眼即可对上是哪台。
 *
 * ⚠ 改长了要留意广播包 31 字节预算: Flags(3) + 名字段(2 + 名字长度) 必须放得下,
 *   即名字 <= 26 字符。超了 ble_advdata_encode() 【不返回错误】, 而是静默降级成
 *   Short Local Name 把名字截断(见 ble_advdata.c 的 name_encode)—— 空中名字变短
 *   却无人报错, 很难查。ble_link.c 里有编译期断言兜住这一点。
 *   NUS 的 128bit UUID 已挪到扫描响应包里, 不占这 31 字节。 */
#define BLE_LINK_DEVICE_NAME_BASE   "water"

/* 名字后缀的字符数: '_' + 4 位十六进制。 */
#define BLE_LINK_NAME_SUFFIX_LEN    5

/* ==================================================================
 *  MAC(GAP identity address)是怎么来的, 以及要改的话怎么改
 * ==================================================================
 *
 * 本工程【没有】设置过 MAC —— 用的是 SoftDevice 的默认行为:
 *   sd_softdevice_enable() 时协议栈自动装上一个 BLE_GAP_ADDR_TYPE_RANDOM_STATIC
 *   地址, 取自芯片出厂时烧进 FICR->DEVICEADDR[0..1] 的随机数, 每颗芯片唯一且
 *   终生不变(s112/headers/ble_gap.h 的 sd_ble_gap_addr_set 注释原文:
 *   "populated during the IC manufacturing process and remains unchanged for the
 *   lifetime of each IC")。所以名字后缀天然就是"这颗芯片的身份", 不需要标定,
 *   也不会因为重启或重新烧写而变。
 *
 * ⚠ Random static 地址的高两位被协议栈强制置成 11, 即 addr[5] |= 0xC0 ——
 *   所以观察到的 MAC 首字节总在 C0..FF, 这不是 bug。
 *
 * 真要改成自定的地址, 用 sd_ble_gap_addr_set(&addr), 约束如下(均来自 ble_gap.h):
 *   - addr_type 只能是 BLE_GAP_ADDR_TYPE_PUBLIC 或 ..._RANDOM_STATIC;
 *     两种 private 类型不能用这个 API 设, 要走 sd_ble_gap_privacy_set();
 *   - 选 RANDOM_STATIC 时 addr[5] 的高两位必须是 11, 否则返回
 *     BLE_ERROR_GAP_INVALID_BLE_ADDR;
 *   - 选 PUBLIC 意味着声称拥有一个 IEEE 分配的 OUI, 没买 OUI 就别用;
 *   - 广播期间不能改, 会返回 NRF_ERROR_INVALID_STATE → 必须在
 *     ble_link_adv_start() 之前(本模块的 init 阶段)调用;
 *   - addr[] 是【小端】: addr[0] 是最低字节, 手机上显示的 MAC 是
 *     addr[5]:addr[4]:...:addr[0]。填反了名字后缀与扫描列表就对不上。
 *
 * 若日后要"MAC 可配置", 落点是 app_storage 的配置区(加 6 字节字段 + 一个
 * "是否启用自定义 MAC"的标志), 并在 ble_link_init() 最前面 set 一次。
 * 当前不做 —— 出厂唯一地址已经满足"能区分设备"这个唯一需求。 */

/* 广播间隔(ms)。可连接广播, 40ms 是"搜得快"与"功耗可接受"的常用折中。 */
#define BLE_LINK_ADV_INTERVAL_MS    40

/* 发射功率(dBm), S112 支持 -40..+4 的若干档位。TX 功率是跟着广播集的,
 * 每次重配后都要重设 —— 这件事由 ble_adv_mux 统一代劳, 这里只给值。 */
#define BLE_LINK_TX_POWER_DBM       0

/* 可连接广播时长(ms)。0 = 不限时, 一直播到被连上。
 *
 * 取 30000(30s): 让协议栈自己在到期时产生
 * BLE_GAP_EVT_ADV_SET_TERMINATED(reason = TIMEOUT), 比自起 app_timer 省资源。
 * 窗口不宜过长: 期间设备是可被连接的, 且 40ms 间隔的可连接广播比数据广播费电。
 *
 * ⚠ 单位换算: 本宏是毫秒, 而 ble_gap_adv_params_t::duration 的单位是 10ms,
 *   转换在 advertising_config() 里做(/10)。上限
 *   BLE_GAP_ADV_TIMEOUT_LIMITED_MAX = 18000(10ms 单位) = 180 秒, 超过会让
 *   sd_ble_gap_adv_set_configure() 返回 NRF_ERROR_INVALID_PARAM。
 *
 * 到期后本模块上报 BLE_LINK_EVT_ADV_TIMEOUT, 由 main.c 关灯收尾。 */
#define BLE_LINK_ADV_DURATION_MS    30000

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

/**@brief 链路状态事件。
 *
 * 存在的理由: 广播集是共享资源(见文件头), "连上/断开/窗口到期之后该谁播"
 * 是应用层策略, 本模块不该自作主张。所以这些时刻只上报, 由 main.c 决定
 * 恢复数据广播、关灯等动作。
 */
typedef enum
{
    BLE_LINK_EVT_CONNECTED = 0,     /**< 已建立连接(广播已由协议栈自动停止) */
    BLE_LINK_EVT_DISCONNECTED,      /**< 连接已断开。本模块【不会】自动重开播 */
    BLE_LINK_EVT_ADV_TIMEOUT        /**< 限时可连接广播到期而无人连上 */
} ble_link_evt_t;

/**@brief 链路状态事件回调。
 *
 * @note ⚠ 与 rx_handler 同样在 SoftDevice 事件中断上下文中执行。回调内只做
 *       轻量操作(设标志、启停定时器、切广播), 不可阻塞, 不可 APP_ERROR_CHECK
 *       —— 中断里断言失败会直接进 fault handler。
 */
typedef void (*ble_link_evt_handler_t)(ble_link_evt_t evt);

/**@brief 事件名字符串(用于日志打印)。 */
const char * ble_link_evt_str(ble_link_evt_t evt);

/**@brief 实际播出去的完整广播名(BLE_LINK_DEVICE_NAME_BASE + "_" + MAC 后四位),
 *        以 NUL 结尾, 指向模块内的静态缓冲, 调用方不要修改也不必拷贝。
 *
 * 后缀在 ble_link_init() 里组装 —— sd_ble_gap_addr_get() 要求 SoftDevice 已使能,
 * 所以拿不到更早。
 *
 * @note 在 ble_link_init() 之前调用会返回【只有前缀】的名字(那时还没读到 MAC),
 *       而不是空串或野指针 —— 这样日志可以无条件调用它, 不用判初始化状态。
 */
const char * ble_link_device_name(void);

/**@brief 初始化 GAP / GATT / NUS / 连接参数协商, 并准备好广播内容。
 *
 * 本函数只做"配置", 不开始广播 —— 开播由 ble_link_adv_start() 显式触发,
 * 便于调用方控制时机(例如先把传感器跑起来再放出连接入口)。
 *
 * @param rx_handler   收到对端数据的回调, 可传 NULL(此时仅打印日志)。
 * @param evt_handler  链路状态事件回调, 可传 NULL(此时仅打印日志)。
 *                     ⚠ 传 NULL 意味着断开后没人恢复数据广播 —— 本模块
 *                     不再自动重开播, 详见 ble_link_evt_t 说明。
 *
 * @note 必须在 ble_stack_init() 与 app_timer_init() 之后调用。重复调用安全。
 *
 * @retval NRF_SUCCESS 成功, 否则为底层 SoftDevice / SDK 模块的错误码。
 */
ret_code_t ble_link_init(ble_link_rx_handler_t  rx_handler,
                         ble_link_evt_handler_t evt_handler);

/**@brief 开始可连接广播(向 ble_adv_mux 抢占广播集)。
 *
 * ⚠ 会抢占: 若当前是数据广播在播, 那边会被停掉。这是"长按打开调试口"
 *   所需要的行为, 是否该抢由调用方决定。
 *
 * 已在广播中时重复调用安全(直接返回成功)。 */
ret_code_t ble_link_adv_start(void);

/**@brief 停止可连接广播, 把广播集交还给 ble_adv_mux。未在广播时安全。
 *
 * 停播后设备不再可被搜到/连接, 但已建立的连接不受影响 —— 本函数只关广播,
 * 不断开连接。要彻底关掉调试口, 用 ble_link_disconnect() 再调本函数。
 *
 * @retval NRF_SUCCESS             已停播, 或本来就没在播。
 * @retval NRF_ERROR_INVALID_STATE 模块未初始化。
 */
ret_code_t ble_link_adv_stop(void);

/**@brief 主动断开当前连接。未连接时直接返回成功。
 *
 * 用于"收到关闭命令后主动挂断"这类场景。断开后会产生
 * BLE_LINK_EVT_DISCONNECTED, 由上层决定下一步(本模块不自动重开播)。
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

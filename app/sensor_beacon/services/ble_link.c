/**
 * ble_link —— 可连接广播 + NUS 透传链路实现
 * ------------------------------------------------------------------
 * 模块装配关系(自下而上):
 *
 *   SoftDevice S112 (由 main.c 的 ble_stack_init 使能)
 *        ↓  BLE 事件经 nrf_sdh_ble 分发给各 NRF_SDH_BLE_OBSERVER
 *   ┌────────────┬─────────────┬──────────────┬────────────────┐
 *   │ nrf_ble_gatt│ nrf_ble_qwr │ ble_conn_params│ ble_nus       │
 *   │ MTU 协商    │ 长写入排队  │ 连接参数协商   │ RX/TX 特性    │
 *   └────────────┴─────────────┴──────────────┴────────────────┘
 *        ↓ NUS 的 RX 数据事件
 *   本文件的 nus_data_handler → 打印 + 转交上层 rx_handler
 *
 * 每个 SDK 模块用各自的 xxx_DEF 宏在文件作用域注册自己的事件观察者,
 * 所以它们都必须是静态全局 —— 这是链接段(section)机制的要求, 不能挪进函数。
 */

#include "ble_link.h"

#include <string.h>

#include "nordic_common.h"
#include "app_error.h"
#include "app_timer.h"
#include "app_util.h"       /* STATIC_ASSERT —— 兜住设备名的 31 字节预算 */

#include "ble.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_conn_params.h"
#include "ble_nus.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"
#include "nrf_sdh_ble.h"

#include "ble_adv_mux.h"

#define NRF_LOG_MODULE_NAME ble_link
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

/* 本模块自己的 BLE 事件观察者优先级。
 * 3 与 main.c 的 APP_BLE_OBSERVER_PRIO 相同: 同优先级的观察者都会被调用,
 * 互不排斥, 只是先后顺序由链接顺序决定。gatt=1 / qwr=2 / nus=2 由各自
 * sdk_config 中的 xxx_BLE_OBSERVER_PRIO 指定, 数值小的先收到事件。 */
#define BLE_LINK_OBSERVER_PRIO      3

/* 连接参数协商时序(照抄 SDK ble_app_uart 的推荐值)。
 * 首次延迟 5s 是为了避开连接刚建立时的服务发现高峰。 */
#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(5000)
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(30000)
#define MAX_CONN_PARAMS_UPDATE_COUNT    3

/* ---------- SDK 模块实例(必须文件作用域: xxx_DEF 内含链接段变量) ---------- */

BLE_NUS_DEF(m_nus, NRF_SDH_BLE_TOTAL_LINK_COUNT);   /* NUS 服务实例 */
NRF_BLE_GATT_DEF(m_gatt);                           /* MTU 协商 */
NRF_BLE_QWR_DEF(m_qwr);                             /* 长写入排队 */

/* ---------- 模块状态 ---------- */

static bool                     m_inited      = false;
static uint16_t                 m_conn_handle = BLE_CONN_HANDLE_INVALID;
static ble_link_rx_handler_t    m_rx_handler  = NULL;
static ble_link_evt_handler_t   m_evt_handler = NULL;

/* 实际广播名: 前缀 + "_" + MAC 后两字节的大写十六进制 + NUL。
 *
 * 初值【只有前缀】而不是空串: 万一 MAC 还没读到(init 之前)就有人打日志, 拿到的
 * 是"不完整但可读"的名字, 而不是空串让人以为名字丢了。 */
static char m_dev_name[sizeof(BLE_LINK_DEVICE_NAME_BASE) + BLE_LINK_NAME_SUFFIX_LEN]
                = BLE_LINK_DEVICE_NAME_BASE;

/* 名字必须放得进广播包, 否则 ble_advdata_encode() 会静默降级成 Short Local Name
 * 把它截断(不报错), 空中名字与代码里的不一致且无人提示。
 * 预算: 31 - Flags 段(3) - 名字段头(2) = 26 字符。 */
STATIC_ASSERT((sizeof(m_dev_name) - 1) <=
              (BLE_GAP_ADV_SET_DATA_SIZE_MAX - AD_TYPE_FLAGS_SIZE - AD_DATA_OFFSET));

/* 注意这里【没有】m_adv_handle: 广播句柄由 ble_adv_mux 独占(S112 只有一个
 * 广播集), 本模块只提交"内容 + 参数"。也没有 m_advertising ——
 * "我是否在播"直接问 mux(见 ble_link_is_advertising), 避免两处状态打架。
 *
 * 也没有"希望保持可连接"的意图标志了: 断开后是否恢复什么广播, 由上层按
 * BLE_LINK_EVT_DISCONNECTED 决定。本模块不再自动重开播 —— 广播集是共享的,
 * 自作主张重开会把上层刚恢复的数据广播抢掉。 */

/* 编码后的广播数据缓冲。
 * ⚠ 这两块内存必须在整个广播期间保持有效 —— sd_ble_gap_adv_set_configure()
 *   只记住指针, SoftDevice 每次发包时直接读这里, 不做内部拷贝。
 *   本模块的广播内容(设备名 + NUS UUID)是恒定的, 故只编码一次、常驻使用;
 *   不需要像 ble_beacon 那样双缓冲(那是"广播中更新数据"才有的要求)。 */
static uint8_t                  m_enc_advdata[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t                  m_enc_scanrsp[BLE_GAP_ADV_SET_DATA_SIZE_MAX];

static ble_gap_adv_data_t       m_adv_data =
{
    .adv_data      = { .p_data = m_enc_advdata, .len = BLE_GAP_ADV_SET_DATA_SIZE_MAX },
    .scan_rsp_data = { .p_data = m_enc_scanrsp, .len = BLE_GAP_ADV_SET_DATA_SIZE_MAX }
};
static ble_gap_adv_params_t     m_adv_params;

/* NUS 的 128bit 厂商 UUID。放在扫描响应包里而非广播包 ——
 * 16 字节 UUID + 2 字节头就占掉 31 字节预算的一大半, 与设备名挤不下。
 *
 * ⚠ type 字段留空, 在 advertising_config() 里填入 m_nus.uuid_type ——
 *   那是 ble_nus_init() 调 sd_ble_uuid_vs_add() 后由协议栈实际分配的槽位号。
 *   不硬编码 BLE_UUID_TYPE_VENDOR_BEGIN: 若日后再注册别的厂商 UUID,
 *   NUS 不一定还是第一个, 硬编码会广播出错误的 UUID。 */
static ble_uuid_t m_adv_uuids[] =
{
    { BLE_UUID_NUS_SERVICE, BLE_UUID_TYPE_UNKNOWN }
};

/* ==================================================================
 *  NUS 数据回调: 收到对端写入
 * ================================================================== */

static void nus_data_handler(ble_nus_evt_t * p_evt)
{
    switch (p_evt->type)
    {
        case BLE_NUS_EVT_RX_DATA:
            /* 收发日志按需求打印: 先摘要(长度), 再 hexdump 原始字节。
             * 后续 cmd 协议接进来后, 这里的 hexdump 就是最原始的抓包证据。 */
            NRF_LOG_INFO("RX %u bytes:", p_evt->params.rx_data.length);
            NRF_LOG_HEXDUMP_INFO(p_evt->params.rx_data.p_data,
                                 p_evt->params.rx_data.length);

            if (m_rx_handler != NULL)
            {
                m_rx_handler(p_evt->params.rx_data.p_data,
                             p_evt->params.rx_data.length);
            }
            break;

        case BLE_NUS_EVT_COMM_STARTED:
            /* 对端使能了 TX 特性的 Notify —— 此刻起 ble_link_send() 才有意义 */
            NRF_LOG_INFO("NUS notifications enabled (link ready for TX).");
            break;

        case BLE_NUS_EVT_COMM_STOPPED:
            NRF_LOG_INFO("NUS notifications disabled.");
            break;

        case BLE_NUS_EVT_TX_RDY:
            /* 上一包已发出, 发送缓冲有空位。当前实现不做排队重发,
             * 上层遇到 NRF_ERROR_RESOURCES 自行退避即可。 */
            break;

        default:
            break;
    }
}

/* ==================================================================
 *  BLE 事件处理
 * ================================================================== */

const char * ble_link_evt_str(ble_link_evt_t evt)
{
    switch (evt)
    {
        case BLE_LINK_EVT_CONNECTED:    return "CONNECTED";
        case BLE_LINK_EVT_DISCONNECTED: return "DISCONNECTED";
        case BLE_LINK_EVT_ADV_TIMEOUT:  return "ADV_TIMEOUT";
        default:                        return "?";
    }
}

/* 上报状态事件。回调可能为 NULL(上层不关心), 那就只留日志。 */
static void evt_report(ble_link_evt_t evt)
{
    if (m_evt_handler != NULL)
    {
        m_evt_handler(evt);
    }
    else
    {
        NRF_LOG_INFO("link evt %s (no handler registered)", ble_link_evt_str(evt));
    }
}

static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    (void)p_context;

    ret_code_t err;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;

            NRF_LOG_INFO("Connected (handle 0x%04x).", m_conn_handle);

            /* 把连接句柄交给 QWR, 否则长写入(超过单包)会被拒 */
            err = nrf_ble_qwr_conn_handle_assign(&m_qwr, m_conn_handle);
            if (err != NRF_SUCCESS)
            {
                NRF_LOG_WARNING("qwr_conn_handle_assign failed (0x%08x)", err);
            }

            /* 广播已由协议栈自动停止(ble_adv_mux 在优先级 1 已同步过状态),
             * 但广播集仍记在本模块名下 —— 上层若要开数据广播会正常抢过去。 */
            evt_report(BLE_LINK_EVT_CONNECTED);
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            NRF_LOG_INFO("Disconnected (reason 0x%02x).",
                         p_ble_evt->evt.gap_evt.params.disconnected.reason);
            m_conn_handle = BLE_CONN_HANDLE_INVALID;

            /* 【不】自动重开可连接广播 —— 广播集是与 ble_beacon 共享的,
             * 自作主张重开会把上层想恢复的数据广播抢掉。断开之后该播什么
             * 是应用层策略, 只上报, 由 main.c 决定。 */
            evt_report(BLE_LINK_EVT_DISCONNECTED);
            break;

        case BLE_GAP_EVT_ADV_SET_TERMINATED:
        {
            uint8_t reason = p_ble_evt->evt.gap_evt.params.adv_set_terminated.reason;

            NRF_LOG_INFO("Advertising terminated (reason 0x%02x).", reason);

            /* ⚠ 这个事件对【任何】广播集持有者都会来一次, 包括 ble_beacon
             *   的限时广播。只有"刚终止的广播确实是我的"才该上报 ——
             *   靠 mux 的归属信息判断(它不在 CONNECTED/TERMINATED 时清 owner,
             *   正是为了这个用途)。
             *
             *   而 TIMEOUT 之外的 reason(如 LIMIT_REACHED)不属于"窗口到期",
             *   本模块的 duration 是唯一会触发 TIMEOUT 的配置, 故只认它。 */
            if ((reason == BLE_GAP_EVT_ADV_SET_TERMINATED_REASON_TIMEOUT) &&
                (ble_adv_mux_owner() == BLE_ADV_OWNER_LINK))
            {
                evt_report(BLE_LINK_EVT_ADV_TIMEOUT);
            }
        } break;

        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
        {
            /* 对端请求换 PHY: 一律回 AUTO, 让协议栈自己挑最优 */
            ble_gap_phys_t const phys =
            {
                .rx_phys = BLE_GAP_PHY_AUTO,
                .tx_phys = BLE_GAP_PHY_AUTO,
            };
            err = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            if (err != NRF_SUCCESS)
            {
                NRF_LOG_WARNING("phy_update failed (0x%08x)", err);
            }
        } break;

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            /* 不支持配对/加密: 透传口当前是明文的。
             * ⚠ 后续 cmd 协议若含敏感操作(标定写入、清零等), 应在协议层加
             *   鉴权, 或改为要求配对 —— 现在任何人连上就能发命令。 */
            err = sd_ble_gap_sec_params_reply(m_conn_handle,
                                              BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP,
                                              NULL, NULL);
            if (err != NRF_SUCCESS)
            {
                NRF_LOG_WARNING("sec_params_reply failed (0x%08x)", err);
            }
            break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            /* 未保存系统属性(无 bonding) → 回空 */
            err = sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
            if (err != NRF_SUCCESS)
            {
                NRF_LOG_WARNING("sys_attr_set failed (0x%08x)", err);
            }
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            /* GATT 事务超时 → 规范要求断开该连接 */
            err = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                        BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            if (err != NRF_SUCCESS)
            {
                NRF_LOG_WARNING("disconnect on GATTS timeout failed (0x%08x)", err);
            }
            break;

        default:
            break;
    }
}

/* 事件观察者必须在文件作用域注册(链接段内的静态变量) */
NRF_SDH_BLE_OBSERVER(m_ble_link_observer, BLE_LINK_OBSERVER_PRIO, ble_evt_handler, NULL);

/* ==================================================================
 *  各子模块初始化
 * ================================================================== */

/* 连接参数协商失败 → 断开。通过 error_handler 回调上报。 */
static void conn_params_error_handler(uint32_t nrf_error)
{
    NRF_LOG_WARNING("conn_params error (0x%08x)", nrf_error);
}

/* QWR 内存不足等错误 */
static void qwr_error_handler(uint32_t nrf_error)
{
    NRF_LOG_WARNING("qwr error (0x%08x)", nrf_error);
}

/* 把 MAC 后两字节追加成名字后缀: m_dev_name = 前缀 + "_" + "ABCD"。
 *
 * ⚠ ble_gap_addr_t::addr 是【小端】(addr[0] 为最低字节), 而手机扫描列表里显示的
 *   MAC 是 addr[5]:addr[4]:...:addr[0]。所以"显示形式的后四位"= addr[1] 再 addr[0],
 *   两字节的先后不能反 —— 反了名字后缀和地址后四位对不上, 加后缀这件事就白做了。
 *
 * ⚠ 不用 snprintf("%02X"): 那会把 C 库的格式化实现拖进 flash(几 KB), 而这里要做的
 *   只是 4 次查表。手工展开既短也更明确。
 *
 * ⚠ 幂等: 每次都从固定前缀长度处开始写, 所以重复调用不会把后缀叠加成
 *   "water_ABCD_ABCD"。 */
static ret_code_t device_name_build(void)
{
    static const char HEX[] = "0123456789ABCDEF";

    ble_gap_addr_t addr;

    /* 只能在 SoftDevice 使能后调用 —— 本函数只被 gap_params_init() 用,
     * 而 ble_link_init() 的前置条件(见头文件)已经保证了这一点。 */
    ret_code_t err = sd_ble_gap_addr_get(&addr);
    if (err != NRF_SUCCESS)
    {
        /* 读不到地址就保持"只有前缀"的名字继续跑: 名字不好看是显示问题,
         * 不值得让整个链路初始化失败。调用方据此只记警告, 不中断 init。 */
        NRF_LOG_WARNING("addr_get failed (0x%08x); name stays \"%s\".",
                        err, m_dev_name);
        return err;
    }

    size_t n = strlen(BLE_LINK_DEVICE_NAME_BASE);

    m_dev_name[n++] = '_';
    m_dev_name[n++] = HEX[(addr.addr[1] >> 4) & 0x0F];
    m_dev_name[n++] = HEX[ addr.addr[1]       & 0x0F];
    m_dev_name[n++] = HEX[(addr.addr[0] >> 4) & 0x0F];
    m_dev_name[n++] = HEX[ addr.addr[0]       & 0x0F];
    m_dev_name[n]   = '\0';

    /* ⚠ 本 SDK 的日志一条最多 6 个格式参数(LOG_INTERNAL_6), 故 MAC 与名字分两条打。
     * ⚠ %s 传的是静态缓冲(不是栈上的), 无需 NRF_LOG_PUSH —— 即便日志改成
     *   deferred 模式, 刷出时这块内存依然有效且内容不再变化。 */
    NRF_LOG_INFO("MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 addr.addr[5], addr.addr[4], addr.addr[3],
                 addr.addr[2], addr.addr[1], addr.addr[0]);
    NRF_LOG_INFO("  addr_type %u (1=random static) -> device name \"%s\"",
                 addr.addr_type, m_dev_name);

    return NRF_SUCCESS;
}

static ret_code_t gap_params_init(void)
{
    ret_code_t              err;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    /* 先把带 MAC 后缀的名字拼出来, 再设进协议栈。
     * 失败不阻断: device_name_build() 已经退回"只有前缀"的可用名字。 */
    (void)device_name_build();

    /* 设备名开放读取, 无需加密 */
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    /* ⚠ 这里设的是 GAP 设备名, 而广播包里的名字是 ble_advdata_encode() 在
     *   advertising_config() 里用 sd_ble_gap_device_name_get() 读回来编码的 ——
     *   所以设完这一次就够了, 不需要在广播数据里再写一遍名字。
     *   反过来说: 本调用必须排在 advertising_config() 之前, 否则广播包里
     *   编进去的还是协议栈默认的 "nRF5x"。 */
    err = sd_ble_gap_device_name_set(&sec_mode,
                                     (const uint8_t *)m_dev_name,
                                     strlen(m_dev_name));
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));
    gap_conn_params.min_conn_interval = MSEC_TO_UNITS(BLE_LINK_MIN_CONN_INTERVAL_MS, UNIT_1_25_MS);
    gap_conn_params.max_conn_interval = MSEC_TO_UNITS(BLE_LINK_MAX_CONN_INTERVAL_MS, UNIT_1_25_MS);
    gap_conn_params.slave_latency     = BLE_LINK_SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = MSEC_TO_UNITS(BLE_LINK_CONN_SUP_TIMEOUT_MS, UNIT_10_MS);

    return sd_ble_gap_ppcp_set(&gap_conn_params);
}

static ret_code_t gatt_init(void)
{
    /* evt_handler 传 NULL: 本模块不需要感知 MTU 变化事件, 需要时用
     * nrf_ble_gatt_eff_mtu_get() 现查即可(见 ble_link_max_data_len)。 */
    return nrf_ble_gatt_init(&m_gatt, NULL);
}

static ret_code_t services_init(void)
{
    ret_code_t          err;
    nrf_ble_qwr_init_t  qwr_init = {0};
    ble_nus_init_t      nus_init;

    qwr_init.error_handler = qwr_error_handler;
    err = nrf_ble_qwr_init(&m_qwr, &qwr_init);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    memset(&nus_init, 0, sizeof(nus_init));
    nus_init.data_handler = nus_data_handler;

    return ble_nus_init(&m_nus, &nus_init);
}

static ret_code_t conn_params_init(void)
{
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    /* p_conn_params = NULL → 使用上面 sd_ble_gap_ppcp_set() 设定的值 */
    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;  /* 连接即开始协商 */
    cp_init.disconnect_on_fail             = false;  /* 协商失败也保持连接, 只记日志 */
    cp_init.evt_handler                    = NULL;
    cp_init.error_handler                  = conn_params_error_handler;

    return ble_conn_params_init(&cp_init);
}

static ret_code_t advertising_config(void)
{
    ret_code_t   err;
    ble_advdata_t advdata;
    ble_advdata_t srdata;

    /* --- 广播包: Flags + 完整设备名 --- */
    memset(&advdata, 0, sizeof(advdata));
    advdata.name_type = BLE_ADVDATA_FULL_NAME;
    advdata.flags     = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;

    m_adv_data.adv_data.len = BLE_GAP_ADV_SET_DATA_SIZE_MAX;
    err = ble_advdata_encode(&advdata, m_adv_data.adv_data.p_data,
                             &m_adv_data.adv_data.len);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    /* --- 扫描响应包: NUS 的 128bit UUID --- */

    /* 用 ble_nus_init() 实际拿到的 UUID 类型槽位(见 m_adv_uuids 处说明)。
     * 故本函数必须排在 services_init() 之后。 */
    m_adv_uuids[0].type = m_nus.uuid_type;

    memset(&srdata, 0, sizeof(srdata));
    srdata.uuids_complete.uuid_cnt = ARRAY_SIZE(m_adv_uuids);
    srdata.uuids_complete.p_uuids  = m_adv_uuids;

    m_adv_data.scan_rsp_data.len = BLE_GAP_ADV_SET_DATA_SIZE_MAX;
    err = ble_advdata_encode(&srdata, m_adv_data.scan_rsp_data.p_data,
                             &m_adv_data.scan_rsp_data.len);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    /* --- 广播参数: 可连接可扫描 --- */
    memset(&m_adv_params, 0, sizeof(m_adv_params));
    m_adv_params.properties.type = BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED;
    m_adv_params.p_peer_addr     = NULL;            /* 非定向 */
    m_adv_params.filter_policy   = BLE_GAP_ADV_FP_ANY;
    m_adv_params.interval        = MSEC_TO_UNITS(BLE_LINK_ADV_INTERVAL_MS, UNIT_0_625_MS);
    m_adv_params.duration        = BLE_LINK_ADV_DURATION_MS / 10;   /* 单位 10ms, 0=不限时 */

    /* 到此只是"把内容和参数备好"。真正的 sd_ble_gap_adv_set_configure()
     * 由 ble_adv_mux_start() 在抢到广播集时调用 —— 广播集只有一个, 谁在播
     * 就得由谁最后 configure, 本模块在 init 阶段抢跑只会把 beacon 的配置顶掉
     * (或反之被顶掉), 状态无从对账。 */
    return NRF_SUCCESS;
}

/* ==================================================================
 *  公共接口
 * ================================================================== */

ret_code_t ble_link_init(ble_link_rx_handler_t  rx_handler,
                         ble_link_evt_handler_t evt_handler)
{
    ret_code_t err;

    if (m_inited)
    {
        /* 允许重复调用时只更新回调 */
        m_rx_handler  = rx_handler;
        m_evt_handler = evt_handler;
        return NRF_SUCCESS;
    }

    m_rx_handler  = rx_handler;
    m_evt_handler = evt_handler;
    m_conn_handle = BLE_CONN_HANDLE_INVALID;

    err = gap_params_init();
    if (err != NRF_SUCCESS) { NRF_LOG_ERROR("gap_params_init: 0x%08x", err);  return err; }

    err = gatt_init();
    if (err != NRF_SUCCESS) { NRF_LOG_ERROR("gatt_init: 0x%08x", err);        return err; }

    err = services_init();
    if (err != NRF_SUCCESS) { NRF_LOG_ERROR("services_init: 0x%08x", err);    return err; }

    err = conn_params_init();
    if (err != NRF_SUCCESS) { NRF_LOG_ERROR("conn_params_init: 0x%08x", err); return err; }

    err = advertising_config();
    if (err != NRF_SUCCESS) { NRF_LOG_ERROR("advertising_config: 0x%08x", err); return err; }

    m_inited = true;

    NRF_LOG_INFO("BLE link ready: name=\"%s\", NUS transparent (not advertising yet).",
                 m_dev_name);
    NRF_LOG_INFO("  adv interval %ums, window %us, max payload %u bytes",
                 BLE_LINK_ADV_INTERVAL_MS, BLE_LINK_ADV_DURATION_MS / 1000,
                 ble_link_max_data_len());
    return NRF_SUCCESS;
}

ret_code_t ble_link_adv_start(void)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    /* 已连接时不能开【可连接】广播: S112 只有 1 个 peripheral 连接槽,
     * 槽位被占满时 sd_ble_gap_adv_start() 会返回 NRF_ERROR_CONN_COUNT。
     * 提前返回成功: 调用方想要的"能被连上"这件事已经成立(它就连着),
     * 不该让它去区分这种无意义的失败。
     *
     * ⚠ 该限制只针对可连接广播。连接期间开【不可连接】广播是允许的
     *   (Broadcaster 与 Peripheral 是两个独立 role), ble_beacon 不受此约束
     *   —— 详见 ble_link.h 头部说明。 */
    if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
    {
        NRF_LOG_INFO("Already connected; connectable advertising is moot.");
        return NRF_SUCCESS;
    }

    /* 抢占式: mux 内部会先停掉当前持有者(可能是数据广播), 再 configure
     * 并重设 TX 功率, 最后 start。本模块不碰广播句柄。 */
    ret_code_t err = ble_adv_mux_start(BLE_ADV_OWNER_LINK, &m_adv_data,
                                       &m_adv_params, BLE_LINK_TX_POWER_DBM);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    NRF_LOG_INFO("Advertising as \"%s\" (connectable, %us window).",
                 m_dev_name, BLE_LINK_ADV_DURATION_MS / 1000);
    return NRF_SUCCESS;
}

ret_code_t ble_link_adv_stop(void)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    /* 礼让式: 若广播集当前不在本模块名下(例如已被数据广播抢走), mux 直接
     * 返回成功 —— 我们的目标状态"不再播可连接包"本就已达成。 */
    ret_code_t err = ble_adv_mux_stop(BLE_ADV_OWNER_LINK);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    NRF_LOG_INFO("Connectable advertising stopped (adv set released).");
    return NRF_SUCCESS;
}

ret_code_t ble_link_disconnect(void)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
    {
        return NRF_SUCCESS;         /* 本来就没连接 */
    }

    /* 断开是异步的: 真正断开要等 BLE_GAP_EVT_DISCONNECTED。
     * 故此处不清 m_conn_handle, 交给事件分支统一清理。 */
    return sd_ble_gap_disconnect(m_conn_handle,
                                 BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
}

const char * ble_link_device_name(void)
{
    return m_dev_name;
}

bool ble_link_is_advertising(void)
{
    /* 不自己记状态: 广播集是共享的, 本模块的"在播"当且仅当
     * 广播集归我 且 确实在播。 */
    return (ble_adv_mux_owner() == BLE_ADV_OWNER_LINK) && ble_adv_mux_is_advertising();
}

bool ble_link_is_connected(void)
{
    return (m_conn_handle != BLE_CONN_HANDLE_INVALID);
}

uint16_t ble_link_max_data_len(void)
{
    if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
    {
        /* 未连接: 按编译期配置的 MTU 上限估算 */
        return BLE_NUS_MAX_DATA_LEN;
    }

    /* 已连接: 用协商后的实际 MTU。ATT 头开销 = opcode(1) + handle(2)。 */
    uint16_t mtu = nrf_ble_gatt_eff_mtu_get(&m_gatt, m_conn_handle);
    if (mtu < (OPCODE_LENGTH + HANDLE_LENGTH))
    {
        return 0;
    }
    return (uint16_t)(mtu - OPCODE_LENGTH - HANDLE_LENGTH);
}

ret_code_t ble_link_send(const uint8_t * p_data, uint16_t len)
{
    if (!m_inited || (m_conn_handle == BLE_CONN_HANDLE_INVALID))
    {
        return NRF_ERROR_INVALID_STATE;
    }

    if ((p_data == NULL) || (len == 0))
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    if (len > ble_link_max_data_len())
    {
        return NRF_ERROR_DATA_SIZE;
    }

    /* ble_nus_data_send 会把实际发出的长度写回 length, 且形参非 const,
     * 故这里用局部变量承接, 不改动调用方缓冲。 */
    uint16_t   length = len;
    ret_code_t err    = ble_nus_data_send(&m_nus, (uint8_t *)p_data,
                                          &length, m_conn_handle);

    if (err == NRF_SUCCESS)
    {
        NRF_LOG_INFO("TX %u bytes:", length);
        NRF_LOG_HEXDUMP_INFO(p_data, length);
    }
    else if (err != NRF_ERROR_RESOURCES)
    {
        /* RESOURCES 是正常的背压信号, 不当异常刷日志 */
        NRF_LOG_WARNING("TX failed (0x%08x)", err);
    }

    return err;
}

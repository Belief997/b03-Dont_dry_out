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

#include "ble.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_conn_params.h"
#include "ble_nus.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"
#include "nrf_sdh_ble.h"

#define NRF_LOG_MODULE_NAME ble_link
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

/* SoftDevice 连接配置标签。必须与 main.c 里 nrf_sdh_ble_default_cfg_set()
 * 使用的标签一致 —— sd_ble_gap_adv_start() 靠它找到对应的连接配置。 */
#define BLE_LINK_CONN_CFG_TAG       1

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

static bool                     m_inited     = false;
static uint16_t                 m_conn_handle = BLE_CONN_HANDLE_INVALID;
static uint8_t                  m_adv_handle  = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static bool                     m_advertising = false;
static ble_link_rx_handler_t    m_rx_handler  = NULL;

/* "希望保持可连接" 的意图标志, 与 m_advertising(实际是否在播) 分开记。
 *
 * 为什么需要两个状态: 广播会因为"被连上"而由 SoftDevice 自动停止, 此时
 * m_advertising 变 false, 但我们的意图仍然是"这个口应该开着" —— 断开后
 * 要自动重播。而 ble_link_adv_stop() 表达的是相反的意图: 从此别再播了,
 * 断开后也不要自己起来。用一个 bool 区分不了这两种 false。 */
static bool                     m_adv_enabled = false;

/* 编码后的广播数据缓冲。
 * ⚠ 这两块内存必须在整个广播期间保持有效 —— sd_ble_gap_adv_set_configure()
 *   只记住指针, SoftDevice 每次发包时直接读这里, 不做内部拷贝。 */
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

static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    (void)p_context;

    ret_code_t err;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            m_advertising = false;      /* 连上后 SoftDevice 自动停播 */

            NRF_LOG_INFO("Connected (handle 0x%04x).", m_conn_handle);

            /* 把连接句柄交给 QWR, 否则长写入(超过单包)会被拒 */
            err = nrf_ble_qwr_conn_handle_assign(&m_qwr, m_conn_handle);
            if (err != NRF_SUCCESS)
            {
                NRF_LOG_WARNING("qwr_conn_handle_assign failed (0x%08x)", err);
            }
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            NRF_LOG_INFO("Disconnected (reason 0x%02x).",
                         p_ble_evt->evt.gap_evt.params.disconnected.reason);
            m_conn_handle = BLE_CONN_HANDLE_INVALID;

            /* 仅在"意图仍是开着"时自动重开播。调用过 ble_link_adv_stop()
             * 之后 m_adv_enabled 为 false, 断开就真的安静下来 ——
             * 否则"关闭广播"这个接口会被这里的自动重播悄悄推翻。
             *
             * ⚠ 在中断上下文里不能用 APP_ERROR_CHECK —— 断言失败会进 fault。 */
            if (m_adv_enabled)
            {
                err = ble_link_adv_start();
                if (err != NRF_SUCCESS)
                {
                    NRF_LOG_WARNING("adv restart failed (0x%08x)", err);
                }
            }
            else
            {
                NRF_LOG_INFO("Advertising stays off (disabled by adv_stop).");
            }
            break;

        case BLE_GAP_EVT_ADV_SET_TERMINATED:
            /* 限时广播到期(BLE_LINK_ADV_DURATION_MS != 0 时才会发生)。
             * 同时清掉意图标志 —— 到期是"广播生命周期自然结束", 不该让
             * 后续某次断开连接把它又拉起来。需要继续播请显式 adv_start()。 */
            m_advertising = false;
            m_adv_enabled = false;
            NRF_LOG_INFO("Advertising terminated (reason 0x%02x).",
                         p_ble_evt->evt.gap_evt.params.adv_set_terminated.reason);
            break;

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

static ret_code_t gap_params_init(void)
{
    ret_code_t              err;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    /* 设备名开放读取, 无需加密 */
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err = sd_ble_gap_device_name_set(&sec_mode,
                                     (const uint8_t *)BLE_LINK_DEVICE_NAME,
                                     strlen(BLE_LINK_DEVICE_NAME));
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

    return sd_ble_gap_adv_set_configure(&m_adv_handle, &m_adv_data, &m_adv_params);
}

/* ==================================================================
 *  公共接口
 * ================================================================== */

ret_code_t ble_link_init(ble_link_rx_handler_t rx_handler)
{
    ret_code_t err;

    if (m_inited)
    {
        m_rx_handler = rx_handler;   /* 允许重复调用时只更新回调 */
        return NRF_SUCCESS;
    }

    m_rx_handler  = rx_handler;
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

    NRF_LOG_INFO("BLE link ready: name=\"%s\", connectable, NUS transparent.",
                 BLE_LINK_DEVICE_NAME);
    NRF_LOG_INFO("  adv interval %ums, max payload %u bytes",
                 BLE_LINK_ADV_INTERVAL_MS, ble_link_max_data_len());
    return NRF_SUCCESS;
}

ret_code_t ble_link_adv_start(void)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    /* 记下意图: 此后断开连接会自动重开播 */
    m_adv_enabled = true;

    if (m_advertising)
    {
        return NRF_SUCCESS;         /* 已在广播, 不重复启动 */
    }

    /* 已连接时不能开【可连接】广播: S112 只有 1 个 peripheral 连接槽,
     * 槽位被占满时 sd_ble_gap_adv_start() 会返回 NRF_ERROR_CONN_COUNT。
     * 这里提前返回成功并保留 m_adv_enabled —— 断开后由 DISCONNECTED
     * 分支自动补上开播, 语义上"意图已登记", 不算失败。
     *
     * ⚠ 该限制只针对可连接广播。连接期间开【不可连接】广播是允许的
     *   (Broadcaster 与 Peripheral 是两个独立 role), 将来的传感器数据
     *   广播模块不必受这里的早退约束 —— 详见 ble_link.h 头部说明。 */
    if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
    {
        NRF_LOG_INFO("Connected; advertising will resume after disconnect.");
        return NRF_SUCCESS;
    }

    ret_code_t err = sd_ble_gap_adv_start(m_adv_handle, BLE_LINK_CONN_CFG_TAG);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    m_advertising = true;
    NRF_LOG_INFO("Advertising as \"%s\".", BLE_LINK_DEVICE_NAME);
    return NRF_SUCCESS;
}

ret_code_t ble_link_adv_stop(void)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    /* 先清意图, 再停播 —— 顺序重要: 若反过来, sd_ble_gap_adv_stop() 之后
     * 到清标志之前若插入 DISCONNECTED 事件(中断优先级高于本函数), 会被
     * 自动重开播逻辑立刻推翻。 */
    m_adv_enabled = false;

    if (!m_advertising)
    {
        return NRF_SUCCESS;         /* 本来就没在播 */
    }

    ret_code_t err = sd_ble_gap_adv_stop(m_adv_handle);

    /* INVALID_STATE = 协议栈认为本就没在播(例如刚被连上而事件还没处理完)。
     * 我们的目标状态已达成, 按成功处理, 不让调用方去区分这种竞态。 */
    if ((err != NRF_SUCCESS) && (err != NRF_ERROR_INVALID_STATE))
    {
        return err;
    }

    m_advertising = false;
    NRF_LOG_INFO("Advertising stopped (no longer connectable).");
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

bool ble_link_is_advertising(void)
{
    return m_advertising;
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

/**
 * ble_beacon —— 不可连接数据广播实现(按次触发)
 * ------------------------------------------------------------------
 * 只有一块编码缓冲(不再双缓冲): 每次触发都是 stop → configure → start 的
 * 完整一轮, configure 发生在没有广播的时刻, 复用同一块缓冲是合法的。
 * 双缓冲只有"广播中不停播地换数据"才需要, 而那个用法已随需求变更删除。
 *
 * 播够 BLE_BEACON_ADV_EVENTS 次后由协议栈自动停播并产生
 * BLE_GAP_EVT_ADV_SET_TERMINATED(LIMIT_REACHED), ble_adv_mux 在优先级 1
 * 收到该事件时会同步 m_advertising = false, 所以本模块不需要自己数次数。
 */

#include "ble_beacon.h"

#include <string.h>

#include "ble_advdata.h"
#include "ble_gap.h"
#include "ble_adv_mux.h"

#define NRF_LOG_MODULE_NAME ble_beacon
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

static bool     m_inited = false;
static uint8_t  m_payload[BLE_BEACON_PAYLOAD_LEN];
static uint8_t  m_enc[BLE_GAP_ADV_SET_DATA_SIZE_MAX];

static ble_gap_adv_params_t m_adv_params;

/* 把 m_payload 编码进 m_enc, 输出的 ble_gap_adv_data_t 指向它。
 *
 * ⚠ m_enc 必须在整个广播期间保持有效 —— sd_ble_gap_adv_set_configure()
 *   只记住指针, SoftDevice 每次发包时直接读这里, 不做内部拷贝。故它是
 *   文件作用域的静态变量, 不能是栈上的。 */
static ret_code_t payload_encode(ble_gap_adv_data_t * p_out)
{
    ble_advdata_manuf_data_t manuf =
    {
        .company_identifier = BLE_BEACON_COMPANY_ID,
        .data.p_data        = m_payload,
        .data.size          = BLE_BEACON_PAYLOAD_LEN
    };

    ble_advdata_t advdata;
    memset(&advdata, 0, sizeof(advdata));
    advdata.name_type             = BLE_ADVDATA_NO_NAME;   /* 不放设备名: 省字节, 且不需要被认出 */
    advdata.flags                 = BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED;
    advdata.p_manuf_specific_data = &manuf;

    uint16_t len = BLE_GAP_ADV_SET_DATA_SIZE_MAX;

    ret_code_t err = ble_advdata_encode(&advdata, m_enc, &len);
    if (err != NRF_SUCCESS)
    {
        /* 最可能的原因是载荷太长: 31 - Flags(3) - 厂商段头(4) = 24 字节上限 */
        NRF_LOG_ERROR("advdata_encode failed (0x%08x), payload_len=%u",
                      err, BLE_BEACON_PAYLOAD_LEN);
        return err;
    }

    p_out->adv_data.p_data      = m_enc;
    p_out->adv_data.len         = len;
    /* 不可连接不可扫描 → 没有扫描响应包, 必须传 NULL/0。
     * 给不可扫描广播配扫描响应数据会被协议栈判为 INVALID_PARAM。 */
    p_out->scan_rsp_data.p_data = NULL;
    p_out->scan_rsp_data.len    = 0;

    return NRF_SUCCESS;
}

ret_code_t ble_beacon_init(void)
{
    if (m_inited)
    {
        return NRF_SUCCESS;
    }

    memset(m_payload, 0, sizeof(m_payload));

    memset(&m_adv_params, 0, sizeof(m_adv_params));
    m_adv_params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
    m_adv_params.p_peer_addr     = NULL;            /* 非定向 */
    m_adv_params.filter_policy   = BLE_GAP_ADV_FP_ANY;
    m_adv_params.interval        = MSEC_TO_UNITS(BLE_BEACON_ADV_INTERVAL_MS, UNIT_0_625_MS);
    m_adv_params.primary_phy     = BLE_GAP_PHY_1MBPS;

    /* 播够 N 个广播事件就自动停 —— 这是"只播三次"的实现核心。
     * duration 保持 0: 次数已经限定了时长(N × interval), 再加时间限制只会
     * 引入"两个终止条件谁先到"的不确定性。 */
    m_adv_params.max_adv_evts    = BLE_BEACON_ADV_EVENTS;
    m_adv_params.duration        = 0;

    m_inited = true;

    NRF_LOG_INFO("Beacon ready: non-connectable, %u events/burst, interval %ums, payload %u bytes.",
                 BLE_BEACON_ADV_EVENTS, BLE_BEACON_ADV_INTERVAL_MS,
                 BLE_BEACON_PAYLOAD_LEN);
    return NRF_SUCCESS;
}

ret_code_t ble_beacon_burst(const uint8_t * p_payload)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    if (p_payload == NULL)
    {
        return NRF_ERROR_NULL;
    }

    memcpy(m_payload, p_payload, BLE_BEACON_PAYLOAD_LEN);

    ble_gap_adv_data_t data;
    ret_code_t err = payload_encode(&data);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    /* mux 内部会先 stop(若上一轮还没播完) 再 configure 再 start,
     * 所以"上一轮被丢弃、从头数 N 次"是自然结果, 不需要额外处理。 */
    err = ble_adv_mux_start(BLE_ADV_OWNER_BEACON, &data, &m_adv_params,
                            BLE_BEACON_TX_POWER_DBM);
    if (err == NRF_SUCCESS)
    {
        NRF_LOG_INFO("Beacon burst: %u events of the same packet.",
                     BLE_BEACON_ADV_EVENTS);
    }
    return err;
}

ret_code_t ble_beacon_stop(void)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    return ble_adv_mux_stop(BLE_ADV_OWNER_BEACON);
}

bool ble_beacon_is_advertising(void)
{
    return (ble_adv_mux_owner() == BLE_ADV_OWNER_BEACON) &&
           ble_adv_mux_is_advertising();
}

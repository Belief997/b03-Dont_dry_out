/**
 * ble_beacon —— 不可连接数据广播实现
 * ------------------------------------------------------------------
 * 缓冲布局(为什么是两块):
 *   m_enc[0] / m_enc[1] 交替使用。sd_ble_gap_adv_set_configure() 只记指针,
 *   SoftDevice 发包时直接读原地址, 所以"正在播的那块"不能被改写 ——
 *   要更新就得把新内容编码到另一块, 再把新指针交给协议栈。
 *   m_idx 指向【下一次要写入】的那块, 也就是当前没在用的那块。
 *
 * 载荷副本 m_payload 的作用: 让 ble_beacon_update() 在"广播集不在自己手上"
 * 时也能收下新数据 —— 等下次 start() 时直接用, 上层不必自己缓存。
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

/* 双缓冲: m_idx 是"下一次写哪块"。 */
static uint8_t  m_enc[2][BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t  m_idx = 0;

static ble_gap_adv_params_t m_adv_params;

/* 把 m_payload 编码进 m_enc[m_idx], 并翻转 m_idx。
 * 输出的 ble_gap_adv_data_t 指向刚写好的那块。 */
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

    uint8_t  * p_buf = m_enc[m_idx];
    uint16_t   len   = BLE_GAP_ADV_SET_DATA_SIZE_MAX;

    ret_code_t err = ble_advdata_encode(&advdata, p_buf, &len);
    if (err != NRF_SUCCESS)
    {
        /* 最可能的原因是载荷太长: 31 - Flags(3) - 厂商段头(4) = 24 字节上限 */
        NRF_LOG_ERROR("advdata_encode failed (0x%08x), payload_len=%u",
                      err, BLE_BEACON_PAYLOAD_LEN);
        return err;
    }

    p_out->adv_data.p_data      = p_buf;
    p_out->adv_data.len         = len;
    /* 不可连接不可扫描 → 没有扫描响应包, 必须传 NULL/0。
     * 给不可扫描广播配扫描响应数据会被协议栈判为 INVALID_PARAM。 */
    p_out->scan_rsp_data.p_data = NULL;
    p_out->scan_rsp_data.len    = 0;

    m_idx ^= 1;    /* 下次写另一块 */
    return NRF_SUCCESS;
}

ret_code_t ble_beacon_init(void)
{
    if (m_inited)
    {
        return NRF_SUCCESS;
    }

    memset(m_payload, 0, sizeof(m_payload));
    m_idx = 0;

    memset(&m_adv_params, 0, sizeof(m_adv_params));
    m_adv_params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
    m_adv_params.p_peer_addr     = NULL;            /* 非定向 */
    m_adv_params.filter_policy   = BLE_GAP_ADV_FP_ANY;
    m_adv_params.interval        = MSEC_TO_UNITS(BLE_BEACON_ADV_INTERVAL_MS, UNIT_0_625_MS);
    m_adv_params.duration        = 0;               /* 不限时: 平时一直播 */
    m_adv_params.primary_phy     = BLE_GAP_PHY_1MBPS;

    m_inited = true;

    NRF_LOG_INFO("Beacon ready: non-connectable, interval %ums, payload %u bytes.",
                 BLE_BEACON_ADV_INTERVAL_MS, BLE_BEACON_PAYLOAD_LEN);
    return NRF_SUCCESS;
}

ret_code_t ble_beacon_start(const uint8_t * p_payload)
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

    return ble_adv_mux_start(BLE_ADV_OWNER_BEACON, &data, &m_adv_params,
                             BLE_BEACON_TX_POWER_DBM);
}

ret_code_t ble_beacon_update(const uint8_t * p_payload)
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

    /* 广播集不在自己手上(让给了 ble_link, 或正在连接中) → 只留副本。
     * 返回 INVALID_STATE 让调用方知道"这次没播出去", 但数据不会丢。 */
    if (!ble_beacon_is_advertising())
    {
        return NRF_ERROR_INVALID_STATE;
    }

    ble_gap_adv_data_t data;
    ret_code_t err = payload_encode(&data);
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    /* 不中断广播地换数据 —— 靠的就是 payload_encode 每次给出不同缓冲。 */
    return ble_adv_mux_update_data(BLE_ADV_OWNER_BEACON, &data);
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

/**
 * ble_adv_mux —— 广播集仲裁实现
 * ------------------------------------------------------------------
 * 全部状态只有三个变量: 句柄、持有者、是否在播。刻意不引入状态机 ——
 * 仲裁规则简单到用两条 if 就能说清, 加状态机反而掩盖真实约束。
 *
 * 关于"停播"的错误处理: sd_ble_gap_adv_stop() 返回 NRF_ERROR_INVALID_STATE
 * 表示协议栈认为本来就没在播(例如刚被连上, 而 CONNECTED 事件还没轮到我们处理,
 * 此时 m_advertising 仍是 true)。这是正常竞态, 一律按成功处理, 不外泄给调用方。
 */

#include "ble_adv_mux.h"

#include "ble.h"
#include "nrf_sdh_ble.h"

#define NRF_LOG_MODULE_NAME ble_adv_mux
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

/* 唯一的广播句柄 —— 整个工程只有这一个, 这是本模块存在的理由。 */
static uint8_t         m_adv_handle  = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static ble_adv_owner_t m_owner       = BLE_ADV_OWNER_NONE;
static bool            m_advertising = false;

const char * ble_adv_owner_str(ble_adv_owner_t owner)
{
    switch (owner)
    {
        case BLE_ADV_OWNER_BEACON: return "beacon";
        case BLE_ADV_OWNER_LINK:   return "link";
        case BLE_ADV_OWNER_NONE:   return "none";
        default:                   return "?";
    }
}

/* 停播的内部实现: 不判归属, 调用方负责判。 */
static ret_code_t adv_stop_raw(void)
{
    if (!m_advertising)
    {
        return NRF_SUCCESS;
    }

    ret_code_t err = sd_ble_gap_adv_stop(m_adv_handle);

    /* INVALID_STATE = 协议栈认为本就没在播, 目标状态已达成 → 视作成功。
     * INVALID_ADV_HANDLE 理论上不会出现(句柄由本模块独占且已配置过),
     * 但真出现了也说明"没有广播在播", 同样不必上报。 */
    if ((err != NRF_SUCCESS) &&
        (err != NRF_ERROR_INVALID_STATE) &&
        (err != BLE_ERROR_INVALID_ADV_HANDLE))
    {
        NRF_LOG_WARNING("adv_stop failed (0x%08x)", err);
        return err;
    }

    m_advertising = false;
    return NRF_SUCCESS;
}

ret_code_t ble_adv_mux_start(ble_adv_owner_t              owner,
                             ble_gap_adv_data_t   const * p_data,
                             ble_gap_adv_params_t const * p_params,
                             int8_t                       tx_power_dbm)
{
    if ((p_data == NULL) || (p_params == NULL))
    {
        return NRF_ERROR_NULL;
    }
    if (owner == BLE_ADV_OWNER_NONE)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    ret_code_t err;

    /* 1) 先让广播集空出来。协议栈禁止在广播中改参数, 见头文件说明。 */
    err = adv_stop_raw();
    if (err != NRF_SUCCESS)
    {
        m_owner = BLE_ADV_OWNER_NONE;   /* 状态不确定, 不记在任何人名下 */
        return err;
    }

    /* 2) 重配广播集。m_adv_handle 首次为 HANDLE_NOT_SET, 协议栈分配后写回;
     *    之后一直复用同一个句柄 —— 这正是避免 NRF_ERROR_NO_MEM 的关键。 */
    err = sd_ble_gap_adv_set_configure(&m_adv_handle, p_data, p_params);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("adv_set_configure failed (0x%08x) for %s",
                      err, ble_adv_owner_str(owner));
        m_owner = BLE_ADV_OWNER_NONE;
        return err;
    }

    /* 3) TX 功率跟着广播集走, 每次 configure 之后都要重设。 */
    err = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV, m_adv_handle, tx_power_dbm);
    if (err != NRF_SUCCESS)
    {
        /* 功率没设上不影响能否广播, 记警告后继续 —— 让广播先跑起来。 */
        NRF_LOG_WARNING("tx_power_set failed (0x%08x)", err);
    }

    /* 4) 开播。conn_cfg_tag 对不可连接广播被忽略, 统一传同一个标签即可。 */
    err = sd_ble_gap_adv_start(m_adv_handle, BLE_ADV_MUX_CONN_CFG_TAG);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("adv_start failed (0x%08x) for %s",
                      err, ble_adv_owner_str(owner));
        m_owner = BLE_ADV_OWNER_NONE;
        return err;
    }

    m_owner       = owner;
    m_advertising = true;

    NRF_LOG_INFO("Adv set -> %s (type %u, interval %u, duration %u)",
                 ble_adv_owner_str(owner), p_params->properties.type,
                 p_params->interval, p_params->duration);
    return NRF_SUCCESS;
}

ret_code_t ble_adv_mux_stop(ble_adv_owner_t owner)
{
    /* 礼让: 不是自己在播就什么都不做。避免 A 的 stop 误伤 B 刚抢到的广播。 */
    if (owner != m_owner)
    {
        return NRF_SUCCESS;
    }

    ret_code_t err = adv_stop_raw();
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    m_owner = BLE_ADV_OWNER_NONE;
    NRF_LOG_INFO("Adv set released by %s.", ble_adv_owner_str(owner));
    return NRF_SUCCESS;
}

ble_adv_owner_t ble_adv_mux_owner(void)
{
    return m_owner;
}

bool ble_adv_mux_is_advertising(void)
{
    return m_advertising;
}

uint8_t ble_adv_mux_handle(void)
{
    return m_adv_handle;
}

/* ==================================================================
 *  BLE 事件: 广播被协议栈单方面停掉的两种情形
 * ==================================================================
 *
 * 这两种情形下 sd_ble_gap_adv_stop() 并未被调用, 但广播确实停了。
 * 若不在这里同步 m_advertising, 后续 adv_stop_raw() 会去停一个不存在的广播
 * (拿到 INVALID_STATE, 虽被容忍但属于自己骗自己), 更麻烦的是
 * ble_adv_mux_is_advertising() 会一直返回 true, 上层据此做的判断全错。
 *
 * 注意这里【不】清 m_owner: 归属信息对上层仍有用 —— main.c 要靠
 * "刚终止的广播是谁的"来决定下一步(见那里的 on_adv_terminated 编排)。
 */
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    (void)p_context;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            /* 可连接广播被连上 → 协议栈自动停播 */
            m_advertising = false;
            break;

        case BLE_GAP_EVT_ADV_SET_TERMINATED:
            /* 两种情形都会来这里:
             *   reason = TIMEOUT       —— 限时广播到期(ble_link 的 30s 窗口)
             *   reason = LIMIT_REACHED —— max_adv_evts 播够(ble_beacon 的 3 次)
             * 本模块不区分, 只管把"已经不在播了"这个事实记下来;
             * 区分留给 ble_link / main.c 的观察者(优先级 3, 在本模块之后)。 */
            m_advertising = false;
            break;

        default:
            break;
    }
}

/* 优先级 1: 必须【早于】ble_link(3) 和 main.c(3) 收到事件 ——
 * 那两处的处理逻辑会调用本模块的接口, 需要先看到已同步的 m_advertising。
 * 数值越小越先收到(与各 sdk_config 里 xxx_BLE_OBSERVER_PRIO 同一套规则)。 */
NRF_SDH_BLE_OBSERVER(m_adv_mux_observer, 1, ble_evt_handler, NULL);

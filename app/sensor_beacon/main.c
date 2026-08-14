/**
 * sensor_beacon —— nRF52810 低功耗事件触发广播上报
 * ------------------------------------------------------------------
 * 目标芯片 : nRF52810 (Cortex-M4 无 FPU, 192KB Flash / 24KB RAM)
 * SoftDevice: S112 v6.0.0 (仅外设/广播角色)
 * SDK       : nRF5 SDK 15.0.0
 * 构建目标  : pca10040e (在 nRF52832 DK 上仿真 52810)
 *
 * 功能概述:
 *   常态处于 System OFF 深度睡眠(~0.4µA)。外部比较器在事件发生时输出
 *   上升沿并保持高电平(条件消失后自动回到低电平)。GPIO SENSE(电平检测)
 *   将 MCU 从 System OFF 唤醒(唤醒 = 复位)。唤醒后:
 *     1) 读复位原因 + GPREGRET 中保存的 8bit 计数器;
 *     2) 读唤醒引脚电平判定当前处于事件段(高)还是清除段(低);
 *     3) 事件段: 计数器 +1 → 等待 1~2s 传感器稳定 → SAADC 采集(电池电压)
 *        → 使能 S112 → 发送非连接广播(含设备 ID/计数器/电池电压)
 *        → 广播结束后重新武装 SENSE → 回到 System OFF;
 *     4) 清除段/冷启动: 不广播,重新武装 SENSE → 回到 System OFF。
 *
 *   "SENSE 翻转状态机": 用引脚电平本身编码状态,无需额外状态标志。
 *     - 处于"待事件"态时武装 SENSE=HIGH;高电平唤醒后处理事件,随后武装
 *       SENSE=LOW;信号回落时低电平唤醒,不广播,再武装 SENSE=HIGH。
 *     - 每个物理事件恰好产生 2 次唤醒:1 次高(广播)+ 1 次低(复位武装)。
 *
 * ⚠ 关键约束:
 *   - 稳定等待(1~2s)必须用 app_timer(RTC)+ WFE 空闲,严禁 nrf_delay 忙等,
 *     否则该时段 CPU 满载,功耗完全失去意义。
 *   - GPREGRET / RESETREAS / SYSTEMOFF 在 SoftDevice 未使能时用寄存器直接读写;
 *     一旦 SoftDevice 使能,必须改用 sd_power_* SVC 调用。本工程在读复位状态、
 *     写计数器、以及"清除段直接进 OFF"时 SD 尚未使能 → 直接寄存器访问。
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "nrf.h"
#include "nrf_gpio.h"
#include "nrf_soc.h"
#include "nordic_common.h"
#include "app_error.h"
#include "app_timer.h"

#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "ble_advdata.h"
#include "ble_gap.h"

#include "nrf_pwr_mgmt.h"
#include "nrf_drv_saadc.h"

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "nrf_delay.h"

/* ==================================================================
 *  可配置参数(TODO: 按实际硬件/需求修改)
 * ================================================================== */

/* 唤醒引脚: 外部比较器输出接入的 GPIO。
 * TODO: 改为实际布线的引脚号。当前 13 = pca10040 DK 的 Button1,便于台架测试。 */
#define WAKE_PIN                    13

/* 唤醒引脚上拉/下拉: 使能内部上拉 */
#define WAKE_PIN_PULL               NRF_GPIO_PIN_PULLUP

/* 电池电压 ADC 通道: 内部 VDD 输入,无需外部引脚 */
#define SAADC_CH_BATTERY            1

/* 传感器上电到数据稳定的等待时间(ms)。TODO: 按传感器手册确定 (1000~2000)。 */
#define SETTLING_TIME_MS            1500

/* 广播参数。TODO: 按上报可靠性/功耗折中确定。 */
#define ADV_INTERVAL_MS             100     /* 广播间隔(ms) */
#define ADV_DURATION_MS             2000    /* 广播总时长(ms),到时 SD 自动停止 */
#define ADV_TX_POWER_DBM            0       /* 发射功率(dBm): -40..+4,S112 支持 */

/* 厂商自定义数据 —— 公司标识。0xFFFF 为 SIG 保留(测试用)。
 * TODO: 若有 SIG 分配的 Company ID 请替换。 */
#define APP_COMPANY_IDENTIFIER      0xFFFF

/* 负载协议魔数 + 版本(置于负载最前):
 *   - 魔数供网关在 0xFFFF 通用 Company ID 下过滤掉他人广播;
 *   - 版本供日后平滑演进负载格式。改布局时请递增版本并同步对接文档。 */
#define APP_PROTO_MAGIC             0xAB
#define APP_PROTO_VERSION           0x01

/* 广播里放几字节设备 ID(取自 FICR->DEVICEID)。TODO: 2 或 4。 */
#define DEVICE_ID_LEN               2

#define APP_BLE_CONN_CFG_TAG        1       /* SoftDevice 连接配置标签 */
#define APP_BLE_OBSERVER_PRIO       3       /* BLE 事件观察者优先级 */

/* 计数器存放的 GPREGRET 编号(0 = GPREGRET,1 = GPREGRET2) */
#define GPREGRET_ID_COUNTER         0

/* ==================================================================
 *  HX711 引脚 & 参数
 * ==================================================================
 *
 * 引脚选取约束(同时满足 PCA10040 DK 台架调试与 E73-TBM 真机):
 *   - DK 占用    : P0.13~16 = Button1~4, P0.17~20 = LED1~4, P0.06/08 = UART
 *   - E73-TBM 占用: P0.13/14 = 按键, P0.17/18 = LED, P0.05~08 = CH340 UART,
 *                   P0.09/10 = NFC 焊盘, P0.21 = 复位
 *   - P0.25~29 紧邻射频,数据手册 Table 100 限定"低驱动、低频 I/O only",
 *     SCK 为数百 kHz 时钟脉冲,不可用
 *   - P0.02~05 = AIN0~3,留给模拟输入
 *   → 两板交集中安全可用: P0.11, P0.12, P0.22, P0.23, P0.24
 *
 * ⚠ P0.22/23/24 在 QFN48 上与 SWDIO/SWDCLK 相邻但功能独立,不影响调试。
 */

/* --- HX711 #1 --- */
#define HX711_1_DT_PIN              11
#define HX711_1_SCK_PIN             12

/* --- HX711 #2 --- */
#define HX711_2_DT_PIN              23
#define HX711_2_SCK_PIN             24

/* 不同增益对应的 SCK 脉冲总数(含 24bit 数据 + N 个增益选择脉冲) */
#define HX711_PULSES_CHA_128        25
#define HX711_PULSES_CHB_32         26
#define HX711_PULSES_CHA_64         27

/* 通道标识 */
#define HX711_CH_A                  0
#define HX711_CH_B                  1

/* 批次读取参数:
 *   BATCH_SIZE = 每通道连续读的帧数(10)。
 *   ⚠ 切通道后首帧的 DOUT 就绪会延后(约 4 个周期), 数据本身有效,
 *     非阻塞轮询自然跳过未就绪的 tick, 无需丢弃样本。 */
#define HX711_BATCH_SIZE            10

/* HX711 设备实例描述符 */
typedef struct {
    uint8_t  dt_pin;    /* DT(DOUT) 引脚号 */
    uint8_t  sck_pin;   /* SCK(PD_SCK) 引脚号 */
    uint8_t  pulses;    /* 上一次读取使用的脉冲数(决定本次读回数据的通道) */
} hx711_t;

static hx711_t m_hx711_1 = {
    .dt_pin  = HX711_1_DT_PIN,
    .sck_pin = HX711_1_SCK_PIN,
    .pulses  = HX711_PULSES_CHA_128
};
static hx711_t m_hx711_2 = {
    .dt_pin  = HX711_2_DT_PIN,
    .sck_pin = HX711_2_SCK_PIN,
    .pulses  = HX711_PULSES_CHA_128
};

/* #1 批次读取状态机(A 批 10 帧 → B 批 10 帧 → 循环) */
static uint8_t  m_b1_target   = HX711_CH_A;   /* 当前批次目标通道 */
static uint8_t  m_b1_idx      = 0;            /* 本批次已读帧数(0..BATCH_SIZE-1) */
static bool     m_b1_switched = false;        /* 本批次切换帧是否已完成 */

APP_TIMER_DEF(m_hx711_timer);               /* HX711 周期采样定时器 */

/* ==================================================================
 *  滑动平均滤波器 —— 窗口=10, O(1) 增量平均
 * ================================================================== */
#define MA_WINDOW_SIZE              10

typedef struct {
    int32_t buf[MA_WINDOW_SIZE];    /* 环形缓冲 */
    uint8_t idx;                    /* 下一个写入位置 */
    uint8_t count;                  /* 已积累样本数 (0..WINDOW_SIZE) */
    int64_t sum;                    /* 当前窗口内样本总和 */
} ma_filter_t;

/* 各通道滑动平均实例(#2 只读 chA) */
static ma_filter_t m_ma_1a;     /* HX711 #1, Channel A */
static ma_filter_t m_ma_1b;     /* HX711 #1, Channel B */
static ma_filter_t m_ma_2a;     /* HX711 #2, Channel A */

static void ma_init(ma_filter_t * f)
{
    memset(f, 0, sizeof(*f));
}

static void ma_push(ma_filter_t * f, int32_t val)
{
    if (f->count < MA_WINDOW_SIZE)
    {
        /* 窗口未满, 直接追加 */
        f->buf[f->idx] = val;
        f->sum += val;
        f->count++;
    }
    else
    {
        /* 窗口已满, 替换最旧样本 */
        f->sum += val - f->buf[f->idx];
        f->buf[f->idx] = val;
    }
    f->idx = (uint8_t)((f->idx + 1) % MA_WINDOW_SIZE);
}

static int32_t ma_avg(const ma_filter_t * f)
{
    if (f->count == 0) return 0;
    return (int32_t)(f->sum / f->count);
}

static bool ma_ready(const ma_filter_t * f)
{
    return (f->count == MA_WINDOW_SIZE);
}

/* ==================================================================
 *  广播负载布局(厂商自定义数据段内的字节偏移)
 * ==================================================================
 *   [0]                        魔数   APP_PROTO_MAGIC
 *   [1]                        版本   APP_PROTO_VERSION
 *   [2 .. 1+DEVICE_ID_LEN]     设备 ID (小端)
 *   [..]                       计数器   (1 字节)
 *   [.. +1]                    电池电压 (2 字节, 小端, 单位 mV)
 * 网关先校验 [0]=魔数、[1]=版本,再按版本解析后续字段。
 */
#define OFF_MAGIC                   0
#define OFF_VERSION                 1
#define OFF_DEVICE_ID               2
#define OFF_COUNTER                 (OFF_DEVICE_ID + DEVICE_ID_LEN)
#define OFF_BATTERY                 (OFF_COUNTER + 1)
#define MANUF_DATA_LEN              (OFF_BATTERY + 2)

/* ==================================================================
 *  全局状态
 * ================================================================== */

static uint8_t                  m_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static uint8_t                  m_enc_advdata[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t                  m_manuf_data[MANUF_DATA_LEN];

static ble_gap_adv_data_t       m_adv_data =
{
    .adv_data      = { .p_data = m_enc_advdata, .len = BLE_GAP_ADV_SET_DATA_SIZE_MAX },
    .scan_rsp_data = { .p_data = NULL,          .len = 0 }
};
static ble_gap_adv_params_t     m_adv_params;

APP_TIMER_DEF(m_settling_timer);            /* 传感器稳定等待定时器(单次) */
APP_TIMER_DEF(m_test_timer);                /* 测试: 500ms 周期采样定时器 */

static volatile bool            m_settled  = false;   /* 稳定等待结束 */
static volatile bool            m_adv_done = false;    /* 广播时长到,已终止 */

static uint8_t                  m_counter  = 0;        /* 8bit 事件计数器 */

/* ==================================================================
 *  启动阶段: 复位原因 / 计数器 / 唤醒电平(SoftDevice 未使能 → 直接寄存器)
 * ================================================================== */

/* 是否为从 System OFF(SENSE)唤醒。false = 冷启动/上电复位。 */
static bool boot_read_and_classify(uint32_t * p_resetreas)
{
    uint32_t reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xFFFFFFFF;      /* 写 1 清除,便于下次判定 */
    *p_resetreas = reason;

    /* GPREGRET 在 System OFF/复位间保持,仅上电/掉电复位清零 */
    m_counter = (uint8_t)(NRF_POWER->GPREGRET & 0xFFu);

    if (reason == 0)
    {
        /* 冷启动(上电):计数器归零 */
        m_counter = 0;
        NRF_POWER->GPREGRET = 0;
        return false;
    }
    /* POWER_RESETREAS_OFF_Msk (bit16) = 由 System OFF 的 SENSE 唤醒 */
    return (reason & POWER_RESETREAS_OFF_Msk) != 0;
}

/* 直接进入 System OFF(SoftDevice 未使能时使用)。
 * 调用前必须已用 nrf_gpio_cfg_sense_set() 武装唤醒电平。 */
static void system_off_direct(void)
{
    NRF_LOG_INFO("System OFF (direct), sense armed.");
    NRF_LOG_FLUSH();

    NRF_POWER->SYSTEMOFF = 1;
    __DSB();
    /* SWD 调试器连接时 System OFF 被仿真,调用会返回 → 死循环等待掉电 */
    for (;;)
    {
        __WFE();
    }
}

/* 武装唤醒引脚的 SENSE 电平并保存计数器,然后进 OFF(SD 未使能路径) */
static void rearm_and_off(nrf_gpio_pin_sense_t sense)
{
    NRF_POWER->GPREGRET = m_counter;                 /* 直接写整字节 */
    nrf_gpio_cfg_input(WAKE_PIN, WAKE_PIN_PULL);
    nrf_gpio_cfg_sense_set(WAKE_PIN, sense);
    system_off_direct();
}

/* ==================================================================
 *  SAADC —— 唤醒后单次阻塞采样(非连续,省电省代码)
 * ================================================================== */

/* 空回调: 阻塞式 sample_convert 不经过回调,但 nrf_drv_saadc_init 需要一个 */
static void saadc_callback(nrf_drv_saadc_evt_t const * p_event)
{
    (void)p_event;
}

static void saadc_init(void)
{
    ret_code_t err;

    err = nrf_drv_saadc_init(NULL, saadc_callback);
    APP_ERROR_CHECK(err);

    /* 电池通道: 内部 VDD 输入,单端,增益 1/6,内部 0.6V 参考 → 满量程 3.6V */
    nrf_saadc_channel_config_t ch_batt =
        NRF_DRV_SAADC_DEFAULT_CHANNEL_CONFIG_SE(NRF_SAADC_INPUT_VDD);
    err = nrf_drv_saadc_channel_init(SAADC_CH_BATTERY, &ch_batt);
    APP_ERROR_CHECK(err);
}

/* 采集一路通道(阻塞)。负值(共模噪声)钳到 0。 */
static uint16_t saadc_sample_channel(uint8_t channel)
{
    nrf_saadc_value_t v = 0;
    ret_code_t err = nrf_drv_saadc_sample_convert(channel, &v);
    APP_ERROR_CHECK(err);
    return (v < 0) ? 0 : (uint16_t)v;
}

/* 12bit + 1/6增益 + 0.6V 参考: mV = raw * 3600 / 4096 */
static inline uint16_t saadc_raw_to_mv(uint16_t raw)
{
    return (uint16_t)(((uint32_t)raw * 3600u) / 4096u);
}

static void saadc_uninit(void)
{
    nrf_drv_saadc_uninit();
}

/* ==================================================================
 *  广播负载组装
 * ================================================================== */

static void payload_build(uint16_t batt_raw)
{
    uint32_t dev_id = NRF_FICR->DEVICEID[0];    /* 64bit 唯一 ID 的低 32bit */

    m_manuf_data[OFF_MAGIC]   = APP_PROTO_MAGIC;
    m_manuf_data[OFF_VERSION] = APP_PROTO_VERSION;

    /* 设备 ID (小端) */
    for (uint8_t i = 0; i < DEVICE_ID_LEN; i++)
    {
        m_manuf_data[OFF_DEVICE_ID + i] = (uint8_t)(dev_id >> (8 * i));
    }

    m_manuf_data[OFF_COUNTER]      = m_counter;

    /* 电池: 这里放毫伏值(也可直接放 raw,与网关约定即可) */
    uint16_t batt_mv = saadc_raw_to_mv(batt_raw);
    m_manuf_data[OFF_BATTERY]      = (uint8_t)(batt_mv & 0xFF);
    m_manuf_data[OFF_BATTERY + 1]  = (uint8_t)(batt_mv >> 8);
}

/* ==================================================================
 *  BLE 协议栈 / 广播
 * ================================================================== */

static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_ADV_SET_TERMINATED:
            /* 广播时长到 → 结束本次活动 */
            m_adv_done = true;
            break;

        default:
            break;
    }
}

/* BLE 事件观察者(必须在文件作用域注册:它是链接段内的静态变量) */
NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);

static void ble_stack_init(void)
{
    ret_code_t err;

    err = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err);

    uint32_t ram_start = 0;
    err = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err);

    err = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err);   /* 若报 NRF_ERROR_NO_MEM,按提示调整 .ld 中 RAM ORIGIN */
}

static void advertising_init(uint16_t batt_raw)
{
    ret_code_t err;

    payload_build(batt_raw);

    ble_advdata_manuf_data_t manuf =
    {
        .company_identifier = APP_COMPANY_IDENTIFIER,
        .data.p_data        = m_manuf_data,
        .data.size          = MANUF_DATA_LEN
    };

    ble_advdata_t advdata;
    memset(&advdata, 0, sizeof(advdata));
    advdata.name_type             = BLE_ADVDATA_NO_NAME;
    advdata.flags                 = BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED;
    advdata.p_manuf_specific_data = &manuf;

    err = ble_advdata_encode(&advdata, m_adv_data.adv_data.p_data,
                             &m_adv_data.adv_data.len);
    APP_ERROR_CHECK(err);

    memset(&m_adv_params, 0, sizeof(m_adv_params));
    m_adv_params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
    m_adv_params.p_peer_addr     = NULL;
    m_adv_params.filter_policy   = BLE_GAP_ADV_FP_ANY;
    m_adv_params.interval        = MSEC_TO_UNITS(ADV_INTERVAL_MS, UNIT_0_625_MS);
    m_adv_params.duration        = ADV_DURATION_MS / 10;   /* 单位 10ms,0=不限时 */

    err = sd_ble_gap_adv_set_configure(&m_adv_handle, &m_adv_data, &m_adv_params);
    APP_ERROR_CHECK(err);

    /* 设置发射功率(需在 adv_set_configure 之后,针对该 adv handle) */
    err = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV, m_adv_handle, ADV_TX_POWER_DBM);
    APP_ERROR_CHECK(err);
}

static void advertising_start(void)
{
    ret_code_t err = sd_ble_gap_adv_start(m_adv_handle, APP_BLE_CONN_CFG_TAG);
    APP_ERROR_CHECK(err);
}

/* ==================================================================
 *  定时器 / 电源管理 / 日志
 * ================================================================== */

static void settling_timeout_handler(void * p_context)
{
    (void)p_context;
    m_settled = true;
}

static void timers_init(void)
{
    ret_code_t err = app_timer_init();
    APP_ERROR_CHECK(err);

    err = app_timer_create(&m_settling_timer, APP_TIMER_MODE_SINGLE_SHOT,
                           settling_timeout_handler);
    APP_ERROR_CHECK(err);
}

static void log_init(void)
{
    ret_code_t err = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err);
    NRF_LOG_DEFAULT_BACKENDS_INIT();
}

static void power_management_init(void)
{
    ret_code_t err = nrf_pwr_mgmt_init();
    APP_ERROR_CHECK(err);
}

/* 阻塞空闲: CPU 进 WFE,等待 SD/定时器事件唤醒(不忙等) */
static void idle_until(volatile bool * p_flag)
{
    while (!(*p_flag))
    {
        nrf_pwr_mgmt_run();
    }
}

/* ==================================================================
 *  事件处理(高电平路径): 稳定等待 → 采样 → 广播 → 回 OFF
 * ================================================================== */

static void handle_event_active(void)
{
    ret_code_t err;

    /* 1) 计数器 +1 并立即落盘(此刻 SD 尚未使能,直接写寄存器) */
    m_counter = (uint8_t)(m_counter + 1);
    NRF_POWER->GPREGRET = m_counter;
    NRF_LOG_INFO("Event wake. counter=%u", m_counter);

    /* 2) 使能协议栈(会启动 LFCLK=XTAL),再初始化依赖 LFCLK 的定时器 */
    ble_stack_init();
    timers_init();
    power_management_init();
    saadc_init();

    /* 3) 等待传感器稳定(RTC 定时 + WFE 空闲,严禁 nrf_delay 忙等) */
    m_settled = false;
    err = app_timer_start(m_settling_timer, APP_TIMER_TICKS(SETTLING_TIME_MS), NULL);
    APP_ERROR_CHECK(err);
    idle_until(&m_settled);

    /* 4) 采集电池电压 */
    uint16_t batt_raw = saadc_sample_channel(SAADC_CH_BATTERY);
    saadc_uninit();
    NRF_LOG_INFO("batt_mv=%u", saadc_raw_to_mv(batt_raw));

    /* 5) 组装并发送非连接广播,等待时长到(SD 产生 ADV_SET_TERMINATED) */
    m_adv_done = false;
    advertising_init(batt_raw);
    advertising_start();
    idle_until(&m_adv_done);

    /* 6) 广播结束 → 武装 SENSE=LOW(等待信号回落的低电平唤醒) → System OFF。
     *    此处 SD 已使能,须用 sd_power_* 而非直接寄存器。 */
    NRF_LOG_INFO("Adv done. Arm SENSE_LOW, System OFF.");
    NRF_LOG_FLUSH();

    (void)sd_ble_gap_adv_stop(m_adv_handle);   /* 安全起见显式停止 */

    nrf_gpio_cfg_input(WAKE_PIN, WAKE_PIN_PULL);
    nrf_gpio_cfg_sense_set(WAKE_PIN, NRF_GPIO_PIN_SENSE_LOW);

    err = sd_power_system_off();
    APP_ERROR_CHECK(err);
    for (;;) { __WFE(); }      /* 调试器连接时 System OFF 被仿真,循环等待 */
}

/* ==================================================================
 *  测试: 500ms 周期电池电压采样回调
 * ================================================================== */
static void test_timer_handler(void * p_context)
{
    (void)p_context;

    uint16_t batt_raw = saadc_sample_channel(SAADC_CH_BATTERY);

    NRF_LOG_INFO("ADC: batt=%u raw (%u mV)", batt_raw, saadc_raw_to_mv(batt_raw));
}

/* ==================================================================
 *  HX711 驱动 —— 双通道桥式传感器 ADC（支持多实例）
 * ==================================================================
 *
 * HX711 接口:
 *   DT  (DOUT)   — 数据输出,空闲高,转换完成自动拉低,读完 24+ 脉冲后恢复高
 *   SCK (PD_SCK) — 时钟输入,由 MCU 输出 25/26/27 个正脉冲
 *   每帧 = 24bit 数据(MSB first, 二进制补码) + 1~3 个增益选择脉冲
 *
 * 通道批次读法:
 *   上电默认 = Channel A @ 增益 128, 后续通道由每帧最后的多余脉冲数决定:
 *     25 脉冲 → 下一次转换 = Channel A, 增益 128
 *     26 脉冲 → 下一次转换 = Channel B, 增益 32
 *     27 脉冲 → 下一次转换 = Channel A, 增益 64
 *   读回的数据属于"上一次"脉冲序列选定的通道。切换通道时首帧返回的
 *   是旧通道数据(丢弃); 且切换后首帧的 DOUT 就绪会延后(约 4 个
 *   周期)——数据本身有效, 非阻塞轮询自然跳过等待, 无需丢弃样本。
 *   为减少切换频率, 每通道连续读 BATCH_SIZE 帧再切换。
 *
 * 关键时序(VDDA=5V, 典型值):
 *   SCK 高/低电平最小 0.2µs, 最大 50µs → 本驱动用 1µs 延时
 *   数据在 SCK 上升沿后 ≤0.1µs 稳定, 在 SCK 高电平期间采样
 *   转换时间: 10Hz 模式 ~100ms, 80Hz 模式 ~12.5ms(RATE 引脚控制)
 *   ⚠ 实测本工程模块: #1 每帧 ≈440ms(2.3Hz), #2 每帧 ≈508ms(2Hz)。
 */

static void hx711_init(hx711_t * dev)
{
    nrf_gpio_cfg_output(dev->sck_pin);
    nrf_gpio_pin_clear(dev->sck_pin);
    nrf_gpio_cfg_input(dev->dt_pin, NRF_GPIO_PIN_PULLUP);
    /* 上电默认正在转换 Channel A: "上一次读取"视为 25 脉冲 */
    dev->pulses = HX711_PULSES_CHA_128;
}

/* 检查 HX711 是否数据就绪(DT == 低) */
static inline bool hx711_is_ready(const hx711_t * dev)
{
    return (nrf_gpio_pin_read(dev->dt_pin) == 0);
}

/* 非阻塞尝试读一帧。
 *   dev    : HX711 实例指针
 *   pulses : 总脉冲数 = 24(数据) + N(增益选择), N ∈ {1,2,3}
 *   返回   : 24bit 有符号值(符号扩展到 int32_t)
 *   未就绪 : DOUT 未拉低(转换未完成)时立即返回 INT32_MIN, 本 tick 跳过
 *
 * ⚠ 本函数不等待: 就绪时总耗时 <60µs; 未就绪时仅一次 GPIO 读, 微秒级。
 *   切通道后首帧 DOUT 会延后约 4 个周期, 轮询会自然跳过等待, 无忙等。 */
static int32_t hx711_try_read_raw(hx711_t * dev, uint8_t pulses)
{
    int32_t val = 0;

    if (!hx711_is_ready(dev))
    {
        return INT32_MIN;   /* 数据未就绪, 本 tick 跳过 */
    }

    /* 读 24bit, MSB first; 在 SCK 高电平期间采样 DT */
    for (uint8_t i = 0; i < 24; i++)
    {
        nrf_gpio_pin_set(dev->sck_pin);
        nrf_delay_us(1);
        val = (val << 1) | (nrf_gpio_pin_read(dev->dt_pin) ? 1 : 0);
        nrf_gpio_pin_clear(dev->sck_pin);
        nrf_delay_us(1);
    }

    /* 第 25~pulses 个脉冲: 不读数据,仅用于设置下次转换的通道/增益 */
    for (uint8_t i = 24; i < pulses; i++)
    {
        nrf_gpio_pin_set(dev->sck_pin);
        nrf_delay_us(1);
        nrf_gpio_pin_clear(dev->sck_pin);
        nrf_delay_us(1);
    }

    dev->pulses = pulses;    /* 记录当前增益设定 */

    /* 符号扩展: 24bit 二进制补码 → 32bit */
    if (val & 0x800000)
    {
        val |= 0xFF000000;
    }

    return val;
}

/* 各通道最近一次原始值 */
static int32_t m_latest_1a = 0;
static int32_t m_latest_1b = 0;
static int32_t m_latest_2a = 0;

/* HX711 定时采样回调(10Hz 非阻塞轮询):
 *   定时器每 100ms 触发; 每 tick 只"尝试读", 数据未就绪立即跳过, 无忙等。
 *   实际数据更新率受模块转换速率限制(#1≈2.3Hz, #2≈2Hz)。
 *   #1: 批次状态机 —— 就绪时推进 1 帧, 单通道连读 BATCH_SIZE 帧后切换;
 *       批次首帧为切换帧(返回旧通道数据, 丢弃); 切通道后首帧 DOUT 延后
 *       约 4 个周期, 轮询自然跳过, 数据有效, 全部入窗。
 *   #2: 只读 chA, 不做通道切换。 */
static void hx711_timer_handler(void * p_context)
{
    (void)p_context;

    int32_t v;
    bool    new1 = false, new2 = false;

    /* ---- HX711 #1: 批次状态机, 非阻塞 ---- */
    {
        uint8_t pulses = (m_b1_target == HX711_CH_A)
                         ? HX711_PULSES_CHA_128 : HX711_PULSES_CHB_32;

        v = hx711_try_read_raw(&m_hx711_1, pulses);
        if (v != INT32_MIN)     /* 就绪 → 推进 1 帧; 未就绪 → 本 tick 跳过 */
        {
            new1 = true;
            if (!m_b1_switched)
            {
                /* 批次首帧(切换帧): 返回旧通道数据 → 丢弃 */
                m_b1_switched = true;
            }
            else
            {
                /* 正常帧: 目标通道数据, 全部有效, 直接入窗 */
                if (m_b1_target == HX711_CH_A)
                {
                    m_latest_1a = v;
                    ma_push(&m_ma_1a, v);
                }
                else
                {
                    m_latest_1b = v;
                    ma_push(&m_ma_1b, v);
                }
                m_b1_idx++;
                if (m_b1_idx >= HX711_BATCH_SIZE)
                {
                    /* 本批次完成 → 切换目标通道, 重置批次 */
                    m_b1_target   = (m_b1_target == HX711_CH_A)
                                    ? HX711_CH_B : HX711_CH_A;
                    m_b1_idx      = 0;
                    m_b1_switched = false;
                }
            }
        }
    }

    /* ---- HX711 #2: 只读 chA, 非阻塞 ---- */
    v = hx711_try_read_raw(&m_hx711_2, HX711_PULSES_CHA_128);
    if (v != INT32_MIN)
    {
        new2 = true;
        m_latest_2a = v;
        ma_push(&m_ma_2a, v);
    }

    /* 打印各通道最新原始值 + 本 tick 是否读到新帧
     * ⚠ 本 SDK 日志模块最多支持 6 个格式参数(LOG_INTERNAL_6), 勿超过 */
    NRF_LOG_INFO("HX711 raw:  1:chA=%6ld chB=%6ld  2:chA=%6ld [n=%u%u]",
                 m_latest_1a, m_latest_1b, m_latest_2a,
                 new1 ? 1u : 0u, new2 ? 1u : 0u);

    /* 窗口满后打印滑动平均值 */
    if (ma_ready(&m_ma_1a))
    {
        NRF_LOG_INFO("HX711 avg:  1:chA=%6ld chB=%6ld  2:chA=%6ld",
                     ma_avg(&m_ma_1a), ma_avg(&m_ma_1b), ma_avg(&m_ma_2a));
    }
}

/* ==================================================================
 *  main
 * ================================================================== */

int main(void)
{
    log_init();
    {
        NRF_LOG_INFO("sensor_beacon boot.");
        // NRF_LOG_FLUSH();
    }

#if 1
    /* ===== HX711 测试: 定时读取两通道 ADC 数值并通过日志/串口打印 ===== */

    /* 1) 使能 BLE 协议栈 (提供 LFCLK 给 app_timer) */
    ble_stack_init();

    /* 2) 初始化定时器 & 电源管理 */
    timers_init();
    power_management_init();

    /* 3) 初始化两路 HX711(配置各自 DT/SCK GPIO) */
    hx711_init(&m_hx711_1);
    hx711_init(&m_hx711_2);

    /* 3.5) 初始化滑动平均滤波器(#2 只读 chA) */
    ma_init(&m_ma_1a);
    ma_init(&m_ma_1b);
    ma_init(&m_ma_2a);

    /* 4) 创建周期定时器 —— 10Hz 非阻塞采样:
     *    每 100ms 触发一次, 回调内仅"尝试读"(就绪才读, 未就绪跳过),
     *    无忙等, 回调耗时微秒级。实际数据更新率受模块转换速率限制
     *    (#1≈2.3Hz, #2≈2Hz), 采样节奏保持 10Hz。 */
    {
        ret_code_t err = app_timer_create(&m_hx711_timer, APP_TIMER_MODE_REPEATED,
                                          hx711_timer_handler);
        APP_ERROR_CHECK(err);
        err = app_timer_start(m_hx711_timer, APP_TIMER_TICKS(100), NULL);
        APP_ERROR_CHECK(err);
    }

    NRF_LOG_INFO("HX711 test running: read chA/chB every 500ms...");
    NRF_LOG_FLUSH();

    /* 5) 主循环: 空闲 + 刷新日志（日志后端若配置为 UART 则从串口输出） */
    for (;;)
    {
        nrf_pwr_mgmt_run();
    }
#endif


#if 0
    /* ===== ADC 测试: 每 500ms 采样并通过日志/串口打印 ===== */

    /* 1) 使能 BLE 协议栈 (提供 LFCLK 给 app_timer) */
    ble_stack_init();

    /* 2) 初始化定时器 & 电源管理 */
    timers_init();
    power_management_init();

    /* 3) 初始化 SAADC */
    saadc_init();

    /* 4) 创建 500ms 周期定时器并启动 */
    {
        ret_code_t err = app_timer_create(&m_test_timer, APP_TIMER_MODE_REPEATED,
                                          test_timer_handler);
        APP_ERROR_CHECK(err);
        err = app_timer_start(m_test_timer, APP_TIMER_TICKS(500), NULL);
        APP_ERROR_CHECK(err);
    }

    NRF_LOG_INFO("ADC test running: sample every 500ms...");
    NRF_LOG_FLUSH();

    /* 5) 主循环: 空闲 + 刷新日志（日志后端若配置为 UART 则从串口输出） */
    for (;;)
    {
        nrf_pwr_mgmt_run();
    }
#endif


#if 0


    /* 读复位状态与计数器(SoftDevice 尚未使能 → 直接寄存器访问) */
    uint32_t resetreas = 0;
    bool woke_from_off = boot_read_and_classify(&resetreas);
    (void)woke_from_off;

    /* 读唤醒引脚当前电平以判定所处阶段 */
    nrf_gpio_cfg_input(WAKE_PIN, WAKE_PIN_PULL);
    uint32_t level = nrf_gpio_pin_read(WAKE_PIN);
    NRF_LOG_INFO("resetreas=0x%08x wake_pin=%u", resetreas, level);
    NRF_LOG_FLUSH();

    if (level != 0)
    {
        /* 高电平:真实事件段 → 采集并广播,内部不返回(以 System OFF 结束) */
        handle_event_active();
    }
    else
    {
        /* 低电平:信号清除段或冷启动空闲 → 不广播,重新武装 SENSE=HIGH 等待下次事件。
         *   SD 未使能 → 直接寄存器路径。 */
        rearm_and_off(NRF_GPIO_PIN_SENSE_HIGH);
    }

    /* 正常不会到达 */
    for (;;)
    {
        NRF_LOG_FLUSH();
        nrf_pwr_mgmt_run();
    }

#endif
}

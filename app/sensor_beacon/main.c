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
#include "nrf_drv_gpiote.h"

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "nrf_delay.h"

/* 板载外设驱动(独立源文件, 见 dev/app/sensor_beacon/drivers/) */
#include "drv_led.h"
#include "drv_key.h"
#include "ble_link.h"
#include "ble_adv_mux.h"
#include "ble_beacon.h"

/* ==================================================================
 *  可配置参数(TODO: 按实际硬件/需求修改)
 * ================================================================== */

/* 唤醒引脚: 外部电平变化(比较器/干簧管/霍尔等)接入的 GPIO。
 *
 * 选 P0.22 的理由(详见下方 HX711 段的引脚约束表):
 *   - 两板都引出且都空闲: DK 上是 Arduino 排针 D10(非 Button/LED/UART),
 *     E73-TBM 上是 P2 排针第 27 脚(原理图 E73-TBM-01-SCH)。
 *   - 不在数据手册 Table 100 的"低驱动低频 only"名单里(那份名单只含
 *     P0.25~P0.29),故无射频邻近限制。
 *   - QFN48 第 27 脚, 与 SWDCLK(25)/SWDIO(26) 相邻但功能独立, 不碍调试。
 *   - 不占 AIN(P0.02~05 / P0.28~31), 给模拟输入留余量。
 *
 * ⚠ SENSE 一次只能武装一个方向(高或低), 硬件不支持"任意边沿"。双向唤醒的
 *   做法见 rearm_and_off(): 读当前电平, 武装相反方向。 */
#define WAKE_PIN                    22

/* 唤醒引脚上拉/下拉: 使能内部上拉(RPU 典型 13kΩ, 数据手册 GPIO 电气规格)。
 *
 * 选上拉而非 NOPULL: 外部若是开漏/集电极开路输出(比较器常见), 必须有上拉
 * 才能确定高电平; 且悬空输入在 System OFF 下会因电平漂移引起误唤醒。
 * 代价是被拉低期间有 VDD/13k ≈ 250µA 漏流 —— 若外部确认为推挽输出,
 * 可改 NRF_GPIO_PIN_NOPULL 省掉这笔。 */
#define WAKE_PIN_PULL               NRF_GPIO_PIN_PULLUP

/* 电池电压 ADC 通道: 内部 VDD 输入,无需外部引脚 */
#define SAADC_CH_BATTERY            1

/* 传感器上电到数据稳定的等待时间(ms)。TODO: 按传感器手册确定 (1000~2000)。 */
#define SETTLING_TIME_MS            1500

/* 单击一次要播几个广播事件 —— 现由协议栈的 max_adv_evts 保证, 见
 * BLE_BEACON_ADV_EVENTS。这里不再有 SAMPLE_BURST_COUNT / GAP 那套应用层节拍:
 *
 * 需求是"3 次的包为重复一样的包", 而 max_adv_evts 天然就是"同一份数据重播 N
 * 次"(数据在 configure 时定下, 之后不变), 比应用层起定时器数三拍更准 ——
 * 后者只能保证"播了大约 3×300ms", 播出去几个包取决于广播间隔与协议栈调度。
 *
 * 与之一并删除的是"平时常驻数据广播": 现在平时【不播】, 只在单击后播 3 次。 */

/* 深度休眠路径(参考分支 3)每次唤醒后的数据广播窗口(ms)。
 * 只被 handle_event_active() 使用 —— 那条路径要等广播播完再回 System OFF。 */
#define ADV_WINDOW_MS               2000

/* 厂商自定义数据 —— 公司标识。0xFFFF 为 SIG 保留(测试用)。
 * TODO: 若有 SIG 分配的 Company ID 请替换。 */
#define APP_COMPANY_IDENTIFIER      0xFFFF

/* 负载协议魔数 + 版本(置于负载最前):
 *   - 魔数供网关在 0xFFFF 通用 Company ID 下过滤掉他人广播;
 *   - 版本供日后平滑演进负载格式。改布局时请递增版本并同步对接文档。
 *
 * v0x02: 移除外部传感器电压字段, 新增 3 路 24bit 传感器通道(见负载布局)。
 *        与 v0x01 不兼容, 网关须按版本分支解析。 */
#define APP_PROTO_MAGIC             0xAB
#define APP_PROTO_VERSION           0x02

/* 广播里放几字节设备 ID(取自 FICR->DEVICEID)。TODO: 2 或 4。 */
#define DEVICE_ID_LEN               2

#define APP_BLE_CONN_CFG_TAG        1       /* SoftDevice 连接配置标签 */

/* 本文件不再注册 NRF_SDH_BLE_OBSERVER, 故原 APP_BLE_OBSERVER_PRIO 已删。
 * 各模块的观察者优先级: ble_adv_mux=1, ble_link=3(见各自源文件)。 */

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
 *
 * 当前分配情况(上面 5 个安全引脚已全部用掉):
 *   P0.11/12 = HX711 #1, P0.23/24 = HX711 #2, P0.22 = WAKE_PIN
 * 若日后还需引脚, 候选只剩下面这些, 各有代价:
 *   - P0.15/16 : 真机空闲, 但 DK 上是 Button3/4(台架调试会冲突)
 *   - P0.19/20 : 真机空闲, 但 DK 上是 LED3/4
 *   - P0.30/31 : 两板都空闲且不在 Table 100 名单内, 但占用 AIN6/AIN7
 *   - P0.25~29 : 仅限低速信号(慢速电平输入可以, 时钟/PWM 不行)
 *   - P0.09/10 : 真机是 NFC 焊盘, 但 nRF52810 无 NFCT 外设(已核
 *                nrf52810_peripherals.h 无 NFCT_PRESENT), 故可当普通 GPIO
 *   - P0.00/01 : 若 LF 时钟改用内部 RC 则可释放。当前
 *                NRF_SDH_CLOCK_LF_SRC = 1 (XTAL), 被 32.768kHz 晶振占用
 */

/* --- HX711 #1 --- */
#define HX711_1_DT_PIN              11
#define HX711_1_SCK_PIN             12

/* --- HX711 #2 --- */
#define HX711_2_DT_PIN              23
#define HX711_2_SCK_PIN             24

/* 不同增益对应的 SCK 脉冲总数(含 24bit 数据 + N 个增益选择脉冲)
 * ⚠ chA 与 chB 增益不对等(硬件固定): chB 只能是 32, chA 可 128/64。
 *   故 chA(128) 与 chB(32) 之间有固有 4 倍灵敏度差, 详见下方驱动注释。 */
#define HX711_PULSES_CHA_128        25   /* Channel A, 增益 128, FS ±19.53mV */
#define HX711_PULSES_CHB_32         26   /* Channel B, 增益  32, FS ±78.12mV */
#define HX711_PULSES_CHA_64         27   /* Channel A, 增益  64, FS ±39.06mV (未使用) */

/* 通道标识 */
#define HX711_CH_A                  0
#define HX711_CH_B                  1

/* 批次读取参数:
 *   BATCH_SIZE = 每通道连续读的帧数(10)。
 *   切通道后模块需重新稳定 4 个输出周期(~400ms)才拉低 DOUT, 这段开销由
 *   本批次的 BATCH_SIZE 帧摊薄: 10 帧时约 1400ms/批, 折合 ~7Hz。
 *   调大 → 平均速率更接近 10Hz, 但两通道数据的时效差变大。 */
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
 *   [.. +2]                    传感器通道 0 (3 字节, 小端, 有符号 24bit)
 *   [.. +3]                    传感器通道 1 (3 字节, 小端, 有符号 24bit)
 *   [.. +3]                    传感器通道 2 (3 字节, 小端, 有符号 24bit)
 * 网关先校验 [0]=魔数、[1]=版本,再按版本解析后续字段。
 *
 * 传感器通道语义(与 HX711 实例对应):
 *   ch0 = HX711 #1 Channel A   ch1 = HX711 #1 Channel B   ch2 = HX711 #2 Channel A
 *
 * ⚠ 三通道增益不一致, 且负载中不携带增益字段。网关须按隐式约定换算:
 *     ch0 → 增益 128 (FS ±19.53mV)
 *     ch1 → 增益  32 (FS ±78.12mV)   ← chB 硬件固定 32, 不可改
 *     ch2 → 增益 128 (FS ±19.53mV)
 *   即 ch1 与 ch0/ch2 相差 4 倍灵敏度, 直接比较三者原始计数是错的。
 *   若日后增益配置可能变更, 应在负载中显式编码(当前余量 8 字节)。
 *
 * ⚠ 每通道 3 字节而非 2 字节: HX711 原生输出有符号 24bit,实测读数
 *   (见 _solve.py 标定数据)达 ±8 万量级,远超 int16 的 ±32767,压成
 *   2 字节会溢出失真。故保留 24bit 原样传输,由网关做符号扩展。
 */
#define SENSOR_CH_COUNT             3
#define SENSOR_VAL_BYTES            3       /* 有符号 24bit */

#define OFF_MAGIC                   0
#define OFF_VERSION                 1
#define OFF_DEVICE_ID               2
#define OFF_COUNTER                 (OFF_DEVICE_ID + DEVICE_ID_LEN)
#define OFF_BATTERY                 (OFF_COUNTER + 1)
#define OFF_SENSORS                 (OFF_BATTERY + 2)
#define MANUF_DATA_LEN              (OFF_SENSORS + SENSOR_CH_COUNT * SENSOR_VAL_BYTES)

/* 广播包字节预算(BLE 4.x 传统广播, 上限 31 字节):
 *     Flags 段            = 长度1 + 类型1 + 数据1            =  3
 *     厂商自定义段头      = 长度1 + 类型1 + Company ID 2     =  4
 *     → 负载可用          = 31 - 3 - 4                       = 24
 * 当前 MANUF_DATA_LEN = 16, 余量 8 字节。加字段时此断言会兜住超长。 */
#define MANUF_DATA_LEN_MAX          (BLE_GAP_ADV_SET_DATA_SIZE_MAX \
                                     - AD_TYPE_FLAGS_SIZE          \
                                     - AD_DATA_OFFSET              \
                                     - AD_TYPE_MANUF_SPEC_DATA_ID_SIZE)
STATIC_ASSERT(MANUF_DATA_LEN <= MANUF_DATA_LEN_MAX);

/* 载荷的封装(公司标识 + 段结构)由 ble_beacon 负责, 本文件只填字节。
 * 两边的常量必须一致, 否则网关按 APP_COMPANY_IDENTIFIER 过滤会漏掉本设备。 */
STATIC_ASSERT(APP_COMPANY_IDENTIFIER == BLE_BEACON_COMPANY_ID);

/* ==================================================================
 *  全局状态
 * ================================================================== */

static uint8_t                  m_manuf_data[MANUF_DATA_LEN];

/* 载荷长度必须与 ble_beacon 的约定一致 —— 那个模块按固定长度拷贝载荷,
 * 两边不一致会读越界或少播字节, 且是静默的。故编译期钉死。 */
STATIC_ASSERT(MANUF_DATA_LEN == BLE_BEACON_PAYLOAD_LEN);

APP_TIMER_DEF(m_settling_timer);            /* 传感器稳定等待定时器(单次) */

static volatile bool            m_settled  = false;   /* 稳定等待结束 */

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
 *  唤醒引脚 —— 运行态的边沿中断(联调用, 与 System OFF 的 SENSE 唤醒互补)
 * ==================================================================
 *
 * 用途: 在【系统运行时】验证 WAKE_PIN 的外部布线与信号极性是否正确 ——
 *       接上外部信号, 电平每变化一次就打一条日志。这样在把休眠逻辑真正
 *       接进来之前, 先把"引脚选对了没、线接通了没、信号干净不干净"这些
 *       硬件问题排掉。
 *
 * ⚠ 这【不是】深度休眠唤醒机制, 两者是不同的硬件路径:
 *     - 本模块 = GPIOTE 边沿中断, 只在 System ON(运行/睡眠)下有效;
 *     - 深度唤醒 = GPIO SENSE + DETECT, System OFF 下唯一可用的路径,
 *       且唤醒等于芯片复位(见 rearm_and_off() / boot_read_and_classify())。
 *   进 System OFF 前 GPIOTE 配置会失效, 必须另行 nrf_gpio_cfg_sense_set()。
 *
 * 为什么用 SENSE_TOGGLE 而不是 HITOLO/LOTOHI:
 *   需求是"电平变化"即触发, 双向都要报。GPIOTE 的 TOGGLE 极性正好对应,
 *   一个通道搞定; 若分开配上升/下降则要占两个通道。
 *
 * 关于 hi_accuracy(传给 GPIOTE_CONFIG_IN_SENSE_TOGGLE 的参数):
 *   true  = IN_EVENT, 独占一个 GPIOTE 通道(共 8 个), 精度高但常开 HFCLK,
 *           空闲功耗显著上升(数百 µA 级);
 *   false = PORT_EVENT, 多引脚共享一个 PORT 事件, 低功耗。
 *   本模块选 false —— 慢速电平变化不需要高精度时间戳, 且这颗芯片的定位
 *   就是低功耗, 不能为了联调日志把静态功耗抬上去。
 *   注意 PORT_EVENT 是所有引脚共享的, app_button(DRV_KEY_PIN) 也走这条,
 *   nrf_drv_gpiote 内部会统一分发, 不冲突。
 *
 * ⚠ 上下文: 回调在 GPIOTE 中断里执行(优先级 GPIOTE_CONFIG_IRQ_PRIORITY = 7),
 *   故只做计数与日志入队, 不做阻塞操作, 不用 APP_ERROR_CHECK。
 */

/* 累计触发次数。volatile: 中断里写, 主循环/日志读。 */
static volatile uint32_t m_wake_edge_count = 0;

static void wake_pin_evt_handler(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action)
{
    (void)action;   /* TOGGLE 模式下 action 恒为 TOGGLE, 无区分意义 */

    if (pin != WAKE_PIN)
    {
        return;
    }

    m_wake_edge_count++;

    /* 读回当前电平以区分是上升沿还是下降沿。
     * ⚠ 这是"中断响应时"的电平, 不等于"触发瞬间"的电平 —— 若信号变化快于
     *   中断延迟, 或存在抖动, 两者可能不一致。本模块只用于联调观察,
     *   不做去抖; 若外部是机械触点(干簧管等), 一次动作可能打出多条日志,
     *   那正是需要看到的现象。 */
    uint32_t level = nrf_gpio_pin_read(WAKE_PIN);

    NRF_LOG_INFO("WAKE_PIN edge #%u: P0.%02u now %s",
                 m_wake_edge_count, WAKE_PIN, level ? "HIGH" : "LOW");
}

/* 配置 WAKE_PIN 为双向边沿中断并使能。
 *
 * 前置条件: 无(nrf_drv_gpiote_init 幂等, 内部有 is_init 判断)。
 *           但若 app_button 已初始化过 GPIOTE, 这里的 init 会被跳过,
 *           属正常路径 —— 故必须容忍 NRF_ERROR_INVALID_STATE。 */
static void wake_pin_irq_init(void)
{
    ret_code_t err;

    /* app_button 可能已经初始化过 GPIOTE。重复 init 返回 INVALID_STATE,
     * 这不是错误, 忽略即可; 其余错误码才需要断言。 */
    if (!nrf_drv_gpiote_is_init())
    {
        err = nrf_drv_gpiote_init();
        APP_ERROR_CHECK(err);
    }

    /* hi_accuracy = false → 走低功耗的 PORT_EVENT, 见上方说明 */
    nrf_drv_gpiote_in_config_t cfg = GPIOTE_CONFIG_IN_SENSE_TOGGLE(false);

    /* 宏默认 pull = NOPULL, 这里覆盖为与深度唤醒路径一致的上拉配置,
     * 保证两条路径看到的静态电平相同(开漏信号必须有上拉才能确定高电平)。 */
    cfg.pull = WAKE_PIN_PULL;

    err = nrf_drv_gpiote_in_init(WAKE_PIN, &cfg, wake_pin_evt_handler);
    APP_ERROR_CHECK(err);

    /* 第二参数 true = 使能中断(false 则只产生事件供 PPI 用, 不进 CPU) */
    nrf_drv_gpiote_in_event_enable(WAKE_PIN, true);

    NRF_LOG_INFO("WAKE_PIN irq armed: P0.%02u, both edges, pull-up, initial %s",
                 WAKE_PIN, nrf_gpio_pin_read(WAKE_PIN) ? "HIGH" : "LOW");
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

/* 采集一路通道(阻塞)。负值(共模噪声)钳到 0。
 *
 * ⚠ 不用 APP_ERROR_CHECK: 本函数会在 app_timer 中断上下文被调用
 *   (单击连播时的 payload_refresh), 中断里断言失败直接进 fault handler。
 *   失败只可能是 NRFX_ERROR_BUSY(SAADC 已在转换) —— 本工程只有这一处
 *   在用 SAADC, 正常不会发生; 真发生了返回 0 比死机好。
 *
 * 阻塞时长: 12bit + 默认采集时间 10µs, 单次转换约十几微秒, 中断里可接受。 */
static uint16_t saadc_sample_channel(uint8_t channel)
{
    nrf_saadc_value_t v = 0;
    ret_code_t err = nrf_drv_saadc_sample_convert(channel, &v);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_WARNING("saadc convert failed (0x%08x)", err);
        return 0;
    }
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

/* 写入一个有符号 24bit 值(小端)。超出 24bit 量程时钳位,避免高位被截断
 * 后符号翻转(例如 0x00800000 被截成负数)。 */
static void payload_put_s24(uint8_t * p_dst, int32_t val)
{
    if (val > 0x7FFFFF)        { val = 0x7FFFFF; }
    else if (val < -0x800000)  { val = -0x800000; }

    uint32_t u = (uint32_t)val;      /* 负数按二进制补码取低 24bit */
    p_dst[0] = (uint8_t)(u & 0xFF);
    p_dst[1] = (uint8_t)((u >> 8) & 0xFF);
    p_dst[2] = (uint8_t)((u >> 16) & 0xFF);
}

/* p_sensors: SENSOR_CH_COUNT 个通道值(ch0=#1chA, ch1=#1chB, ch2=#2chA)。
 *            传 NULL 表示本次无传感器数据,对应字段填 0。 */
static void payload_build(uint16_t batt_raw, const int32_t * p_sensors)
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

    /* 3 路传感器通道, 各 3 字节小端有符号 24bit */
    for (uint8_t ch = 0; ch < SENSOR_CH_COUNT; ch++)
    {
        payload_put_s24(&m_manuf_data[OFF_SENSORS + ch * SENSOR_VAL_BYTES],
                        (p_sensors != NULL) ? p_sensors[ch] : 0);
    }
}

/* ==================================================================
 *  BLE 协议栈
 * ==================================================================
 *
 * 本文件【不再】注册自己的 NRF_SDH_BLE_OBSERVER, 也不再持有广播句柄:
 *   - 广播集(S112 只有一个)由 services/ble_adv_mux.c 独占, 它在优先级 1
 *     监听 CONNECTED / ADV_SET_TERMINATED 以同步"是否在播";
 *   - 连接与可连接广播的生命周期由 services/ble_link.c 监听(优先级 3),
 *     经 ble_link_evt_handler() 上报到本文件的策略层。
 * 原先本文件那个只为 m_adv_done 服务的 observer 已随之删除 ——
 * 数据广播现在是 duration=0 的常驻广播, 不存在"广播时长到了"这回事。
 */

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

/* 本文件的连接配置标签必须与 mux 实际用于 sd_ble_gap_adv_start() 的一致,
 * 否则可连接广播会带着一份没被 nrf_sdh_ble_default_cfg_set() 配置过的
 * conn_cfg 开播。两边都是 1, 编译期钉死以防日后单边改动。 */
STATIC_ASSERT(APP_BLE_CONN_CFG_TAG == BLE_ADV_MUX_CONN_CFG_TAG);

/* ==================================================================
 *  广播策略层 —— "什么时候该播什么"集中在这里
 * ==================================================================
 *
 * 广播集只有一个(见 ble_link.h 头部的长篇论证), 所以"数据广播"与"可连接
 * 广播"是分时的。谁该占用它不是任何一个模块自己能决定的事, 必须由应用层
 * 编排 —— 这就是本节存在的理由。ble_link.c / ble_beacon.c 都只上报事件、
 * 提供动作, 不做策略。
 *
 * 状态迁移(与需求逐条对应):
 *   开机            → 【什么都不播】, LED 灭
 *   单击            → LED 闪一下 + 同一份数据播 BLE_BEACON_ADV_EVENTS 次,
 *                     播完协议栈自动停 → 回到"什么都不播"
 *   长按            → LED 快闪 + 可连接广播开(30s 窗口)
 *   连上            → LED 常亮
 *   断开 / 窗口到期 → LED 灭 + 可连接广播关 → 回到"什么都不播"
 *
 * ⚠ 平时不播任何广播(需求: "平时数据广播不能持续广播")。所以设备在没被按过
 *   按键、也没连接的时候是完全静默的 —— 网关只在有人按键时能收到数据。
 *   这也意味着"设备是否活着"无法从空气中判断, 若日后需要心跳, 应该是
 *   起一个长周期定时器调 adv_policy_burst_data(), 而不是把 beacon 改回常驻。
 *
 * ⚠ 连接期间为什么不播数据: 需求明确写的是"打开可连接蓝牙, 暂停数据广播",
 *   且连接期间数据另有通路(NUS)。硬件其实【允许】同时播(Broadcaster 与
 *   Peripheral 是独立 role, 详见 ble_link.h), 真要如此在
 *   BLE_LINK_EVT_CONNECTED 分支加一次 adv_policy_burst_data() 即可。
 */

/* 取当前三通道滑动平均 + 重采一次电量, 组装进 m_manuf_data。 */
static void payload_refresh(void)
{
    int32_t sensors[SENSOR_CH_COUNT] =
    {
        ma_avg(&m_ma_1a),   /* ch0: HX711 #1 Channel A */
        ma_avg(&m_ma_1b),   /* ch1: HX711 #1 Channel B */
        ma_avg(&m_ma_2a)    /* ch2: HX711 #2 Channel A */
    };

    payload_build(saadc_sample_channel(SAADC_CH_BATTERY), sensors);
}

/* 采一份新数据并播 BLE_BEACON_ADV_EVENTS 次(同一份数据重复播)。
 * 播完由协议栈自动停 —— 本函数不需要善后, 也没有配套的 stop。
 *
 * ⚠ 可在中断上下文调用 —— 故只记日志, 不 APP_ERROR_CHECK。 */
static void adv_policy_burst_data(void)
{
    /* 可连接窗口/连接期间广播集不在数据广播手上 —— 此时开播会把可连接广播
     * 抢掉, 等于用户刚长按打开的调试口被一次单击关掉了。宁可不发。 */
    if (ble_link_is_advertising() || ble_link_is_connected())
    {
        NRF_LOG_INFO("Connectable window active; data burst skipped.");
        return;
    }

    m_counter = (uint8_t)(m_counter + 1);
    payload_refresh();

    ret_code_t err = ble_beacon_burst(m_manuf_data);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_WARNING("beacon burst failed (0x%08x)", err);
        return;
    }

    NRF_LOG_INFO("Data burst started (counter=%u, %u identical packets).",
                 m_counter, BLE_BEACON_ADV_EVENTS);
}

/* 打开可连接广播(抢占广播集; 若数据广播正播到一半, 那半轮被丢弃)。 */
static void adv_policy_open_connectable(void)
{
    ret_code_t err = drv_led_blink(DRV_LED_BLINK_FAST);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_WARNING("LED fast blink failed (0x%08x)", err);
    }

    err = ble_link_adv_start();
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_WARNING("connectable adv failed (0x%08x)", err);
        /* 开不起来就别留着快闪骗人 —— 灯灭, 回到"什么都不播"的静默态。 */
        drv_led_off();
        return;
    }

    NRF_LOG_INFO("Connectable window open (%us).", BLE_LINK_ADV_DURATION_MS / 1000);
}

/* 关闭可连接广播并回到"平时"状态: 灯灭 + 什么都不播。
 * 连接断开与窗口到期两条路径都收敛到这里。
 *
 * ⚠ 这里【不】补一次数据广播: 平时静默是需求。要数据就再单击一次。 */
static void adv_policy_idle(void)
{
    drv_led_off();

    ret_code_t err = ble_link_adv_stop();
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_WARNING("connectable adv stop failed (0x%08x)", err);
    }

    NRF_LOG_INFO("Idle: no advertising until next key press.");
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

    /* 5) 组装并发送非连接广播, 播 BLE_BEACON_ADV_EVENTS 次后由协议栈自动停。
     *    传感器值取滑动平均(比单帧原始值更稳);当前唤醒路径尚未接入 HX711
     *    采集,窗口为空时 ma_avg() 返回 0,即三通道字段为占位 0。
     *    TODO: 接入 HX711 采集后,此处应在稳定等待期内完成取样再广播。
     *
     *    ⚠ 这里仍用定时器等 ADV_WINDOW_MS 而非等 ADV_SET_TERMINATED 事件:
     *      本函数是顺序执行的裸流程(idle_until 阻塞式等标志), 接事件回调要多
     *      一个静态标志, 而"数够时间"在这条路径上足够 —— ADV_WINDOW_MS(2000ms)
     *      远大于 3 次广播实际耗时(3 × 100ms)。 */
    int32_t sensors[SENSOR_CH_COUNT] =
    {
        ma_avg(&m_ma_1a),   /* ch0: HX711 #1 Channel A */
        ma_avg(&m_ma_1b),   /* ch1: HX711 #1 Channel B */
        ma_avg(&m_ma_2a)    /* ch2: HX711 #2 Channel A */
    };

    payload_build(batt_raw, sensors);

    err = ble_beacon_init();
    APP_ERROR_CHECK(err);
    err = ble_beacon_burst(m_manuf_data);
    APP_ERROR_CHECK(err);

    m_settled = false;      /* 复用同一个单次定时器计广播窗口 */
    err = app_timer_start(m_settling_timer, APP_TIMER_TICKS(ADV_WINDOW_MS), NULL);
    APP_ERROR_CHECK(err);
    idle_until(&m_settled);

    /* 6) 广播结束 → 武装 SENSE=LOW(等待信号回落的低电平唤醒) → System OFF。
     *    此处 SD 已使能,须用 sd_power_* 而非直接寄存器。 */
    NRF_LOG_INFO("Adv done. Arm SENSE_LOW, System OFF.");
    NRF_LOG_FLUSH();

    (void)ble_beacon_stop();

    nrf_gpio_cfg_input(WAKE_PIN, WAKE_PIN_PULL);
    nrf_gpio_cfg_sense_set(WAKE_PIN, NRF_GPIO_PIN_SENSE_LOW);

    err = sd_power_system_off();
    APP_ERROR_CHECK(err);
    for (;;) { __WFE(); }      /* 调试器连接时 System OFF 被仿真,循环等待 */
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
 *   是旧通道数据(丢弃)。
 *   为减少切换频率, 每通道连续读 BATCH_SIZE 帧再切换。
 *
 * ⚠ 两通道增益不对等(芯片硬件决定, 不可配置):
 *     Channel A — 可选 128 或 64
 *     Channel B — 固定 32, 无任何脉冲组合/寄存器能改
 *   本工程实际使用: #1 chA=128, #1 chB=32, #2 chA=128 (27 脉冲/增益 64 未用)。
 *   因此 chA 与 chB 之间存在固有的 128/32 = 4 倍灵敏度差:
 *     同一差分输入电压, chA 读回的计数约为 chB 的 4 倍;
 *     反之 chB 的量程是 chA 的 4 倍, 但分辨率低 4 倍。
 *
 *   满量程差分输入 = ±0.5 × VDDA / 增益 (VDDA=5V):
 *     增益 128 → ±19.53 mV, 1 LSB ≈ 0.0023 µV   (高分辨率, 易饱和)
 *     增益  64 → ±39.06 mV, 1 LSB ≈ 0.0047 µV
 *     增益  32 → ±78.12 mV, 1 LSB ≈ 0.0093 µV   (大量程, 低分辨率)
 *
 *   ⚠ 广播负载中三通道均为"原始计数值", 不携带增益信息(协议 v0x02 无增益
 *     字段)。网关必须按隐式约定换算: ch0=128, ch1=32, ch2=128。
 *     若日后增益可能变更, 应在负载中显式编码(当前尚有 8 字节余量)。
 *
 * 关键时序(VDDA=5V, 典型值):
 *   SCK 高/低电平最小 0.2µs, 最大 50µs → 本驱动用 1µs 延时
 *   数据在 SCK 上升沿后 ≤0.1µs 稳定, 在 SCK 高电平期间采样
 *   转换时间: 10Hz 模式 ~100ms, 80Hz 模式 ~12.5ms(RATE 引脚控制)
 *
 * 输出速率(10Hz 模式, 实测确认):
 *   单通道持续读取 = 满速 10Hz(每帧 ~100ms), 这是模块能力上限。
 *   切换通道后, 模块需重新稳定 4 个输出周期(~400ms)才会拉低 DOUT,
 *   此期间 DOUT 保持高电平, 属正常行为而非故障。
 *   因此交替读取的平均速率必然低于 10Hz —— 符合预期, 无需排查:
 *     每半批次 = 400ms(切换稳定) + BATCH_SIZE 帧 × 100ms
 *     BATCH_SIZE=10 时 → 1400ms / 10 帧 = 140ms/帧 ≈ 7Hz(推算值)
 *   增大 BATCH_SIZE 可摊薄切换开销, 代价是两通道的数据时效差变大。
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
 *   同通道连读时每 ~100ms 就绪一次; 切通道后需 ~400ms(4 个输出周期)
 *   才会拉低 DOUT, 期间轮询持续返回 INT32_MIN, 无忙等。 */
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

/* HX711 定时采样回调(非阻塞轮询):
 *   定时器每 100ms 触发; 每 tick 只"尝试读", 数据未就绪立即跳过, 无忙等。
 *   #1: 批次状态机 —— 就绪时推进 1 帧, 单通道连读 BATCH_SIZE 帧后切换;
 *       批次首帧为切换帧(返回旧通道数据, 丢弃); 切换后模块需 ~400ms
 *       重新稳定, 期间 DOUT 不拉低, 轮询自然跳过。
 *   #2: 只读 chA, 不切通道 → 可达满速 10Hz。
 *   ⚠ tick 周期(100ms)与 10Hz 模式的转换周期(~100ms)相同, 两者相位漂移
 *     会导致部分转换被整拍错过。若需稳定贴近 10Hz, 应把 tick 缩短到
 *     ~20~50ms(回调本身仅微秒级, 开销可忽略)。 */
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
 *  按键手势 → 广播/LED 行为
 * ==================================================================
 *
 * 事件映射(需求原文的逐条落地):
 *   单击 → LED 闪一下 + 同一份采样数据播 BLE_BEACON_ADV_EVENTS 次
 *          (数据 = 3 通道滑动窗口平均 + 电量, 见 payload_build)
 *   长按 → LED 快闪 + 打开可连接广播(30s 窗口)
 *   双击 → 主动关掉可连接窗口(需求未规定; 选这个是因为长按开的窗口总得有个
 *          提前收工的办法, 否则只能干等超时)
 *
 * 事件名由驱动统一打印(见 drv_key.c 的 evt_report), 这里再打印"动作后的
 * 结果", 便于在串口日志上把"事件"与"效果"对上。
 *
 * ⚠ 本回调在 app_timer 中断上下文执行(见 drv_key.h 说明), 因此只做 GPIO
 *   翻转、定时器启停、SoftDevice 调用与日志入队, 不做阻塞操作, 且一律
 *   不用 APP_ERROR_CHECK —— 中断里断言失败会直接进 fault handler。
 */

static void key_evt_handler(drv_key_evt_t evt)
{
    switch (evt)
    {
        case DRV_KEY_EVT_SINGLE_CLICK:
        {
            ret_code_t err = drv_led_flash_once(0);
            if (err != NRF_SUCCESS)
            {
                NRF_LOG_WARNING("LED flash failed (0x%08x)", err);
            }

            NRF_LOG_INFO("KEY %s -> flash + %u identical data packets",
                         drv_key_evt_str(evt), BLE_BEACON_ADV_EVENTS);
            adv_policy_burst_data();
        } break;

        case DRV_KEY_EVT_LONG_PRESS:
            NRF_LOG_INFO("KEY %s -> open connectable window", drv_key_evt_str(evt));
            adv_policy_open_connectable();
            break;

        case DRV_KEY_EVT_DOUBLE_CLICK:
            /* 提前收工: 连着的先挂断(断开事件里会收敛到 adv_policy_idle),
             * 没连上的直接关窗口。 */
            if (ble_link_is_connected())
            {
                NRF_LOG_INFO("KEY %s -> disconnect", drv_key_evt_str(evt));
                (void)ble_link_disconnect();
            }
            else
            {
                NRF_LOG_INFO("KEY %s -> close connectable window",
                             drv_key_evt_str(evt));
                adv_policy_idle();
            }
            break;

        default:
            break;
    }
}

/* 一次性装配 LED + KEY。
 * 前置条件: app_timer_init() 已完成(两个驱动都依赖 app_timer)。 */
static void key_led_init(void)
{
    ret_code_t err;

    err = drv_led_init();
    APP_ERROR_CHECK(err);

    err = drv_key_init(key_evt_handler);
    APP_ERROR_CHECK(err);

    NRF_LOG_INFO("KEY/LED ready: single=burst, long=connectable, double=close.");
}

/* ==================================================================
 *  BLE 透传链路: 收到数据的处理(后续 cmd 协议的接入点)
 * ==================================================================
 *
 * 当前实现只做两件事: 打印 + 原样回显。
 *   - 打印: 原始字节的 hexdump 已由 ble_link.c 统一打印(收/发各一次),
 *           这里只补一条"可打印形式", 方便手机端直接发文本调试。
 *   - 回显: 把收到的字节发回去。这既是"收发数据都能打印"的最简闭环验证,
 *           也占住了后续 cmd 协议的回包位置 —— 协议接进来后, 这里换成
 *           "解析 → 入队 → 主循环执行 → 回包"即可。
 *
 * ⚠ 本回调在 SoftDevice 事件中断上下文执行(见 ble_link.h 说明), 因此:
 *     - 不可阻塞、不可长延时;
 *     - 不用 APP_ERROR_CHECK —— 中断里断言失败直接进 fault handler;
 *     - 回显失败(NRF_ERROR_RESOURCES: 发送缓冲满)只记日志, 不重试。
 *       真正需要可靠回包时, 应改为入队 + BLE_NUS_EVT_TX_RDY 驱动续发。
 */
static void ble_link_rx_handler(const uint8_t * p_data, uint16_t len)
{
    /* 转成以 NUL 结尾的字符串再打印: NRF_LOG 的 %s 需要 NUL 结尾,
     * 而 NUS 收到的是裸字节流, 不保证有终止符。
     * 缓冲取 32 字节即可 —— 只为"看一眼", 超长部分靠 hexdump 看全。 */
    char text[32];
    uint16_t n = (len < (sizeof(text) - 1)) ? len : (uint16_t)(sizeof(text) - 1);

    for (uint16_t i = 0; i < n; i++)
    {
        /* 非可打印字符替换成 '.', 免得控制字符把终端搞乱 */
        text[i] = ((p_data[i] >= 0x20) && (p_data[i] < 0x7f)) ? (char)p_data[i] : '.';
    }
    text[n] = '\0';

    NRF_LOG_INFO("APP got %u bytes: \"%s\"%s", len, NRF_LOG_PUSH(text),
                 (n < len) ? " (truncated)" : "");

    /* 回显 */
    ret_code_t err = ble_link_send(p_data, len);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_WARNING("echo failed (0x%08x)", err);
    }
}

/* 链路状态事件 → LED 与广播策略。
 *
 * ⚠ 本回调在 SoftDevice 事件中断上下文执行(见 ble_link.h 说明), 同样
 *   不可阻塞、不可 APP_ERROR_CHECK。 */
static void ble_link_evt_handler(ble_link_evt_t evt)
{
    NRF_LOG_INFO("LINK %s", ble_link_evt_str(evt));

    switch (evt)
    {
        case BLE_LINK_EVT_CONNECTED:
            /* 常亮 = 已连上。不播数据广播(需求, 见广播策略层说明)。 */
            drv_led_on();
            break;

        case BLE_LINK_EVT_DISCONNECTED:
            /* 断开 → 灯灭 + 关闭可连接广播 → 回到静默。 */
            adv_policy_idle();
            break;

        case BLE_LINK_EVT_ADV_TIMEOUT:
            /* 窗口到期无人连接 → 与断开同样处理 */
            NRF_LOG_INFO("Connectable window expired.");
            adv_policy_idle();
            break;

        default:
            break;
    }
}

/* 一次性装配 BLE: 透传服务 + 数据广播模块。
 *
 * ⚠ 开机【什么广播都不开】(需求):
 *     - 不开可连接广播 —— 要连就长按按键开 30s 窗口;
 *     - 也不开数据广播 —— 数据只在单击后播 BLE_BEACON_ADV_EVENTS 次。
 *   所以本函数只做初始化, 开机后设备在空中是完全静默的。
 *
 * 前置条件: ble_stack_init() 与 timers_init() 均已完成
 *           (ble_conn_params 内部要用 app_timer)。 */
static void ble_start(void)
{
    ret_code_t err;

    /* 可连接广播 + NUS: 只做配置, 不开播 */
    err = ble_link_init(ble_link_rx_handler, ble_link_evt_handler);
    APP_ERROR_CHECK(err);

    /* 数据广播: 同样只做配置。首帧载荷等到第一次单击时才组装 ——
     * 那时 HX711 滑动窗口已经有真实数据了。 */
    err = ble_beacon_init();
    APP_ERROR_CHECK(err);
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

    /* 3.6) 板载外设: LED + 按键。
     *      必须在 timers_init() 之后 —— 两个驱动都要 app_timer_create。 */
    key_led_init();

    /* 3.65) SAADC: 电量采集。常驻初始化而不反复 init/uninit ——
     *       单击时要在中断上下文里采一次电量, 那里不适合做外设的
     *       初始化/反初始化。静态电流代价可忽略(SAADC 空闲不耗电,
     *       只有转换那十几微秒才拉电流)。 */
    saadc_init();

    /* 3.7) BLE: NUS 透传服务 + 数据广播模块, 两者都只初始化不开播。
     *      ⚠ 开机什么广播都不开(需求): 长按才开可连接窗口, 单击才播数据。
     *      必须在 ble_stack_init()(协议栈) 与 timers_init()(app_timer) 之后。 */
    ble_start();

    /* 3.8) 唤醒引脚边沿中断(联调): 外部电平每变化一次打一条日志。
     *      必须在 key_led_init() 之后 —— 那里的 app_button 会初始化
     *      GPIOTE, 本函数复用同一个驱动实例(内部有 is_init 判断, 顺序反了
     *      也能工作, 但按依赖顺序写更清楚)。
     *
     *      ⚠ 这是 System ON 下的 GPIOTE 边沿中断, 不是深度休眠唤醒。
     *        深度唤醒走 GPIO SENSE, 是另一条路径, 当前分支未启用。 */
    wake_pin_irq_init();

    /* 4) 创建周期定时器 —— 100ms 非阻塞采样:
     *    每 tick 仅"尝试读"(就绪才读, 未就绪跳过), 无忙等, 回调耗时微秒级。
     *    模块 10Hz 模式下: #2 单通道连读可达满速 10Hz; #1 因交替切换需在
     *    每批次开头等 ~400ms 重新稳定, 平均速率略低(~7Hz), 属预期行为。 */
    {
        ret_code_t err = app_timer_create(&m_hx711_timer, APP_TIMER_MODE_REPEATED,
                                          hx711_timer_handler);
        APP_ERROR_CHECK(err);
        err = app_timer_start(m_hx711_timer, APP_TIMER_TICKS(100), NULL);
        APP_ERROR_CHECK(err);
    }

    NRF_LOG_INFO("HX711 test running: poll every 100ms...");
    NRF_LOG_INFO("KEY/LED active: KEY=P0.%02u LED=P0.%02u", DRV_KEY_PIN, DRV_LED_PIN);
    NRF_LOG_INFO("  single -> flash + %u identical data packets",
                 BLE_BEACON_ADV_EVENTS);
    NRF_LOG_INFO("  long   -> connectable \"%s\" for %us",
                 BLE_LINK_DEVICE_NAME, BLE_LINK_ADV_DURATION_MS / 1000);
    NRF_LOG_INFO("  double -> close connectable window / disconnect");
    NRF_LOG_INFO("BLE: silent until a key press (no periodic advertising).");
    NRF_LOG_INFO("WAKE_PIN P0.%02u: drive it high/low externally to see edge logs.",
                 WAKE_PIN);
    NRF_LOG_FLUSH();

    /* 5) 主循环: 空闲 + 刷新日志（日志后端若配置为 UART 则从串口输出） */
    for (;;)
    {
        nrf_pwr_mgmt_run();
    }
#endif


    /* ==============================================================
     *  原分支 2(ADC 500ms 轮询测试)已于 2026-08-18 按需求删除。
     *  随之清理的独占符号: m_test_timer、test_timer_handler()。
     *  saadc_init/uninit/sample_channel/callback/raw_to_mv 全部保留 ——
     *  下面分支 3 的 handle_event_active() 仍在用, 不是死代码。
     * ============================================================== */


    /* ==============================================================
     *  分支 3 —— 深度休眠/唤醒/广播主逻辑【逻辑参考, 不会直接启用】
     * ==============================================================
     *
     * 这段是完整的"SENSE 唤醒 → 判定阶段 → 采集 → 广播 → 回 System OFF"
     * 闭环。保留它是【作为实现参考】: 将来把休眠逻辑接进上面分支 1 时,
     * 时序与状态判定照这里抄。它本身不会被切成 #if 1 单独启用 ——
     * 直接启用会撞上一个已知问题:
     *
     *   分支 1 里 wake_pin_irq_init() 装的 GPIOTE 边沿中断在 System OFF
     *   下失效 —— 那是另一条硬件路径。进 System OFF 前必须改走
     *   nrf_gpio_cfg_sense_set(), 即本分支 rearm_and_off() 的做法。
     *   两者互补而不可互相替代, 详见 wake_pin_irq_init() 上方说明。
     *
     * (原先还有第二个障碍: 本文件与 ble_link.c 各持一个广播句柄, 而 S112
     *  只有一个广播集, 两条广播路径同时激活必有一方拿到 NRF_ERROR_NO_MEM。
     *  该问题已随 ble_adv_mux 的引入解决 —— 广播集现由 mux 独占, 本分支
     *  也改用 ble_beacon_burst()/stop(), 不再自己碰句柄。)
     * ============================================================== */
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

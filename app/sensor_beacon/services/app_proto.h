/**
 * app_proto —— 广播包格式 v0x03(饮水量记录)
 * ------------------------------------------------------------------
 * 职责: 把"厂商自定义广播段里的字节怎么排"这件事收敛到一个头文件, 供固件
 *       填包与网关/客户端解包共用同一份定义。本文件【只有定义与内联编解码】,
 *       没有 .c —— 不引入任何代码体积, 也不依赖 SoftDevice 头。
 *
 * ============================================================
 *  与 v0x02 的区别: 播的不再是三通道原始计数, 而是【算好的饮水量】
 * ============================================================
 *
 * v0x02 播 3 通道 × 3 字节的 HX711 原始计数, 由客户端做标定换算。v0x03 改成
 * 固件端用配置区里的标定参数算出重量后再播 —— 因为标定参数已经持久化在
 * flash(见 app_storage.h 的 cal_zero/cal_k), 客户端不再需要自己维护一份。
 * 换来的是一包能装【多条历史记录】: 原始计数一条要 9 字节, 算好的重量一条
 * 只要 2 字节。
 *
 * ============================================================
 *  字节布局(厂商自定义数据段, 共 APP_PROTO_PKT_LEN = 24 字节)
 * ============================================================
 *
 *   偏移  长度  字段
 *   ----  ----  --------------------------------------------------
 *     0     1   magic       = APP_PROTO_MAGIC (0xAB), 沿用 v2
 *     1     1   version     = APP_PROTO_VERSION (0x03)
 *     2     2   device_id   小端无符号, 沿用 v2
 *     4     1   battery     电池电压, 压缩编码(见 app_proto_batt_enc)
 *     5     2   last_index  最新那条记录的 seq 低 16 位, 小端
 *     7     1   drink_idx(高4位) | rec_count(低4位)
 *     8    16   8 条记录 × 2 字节, [0] 是最新的一条
 *
 * 每条记录 2 字节(小端 uint16):
 *     bit15..12  饮料类型 idx (0..15)
 *     bit11..0   饮水量, 【有符号】12 位, 单位 1 g, 范围 -2048..+2047
 *
 * ⚠ 饮水量必须是有符号的: 它是"这次测得的重量差", 杯子被加满时是负值。
 *   按无符号解会把"加水 1200g"读成"喝了 2896g"。
 *
 * ============================================================
 *  三个定死的取舍(改之前先看这里)
 * ============================================================
 *
 * ⚠ 1) version 必须独占 offset 1, 不能塞进位域。
 *   曾试过把 version 压成 4 位与计数器共用 1 字节, 省出的空间多放数据。
 *   问题在于: 那样 offset 1 就变成 device_id 的低字节, 而旧客户端正是在
 *   offset 1 读 version 并校验等于 0x02。65536 个 device_id 里有 256 个
 *   (1/256)低字节恰好是 0x02 —— 这些设备发的新包会被旧客户端【当成合法
 *   v2 包按旧布局静默误读】, 画出垃圾数据且不报错。让 version 独占一个
 *   字节, 旧客户端一律判"版本不符"直接丢弃, 是优雅降级。
 *
 * ⚠ 2) v2 的 counter 字段已删除, 省下的字节还给 version。
 *   counter 的唯一用途是客户端去重(一次单击会触发多达 9 次广播回调)。
 *   last_index 完全能替代: 同一轮的 9 个空中包 last_index 相同 → 去重成立;
 *   下一次单击必然新存一条记录 → last_index 必变 → 不会误去重。
 *
 * ⚠ 3) Flags 段(3 字节)保留, 不为了多放数据去掉它。
 *   ble_advdata.c 是 flags != 0 才编码, 把 ble_beacon.c 里的 advdata.flags
 *   设成 0 就能白拿 3 字节, 容量从 24 变 27。但那只多【1 条】记录(8→9),
 *   而代价是部分扫描端(iOS CoreBluetooth 高层 API、某些 Android ROM 的
 *   startScan 过滤器)会忽略不含 Flags 的广播。为 12.5% 容量下这个不可逆的
 *   兼容性赌注不值。
 *
 * ⚠ 为什么装不了更多: S112 6.0.0 的 BLE_GAP_ADV_SET_DATA_SIZE_MAX 是 31,
 *   【不支持】BLE5 扩展广播(255 字节), 所以没有"扩容"这条路。扫描响应是
 *   第二个 31 字节通道, 但要把广播类型改成 ..._SCANNABLE, 而"不可扫描"是
 *   ble_beacon.h 写明的需求, 故不用。
 *   24 = 31 - Flags(3) - 厂商段头(4); 头 8 字节 + 8 条 × 2 字节 = 24, 零浪费。
 */

#ifndef APP_PROTO_H__
#define APP_PROTO_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 标识 ---------- */

#define APP_PROTO_MAGIC_V3          0xAB    /* 与 v2 相同 —— 靠 version 区分 */
#define APP_PROTO_VERSION_V3        0x03

/* ---------- 字段偏移 ---------- */

#define APP_PROTO_OFF_MAGIC         0
#define APP_PROTO_OFF_VERSION       1
#define APP_PROTO_OFF_DEVICE_ID     2
#define APP_PROTO_DEVICE_ID_LEN     2
#define APP_PROTO_OFF_BATTERY       4
#define APP_PROTO_OFF_LAST_INDEX    5
#define APP_PROTO_OFF_DRINK_N       7   /* drink_idx(高4) | rec_count(低4) */
#define APP_PROTO_OFF_RECORDS       8

#define APP_PROTO_HDR_LEN           APP_PROTO_OFF_RECORDS   /* 8 */
#define APP_PROTO_REC_BYTES         2

/* 一包最多带几条。24 字节的段减去 8 字节头, 每条 2 字节 → 正好 8 条。 */
#define APP_PROTO_REC_MAX           8
#define APP_PROTO_PKT_LEN           (APP_PROTO_HDR_LEN + \
                                     APP_PROTO_REC_MAX * APP_PROTO_REC_BYTES)

/* ---------- 饮料类型 ---------- */

/* 类型 idx 占 4 位 → 最多 16 种。配置区的 drink_count 不得超过这个值。 */
#define APP_PROTO_DRINK_MAX         16

/* ---------- 数据状态标志(flags) ---------- */

/* 一次测量的数据状态。存进 app_storage_rec_t.flags(16 位, 原为预留), 也可随
 * 广播上报, 让上层知道"这条数据可不可信"而不是照单全收。
 *
 * ⚠ 为什么需要它: 物体刚放上秤时有明显的机械震荡(实测衰减到 1g 以内约需
 *   2~3 秒)。震荡期间的读数是真实采到的、但【不可信】—— 直接拿去算重量会得
 *   到一个偏差几十克的值。稳定判定就是用来越过这一段的。
 *
 * ⚠ 三个状态是互斥的语义, 但用独立的位而不是枚举: flags 还要容纳后续别的
 *   标志位(如超量程、电量低), 位域比"低 2 位是枚举"更好扩展。 */
#define APP_PROTO_FLAG_STABLE       (1u << 0)   /* 数据已稳定, 可信 */
#define APP_PROTO_FLAG_SETTLING     (1u << 1)   /* 正在震荡/未稳定 */
#define APP_PROTO_FLAG_STALE        (1u << 2)   /* 数据过期(窗口未更新), 不可判定 */

/* 未标定(cal_k 全 0)时算出的重量恒为 0, 标上此位以区分"真的是 0 克"。 */
#define APP_PROTO_FLAG_UNCAL        (1u << 3)

/* ---------- 饮水量编码 ---------- */

/* 12 位有符号, 单位 1 g。
 *
 * 为什么 1 g 够: 固件端算重量的额外算术误差约 0.0003 g(float32), 而传感器
 * 噪声 ±30 counts 已折合约 0.08 g —— 物理误差底线比量化步长小一个数量级还多,
 * 再细的分辨率是在量化噪声。杯装饮水量典型 50~500 g, ±2048 g 的量程足够。 */
#define APP_PROTO_GRAMS_MAX         2047
#define APP_PROTO_GRAMS_MIN         (-2048)

/* ---------- 电池电压编码 ---------- */

/* 2000..3600 mV 线性压成 1 字节(v2 用的是 2 字节原始 mV)。
 * 实测最大量化误差 3.1 mV, 对电量显示无影响, 省下的 1 字节相当于半条记录。 */
#define APP_PROTO_BATT_MV_MIN       2000
#define APP_PROTO_BATT_MV_MAX       3600

/**@brief 电池 mV → 1 字节。超出量程按端点饱和。 */
static inline uint8_t app_proto_batt_enc(uint16_t mv)
{
    if (mv <= APP_PROTO_BATT_MV_MIN)
    {
        return 0;
    }
    if (mv >= APP_PROTO_BATT_MV_MAX)
    {
        return 255;
    }
    /* +半个步长做四舍五入, 全整数运算(固件不为这点事拖进软浮点)。 */
    uint32_t span = APP_PROTO_BATT_MV_MAX - APP_PROTO_BATT_MV_MIN;
    return (uint8_t)((((uint32_t)mv - APP_PROTO_BATT_MV_MIN) * 255U
                      + span / 2U) / span);
}

/**@brief 1 字节 → 电池 mV(解码侧对称实现, 供自测与网关参考)。 */
static inline uint16_t app_proto_batt_dec(uint8_t b)
{
    uint32_t span = APP_PROTO_BATT_MV_MAX - APP_PROTO_BATT_MV_MIN;
    return (uint16_t)(APP_PROTO_BATT_MV_MIN + ((uint32_t)b * span + 127U) / 255U);
}

/* ---------- 记录编解码 ---------- */

/**@brief 打包一条记录: 类型 + 饮水量(g) → 小端 uint16。
 *
 * 超出 12 位量程时按端点【饱和】而不是回绕 —— 回绕会把 +2048 g 变成
 * -2048 g(喝水变加水), 饱和只是数值封顶, 语义不会反。
 */
static inline uint16_t app_proto_rec_pack(uint8_t drink_idx, int32_t grams)
{
    if (grams > APP_PROTO_GRAMS_MAX)
    {
        grams = APP_PROTO_GRAMS_MAX;
    }
    else if (grams < APP_PROTO_GRAMS_MIN)
    {
        grams = APP_PROTO_GRAMS_MIN;
    }
    return (uint16_t)(((uint16_t)(drink_idx & 0x0F) << 12)
                      | ((uint16_t)grams & 0x0FFF));
}

/**@brief 从记录里取饮料类型。 */
static inline uint8_t app_proto_rec_drink(uint16_t v)
{
    return (uint8_t)((v >> 12) & 0x0F);
}

/**@brief 从记录里取饮水量(g), 已做 12 位符号扩展。 */
static inline int16_t app_proto_rec_grams(uint16_t v)
{
    uint16_t u = (uint16_t)(v & 0x0FFF);
    return (int16_t)((u & 0x0800U) ? ((int32_t)u - 4096) : (int32_t)u);
}

#ifdef __cplusplus
}
#endif

#endif /* APP_PROTO_H__ */

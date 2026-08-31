/**
 * app_storage —— flash 尾部的配置区与测量数据区
 * ------------------------------------------------------------------
 * 职责: 在 App flash 的【尾部】划出两块区域, 一块存系统配置(单条, 可覆盖),
 *       一块存测量记录(顺序追加, 满了回卷覆盖最旧的)。
 *
 * 为什么不用 FDS: FDS 实测要 6423 字节 flash + 510 字节 RAM, 换来的是垃圾
 *   回收与记录级更新 —— 而本模块的需求是"一条配置整体覆盖"+"定长记录顺序
 *   追加", 用不到那些。直接用 fstorage 裸写省下这 6KB 多。
 *
 * ⚠ 为什么必须走 fstorage 而不是直接写 NVMC 寄存器:
 *   SoftDevice 使能时, 无线活动期间 CPU 对 flash 的写/擦会被拒绝或破坏时序,
 *   必须通过 sd_flash_write()/sd_flash_page_erase() 让 SD 在空闲窗口里执行,
 *   并等它回 NRF_EVT_FLASH_OPERATION_SUCCESS/ERROR。nrf_fstorage_sd 后端
 *   正是把这套(请求 + 排队 + 等 SOC 事件 + 重试)封装好的, 所以用它。
 *
 * ⚠ 异步: fstorage 的 write/erase 是【异步】的, 返回 NRF_SUCCESS 只代表
 *   "已排入队列"。本模块的写接口内部会忙等到完成(见 wait_done), 因此
 *   【不可在中断上下文调用】—— 忙等里要跑 sd_app_evt_wait(), 中断里等不到。
 *   单击/长按的按键回调是中断上下文, 要存数据得先转到主循环再调本模块。
 *
 * ============================================================
 *  分区布局(与 .uvprojx 的 IROM 设置必须一致!)
 * ============================================================
 *
 *   0x19000 .. 0x2C000   代码区   76 KB  (.uvprojx IROM Size = 0x13000)
 *   0x2C000 .. 0x2D000   配置区    4 KB  = 1 页
 *   0x2D000 .. 0x30000   数据区   12 KB  = 3 页
 *
 * ⚠ 必须同步把 .uvprojx 的 IROM Size 从 0x17000 改成 0x13000, 否则链接器
 *   会把代码放进这 16KB 里, 一擦就把自己的代码擦掉。本文件有编译期断言
 *   兜住地址关系, 但【断言查不出 IROM Size 设错】—— 那是工程设置, 编译器
 *   看不见。改动分区时两处都要改。
 *
 * flash 擦写寿命: 每页 10000 次。配置区只在改配置时擦(极少), 数据区按
 *   "写满一页才擦下一页"的方式回卷 —— 每页承担 1/3 的擦次数。每页 204 条、
 *   3 页共 612 条为一轮, 擦 10000 次相当于约 612 万条记录。按每次按键存 1 条
 *   算, 寿命不是约束。
 */

#ifndef APP_STORAGE_H__
#define APP_STORAGE_H__

#include <stdbool.h>
#include <stdint.h>
#include "sdk_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 分区地址(改这里也要改 .uvprojx 的 IROM Size) ---------- */

#define APP_STORAGE_PAGE_SIZE       4096            /* nRF52810 flash 页大小 */

#define APP_STORAGE_CFG_ADDR        0x0002C000UL    /* 配置区起始 */
#define APP_STORAGE_CFG_PAGES       1
#define APP_STORAGE_CFG_END         (APP_STORAGE_CFG_ADDR + \
                                     APP_STORAGE_CFG_PAGES * APP_STORAGE_PAGE_SIZE)

#define APP_STORAGE_REC_ADDR        APP_STORAGE_CFG_END     /* 数据区起始 0x2D000 */
#define APP_STORAGE_REC_PAGES       3
#define APP_STORAGE_REC_END         (APP_STORAGE_REC_ADDR + \
                                     APP_STORAGE_REC_PAGES * APP_STORAGE_PAGE_SIZE)

/* ---------- 配置数据 ---------- */

/* 系统配置。
 *
 * ⚠ 加字段【只能往尾部加】并递增 version: 旧设备升级后读到的是旧 version,
 *   app_storage_cfg_load() 会发现 version 不匹配而回退到默认值 —— 这是
 *   有意的保守行为(宁可用默认值, 不要把旧字节按新布局误读)。
 *
 * ⚠ 大小必须是 4 的倍数: fstorage 要求写入长度 4 字节对齐。末尾的
 *   STATIC_ASSERT 兜住这一点。
 *
 * ============================================================
 *  v2 相对 v1 的变化: 标定模型换了, 不是加字段
 * ============================================================
 *
 * v1 有 cal_scale_q16[3], 语义是"每通道各自一个增益"。但 desktop 端
 * (index.html 的 calSolve/最小二乘)实际用的是【三通道联合】求一个重量:
 *
 *     W = k0·(x0 - z0) + k1·(x1 - z1) + k2·(x2 - z2)
 *
 * 这两件事语义不兼容 —— 联合模型里单个 k 不是"该通道的增益", 而是最小二乘
 * 解出的权重, 实测里甚至【有一个是负数】(真实标定数据 k2 = -2.555e-03)。
 * 所以是替换 cal_scale_q16 而不是在尾部加字段, version 因此从 1 跳到 2。
 *
 * ⚠ 为什么 cal_k 用 float 而不是定点:
 *   实测过四种方案的额外算术误差(物理误差底线约 0.08 g, 由 ±30 counts 噪声
 *   与拟合残差决定):
 *       float32  0.0001~0.0003 g   +964 B flash(软浮点, 当前工程净新增)
 *       double   更小, 但已在噪声以下   +1560 B
 *       Q30 定点 0.0002~0.002 g     +0 B
 *       Q20 定点 4.89 g             +0 B   ← k 悬殊时崩掉, 不可用
 *   float32 与 double 在真实标定数据上只差 0.000002 g, 且 int32→float 对
 *   ±8388607 是精确的(float 24 位有效位刚好覆盖 24 位 ADC 输出, 无丢位)。
 *   需求是"速度可以慢一点、误差尽量小", 964 字节买下"不用推敲定点定标"的
 *   确定性, 划算。 */
#define APP_STORAGE_CFG_VERSION     2

/* 饮料类型数量上限。与广播包的 4 位类型字段对应(app_proto.h), 两边必须一致 ——
 * 配置里存了 17 种而广播只能表达 16 种会静默截断。 */
#define APP_STORAGE_DRINK_MAX       16

typedef struct
{
    uint32_t magic;             /* CFG_MAGIC, 用于识别"这页有有效配置" */
    uint16_t version;           /* APP_STORAGE_CFG_VERSION */
    uint16_t adv_events;        /* 一轮广播事件数(覆盖 BLE_BEACON_ADV_EVENTS) */
    uint16_t adv_interval_ms;   /* 广播间隔 ms(覆盖 BLE_BEACON_ADV_INTERVAL_MS) */

    uint8_t  drink_count;       /* 已启用的饮料类型数, 1..APP_STORAGE_DRINK_MAX */
    uint8_t  drink_idx;         /* 当前选中的类型, 必须 < drink_count */

    int32_t  cal_zero[3];       /* 三通道零点 z[], 标定时的空杯读数 */
    float    cal_k[3];          /* 联合标定系数 k[], 见上方模型说明 */
} app_storage_cfg_t;

/* ---------- 测量记录 ---------- */

/* 一条测量记录。
 *
 * ⚠ 4 字节的 seq 既是序号也是"这一格写过没有"的标志: 擦除后的 flash 全 1,
 *   所以 seq == 0xFFFFFFFF 表示空格。写入时 seq 从 0 开始递增, 因此
 *   "找写入位置"就是"找第一个 seq 为全 1 的格子", 不需要额外的元数据页。
 *
 * ⚠ 大小必须是 4 的倍数(fstorage 对齐要求)。当前 20 字节。
 *
 * flags 存的是采样那一刻的数据状态(APP_PROTO_FLAG_*, 见 app_proto.h) ——
 * 物体刚放上秤时有 2~3 秒的机械震荡期, 那期间的读数是真采到的但不可信。
 * 记录【不】因为不稳定就拒绝写入(用户按了键就该有记录), 而是靠这个字段自证,
 * 把判断权交给读数据的一方。
 *
 * 20 不能整除 4096 —— 所以槽位是【按页寻址】的(见 slot_addr), 每页放
 * APP_STORAGE_RECS_PER_PAGE 条, 尾部剩的 16 字节空着不用。这样记录永远不
 * 跨页边界, 回卷擦页时不会把一条记录擦掉一半。代价是每页浪费 0.4%, 换来的
 * 是 ch[] 保持 int32_t 的自然访问(不必打包成 24bit 再解包)。 */
typedef struct
{
    uint32_t seq;               /* 记录序号, 0xFFFFFFFF = 空格(未写过) */
    uint16_t batt_mv;           /* 电池电压 mV */
    uint16_t flags;             /* 数据状态, APP_PROTO_FLAG_* 的组合 */
    int32_t  ch[3];             /* 三通道值(已符号扩展的 24bit) */
} app_storage_rec_t;

/* 每页放多少条(向下取整, 尾部余量空着) —— 槽位寻址与回卷都按这个走。 */
#define APP_STORAGE_RECS_PER_PAGE   (APP_STORAGE_PAGE_SIZE / sizeof(app_storage_rec_t))

/* 数据区能放多少条 —— 供上层判断容量与做统计。 */
#define APP_STORAGE_REC_CAPACITY    (APP_STORAGE_REC_PAGES * APP_STORAGE_RECS_PER_PAGE)

/* ---------- 接口 ---------- */

/**@brief 初始化 fstorage 并扫描数据区, 定位下一个写入位置。
 *
 * ⚠ 必须在 SoftDevice 使能【之后】调用: nrf_fstorage_sd 要向 SDH 注册
 *   SOC 事件观察者, 且初始化时会读 SD 的使能状态。
 *
 * @retval NRF_SUCCESS 成功(即使数据区为空也算成功)。
 */
ret_code_t app_storage_init(void);

/**@brief 读配置。若 flash 里没有有效配置(magic/version 不匹配), 用默认值
 *        填充 p_cfg 并返回 NRF_ERROR_NOT_FOUND —— 上层通常可以忽略这个返回,
 *        因为 p_cfg 一定是可用的。
 *
 * @param[out] p_cfg  接收配置。
 */
ret_code_t app_storage_cfg_load(app_storage_cfg_t * p_cfg);

/**@brief 写配置(整页擦除后重写)。内部忙等到完成。
 *
 * ⚠ 不可在中断上下文调用(见文件头)。
 * ⚠ 每次调用都会擦一次配置页 —— 不要在循环里频繁调, 页寿命 10000 次。
 *
 * 会先校验 drink_count/drink_idx 的取值范围, 越界返回 NRF_ERROR_INVALID_PARAM
 * 且【不写 flash】—— 宁可拒绝, 不要把非法配置落盘后每次开机都读出来。
 *
 * @param[in] p_cfg  要写入的配置。magic/version 由本函数填, 调用者不必管。
 */
ret_code_t app_storage_cfg_save(const app_storage_cfg_t * p_cfg);

/**@brief 用配置里的标定参数把三通道原始计数换算成重量(克)。
 *
 *     W = k0·(x0 - z0) + k1·(x1 - z1) + k2·(x2 - z2)
 *
 * 与 desktop 端最小二乘解出的模型一致(见 app_storage_cfg_t 的说明)。
 *
 * ⚠ 这是本工程【唯一】用到浮点的地方。nRF52810 没有 FPU, 走软浮点,
 *   一次调用约几百个周期 —— 相对每次按键才算一次的频率完全可忽略。
 *
 * @param[in] p_cfg  配置(取 cal_zero/cal_k)。
 * @param[in] p_ch   三通道原始计数, 长度须为 3。
 *
 * @return 重量, 单位克。未标定(k 全 0)时返回 0。
 */
float app_storage_weight_calc(const app_storage_cfg_t * p_cfg, const int32_t * p_ch);

/**@brief 追加一条测量记录。写满整个数据区后回卷覆盖最旧的页。
 *        内部忙等到完成。
 *
 * ⚠ 不可在中断上下文调用(见文件头)。
 *
 * @param[in] batt_mv    电池电压 mV。
 * @param[in] p_ch       三通道值, 长度须为 3。
 * @param[in] flags      数据状态标志(APP_PROTO_FLAG_* 的组合)。调用方【必须】
 *                       如实填写采样那一刻的稳定状态 —— 存进去的不可信数据
 *                       靠这个字段自证, 读的一方才有判断依据。
 */
ret_code_t app_storage_rec_append(uint16_t batt_mv, const int32_t * p_ch,
                                  uint16_t flags);

/**@brief 已写入的记录条数(0..APP_STORAGE_REC_CAPACITY)。回卷后恒为容量值。 */
uint32_t app_storage_rec_count(void);

/**@brief 按"从新到旧"的顺序读第 idx 条记录(idx=0 是最新的一条)。
 *
 * @param[in]  idx    0 = 最新。
 * @param[out] p_rec  接收记录。
 *
 * @retval NRF_ERROR_NOT_FOUND  idx 超出已写入的条数。
 */
ret_code_t app_storage_rec_get(uint32_t idx, app_storage_rec_t * p_rec);

/**@brief 擦除全部测量记录(数据区所有页)。内部忙等到完成。
 *
 * ⚠ 不可在中断上下文调用。会擦 APP_STORAGE_REC_PAGES 次。
 */
ret_code_t app_storage_rec_erase_all(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STORAGE_H__ */

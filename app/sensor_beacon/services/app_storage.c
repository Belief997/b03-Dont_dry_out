/**
 * app_storage —— flash 尾部配置区与测量数据区的实现
 * ------------------------------------------------------------------
 * 设计要点见 app_storage.h 的文件头。这里只补实现层面的取舍。
 *
 * 【为什么写接口是忙等而不是回调】
 *   fstorage 的 write/erase 是异步的。本模块的调用点(存一条测量数据、保存
 *   配置)都在主循环里且不赶时间, 忙等换来的是"函数返回即已落盘"这个简单
 *   契约 —— 不用给每个调用点配回调和状态机。代价是不能在中断里调, 已在
 *   头文件里写明。
 *
 * 【为什么用 seq==0xFFFFFFFF 判断空格】
 *   擦除后的 flash 是全 1。所以"没写过"天然等于 0xFFFFFFFF, 不需要单独的
 *   位图或元数据页。写入的 seq 从 0 开始递增, 永远不会等于 0xFFFFFFFF
 *   (要 42 亿条才回绕, 而容量只有 612 条)。
 *
 * 【回卷策略: 整页擦, 不是逐条擦】
 *   flash 不能逐条擦(最小擦除单位是页)。所以写满一页就擦下一页再写 ——
 *   这会丢掉那一整页最旧的记录, 是有意的: 换来的是每页只承担 1/3 的擦写
 *   次数, 以及不需要"搬移存活记录"的垃圾回收逻辑。
 */

#include "app_storage.h"

#include <string.h>

#include "app_proto.h"
#include "nrf_fstorage.h"
#include "nrf_fstorage_sd.h"
#include "nrf_soc.h"
#include "nrf_sdh.h"
#include "app_util.h"
#include "app_error.h"

#define NRF_LOG_MODULE_NAME app_storage
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

/* ---------- 编译期检查 ---------- */

/* fstorage 要求写入长度与地址 4 字节对齐。 */
STATIC_ASSERT((sizeof(app_storage_cfg_t) % 4) == 0);
STATIC_ASSERT((sizeof(app_storage_rec_t) % 4) == 0);

/* 分区必须页对齐, 且配置区紧邻数据区。 */
STATIC_ASSERT((APP_STORAGE_CFG_ADDR % APP_STORAGE_PAGE_SIZE) == 0);
STATIC_ASSERT((APP_STORAGE_REC_ADDR % APP_STORAGE_PAGE_SIZE) == 0);
STATIC_ASSERT(APP_STORAGE_REC_ADDR == APP_STORAGE_CFG_END);

/* 配置结构体要能装进一页。 */
STATIC_ASSERT(sizeof(app_storage_cfg_t) <= APP_STORAGE_PAGE_SIZE);

/* 饮料类型上限必须与广播包的 4 位类型字段一致, 否则配置里存得下的类型广播
 * 表达不了, 且是静默截断。 */
STATIC_ASSERT(APP_STORAGE_DRINK_MAX == APP_PROTO_DRINK_MAX);

/* cal_k 必须是 32 位 float —— 存进 flash 的是它的字节表示, 若某个工具链把
 * float 当成 double(8 字节), 布局会和其他工具链编出来的固件不兼容。 */
STATIC_ASSERT(sizeof(float) == 4);

/* 记录不能大于一页 —— 否则一条记录跨页, 回卷时会被擦一半。
 * 注意: 记录【不必】整除页大小, 槽位是按页寻址的(见 slot_addr), 每页尾部
 * 剩下的不足一条的空间空着不用。 */
STATIC_ASSERT(sizeof(app_storage_rec_t) <= APP_STORAGE_PAGE_SIZE);

#define CFG_MAGIC           0x53544F52UL    /* 'STOR' */
#define REC_EMPTY_SEQ       0xFFFFFFFFUL    /* 擦除后的 flash = 全 1 */

#define RECS_PER_PAGE       APP_STORAGE_RECS_PER_PAGE

/* ---------- fstorage 实例 ---------- */

static void fs_evt_handler(nrf_fstorage_evt_t * p_evt);

/* ⚠ 这个实例通过 NRF_FSTORAGE_DEF 注册进 fs_data 段, 由 nrf_fstorage_init
 *   遍历。start_addr/end_addr 之外的地址一律被 fstorage 拒绝, 这正是我们
 *   想要的保护: 写错地址会返回 NRF_ERROR_INVALID_ADDR 而不是擦掉代码。 */
NRF_FSTORAGE_DEF(static nrf_fstorage_t m_fs) =
{
    .evt_handler = fs_evt_handler,
    .start_addr  = APP_STORAGE_CFG_ADDR,
    .end_addr    = APP_STORAGE_REC_END - 1,
};

static bool              m_inited     = false;
static volatile bool     m_op_done    = false;   /* 当前异步操作已完成 */
static volatile uint32_t m_op_result  = NRF_SUCCESS;

/* 下一条记录要写到哪 —— init 时扫描得出, 之后自己维护。 */
static uint32_t m_next_slot  = 0;                /* 槽位下标 0..CAPACITY-1 */
static uint32_t m_next_seq   = 0;                /* 下一条记录的序号 */
static uint32_t m_rec_count  = 0;                /* 已写入条数 */

static void fs_evt_handler(nrf_fstorage_evt_t * p_evt)
{
    m_op_result = p_evt->result;
    m_op_done   = true;
}

/* 忙等到当前 fstorage 操作完成。
 *
 * ⚠ sd_app_evt_wait() 会睡到有事件才醒 —— flash 操作完成的 SOC 事件正是
 *   唤醒源之一, 所以这里不会睡死。但在中断上下文里 SOC 事件派发不到,
 *   会真的卡住, 故本模块的写接口都标了"不可在中断里调"。 */
static ret_code_t wait_done(void)
{
    while (!m_op_done)
    {
        (void)sd_app_evt_wait();
    }
    return (ret_code_t)m_op_result;
}

static ret_code_t fs_erase_sync(uint32_t page_addr, uint32_t pages)
{
    m_op_done = false;
    ret_code_t err = nrf_fstorage_erase(&m_fs, page_addr, pages, NULL);
    if (err != NRF_SUCCESS)
    {
        return err;
    }
    return wait_done();
}

static ret_code_t fs_write_sync(uint32_t dest, const void * p_src, uint32_t len)
{
    m_op_done = false;
    ret_code_t err = nrf_fstorage_write(&m_fs, dest, p_src, len, NULL);
    if (err != NRF_SUCCESS)
    {
        return err;
    }
    return wait_done();
}

/* 槽位 → flash 地址。
 *
 * ⚠ 按页寻址, 不是简单的 REC_ADDR + slot * sizeof(rec): 20 字节的记录不能
 *   整除 4096, 线性寻址会让某些记录跨页边界, 回卷擦页时被擦掉一半。这里
 *   先定位到页, 再在页内偏移, 每页尾部不足一条的空间空着。 */
static uint32_t slot_addr(uint32_t slot)
{
    uint32_t page   = slot / RECS_PER_PAGE;
    uint32_t in_pg  = slot % RECS_PER_PAGE;
    return APP_STORAGE_REC_ADDR
         + page * APP_STORAGE_PAGE_SIZE
         + in_pg * sizeof(app_storage_rec_t);
}

/* 扫描数据区, 找出下一个空槽与最大序号。
 *
 * 记录是顺序写的, 但回卷之后"最旧"不在槽 0 —— 所以不能只找第一个空槽,
 * 还要顺带记住最大 seq, 让新记录的序号继续递增(读取时靠 seq 排序)。 */
static void scan_records(void)
{
    m_next_slot = 0;
    m_next_seq  = 0;
    m_rec_count = 0;

    uint32_t max_seq       = 0;
    bool     found_any     = false;
    uint32_t max_seq_slot  = 0;

    for (uint32_t i = 0; i < APP_STORAGE_REC_CAPACITY; i++)
    {
        app_storage_rec_t rec;
        if (nrf_fstorage_read(&m_fs, slot_addr(i), &rec, sizeof(rec)) != NRF_SUCCESS)
        {
            break;
        }

        if (rec.seq == REC_EMPTY_SEQ)
        {
            continue;               /* 空槽 */
        }

        m_rec_count++;
        if (!found_any || (rec.seq >= max_seq))
        {
            max_seq      = rec.seq;
            max_seq_slot = i;
            found_any    = true;
        }
    }

    if (found_any)
    {
        m_next_seq  = max_seq + 1;
        /* 下一个槽是"最大序号那条"的后一个(可能回卷到 0) */
        m_next_slot = (max_seq_slot + 1) % APP_STORAGE_REC_CAPACITY;
    }

    NRF_LOG_INFO("Storage: %u/%u records, next slot %u, next seq %u.",
                 m_rec_count, (uint32_t)APP_STORAGE_REC_CAPACITY,
                 m_next_slot, m_next_seq);
}

ret_code_t app_storage_init(void)
{
    if (m_inited)
    {
        return NRF_SUCCESS;
    }

    /* ⚠ 必须在 SD 使能后调用 —— nrf_fstorage_sd 要注册 SOC 观察者。 */
    ret_code_t err = nrf_fstorage_init(&m_fs, &nrf_fstorage_sd, NULL);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("fstorage init failed (0x%08x)", err);
        return err;
    }

    m_inited = true;
    scan_records();

    NRF_LOG_INFO("Storage ready: cfg @0x%05x (1 page), rec @0x%05x (%u pages, %u recs).",
                 APP_STORAGE_CFG_ADDR, APP_STORAGE_REC_ADDR,
                 APP_STORAGE_REC_PAGES, (uint32_t)APP_STORAGE_REC_CAPACITY);
    return NRF_SUCCESS;
}

/* ---------- 配置 ---------- */

static void cfg_set_defaults(app_storage_cfg_t * p_cfg)
{
    memset(p_cfg, 0, sizeof(*p_cfg));
    p_cfg->magic           = CFG_MAGIC;
    p_cfg->version         = APP_STORAGE_CFG_VERSION;
    /* 默认值与 ble_beacon.h 的编译期默认保持一致 —— 配置区为空时行为不变。 */
    p_cfg->adv_events      = 15;
    p_cfg->adv_interval_ms = 100;

    /* 饮料类型: 默认 2 种, 当前选第 0 种。 */
    p_cfg->drink_count     = 2;
    p_cfg->drink_idx       = 0;

    /* 标定参数默认全 0 —— 即"未标定"。
     *
     * ⚠ 这里【故意】不给 k 填 1.0 之类的"看起来能用"的值: k 全 0 时
     *   weight_calc 返回 0, 上层能一眼看出"没标定过", 而填了假系数只会
     *   算出一个似是而非的重量, 更难发现问题。 */
    for (uint8_t i = 0; i < 3; i++)
    {
        p_cfg->cal_zero[i] = 0;
        p_cfg->cal_k[i]    = 0.0f;
    }
}

/* 校验配置的取值范围。写入前调用 —— 非法配置一旦落盘, 每次开机都会读出来。 */
static bool cfg_is_valid(const app_storage_cfg_t * p_cfg)
{
    if ((p_cfg->drink_count < 1) || (p_cfg->drink_count > APP_STORAGE_DRINK_MAX))
    {
        NRF_LOG_ERROR("cfg: drink_count %u out of range 1..%u",
                      p_cfg->drink_count, APP_STORAGE_DRINK_MAX);
        return false;
    }
    if (p_cfg->drink_idx >= p_cfg->drink_count)
    {
        NRF_LOG_ERROR("cfg: drink_idx %u >= drink_count %u",
                      p_cfg->drink_idx, p_cfg->drink_count);
        return false;
    }
    if (p_cfg->adv_events == 0)
    {
        NRF_LOG_ERROR("cfg: adv_events must be >= 1");
        return false;
    }
    /* 广播间隔的协议下限是 20ms(BLE_GAP_ADV_INTERVAL_MIN), 上限约 10.24s。 */
    if ((p_cfg->adv_interval_ms < 20) || (p_cfg->adv_interval_ms > 10240))
    {
        NRF_LOG_ERROR("cfg: adv_interval_ms %u out of range 20..10240",
                      p_cfg->adv_interval_ms);
        return false;
    }
    return true;
}

ret_code_t app_storage_cfg_load(app_storage_cfg_t * p_cfg)
{
    if (p_cfg == NULL)
    {
        return NRF_ERROR_NULL;
    }
    if (!m_inited)
    {
        cfg_set_defaults(p_cfg);
        return NRF_ERROR_INVALID_STATE;
    }

    app_storage_cfg_t onflash;
    ret_code_t err = nrf_fstorage_read(&m_fs, APP_STORAGE_CFG_ADDR,
                                       &onflash, sizeof(onflash));
    if (err != NRF_SUCCESS)
    {
        cfg_set_defaults(p_cfg);
        return err;
    }

    /* magic 不对 = 从未写过(全 1)或被擦过; version 不对 = 布局变了。
     * 两种情况都回退到默认值 —— 宁可用默认值, 不要按新布局误读旧字节。 */
    if ((onflash.magic != CFG_MAGIC) || (onflash.version != APP_STORAGE_CFG_VERSION))
    {
        NRF_LOG_INFO("No valid config on flash (magic=0x%08x ver=%u); using defaults.",
                     onflash.magic, onflash.version);
        cfg_set_defaults(p_cfg);
        return NRF_ERROR_NOT_FOUND;
    }

    /* magic/version 对了也不代表内容可信 —— 可能是写一半掉电, 或早期版本写进
     * 去的越界值。校验不过就退回默认值, 而不是把非法配置交给上层。 */
    if (!cfg_is_valid(&onflash))
    {
        NRF_LOG_WARNING("Config on flash failed validation; using defaults.");
        cfg_set_defaults(p_cfg);
        return NRF_ERROR_INVALID_DATA;
    }

    *p_cfg = onflash;

    /* ⚠ 不打印 cal_k: NRF_LOG 的格式化不支持 %f(浮点要开 NRF_LOG_USES_..., 会
     *   拖进更多代码)。要看系数就读回配置比较, 别为了日志引入浮点格式化。 */
    NRF_LOG_INFO("Config loaded: adv %u evts @ %ums, %u drinks (idx %u).",
                 p_cfg->adv_events, p_cfg->adv_interval_ms,
                 p_cfg->drink_count, p_cfg->drink_idx);
    NRF_LOG_INFO("  cal zero: %d / %d / %d",
                 p_cfg->cal_zero[0], p_cfg->cal_zero[1], p_cfg->cal_zero[2]);
    return NRF_SUCCESS;
}

ret_code_t app_storage_cfg_save(const app_storage_cfg_t * p_cfg)
{
    if (p_cfg == NULL)
    {
        return NRF_ERROR_NULL;
    }
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    /* 先校验再擦 —— 顺序很重要: 反了的话非法配置会先把好配置擦掉再被拒,
     * 结果是配置区变空。 */
    if (!cfg_is_valid(p_cfg))
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    /* ⚠ 必须整页擦后重写: flash 只能把 1 写成 0, 原地覆盖会得到两份数据的
     *   按位与。配置项极少改动, 每次擦一页在 10000 次寿命下无压力。 */
    ret_code_t err = fs_erase_sync(APP_STORAGE_CFG_ADDR, APP_STORAGE_CFG_PAGES);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("cfg erase failed (0x%08x)", err);
        return err;
    }

    /* 本地副本: 保证 magic/version 正确, 且源地址 4 字节对齐(fstorage 要求)。 */
    app_storage_cfg_t tmp = *p_cfg;
    tmp.magic   = CFG_MAGIC;
    tmp.version = APP_STORAGE_CFG_VERSION;

    err = fs_write_sync(APP_STORAGE_CFG_ADDR, &tmp, sizeof(tmp));
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("cfg write failed (0x%08x)", err);
        return err;
    }

    NRF_LOG_INFO("Config saved.");
    return NRF_SUCCESS;
}

float app_storage_weight_calc(const app_storage_cfg_t * p_cfg, const int32_t * p_ch)
{
    if ((p_cfg == NULL) || (p_ch == NULL))
    {
        return 0.0f;
    }

    float w = 0.0f;
    for (uint8_t i = 0; i < 3; i++)
    {
        /* ⚠ 减法在【int32 域】做, 不是先各自转 float 再减: 两个大计数
         *   (实测达 ±8 万)相减的结果小得多, 整数域相减是精确的。反过来先转
         *   float 再减虽然对 24bit 范围也无损, 但整数域更直白。 */
        int32_t dx = p_ch[i] - p_cfg->cal_zero[i];
        w += p_cfg->cal_k[i] * (float)dx;
    }
    return w;
}

/* ---------- 测量记录 ---------- */

ret_code_t app_storage_rec_append(uint16_t batt_mv, const int32_t * p_ch,
                                  uint16_t flags)
{
    if (p_ch == NULL)
    {
        return NRF_ERROR_NULL;
    }
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    /* 若下一个槽是某页的第一个槽, 说明要开始写新的一页 —— 先擦掉它。
     * 这会丢掉那一整页里最旧的记录, 是有意的回卷策略(见文件头)。
     *
     * ⚠ 首次写入(数据区本来就是全 1)时这里也会擦一次。擦全 1 的页是安全的,
     *   代价只是一次擦除耗时, 换来"不需要区分首次/回卷"的简单逻辑。 */
    if ((m_next_slot % RECS_PER_PAGE) == 0)
    {
        /* 槽位是页内第一条 → slot_addr 正好是页首地址。 */
        uint32_t page = slot_addr(m_next_slot);

        ret_code_t err = fs_erase_sync(page, 1);
        if (err != NRF_SUCCESS)
        {
            NRF_LOG_ERROR("rec page erase failed (0x%08x)", err);
            return err;
        }

        /* 擦掉了一整页的记录, 已写入条数要相应减少 —— 但首轮(还没写满)时
         * 这一页本来就是空的, 不能减成负数。 */
        if (m_rec_count >= RECS_PER_PAGE)
        {
            m_rec_count -= RECS_PER_PAGE;
        }
    }

    app_storage_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.seq     = m_next_seq;
    rec.batt_mv = batt_mv;
    rec.flags   = flags;
    rec.ch[0]   = p_ch[0];
    rec.ch[1]   = p_ch[1];
    rec.ch[2]   = p_ch[2];

    ret_code_t err = fs_write_sync(slot_addr(m_next_slot), &rec, sizeof(rec));
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("rec write failed (0x%08x)", err);
        return err;
    }

    NRF_LOG_INFO("Record #%u saved to slot %u (%u total).",
                 m_next_seq, m_next_slot, m_rec_count + 1);

    m_next_seq++;
    m_next_slot = (m_next_slot + 1) % APP_STORAGE_REC_CAPACITY;
    if (m_rec_count < APP_STORAGE_REC_CAPACITY)
    {
        m_rec_count++;
    }

    return NRF_SUCCESS;
}

uint32_t app_storage_rec_count(void)
{
    return m_rec_count;
}

ret_code_t app_storage_rec_get(uint32_t idx, app_storage_rec_t * p_rec)
{
    if (p_rec == NULL)
    {
        return NRF_ERROR_NULL;
    }
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    if (idx >= m_rec_count)
    {
        return NRF_ERROR_NOT_FOUND;
    }

    /* idx=0 是最新 —— 最新那条在 m_next_slot 的前一个, 往前数 idx 个。
     * 用加法避免无符号下溢。 */
    uint32_t slot = (m_next_slot + APP_STORAGE_REC_CAPACITY - 1 - idx)
                    % APP_STORAGE_REC_CAPACITY;

    ret_code_t err = nrf_fstorage_read(&m_fs, slot_addr(slot), p_rec, sizeof(*p_rec));
    if (err != NRF_SUCCESS)
    {
        return err;
    }

    /* 回卷边界上可能读到刚被擦掉的空槽 —— 对上层报"没有"而不是给出全 1 的
     * 垃圾数据。 */
    if (p_rec->seq == REC_EMPTY_SEQ)
    {
        return NRF_ERROR_NOT_FOUND;
    }
    return NRF_SUCCESS;
}

ret_code_t app_storage_rec_erase_all(void)
{
    if (!m_inited)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    ret_code_t err = fs_erase_sync(APP_STORAGE_REC_ADDR, APP_STORAGE_REC_PAGES);
    if (err != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("rec erase all failed (0x%08x)", err);
        return err;
    }

    m_next_slot = 0;
    m_next_seq  = 0;
    m_rec_count = 0;

    NRF_LOG_INFO("All records erased.");
    return NRF_SUCCESS;
}

//! sensor_beacon BLE 广播扫描与解析
//!
//! 扫描本机蓝牙适配器收到的 BLE 广播, 过滤 Company ID = 0xFFFF 的厂商
//! 自定义段, 按设备协议 v0x02 解析载荷(16 字节, 与 main.c 的
//! MANUF_DATA_LEN / BLE_BEACON_PAYLOAD_LEN 一致):
//!
//!   payload[ 0]      魔数       0xAB
//!   payload[ 1]      版本       0x02
//!   payload[ 2..4 ]  设备 ID    (小端, FICR->DEVICEID 低 16bit)
//!   payload[ 4]      计数器     (单击次数, 8bit)
//!   payload[ 5..7 ]  电池电压   (mV, 小端)
//!   payload[ 7..10]  通道 0     = HX711#1 chA(增益128), 有符号 24bit 小端
//!   payload[10..13]  通道 1     = HX711#1 chB(增益 32), 有符号 24bit 小端
//!   payload[13..16]  通道 2     = HX711#2 chA(增益128), 有符号 24bit 小端
//!
//! 广播行为: 设备平时静默, 单击按键后连播 BLE_BEACON_ADV_EVENTS 个广播事件
//! (每个事件在 3 个信道上各发 1 个空中包), 内容相同。本程序按
//! (设备ID, 计数器) 去重: 每轮完整打印第 1 个包, 其余重复包计数显示。
//!
//! ============================================================
//!  关于"漏收" —— 实测结论(不要凭直觉改, 先看这里)
//! ============================================================
//!
//! 症状: 设备一轮播 BLE_BEACON_ADV_EVENTS=15 个广播事件(理论上限 45 个空中包),
//!       扫描端却常常只收到零星几个, 甚至一个都没有。
//!
//! 用 src/bin/ 下的三个诊断工具实测过, 结论与"直觉上的怀疑"相反:
//!
//!  【已排除】btleplug 事件队列溢出。
//!    队列确实只有 16 格(common/adapter_manager.rs:34 `broadcast::channel(16)`),
//!    且滞后时 BroadcastStream 产出的 Err(Lagged(n)) 会被 `.ok()` 静默丢弃
//!    (同文件 :58) —— 看代码非常像元凶。但 `ab.rs` 的 A/B 实测否掉了它:
//!    两个订阅者同时消费同一事件源, 一个不阻塞、一个复刻旧版的
//!    `peripherals().await` + `properties().await`, 20 秒后
//!    【两端都收到 1191 个事件, 丢失 0%】, 慢端平均每事件只花 0.1ms。
//!    原因是这些调用读的都是本地缓存(DashMap + RwLock), 不是跨进程 WinRT 调用。
//!    → 所以"主循环里别做慢操作"在这里【不是】瓶颈。本版仍然保持主循环轻量,
//!      但那是良好习惯, 不是修复。
//!
//!  【已排除】Windows 合并内容相同的广播。
//!    固件一轮里 15 个包载荷完全相同, 很容易怀疑被系统去重。`dupchk.rs`
//!    实测: 30 秒内 670 个包中有 614 个与前一包【字节完全相同】却仍被逐包上报。
//!    → Windows 不做内容去重, 这条【不成立】。
//!
//!  【真正原因】系统扫描窗口太稀疏, 大部分空中包物理上收不到。
//!    `dupchk.rs` 实测的到达间隔分布(环境里 100+ 个设备, 样本 670 包):
//!        最小 2ms, 中位 1346ms, P90 4377ms, 间隔 <100ms 的仅占 4.7%
//!    即 Windows 的 BLE 侦听器大约【每秒才开一次扫描窗口】。而固件一轮
//!    只有 15 × 100ms = 1.5 秒, 期间只有 1~2 个窗口会打开 ——
//!    45 个空中包里能被采到的本来就只有一两个。
//!    这不是 btleplug 的问题, 也不是本程序的问题: Windows 不暴露
//!    scan window/interval 的设置(btleplug watcher.rs:47 只能设
//!    Active/Passive 模式), 应用层【无法】把窗口调密。
//!
//! 所以扫描端能做的只有"把漏收量化出来", 真正的改善在固件侧:
//!
//!   a) 【最有效】把一轮的时间拉长, 让它横跨更多扫描窗口。
//!      两种做法等价地有效, 选一个:
//!        - 加大 BLE_BEACON_ADV_INTERVAL_MS: 100ms → 300~500ms
//!        - 或加大 BLE_BEACON_ADV_EVENTS: 15 → 40~50
//!      按实测的 ~1.3 秒窗口周期, 一轮至少要持续 4~5 秒才能稳定命中 3~4 次。
//!      代价是功耗与"按一下要等多久才出数"的体验, 需你权衡。
//!
//!   b) 在载荷里加"本轮第几包"的序号(1 字节, 当前载荷 16 字节, 上限 24, 有余量)。
//!      这不改善收包率, 但能让漏收【可诊断】—— 直接知道漏的是第几包。
//!      代价是"一轮里的包完全一样"这条需求要放宽, 需你拍板。
//!
//! 本版在扫描端做的改进:
//!   1) 同时监听 ManufacturerDataAdvertisement / DeviceDiscovered / DeviceUpdated
//!      三种事件。winrtble 每个广播会发两个事件, 但 CoreBluetooth 与 bluez 的
//!      发法不同; 只监听一种在跨平台上会漏。不带数据的事件从缓存属性补取。
//!   2) 先订阅事件再 start_scan。反过来的话中间那段广播会全丢 ——
//!      broadcast 对"当时无订阅者"的消息直接丢弃(adapter_manager.rs:51)。
//!   3) 每轮结束打印实收/理论包数与收包率, 让漏收可量化。

use std::collections::HashMap;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};
use btleplug::api::{BDAddr, Central, CentralEvent, Manager as _, Peripheral as _, ScanFilter};
use btleplug::platform::{Adapter, Manager, PeripheralId};
use tokio_stream::StreamExt;

/* ---------- 协议常量(与固件保持一致) ---------- */
const COMPANY_ID: u16 = 0xFFFF;
const MAGIC: u8 = 0xAB;
const VERSION: u8 = 0x02;
const PAYLOAD_LEN: usize = 16;

/* 一轮广播的默认事件数 —— 与固件 ble_beacon.h 的 BLE_BEACON_ADV_EVENTS 对齐。
 * 只用于计算收包率(一个广播事件在 3 个信道各发一包 → 理论上限 ×3)。 */
const DEFAULT_ADV_EVENTS: u32 = 15;
const CHANNELS_PER_EVENT: u32 = 3;

/* 同一 (设备ID, 计数器) 视为同一轮的时间窗。
 * 固件一轮 = ADV_EVENTS × INTERVAL = 15 × 100ms = 1.5s, 留足余量。 */
const ROUND_WINDOW: Duration = Duration::from_secs(10);

/* ---------- 解析 ---------- */

#[derive(Debug, Clone)]
struct BeaconData {
    dev_id: u16,
    counter: u8,
    batt_mv: u16,
    ch0: i32,
    ch1: i32,
    ch2: i32,
}

/// 24bit 小端 → 有符号 i32(符号扩展)
fn sign_extend_24(b: [u8; 3]) -> i32 {
    let v = ((b[2] as u32) << 16) | ((b[1] as u32) << 8) | b[0] as u32;
    if v & 0x0080_0000 != 0 {
        (v | 0xFF00_0000) as i32
    } else {
        v as i32
    }
}

/// 校验魔数/版本并解析 16 字节载荷。不匹配返回 None。
fn parse_payload(data: &[u8]) -> Option<BeaconData> {
    if data.len() < PAYLOAD_LEN {
        return None;
    }
    let p = &data[..PAYLOAD_LEN];
    if p[0] != MAGIC || p[1] != VERSION {
        return None;
    }
    Some(BeaconData {
        dev_id: u16::from_le_bytes([p[2], p[3]]),
        counter: p[4],
        batt_mv: u16::from_le_bytes([p[5], p[6]]),
        ch0: sign_extend_24([p[7], p[8], p[9]]),
        ch1: sign_extend_24([p[10], p[11], p[12]]),
        ch2: sign_extend_24([p[13], p[14], p[15]]),
    })
}

/* ---------- 每轮统计 ---------- */

/// 一轮 = 同一 (设备ID, 计数器) 的一串重复包。
struct Round {
    counter: u8,
    packets: u32,
    started: Instant,
    last: Instant,
    rssi_min: i16,
    rssi_max: i16,
}

impl Round {
    fn new(counter: u8, rssi: Option<i16>) -> Self {
        let now = Instant::now();
        let r = rssi.unwrap_or(0);
        Round {
            counter,
            packets: 1,
            started: now,
            last: now,
            rssi_min: r,
            rssi_max: r,
        }
    }

    fn hit(&mut self, rssi: Option<i16>) {
        self.packets += 1;
        self.last = Instant::now();
        if let Some(r) = rssi {
            if self.rssi_min == 0 || r < self.rssi_min {
                self.rssi_min = r;
            }
            if self.rssi_max == 0 || r > self.rssi_max {
                self.rssi_max = r;
            }
        }
    }
}

/* ---------- 输出 ---------- */

fn print_beacon(addr: Option<BDAddr>, rssi: Option<i16>, b: &BeaconData) {
    println!("==============================");
    println!(
        "MAC       : {}",
        addr.map_or_else(|| "(未知)".to_string(), |a| a.to_string())
    );
    println!(
        "RSSI      : {}",
        rssi.map_or_else(|| "n/a".to_string(), |v| format!("{} dBm", v))
    );
    println!("设备ID    : 0x{:04X} ({})", b.dev_id, b.dev_id);
    println!("计数器    : {}", b.counter);
    println!("电池      : {} mV", b.batt_mv);
    println!("ch0(#1 chA, 增益128) = {:>8}", b.ch0);
    println!("ch1(#1 chB, 增益 32) = {:>8}", b.ch1);
    println!("ch2(#2 chA, 增益128) = {:>8}", b.ch2);
}

/// 一轮结束时打印收包率 —— 这是判断"是否漏收"的核心输出。
fn print_round_summary(dev_id: u16, r: &Round, expect: u32) {
    let dur = r.last.saturating_duration_since(r.started);
    let pct = if expect > 0 {
        (r.packets as f64) * 100.0 / (expect as f64)
    } else {
        0.0
    };
    let rssi = if r.rssi_min == 0 && r.rssi_max == 0 {
        "n/a".to_string()
    } else if r.rssi_min == r.rssi_max {
        format!("{} dBm", r.rssi_min)
    } else {
        format!("{}..{} dBm", r.rssi_min, r.rssi_max)
    };
    println!(
        "  └─ 设备 0x{:04X} 计数器 {} 一轮结束: 收到 {}/{} 包 ({:.0}%), 历时 {:.1}s, RSSI {}",
        dev_id, r.counter, r.packets, expect, pct, dur.as_secs_f64(), rssi
    );
    if pct < 50.0 {
        println!("     ⚠ 收包率偏低。可尝试: 靠近设备 / 关掉其它蓝牙设备 / 见 README「漏收」一节");
    }
    println!();
}

/* ---------- 从事件流顺带收集 MAC 与 RSSI ---------- */

/// 取 MAC 与 RSSI。
///
/// 走的是本地缓存: central.peripheral(&id) 是 DashMap 查表
/// (adapter_manager.rs:85), properties() 读的是 RwLock 里的缓存值
/// (peripheral.rs:115 derive_properties) —— 都不碰系统 API。
///
/// ⚠ 曾怀疑这里是漏收的原因(旧版还额外遍历了 peripherals()), 但 ab.rs 的
/// A/B 实测显示: 20 秒内慢端与快端都收到 1191 个事件, 丢失 0%, 平均每事件
/// 只花 0.1ms。所以这不是瓶颈 —— 真正原因见文件头。
async fn quick_props(central: &Adapter, id: &PeripheralId) -> (Option<BDAddr>, Option<i16>) {
    if let Ok(p) = central.peripheral(id).await {
        if let Ok(Some(props)) = p.properties().await {
            return (Some(props.address), props.rssi);
        }
    }
    (None, None)
}

/* ---------- 主流程 ---------- */

fn parse_args() -> (u32, bool) {
    let mut expect_events = DEFAULT_ADV_EVENTS;
    let mut verbose = false;
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        match a.as_str() {
            "--events" | "-e" => {
                if let Some(v) = it.next() {
                    if let Ok(n) = v.parse::<u32>() {
                        expect_events = n;
                    }
                }
            }
            "--verbose" | "-v" => verbose = true,
            "--help" | "-h" => {
                println!("用法: sensor-beacon-scanner [选项]");
                println!("  -e, --events N   一轮的广播事件数(默认 {}, 须与固件", DEFAULT_ADV_EVENTS);
                println!("                   BLE_BEACON_ADV_EVENTS 一致), 仅用于算收包率");
                println!("  -v, --verbose    打印每个重复包(默认只在行内刷新计数)");
                println!("  -h, --help       显示本帮助");
                std::process::exit(0);
            }
            _ => {}
        }
    }
    (expect_events, verbose)
}

#[tokio::main]
async fn main() -> Result<()> {
    let (expect_events, verbose) = parse_args();
    let expect_packets = expect_events * CHANNELS_PER_EVENT;

    let manager = Manager::new().await.context("创建蓝牙管理器失败")?;
    let adapters = manager.adapters().await.context("枚举蓝牙适配器失败")?;
    if adapters.is_empty() {
        bail!("未找到蓝牙适配器");
    }

    let central = adapters.into_iter().next().unwrap();
    let info = central.adapter_info().await.context("读取适配器信息失败")?;
    println!("适配器: {}", info);
    println!(
        "过滤: Company ID 0x{:04X}, 魔数 0x{:02X}, 版本 0x{:02X}",
        COMPANY_ID, MAGIC, VERSION
    );
    println!(
        "一轮理论包数: {} 事件 × {} 信道 = {} 包(用 --events 改)",
        expect_events, CHANNELS_PER_EVENT, expect_packets
    );
    println!("设备平时静默 —— 单击设备按键后才有数据。Ctrl+C 退出。");
    println!();

    /* ⚠ 先订阅事件再启动扫描。反过来的话, start_scan 到 events 之间收到的
     * 广播会全部丢失 —— broadcast 通道对"当时还没有订阅者"的消息是直接丢弃的
     * (adapter_manager.rs:51 的 Err(lost) 分支只打一条 trace 日志)。 */
    let mut events = central.events().await.context("订阅扫描事件失败")?;

    central
        .start_scan(ScanFilter::default())
        .await
        .context("启动扫描失败")?;

    /* dev_id → 当前轮 */
    let mut rounds: HashMap<u16, Round> = HashMap::new();

    while let Some(ev) = events.next().await {
        /* 三种事件都要处理 —— 只监听 ManufacturerDataAdvertisement 会漏:
         *   winrtble 下每个广播同时发 DeviceUpdated + ManufacturerDataAdvertisement,
         *   而 CoreBluetooth / bluez 的发法不同。带数据的直接用, 不带数据的
         *   去本地缓存取最近一次厂商数据。 */
        let (id, inline_data) = match ev {
            CentralEvent::ManufacturerDataAdvertisement {
                id,
                manufacturer_data,
            } => {
                let d = manufacturer_data.get(&COMPANY_ID).cloned();
                (id, d)
            }
            CentralEvent::DeviceDiscovered(id) | CentralEvent::DeviceUpdated(id) => (id, None),
            _ => continue,
        };

        /* 取 MAC/RSSI(非阻塞查表), 顺带在没有内联数据时取缓存的厂商数据 */
        let (addr, rssi) = quick_props(&central, &id).await;
        let data = match inline_data {
            Some(d) => Some(d),
            None => {
                /* 没有内联数据的事件: 从缓存属性里取。注意这可能取到的是
                 * 上一包的数据 —— 但下面按 (设备ID, 计数器) 去重, 重复的
                 * 只会累加计数, 不会造成错误的新一轮。 */
                match central.peripheral(&id).await {
                    Ok(p) => match p.properties().await {
                        Ok(Some(props)) => props.manufacturer_data.get(&COMPANY_ID).cloned(),
                        _ => None,
                    },
                    Err(_) => None,
                }
            }
        };

        let Some(data) = data else { continue };
        let Some(b) = parse_payload(&data) else {
            continue;
        };

        match rounds.get_mut(&b.dev_id) {
            /* 同一轮的重复包: 只累加计数 */
            Some(r) if r.counter == b.counter && r.started.elapsed() < ROUND_WINDOW => {
                r.hit(rssi);
                if verbose {
                    println!(
                        "     · 重复包 #{:<2} 设备 0x{:04X} RSSI {}",
                        r.packets,
                        b.dev_id,
                        rssi.map_or_else(|| "n/a".into(), |v| format!("{} dBm", v))
                    );
                } else {
                    /* 行内刷新: 不滚屏也能看到收包在涨 */
                    print!("\r  收包 {}/{} ...", r.packets, expect_packets);
                    use std::io::Write;
                    let _ = std::io::stdout().flush();
                }
            }
            /* 新一轮(计数器变了, 或上一轮已超时) */
            prev => {
                if let Some(r) = prev {
                    let old = Round {
                        counter: r.counter,
                        packets: r.packets,
                        started: r.started,
                        last: r.last,
                        rssi_min: r.rssi_min,
                        rssi_max: r.rssi_max,
                    };
                    println!();
                    print_round_summary(b.dev_id, &old, expect_packets);
                }
                rounds.insert(b.dev_id, Round::new(b.counter, rssi));
                print_beacon(addr, rssi, &b);
            }
        }
    }

    Ok(())
}

//! 验证"内容完全相同的广播会不会被 Windows 合并上报"。
//!
//! 这是 scanner 漏收的头号怀疑点(A/B 测试已排除"主循环阻塞导致队列溢出"这条:
//! 实测 peripherals().await 平均仅 0.1ms, 丢失 0%)。
//!
//! 方法: 找环境里播得最勤的设备, 记录它每个包的【原始字节】与到达时刻。
//! 若相邻两包字节完全相同却仍然分别上报 -> Windows 不合并, 原因 3 不成立。
//! 若相同内容的包被明显拉长间隔 / 合并 -> 原因 3 成立。
//!
//! 同时统计每个设备的到达间隔分布, 用来判断"扫描窗口占空比"这条:
//! Windows 的 BLE 侦听器不是 100% 时间在收, 它有 scan window/interval,
//! 窗口之外的空中包物理上收不到 —— 这对"一轮只播 1.5 秒"的设备是致命的。

use std::collections::HashMap;
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use btleplug::api::{Central, CentralEvent, ScanFilter};
use tokio_stream::StreamExt;

struct Track {
    /// 上一包的厂商数据原始字节
    last_bytes: Vec<u8>,
    /// 上一包到达时刻
    last_at: Instant,
    total: u64,
    /// 内容与上一包完全相同的次数
    same_content: u64,
    /// 到达间隔样本(ms)
    gaps: Vec<u64>,
}

#[tokio::main]
async fn main() -> Result<()> {
    let secs: u64 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(30);

    let central = sensor_beacon_scanner::open_adapter().await?;
    println!("For {}s: are identical adverts still reported packet by packet, and what is the arrival gap distribution", secs);
    println!();

    let mut events = central.events().await.context("failed to subscribe to events")?;
    central.start_scan(ScanFilter::default()).await?;

    let mut tracks: HashMap<String, Track> = HashMap::new();
    let t0 = Instant::now();
    let dur = Duration::from_secs(secs);

    while t0.elapsed() < dur {
        let remain = dur.saturating_sub(t0.elapsed());
        let Ok(Some(ev)) = tokio::time::timeout(remain, events.next()).await else {
            break;
        };
        if let CentralEvent::ManufacturerDataAdvertisement { id, manufacturer_data } = ev {
            /* 把所有 company 段拼成一个 key, 作为"这一包的内容" */
            let mut bytes = Vec::new();
            let mut cids: Vec<_> = manufacturer_data.keys().copied().collect();
            cids.sort();
            for c in cids {
                bytes.extend_from_slice(&c.to_le_bytes());
                bytes.extend_from_slice(&manufacturer_data[&c]);
            }
            let key = format!("{:?}", id);
            let now = Instant::now();
            match tracks.get_mut(&key) {
                Some(t) => {
                    t.total += 1;
                    if t.last_bytes == bytes {
                        t.same_content += 1;
                    }
                    t.gaps.push(now.duration_since(t.last_at).as_millis() as u64);
                    t.last_bytes = bytes;
                    t.last_at = now;
                }
                None => {
                    tracks.insert(key, Track {
                        last_bytes: bytes,
                        last_at: now,
                        total: 1,
                        same_content: 0,
                        gaps: Vec::new(),
                    });
                }
            }
        }
    }

    /* 只看播得最勤的几个设备 —— 样本少的统计没意义 */
    let mut v: Vec<_> = tracks.into_iter().filter(|(_, t)| t.total >= 8).collect();
    v.sort_by_key(|(_, t)| std::cmp::Reverse(t.total));

    println!("=== busiest advertisers (>= 8 packets sampled) ===");
    println!("{:<26} {:>5} {:>8} {:>9} {:>9} {:>9}", "device", "pkts", "same", "gapMed", "gapMin", "gapMax");
    for (k, t) in v.iter().take(12) {
        let mut g = t.gaps.clone();
        g.sort();
        let med = if g.is_empty() { 0 } else { g[g.len() / 2] };
        let mn = g.first().copied().unwrap_or(0);
        let mx = g.last().copied().unwrap_or(0);
        let short = if k.len() > 24 { &k[k.len()-24..] } else { k.as_str() };
        println!("{:<26} {:>5} {:>8} {:>7}ms {:>7}ms {:>7}ms",
                 short, t.total, t.same_content, med, mn, mx);
    }

    println!();
    let total_same: u64 = v.iter().map(|(_, t)| t.same_content).sum();
    let total_pk: u64 = v.iter().map(|(_, t)| t.total).sum();
    println!("{} packets total, {} of which were byte-identical to the one before.", total_pk, total_same);
    if total_same > 0 {
        println!("-> So Windows does NOT coalesce identical adverts; every packet is reported.");
        println!("   That rules out the theory that the OS de-duplicates the 15 identical packets of a burst.");
    } else {
        println!("-> No byte-identical adjacent packets observed, so OS de-duplication cannot be ruled out.");
    }

    /* 最小间隔是判断扫描占空比的关键 */
    let mut all_gaps: Vec<u64> = v.iter().flat_map(|(_, t)| t.gaps.clone()).collect();
    all_gaps.sort();
    if !all_gaps.is_empty() {
        println!();
        println!("all arrival gaps: min {}ms, median {}ms, P90 {}ms",
                 all_gaps[0],
                 all_gaps[all_gaps.len()/2],
                 all_gaps[all_gaps.len()*9/10]);
        let sub100 = all_gaps.iter().filter(|g| **g < 100).count();
        println!("gaps under 100ms account for {:.1}%; near zero means the OS scan window is sparse,",
                 sub100 as f64 * 100.0 / all_gaps.len() as f64);
        println!("so with a 100ms advertising interval most packets never hit a window.");
    }

    Ok(())
}

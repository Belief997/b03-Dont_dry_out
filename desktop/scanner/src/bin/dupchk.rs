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

use anyhow::{bail, Context, Result};
use btleplug::api::{Central, CentralEvent, Manager as _, ScanFilter};
use btleplug::platform::Manager;
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

    let manager = Manager::new().await.context("创建蓝牙管理器失败")?;
    let adapters = manager.adapters().await.context("枚举适配器失败")?;
    if adapters.is_empty() {
        bail!("未找到蓝牙适配器");
    }
    let central = adapters.into_iter().next().unwrap();
    println!("适配器: {}", central.adapter_info().await?);
    println!("统计 {} 秒: 相同内容的广播是否仍被逐包上报, 以及到达间隔分布", secs);
    println!();

    let mut events = central.events().await.context("订阅事件失败")?;
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

    println!("=== 播得最勤的设备(样本 >= 8 包) ===");
    println!("{:<26} {:>5} {:>8} {:>9} {:>9} {:>9}", "设备", "包数", "同内容", "间隔中位", "间隔最小", "间隔最大");
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
    println!("合计 {} 包, 其中 {} 包与前一包内容完全相同。", total_pk, total_same);
    if total_same > 0 {
        println!("→ 说明 Windows 【不会】把内容相同的广播合并掉, 逐包都上报。");
        println!("  所以\"一轮 15 个相同包被系统去重\"这条【不成立】。");
    } else {
        println!("→ 未观察到内容相同的相邻包, 无法排除系统去重。");
    }

    /* 最小间隔是判断扫描占空比的关键 */
    let mut all_gaps: Vec<u64> = v.iter().flat_map(|(_, t)| t.gaps.clone()).collect();
    all_gaps.sort();
    if !all_gaps.is_empty() {
        println!();
        println!("全部到达间隔: 最小 {}ms, 中位 {}ms, P90 {}ms",
                 all_gaps[0],
                 all_gaps[all_gaps.len()/2],
                 all_gaps[all_gaps.len()*9/10]);
        let sub100 = all_gaps.iter().filter(|g| **g < 100).count();
        println!("间隔 <100ms 的占 {:.1}% —— 若接近 0, 说明系统扫描窗口稀疏,",
                 sub100 as f64 * 100.0 / all_gaps.len() as f64);
        println!("固件用 100ms 广播间隔时大部分包会撞不上窗口。");
    }

    Ok(())
}

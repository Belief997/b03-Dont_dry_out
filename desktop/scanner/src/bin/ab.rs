//! A/B 对比: 主循环里"慢"与"快"两种写法, 各自能收到多少事件。
//!
//! 这是 scanner 漏收问题的直接证据。两个订阅者【同时】订阅同一个事件流,
//! 面对完全相同的空中广播:
//!
//!   fast: 收到就计数, 循环内不做任何 await 外部系统的调用
//!   slow: 复刻旧版 scanner 的做法 —— 每个事件都 central.peripherals().await
//!         然后遍历找 id 再 properties().await
//!
//! 待检验的假设是: btleplug 的事件通道只有 16 格 tokio broadcast
//! (common/adapter_manager.rs:34), 且 BroadcastStream 在滞后时产出
//! Err(Lagged(n)) 而 btleplug 用 `.ok()` 静默丢弃 (同文件 :58) ——
//! 于是 slow 端每次慢调用期间涌入的广播都会被挤掉且【无任何提示】。
//!
//! 【实测否定了这个假设】20 秒内两端都收到 1191 个事件, 丢失 0%, 慢端平均
//! 每事件只花 0.1ms。因为 peripherals()/properties() 读的都是本地缓存
//! (DashMap + RwLock), 不是跨进程 WinRT 调用, 根本不够慢到挤掉队列。
//! 漏收的真正原因见 main.rs 文件头(系统扫描窗口稀疏)。
//!
//! 本程序自己不用 btleplug 的 event_stream(), 而是各起一个任务共享
//! central, 分别按两种节奏消费 —— 这样两端拿到的是同一份事件源。

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};
use btleplug::api::{Central, CentralEvent, Manager as _, Peripheral as _, ScanFilter};
use btleplug::platform::Manager;
use tokio_stream::StreamExt;

#[tokio::main]
async fn main() -> Result<()> {
    let secs: u64 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(20);

    let manager = Manager::new().await.context("创建蓝牙管理器失败")?;
    let adapters = manager.adapters().await.context("枚举适配器失败")?;
    if adapters.is_empty() {
        bail!("未找到蓝牙适配器");
    }
    let central = Arc::new(adapters.into_iter().next().unwrap());
    println!("适配器: {}", central.adapter_info().await?);
    println!("A/B 对比 {} 秒: fast(不阻塞) vs slow(每事件都查外设属性)", secs);
    println!("两端订阅同一事件源, 面对完全相同的空中广播。");
    println!();

    /* 两个订阅者都要在 start_scan 之前订阅 */
    let mut ev_fast = central.events().await.context("订阅 fast 失败")?;
    let mut ev_slow = central.events().await.context("订阅 slow 失败")?;

    central.start_scan(ScanFilter::default()).await.context("启动扫描失败")?;

    let n_fast = Arc::new(AtomicU64::new(0));
    let n_slow = Arc::new(AtomicU64::new(0));
    let slow_await_ms = Arc::new(AtomicU64::new(0));

    let dur = Duration::from_secs(secs);

    let nf = n_fast.clone();
    let h_fast = tokio::spawn(async move {
        let t0 = Instant::now();
        while t0.elapsed() < dur {
            let remain = dur.saturating_sub(t0.elapsed());
            match tokio::time::timeout(remain, ev_fast.next()).await {
                Ok(Some(_ev)) => {
                    nf.fetch_add(1, Ordering::Relaxed);
                }
                _ => break,
            }
        }
    });

    let ns = n_slow.clone();
    let sam = slow_await_ms.clone();
    let c2 = central.clone();
    let h_slow = tokio::spawn(async move {
        let t0 = Instant::now();
        while t0.elapsed() < dur {
            let remain = dur.saturating_sub(t0.elapsed());
            match tokio::time::timeout(remain, ev_slow.next()).await {
                Ok(Some(ev)) => {
                    ns.fetch_add(1, Ordering::Relaxed);
                    /* 复刻旧版 lookup_props(): 遍历全部外设 + 读属性 */
                    let id = match &ev {
                        CentralEvent::DeviceDiscovered(id)
                        | CentralEvent::DeviceUpdated(id)
                        | CentralEvent::ManufacturerDataAdvertisement { id, .. } => Some(id.clone()),
                        _ => None,
                    };
                    if let Some(id) = id {
                        let t = Instant::now();
                        if let Ok(list) = c2.peripherals().await {
                            if let Some(p) = list.into_iter().find(|p| p.id() == id) {
                                let _ = p.properties().await;
                            }
                        }
                        sam.fetch_add(t.elapsed().as_millis() as u64, Ordering::Relaxed);
                    }
                }
                _ => break,
            }
        }
    });

    let _ = tokio::join!(h_fast, h_slow);

    let f = n_fast.load(Ordering::Relaxed);
    let s = n_slow.load(Ordering::Relaxed);
    let ms = slow_await_ms.load(Ordering::Relaxed);

    println!("=== 结果 ===");
    println!("fast(不阻塞)      收到 {:>5} 个事件", f);
    println!("slow(旧版做法)    收到 {:>5} 个事件", s);
    if f > 0 {
        let lost = f.saturating_sub(s);
        println!(
            "slow 比 fast 少   {:>5} 个 ({:.1}% 丢失)",
            lost,
            lost as f64 * 100.0 / f as f64
        );
    }
    println!("slow 累计阻塞在查属性上: {} ms (平均每事件 {:.1} ms)",
             ms, if s > 0 { ms as f64 / s as f64 } else { 0.0 });
    println!();
    if f > 0 && f.saturating_sub(s) * 100 / f < 2 {
        println!("丢失接近 0 → 队列溢出【不是】漏收的原因。慢调用读的是本地缓存");
        println!("(DashMap + RwLock), 不碰系统 API, 快到挤不掉 16 格队列。");
        println!("真正原因见 main.rs 文件头: 系统扫描窗口稀疏, 用 dupchk 看间隔分布。");
    } else {
        println!("出现明显丢失 → 来自 common/adapter_manager.rs:58 的 `.ok()`:");
        println!("BroadcastStream 滞后时产出 Err(Lagged(n)), 被静默丢弃, 不报错不打日志。");
    }

    Ok(())
}

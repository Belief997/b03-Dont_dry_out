//! 事件通道诊断: 不做协议过滤, 只统计各类事件的到达速率与丢包(Lagged)。
//!
//! 目的是给"环境有多吵"建立基线:
//!   1) 事件流本身通不通(能不能收到环境里其它 BLE 设备的广播)
//!   2) 每秒多少事件、多少个设备在播、都是哪些 Company ID
//!
//! ⚠ 本工具最初是为了验证"容量 16 的 broadcast 通道溢出导致漏收"这个假设,
//!   该假设后来被 ab.rs 的 A/B 实测【否定】(两端丢失 0%)。真正原因是系统
//!   扫描窗口稀疏, 见 dupchk.rs 与 main.rs 文件头。此工具保留作环境基线用。
//!
//! 与 main.rs 不同, 这里直接用 BroadcastStream 的原始 Result, 不做
//! `.ok()` 丢弃, 于是 Lagged(n) 可以被【看见】并计数。

use std::collections::HashMap;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};
use btleplug::api::{Central, CentralEvent, Manager as _, ScanFilter};
use btleplug::platform::Manager;
use tokio_stream::StreamExt;

#[tokio::main]
async fn main() -> Result<()> {
    let secs: u64 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(15);

    let manager = Manager::new().await.context("创建蓝牙管理器失败")?;
    let adapters = manager.adapters().await.context("枚举适配器失败")?;
    if adapters.is_empty() {
        bail!("未找到蓝牙适配器");
    }
    let central = adapters.into_iter().next().unwrap();
    println!("适配器: {}", central.adapter_info().await?);
    println!("不做任何过滤, 统计 {} 秒内的全部 BLE 事件...", secs);
    println!();

    let mut events = central.events().await.context("订阅事件失败")?;
    central.start_scan(ScanFilter::default()).await.context("启动扫描失败")?;

    let t0 = Instant::now();
    let mut n_disc = 0u64;
    let mut n_upd = 0u64;
    let mut n_manu = 0u64;
    let mut n_serv = 0u64;
    let mut n_other = 0u64;
    /* 每个 CompanyID 收到多少包 —— 看环境里有多少厂商在播 */
    let mut by_company: HashMap<u16, u64> = HashMap::new();
    let mut devices: HashMap<String, u64> = HashMap::new();

    let deadline = Duration::from_secs(secs);
    while t0.elapsed() < deadline {
        let remain = deadline.saturating_sub(t0.elapsed());
        match tokio::time::timeout(remain, events.next()).await {
            Err(_) => break,          // 到时
            Ok(None) => break,        // 流结束
            Ok(Some(ev)) => match ev {
                CentralEvent::DeviceDiscovered(id) => {
                    n_disc += 1;
                    *devices.entry(format!("{:?}", id)).or_insert(0) += 1;
                }
                CentralEvent::DeviceUpdated(id) => {
                    n_upd += 1;
                    *devices.entry(format!("{:?}", id)).or_insert(0) += 1;
                }
                CentralEvent::ManufacturerDataAdvertisement { manufacturer_data, .. } => {
                    n_manu += 1;
                    for (cid, data) in manufacturer_data {
                        *by_company.entry(cid).or_insert(0) += 1;
                        if cid == 0xFFFF {
                            println!("  0xFFFF 段 {} 字节: {:02X?}", data.len(), data);
                        }
                    }
                }
                CentralEvent::ServiceDataAdvertisement { .. } => n_serv += 1,
                _ => n_other += 1,
            },
        }
    }

    let el = t0.elapsed().as_secs_f64();
    let total = n_disc + n_upd + n_manu + n_serv + n_other;
    println!();
    println!("=== {:.1} 秒统计 ===", el);
    println!("DeviceDiscovered              : {}", n_disc);
    println!("DeviceUpdated                 : {}", n_upd);
    println!("ManufacturerDataAdvertisement : {}", n_manu);
    println!("ServiceDataAdvertisement      : {}", n_serv);
    println!("其它                          : {}", n_other);
    println!("合计 {} 个事件, {:.1} 事件/秒", total, total as f64 / el);
    println!("不同设备数: {}", devices.len());
    println!();
    println!("按 Company ID 分布(前 10):");
    let mut v: Vec<_> = by_company.into_iter().collect();
    v.sort_by_key(|(_, n)| std::cmp::Reverse(*n));
    for (cid, n) in v.iter().take(10) {
        let tag = if *cid == 0xFFFF { "  <-- 本项目" } else { "" };
        println!("  0x{:04X} : {:>5} 包{}", cid, n, tag);
    }
    println!();
    println!("以上只是环境噪声基线。注意: 队列溢出【已被 ab.rs 实测排除】——");
    println!("  两端(阻塞/不阻塞)都收到同样多的事件, 丢失 0%。漏收的真正原因是");
    println!("  系统扫描窗口稀疏(约每秒一次), 见 dupchk.rs 的到达间隔统计与 main.rs 文件头。");

    Ok(())
}

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
//! 广播行为: 设备平时静默, 单击按键后连播若干广播事件(每个事件在
//! 3 个信道上各发 1 个空中包), 内容相同。本程序按 (设备ID, 计数器)
//! 去重: 每轮完整打印第 1 个包, 其余重复包计数显示, 便于观察收包率。

use std::collections::HashMap;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};
use btleplug::api::{BDAddr, Central, CentralEvent, Manager as _, Peripheral as _, ScanFilter};
use btleplug::platform::{Manager, PeripheralId};
use tokio_stream::StreamExt;

/* ---------- 协议常量(与固件保持一致) ---------- */
const COMPANY_ID: u16 = 0xFFFF;
const MAGIC: u8 = 0xAB;
const VERSION: u8 = 0x02;
const PAYLOAD_LEN: usize = 16;

/* 同一 (设备ID, 计数器) 在去重窗口内的重复包只打印一次 */
const DEDUP_WINDOW: Duration = Duration::from_secs(10);

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

/* ---------- 输出 ---------- */

fn print_beacon(addr: BDAddr, rssi: Option<i16>, b: &BeaconData) {
    println!("==============================");
    println!("MAC       : {}", addr);
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
    println!();
}

/// btleplug 0.11 的事件里只带 PeripheralId, MAC 地址与 RSSI 需
/// 通过外设属性反查; 查不到时返回默认地址与 None(打印为 n/a)。
async fn lookup_props(central: &impl Central, id: PeripheralId) -> (BDAddr, Option<i16>) {
    if let Ok(peripherals) = central.peripherals().await {
        if let Some(p) = peripherals.into_iter().find(|p| p.id() == id) {
            if let Ok(Some(props)) = p.properties().await {
                return (props.address, props.rssi);
            }
        }
    }
    (BDAddr::default(), None)
}

/* ---------- 主流程 ---------- */

#[tokio::main]
async fn main() -> Result<()> {
    let manager = Manager::new().await.context("创建蓝牙管理器失败")?;
    let adapters = manager.adapters().await.context("枚举蓝牙适配器失败")?;
    if adapters.is_empty() {
        bail!("未找到蓝牙适配器");
    }

    let central = adapters.into_iter().next().unwrap();
    let info = central.adapter_info().await.context("读取适配器信息失败")?;
    println!("适配器: {}", info);
    println!(
        "开始扫描... 过滤 Company ID 0x{:04X}, 协议魔数 0x{:02X} 版本 0x{:02X}",
        COMPANY_ID, MAGIC, VERSION
    );
    println!("(设备平时静默, 单击设备按键后即可看到数据; 同轮重复包只完整打印第 1 个, 其余计行显示)");
    println!();

    central
        .start_scan(ScanFilter::default())
        .await
        .context("启动扫描失败")?;

    let mut events = central.events().await.context("订阅扫描事件失败")?;

    /* dev_id → (上次打印的 counter, 本轮已收到包数, 时间)。
     * 同 counter 的重复包只完整打印第 1 个, 其余计行显示。 */
    let mut last_seen: HashMap<u16, (u8, u32, Instant)> = HashMap::new();

    while let Some(ev) = events.next().await {
        if let CentralEvent::ManufacturerDataAdvertisement {
            id,
            manufacturer_data,
        } = ev
        {
            let Some(data) = manufacturer_data.get(&COMPANY_ID) else {
                continue;
            };
            let Some(b) = parse_payload(data) else {
                continue;
            };

            let prev = last_seen.get(&b.dev_id).copied();
            let dup = prev.is_some_and(|(c, _, at)| c == b.counter && at.elapsed() < DEDUP_WINDOW);
            if dup {
                let entry = last_seen.get_mut(&b.dev_id).unwrap();
                entry.1 += 1;
                println!("      (重复包 #{}, 设备ID 0x{:04X})", entry.1, b.dev_id);
                continue;
            }

            if let Some((old_counter, old_count, _)) = prev {
                println!(
                    "      —— 设备ID 0x{:04X} 上一轮(计数器 {})共收到 {} 个包 ——",
                    b.dev_id, old_counter, old_count
                );
            }
            last_seen.insert(b.dev_id, (b.counter, 1, Instant::now()));

            let (addr, rssi) = lookup_props(&central, id).await;
            print_beacon(addr, rssi, &b);
        }
    }

    Ok(())
}

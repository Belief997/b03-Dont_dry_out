//! 暴露给 Dart 的 BLE 接口。
//!
//! 这一层只做三件事: 类型转换、去重、把 `crate::ble` 的回调接到 FRB 的
//! `StreamSink` 上。**任何业务判断都不该写在这里** —— 协议在 `crate::proto`,
//! 适配器状态机在 `crate::ble`。
//!
//! ⚠ 为什么 DTO 在本文件重新定义一遍, 而不是复用 `crate::ble::AdvEvent`:
//!   FRB 的代码生成只扫描 `crate::api`(见 flutter_rust_bridge.yaml 的
//!   `rust_input`)。把内部类型直接摆进签名会让内部结构的每次改动都触发 Dart
//!   侧重新生成, 边界就模糊了。多写一层转换换来"内部随便改, 只要 DTO 不变
//!   Dart 就不用动"。

use flutter_rust_bridge::frb;
use std::sync::Mutex;

use crate::ble;
// ⚠ StreamSink 来自生成的模块, 不是 flutter_rust_bridge 根 ——
//   FRB 2.x 把它放在 crate::frb_generated 下, 写成 flutter_rust_bridge::StreamSink
//   会报 "not found in flutter_rust_bridge"。
use crate::frb_generated::StreamSink;

/// 适配器当前被谁占用。与 [`ble::Mode`] 一一对应。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BleMode {
    /// 空闲。
    Idle,
    /// 正在扫描广播。
    Scanning,
    /// 已连接(命令协议可用)。
    Connected,
}

impl From<ble::Mode> for BleMode {
    fn from(m: ble::Mode) -> Self {
        match m {
            ble::Mode::Idle => Self::Idle,
            ble::Mode::Scanning => Self::Scanning,
            ble::Mode::Connected => Self::Connected,
        }
    }
}

/// 一条已解析的广播(去重后, 每轮 burst 只上报一条)。
#[derive(Debug, Clone)]
pub struct BleAdvEvent {
    pub mac: Option<String>,
    pub rssi: Option<i32>,
    /// 厂商自定义段原始字节(不含 Company ID), 供 UI 做 hexdump。
    pub raw: Vec<u8>,

    // --- 以下字段仅在解析成功时有意义, 由 `ok` 标记 ---
    /// 解析是否成功。
    pub ok: bool,
    /// 解析失败原因(`ok == false` 时)。
    pub error: Option<String>,

    pub version: u8,
    pub device_id: u16,
    /// 单击计数器。同一轮 burst 内不变 —— 本字段即去重依据。
    pub counter: u8,
    pub batt_mv: u16,
    /// 三通道原始计数。
    ///
    /// ⚠ 三通道增益不同(ch0=128, ch1=32, ch2=128)且载荷里不携带增益,
    ///   UI 上直接比较三者数值是错的。实测灵敏度
    ///   ch0≈192.2 / ch1≈49.3 / ch2≈−199.9 counts/g(注意 ch2 为负)。
    pub ch: Vec<i32>,
}

/// 跨 burst 的去重表。
///
/// ⚠ 必须是全局的: 一轮 burst 的多个包分散在多个事件回调里到达, 去重表活不过
///   单次回调。放在 `scan_start` 的闭包里捕获也可以, 但那样每次重启扫描就重置,
///   而设备的 counter 不会重置 —— 重启扫描后第一包会被误判成"重复"而丢掉。
static DEDUP: Mutex<Option<ble::RoundDedup>> = Mutex::new(None);

/// 适配器名字。没有适配器时抛异常。
#[frb(sync)]
pub fn ble_adapter_name() -> anyhow::Result<String> {
    ble::adapter_name()
}

/// 当前适配器占用状态。
#[frb(sync)]
pub fn ble_mode() -> BleMode {
    ble::mode().into()
}

/// 开始扫描。返回的 Dart `Stream` 每轮 burst 产出一条事件(已去重)。
///
/// ⚠ **设备平时是完全静默的** —— 固件只在单击按键后播
///   `BLE_BEACON_ADV_EVENTS` 个广播事件。所以"扫描已启动但一直没有数据"是
///   正常状态, 不是故障, UI 上必须写清楚, 否则用户会以为工具坏了。
///
/// ⚠ Windows 下收包率天然很低(每 ~1.3s 才开一次扫描窗口, 而一轮 burst 只有
///   1.5s)。这是系统限制, 见 `crate::ble` 的文件头。真正的数据采集应该走
///   连接后的 `REC_READ`, 不要指望扫描收全。
pub fn ble_scan_start(sink: StreamSink<BleAdvEvent>) -> anyhow::Result<()> {
    {
        let mut d = DEDUP.lock().expect("DEDUP 被 poison");
        if d.is_none() {
            *d = Some(ble::RoundDedup::default());
        }
    }

    ble::scan_start(move |ev| {
        let out = match ev.payload {
            Some(p) => {
                // 去重: 同一轮 burst 的重复包直接丢, 不过 FFI。
                {
                    let mut guard = DEDUP.lock().expect("DEDUP 被 poison");
                    let d = guard.get_or_insert_with(ble::RoundDedup::default);
                    if !d.is_new_round(p.device_id, p.counter) {
                        return;
                    }
                }
                BleAdvEvent {
                    mac: ev.mac,
                    rssi: ev.rssi,
                    raw: ev.raw,
                    ok: true,
                    error: None,
                    version: p.version,
                    device_id: p.device_id,
                    counter: p.counter,
                    batt_mv: p.batt_mv,
                    ch: p.ch,
                }
            }
            None => BleAdvEvent {
                // 解析失败的也上报 —— 版本不符这种情况必须让用户看见,
                // 否则固件升级后工具"什么都收不到"却毫无线索。
                mac: ev.mac,
                rssi: ev.rssi,
                raw: ev.raw,
                ok: false,
                error: ev.parse_error,
                version: 0,
                device_id: 0,
                counter: 0,
                batt_mv: 0,
                ch: Vec::new(),
            },
        };

        // sink 关闭(Dart 侧取消了订阅)时 add 返回 Err, 忽略即可 ——
        // 下一次 scan_stop 会把任务收掉。
        let _ = sink.add(out);
    })
}

/// 停止扫描。未在扫描时安全。
#[frb(sync)]
pub fn ble_scan_stop() -> anyhow::Result<()> {
    ble::scan_stop()
}

//! BLE 层 —— 独占蓝牙适配器, 对上只暴露"扫描 / 连接"两种互斥状态。
//!
//! ============================================================
//!  为什么扫描与连接必须互斥
//! ============================================================
//!
//! 这不是实现偷懒, 是设备侧的硬约束(固件 `ble_link.h` 有长篇论证):
//!
//!   - S112 只有 **1 个广播集**(`BLE_GAP_ADV_SET_COUNT_MAX == 1`), 且只有
//!     **1 个连接槽**;
//!   - 连接建立后可连接广播由协议栈自动停止;
//!   - 当前固件策略下, 连接期间**不播数据广播** —— 所以"连着的同时扫广播"
//!     在设备侧根本收不到东西;
//!   - Windows 上边扫边连本来也不稳。
//!
//! 于是本模块用一个状态机把两者串行化: 停扫描 → 连接 → … → 断开 → 恢复扫描。
//! 不这么做会出很难查的 bug(收不到包, 但两边都"看起来正常")。
//!
//! ============================================================
//!  ⚠ 关于"自动重连" —— 本工具【不能】有这个功能
//! ============================================================
//!
//! 原生 app 的标准套路是"记住设备 → 后台重连 → 掉线自动恢复"。这套在本设备上
//! **全部失效**: 可连接广播只有 30 秒窗口(`BLE_LINK_ADV_DURATION_MS`), 而且
//! **只能靠人去物理长按按键**才能打开。所以 UI 必须围绕"请去按住按键"这个动作
//! 设计, 不要写重连循环 —— 它永远等不到设备。
//!
//! ============================================================
//!  ⚠ Windows 扫描窗口稀疏(已实测, 不是本代码的 bug)
//! ============================================================
//!
//! `dev/desktop/scanner` 的诊断工具实测: Windows 的 BLE 侦听器大约每 1.3 秒才
//! 开一次扫描窗口(到达间隔中位 1346ms)。固件一轮 burst 只持续
//! 15 × 100ms = 1.5s, 期间只有 1~2 个窗口打开 —— 45 个空中包能采到一两个就是
//! 正常结果。Windows 不暴露 scan window/interval 设置, **应用层无法修复**,
//! 换语言/框架也一样。
//!
//! 因此本工具的定位: **广播扫描只当"活体检测/快速一瞥", 数据采集走连接后的
//! 命令协议**(`REC_READ` 逐字节可靠, 有 ATT 确认与重传)。丢包不影响正确性。

use std::collections::HashMap;
use std::sync::{LazyLock, Mutex};

use anyhow::{anyhow, Context, Result};
use btleplug::api::{BDAddr, Central, CentralEvent, Manager as _, Peripheral as _, ScanFilter};
use btleplug::platform::{Adapter, Manager, PeripheralId};
use tokio::runtime::Runtime;
use tokio::task::JoinHandle;
use tokio_stream::StreamExt;

use crate::proto::adv;

/// 本 crate 自持的 tokio runtime。
///
/// ⚠ 为什么不用 FRB 的执行器: FRB v2 的 async 支持不是跑在 tokio 上, 而 btleplug
///   的 future 依赖 tokio 的 reactor。混用会在运行时 panic
///   ("there is no reactor running")。自持一个 runtime 是最省心的做法。
///
/// ⚠ 必须是 multi_thread: 扫描任务是长驻的, 单线程 runtime 下它会把线程占满,
///   后续 `block_on` 的短调用(如枚举适配器)会排在它后面永远等不到。
static RT: LazyLock<Runtime> = LazyLock::new(|| {
    tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .expect("创建 tokio runtime 失败")
});

/// 适配器只取一次并缓存 —— `Manager::new()` 在 Windows 上要跨进程问 WinRT,
/// 每次调用都重新建会明显变慢。
static ADAPTER: LazyLock<tokio::sync::OnceCell<Adapter>> =
    LazyLock::new(tokio::sync::OnceCell::new);

/// 适配器当前被谁占用。
///
/// ⚠ 用 std 的 Mutex 而不是 tokio 的: 临界区里只做几个赋值, 绝不跨 `.await`
///   (跨 await 持有 std::Mutex 会在多线程 runtime 上死锁)。
static STATE: LazyLock<Mutex<State>> = LazyLock::new(|| Mutex::new(State::default()));

#[derive(Default)]
struct State {
    mode: Mode,
    /// 扫描任务的句柄, 用于 `scan_stop()` 时 abort。
    scan_task: Option<JoinHandle<()>>,
}

/// 适配器的占用状态。与 Dart 侧的 `BleMode` 一一对应。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Mode {
    /// 空闲 —— 既没扫描也没连接。
    #[default]
    Idle,
    /// 正在被动扫描广播。
    Scanning,
    /// 已连接某个设备(命令协议可用)。
    Connected,
}

/// 一条被识别为本项目设备的广播。
#[derive(Debug, Clone)]
pub struct AdvEvent {
    /// "C3:9A:12:34:AB:CD"。取不到时为 None(某些平台在首个事件里还没缓存到地址)。
    pub mac: Option<String>,
    pub rssi: Option<i32>,
    /// 厂商自定义段的原始载荷(不含 Company ID), 便于 UI 上做 hexdump。
    pub raw: Vec<u8>,
    /// 解析成功时的字段; 解析失败时为 None 且 `parse_error` 有值。
    pub payload: Option<AdvPayloadDto>,
    /// 解析失败的原因(人话)。用于区分"我们的设备但版本不对"与"别人的包"。
    pub parse_error: Option<String>,
}

/// [`adv::AdvPayload`] 的 FRB 传输版本。
///
/// ⚠ 为什么不直接把 `adv::AdvPayload` 暴露给 FRB: 那个结构里 `ch` 是 `[i32; 3]`,
///   FRB 对定长数组的支持不如 `Vec`。协议层保持定长(表达"就是 3 通道"),
///   传输层转 Vec —— 两层各自用最合适的类型, 转换只在这一处。
#[derive(Debug, Clone)]
pub struct AdvPayloadDto {
    pub version: u8,
    pub device_id: u16,
    pub counter: u8,
    pub batt_mv: u16,
    /// 三通道原始计数。
    ///
    /// ⚠ 增益不同(ch0=128, ch1=32, ch2=128)且不在载荷里携带, UI 上直接比较三者
    ///   的数值是错的。实测灵敏度 ch0≈192.2 / ch1≈49.3 / ch2≈−199.9 counts/g。
    pub ch: Vec<i32>,
}

impl From<adv::AdvPayload> for AdvPayloadDto {
    fn from(p: adv::AdvPayload) -> Self {
        Self {
            version: p.version,
            device_id: p.device_id,
            counter: p.counter,
            batt_mv: p.batt_mv,
            ch: p.ch.to_vec(),
        }
    }
}

/// 拿到(并缓存)第一个蓝牙适配器。
async fn adapter() -> Result<&'static Adapter> {
    ADAPTER
        .get_or_try_init(|| async {
            let manager = Manager::new().await.context("创建蓝牙管理器失败")?;
            let list = manager.adapters().await.context("枚举蓝牙适配器失败")?;
            list.into_iter()
                .next()
                .ok_or_else(|| anyhow!("未找到蓝牙适配器 —— 确认系统蓝牙已开启"))
        })
        .await
}

/// 适配器名字, 供 UI 显示。
pub fn adapter_name() -> Result<String> {
    RT.block_on(async {
        let a = adapter().await?;
        a.adapter_info().await.context("读取适配器信息失败")
    })
}

pub fn mode() -> Mode {
    STATE.lock().expect("STATE 被 poison").mode
}

/// 取 MAC 与 RSSI。
///
/// 读的是 btleplug 的本地缓存(DashMap + RwLock), 不是跨进程 WinRT 调用 ——
/// `scanner/src/bin/ab.rs` 的 A/B 实测: 20 秒 1191 个事件, 平均每事件 0.1ms,
/// 丢失 0%。所以在事件循环里调它是安全的。
async fn quick_props(central: &Adapter, id: &PeripheralId) -> (Option<BDAddr>, Option<i16>) {
    if let Ok(p) = central.peripheral(id).await {
        if let Ok(Some(props)) = p.properties().await {
            return (Some(props.address), props.rssi);
        }
    }
    (None, None)
}

/// 从一个 central 事件里榨出本项目的厂商数据。
///
/// ⚠ 必须同时处理三种事件, 只监听 `ManufacturerDataAdvertisement` 会漏:
///   winrtble 下每个广播同时发 `DeviceUpdated` + `ManufacturerDataAdvertisement`,
///   而 CoreBluetooth / bluez 的发法不同。带内联数据的直接用, 不带的去本地缓存取。
async fn extract(central: &Adapter, ev: CentralEvent) -> Option<(PeripheralId, Vec<u8>)> {
    let (id, inline) = match ev {
        CentralEvent::ManufacturerDataAdvertisement {
            id,
            manufacturer_data,
        } => {
            let d = manufacturer_data.get(&adv::COMPANY_ID).cloned();
            (id, d)
        }
        CentralEvent::DeviceDiscovered(id) | CentralEvent::DeviceUpdated(id) => (id, None),
        _ => return None,
    };

    let data = match inline {
        Some(d) => d,
        None => {
            // 没有内联数据: 从缓存属性取。可能拿到的是上一包 —— 上层按
            // (device_id, counter) 去重, 重复的只会累加计数, 不会造成错误的新一轮。
            let p = central.peripheral(&id).await.ok()?;
            let props = p.properties().await.ok()??;
            props.manufacturer_data.get(&adv::COMPANY_ID).cloned()?
        }
    };
    Some((id, data))
}

/// 开始扫描, 每收到一条本项目的广播就调一次 `on_event`。
///
/// `on_event` 会在 tokio 工作线程上被调用, 不是 Dart 的 isolate 线程 ——
/// FRB 的 `StreamSink::add` 本身是线程安全的, 所以直接调没问题。
/// ⚠ 回调必须 `Sync` 而不只是 `Send`: 扫描循环里它以 `&F` 的形式跨过 `.await`,
///   而 `&F: Send` 要求 `F: Sync`。FRB 的 `StreamSink` 满足这个约束。
pub fn scan_start<F>(on_event: F) -> Result<()>
where
    F: Fn(AdvEvent) + Send + Sync + 'static,
{
    {
        let st = STATE.lock().expect("STATE 被 poison");
        match st.mode {
            Mode::Scanning => return Ok(()), // 幂等: 已在扫描
            Mode::Connected => {
                return Err(anyhow!(
                    "已连接设备时不能扫描 —— 设备侧连接期间不播数据广播, 请先断开"
                ))
            }
            Mode::Idle => {}
        }
    }

    let task = RT.spawn(async move {
        if let Err(e) = scan_loop(&on_event).await {
            // 扫描循环异常退出: 把状态收回 Idle, 否则 UI 会一直显示"扫描中"
            // 却再也收不到包。错误经 on_event 报不出去(那是数据通道),
            // 只能落日志 —— UI 侧靠 mode() 变回 Idle 察觉。
            eprintln!("[ble] 扫描循环异常退出: {e:#}");
        }
        let mut st = STATE.lock().expect("STATE 被 poison");
        if st.mode == Mode::Scanning {
            st.mode = Mode::Idle;
            st.scan_task = None;
        }
    });

    let mut st = STATE.lock().expect("STATE 被 poison");
    st.mode = Mode::Scanning;
    st.scan_task = Some(task);
    Ok(())
}

async fn scan_loop<F>(on_event: &F) -> Result<()>
where
    F: Fn(AdvEvent) + Send + Sync + 'static,
{
    let central = adapter().await?;

    // ⚠ 必须【先订阅事件, 再启动扫描】。反过来的话, start_scan 到 events 之间
    //   收到的广播会全部丢失 —— btleplug 的 broadcast 通道对"当时还没有订阅者"
    //   的消息直接丢弃(common/adapter_manager.rs 的 Err(lost) 分支只打 trace)。
    //   这条是 scanner 那边踩过并写进注释的坑。
    let mut events = central.events().await.context("订阅扫描事件失败")?;

    // ScanFilter::default() = 不过滤。btleplug 的 ScanFilter 只能按 service UUID
    // 过滤, 而我们要按 Company ID —— 那是厂商自定义段, 只能自己筛。
    central
        .start_scan(ScanFilter::default())
        .await
        .context("启动扫描失败")?;

    while let Some(ev) = events.next().await {
        let Some((id, raw)) = extract(central, ev).await else {
            continue;
        };

        let (addr, rssi) = quick_props(central, &id).await;

        let (payload, parse_error) = match adv::parse(&raw) {
            Ok(p) => (Some(p.into()), None),
            Err(e) => {
                // 环境里有上百个设备在播(实测 78 事件/秒), 别人用同一个测试
                // Company ID 0xFFFF 是常事。魔数不符的直接丢, 不上报 UI ——
                // 否则列表会被别人的包刷满。
                if matches!(e, adv::AdvParseError::BadMagic { .. }) {
                    continue;
                }
                (None, Some(e.to_string()))
            }
        };

        on_event(AdvEvent {
            mac: addr.map(|a| a.to_string()),
            rssi: rssi.map(i32::from),
            raw,
            payload,
            parse_error,
        });
    }

    Ok(())
}

/// 停止扫描并交还适配器。未在扫描时安全。
pub fn scan_stop() -> Result<()> {
    let task = {
        let mut st = STATE.lock().expect("STATE 被 poison");
        if st.mode != Mode::Scanning {
            return Ok(());
        }
        st.mode = Mode::Idle;
        st.scan_task.take()
    };

    if let Some(t) = task {
        t.abort();
    }

    // abort 只是取消我们的循环, 适配器那边还在扫 —— 必须显式停, 否则会一直
    // 耗电并占着适配器, 后续连接可能失败。
    RT.block_on(async {
        if let Ok(central) = adapter().await {
            let _ = central.stop_scan().await;
        }
    });

    Ok(())
}

/// 已发现设备的去重表: (device_id, counter) → 是否见过。
///
/// 一轮 burst 里 15 个广播事件 × 3 信道 = 最多 45 个内容相同的包, `counter`
/// 在一轮内不变(固件刻意如此), 所以按它去重即可。
///
/// ⚠ 放在 Rust 侧而不是 Dart 侧: 去重要在数据进 Dart 之前做完, 否则 UI 每轮
///   要处理几十个重复事件, 白白过一次 FFI。
#[derive(Default)]
pub struct RoundDedup {
    seen: HashMap<u16, u8>,
}

impl RoundDedup {
    /// 返回 true 表示这是新一轮(应当上报), false 表示同一轮的重复包。
    pub fn is_new_round(&mut self, device_id: u16, counter: u8) -> bool {
        match self.seen.get(&device_id) {
            Some(&c) if c == counter => false,
            _ => {
                self.seen.insert(device_id, counter);
                true
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dedup_collapses_one_round() {
        let mut d = RoundDedup::default();
        // 一轮 burst: 同一 counter 的多个包只认第一个
        assert!(d.is_new_round(0x1234, 7));
        assert!(!d.is_new_round(0x1234, 7));
        assert!(!d.is_new_round(0x1234, 7));
        // 下一次单击 counter 变了 → 新一轮
        assert!(d.is_new_round(0x1234, 8));
        // 另一台设备独立计数
        assert!(d.is_new_round(0x5678, 8));
    }

    /// counter 是 8bit, 255 之后回绕到 0 —— 回绕必须被当成新一轮, 不能因为
    /// "0 < 255" 之类的比较而被吞掉。
    #[test]
    fn dedup_handles_counter_wrap() {
        let mut d = RoundDedup::default();
        assert!(d.is_new_round(1, 255));
        assert!(d.is_new_round(1, 0));
        assert!(!d.is_new_round(1, 0));
    }

    #[test]
    fn mode_starts_idle() {
        assert_eq!(Mode::default(), Mode::Idle);
    }
}

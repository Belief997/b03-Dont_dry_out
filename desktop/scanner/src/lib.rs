//! Shared helpers for the four diagnostic binaries.
//!
//! 这个 lib target 存在的唯一理由: 四个 bin 都要做同一件事 —— 探测本机蓝牙硬件
//! 状态。而"为什么报『有无适配器 + 蓝牙开没开』而不是报适配器型号"这段理由足够长,
//! 抄四遍必然漂移。

use anyhow::{bail, Context, Result};
use btleplug::api::{Central, CentralState, Manager as _};
use btleplug::platform::{Adapter, Manager};

/// 关于本机蓝牙硬件, 我们真正能问出来的事。
///
/// ⚠ 刻意【不】报"具体是哪个适配器"。btleplug 在 Windows 上的 `adapter_info()`
///   是硬编码返回 "WinRT" 的 —— winrtble 后端源码里就写着
///   `// TODO: Get information about the adapter.` —— 打出来看着像信息, 实际
///   什么都没说。
///
/// 而排查"扫描收不到东西"时真正有用的两件事是可以分开判断的, 因为
/// `Manager::adapters()` 枚举的是 `Radio::GetRadiosAsync()` 过滤
/// `RadioKind::Bluetooth`, 蓝牙关着的 radio 一样会被列出来:
///
///     列表为空              -> 根本没有适配器
///     列出 + PoweredOn      -> 可以扫
///     列出 + PoweredOff     -> 硬件在, 但蓝牙被关掉了
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AdapterStatus {
    /// 没有蓝牙适配器。
    Absent,
    /// 有适配器, 但蓝牙是关的。
    PoweredOff,
    /// 有适配器且蓝牙已打开。
    Ready,
    /// 有适配器, 但 radio 状态读不出来(非 Windows 后端可能如此)。
    Unknown,
}

impl std::fmt::Display for AdapterStatus {
    /// 两栏定宽, 便于四个工具的首行输出长得一样。
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            Self::Absent => "adapter: none      bluetooth: n/a",
            Self::PoweredOff => "adapter: present   bluetooth: OFF",
            Self::Ready => "adapter: present   bluetooth: on",
            Self::Unknown => "adapter: present   bluetooth: unknown",
        };
        f.write_str(s)
    }
}

/// 探测硬件, 打印那两条事实, 并交回一个可用于扫描的 adapter。
///
/// 只在"扫描不可能成功"时报错退出。特别地, 蓝牙关着时【必须】报错而不是继续 ——
/// 否则程序会正常跑完却一条都收不到, 而这正是最费时间的那种假象。
///
/// ⚠ `Unknown` 不报错: 只有 Windows 后端保证能读到 radio 状态, bluez /
///   CoreBluetooth 上读不出来也照样能扫, 为此拒绝启动是过度反应。
pub async fn open_adapter() -> Result<Adapter> {
    let manager = Manager::new()
        .await
        .context("failed to create the Bluetooth manager")?;
    let adapters = manager
        .adapters()
        .await
        .context("failed to enumerate Bluetooth adapters")?;

    let Some(central) = adapters.into_iter().next() else {
        println!("{}", AdapterStatus::Absent);
        bail!("no Bluetooth adapter on this machine");
    };

    let status = match central.adapter_state().await {
        Ok(CentralState::PoweredOn) => AdapterStatus::Ready,
        Ok(CentralState::PoweredOff) => AdapterStatus::PoweredOff,
        _ => AdapterStatus::Unknown,
    };
    println!("{}", status);

    if status == AdapterStatus::PoweredOff {
        bail!("Bluetooth is switched off -- turn it on, then run this again");
    }

    Ok(central)
}

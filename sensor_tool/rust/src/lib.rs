//! sensor_tool 的 Rust core —— BLE 与协议编解码。
//!
//! ```text
//!   Dart (Flutter UI)
//!        │  flutter_rust_bridge
//!   ─────┼──────────────────────────────────────────────
//!        ▼
//!   api/          FRB 边界: DTO + 薄转换, 不放业务判断
//!        │
//!        ├──▶ ble/     独占蓝牙适配器, 扫描/连接互斥的状态机
//!        │              (btleplug + 自持 tokio runtime)
//!        └──▶ proto/   纯字节编解码, 不碰 IO, 全部可 cargo test
//!                        ├── adv  广播协议 v0x02  (dev/note/05)
//!                        └── cmd  命令协议 v1     (dev/note/06)
//! ```
//!
//! 分层的用意: `proto` 不依赖 `ble`, 于是协议逻辑能在**没有真设备**的情况下跑
//! 单元测试 —— 而这恰恰是最容易出错又最难在设备上调试的部分(多帧重组、
//! 24bit 符号扩展、丢帧检测)。

pub mod api;
pub mod ble;
pub mod proto;

mod frb_generated;

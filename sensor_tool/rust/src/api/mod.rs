//! FRB 的代码生成入口。
//!
//! `flutter_rust_bridge.yaml` 里 `rust_input: crate::api` —— **只有这个目录下的
//! 公开项会被生成到 Dart**。内部实现放 `crate::ble` / `crate::proto`,
//! 本目录只放 DTO 与薄薄一层转换。

pub mod ble;
pub mod simple;

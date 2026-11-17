//! MinIME 原生网络层（Rust），兼容微信输入法的网络协议。
//!
//! 协议核心（pb / crypto / identity / session）不依赖 daemon，将来可以编成
//! staticlib/cdylib 直接链进 engined，或作为 ime_net.dll 的 `net_*` 替身。

pub mod ab;
pub mod clipboard;
pub mod config;
pub mod crypto;
pub mod hotwords;
pub mod identity;
pub mod legacy;
pub mod migrate;
pub mod pb;
pub mod session;

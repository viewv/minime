//! 配置：和 fcitx5 配置面板共用 ~/.config/fcitx5/conf/minime.conf 的 [Network] 段。
//!
//! ```ini
//! [Network]
//! Enabled=False        # 总开关，默认关：关着时完全不联网
//! ClipboardSync=True   # 跨设备剪贴板（文本）
//! ImageSync=False      # 跨设备图片（经腾讯云 COS 中转，链接泄露即可被下载，默认关）
//! DeviceName=Linux     # 显示在对端的设备名（不会自动取主机名）
//! UserDictSync=False   # 同步个人词库（引擎自己做，词库经腾讯云 COS 中转，默认关）
//! ```

use std::path::PathBuf;
use std::time::SystemTime;

#[derive(Debug, Clone, PartialEq)]
pub struct Config {
    pub enabled: bool,
    pub clipboard: bool,
    pub images: bool,
    /// 云输入（实验）：放行引擎的云请求（cmd 4329 等）
    pub cloud: bool,
    /// 同步个人词库：放行引擎的组同步请求（4814/41、42、44……）和 COS 上传下载
    pub user_dict: bool,
    pub device_name: String,
}

impl Default for Config {
    fn default() -> Self {
        Config { enabled: false, clipboard: true, images: false, cloud: false, user_dict: false, device_name: "Linux".into() }
    }
}

pub fn path() -> PathBuf {
    if let Ok(p) = std::env::var("MINIME_NETD_CONFIG") {
        return PathBuf::from(p);
    }
    let base = std::env::var("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".config"));
    base.join("fcitx5/conf/minime.conf")
}

fn parse_bool(v: &str) -> Option<bool> {
    match v.trim() {
        "True" | "true" | "1" | "yes" => Some(true),
        "False" | "false" | "0" | "no" => Some(false),
        _ => None,
    }
}

pub fn parse(text: &str) -> Config {
    let mut c = Config::default();
    let mut in_net = false;
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if line.starts_with('[') {
            in_net = line == "[Network]";
            continue;
        }
        if !in_net {
            continue;
        }
        let Some((k, v)) = line.split_once('=') else { continue };
        match k.trim() {
            "Enabled" => c.enabled = parse_bool(v).unwrap_or(c.enabled),
            "ClipboardSync" => c.clipboard = parse_bool(v).unwrap_or(c.clipboard),
            "ImageSync" => c.images = parse_bool(v).unwrap_or(c.images),
            "CloudInput" => c.cloud = parse_bool(v).unwrap_or(c.cloud),
            "UserDictSync" => c.user_dict = parse_bool(v).unwrap_or(c.user_dict),
            "DeviceName" => {
                let v = v.trim();
                if !v.is_empty() {
                    c.device_name = v.chars().take(32).collect();
                }
            }
            _ => {}
        }
    }
    c
}

/// 读配置并记下 mtime，用于轮询检测变化。
pub fn load() -> (Config, Option<SystemTime>) {
    let p = path();
    let mtime = std::fs::metadata(&p).and_then(|m| m.modified()).ok();
    let cfg = std::fs::read_to_string(&p).map(|t| parse(&t)).unwrap_or_default();
    (cfg, mtime)
}

pub fn mtime() -> Option<SystemTime> {
    std::fs::metadata(path()).and_then(|m| m.modified()).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_sections() {
        let t = "# Traditional=False\nPageSize=9\n[Fuzzy]\nEnabled=True\n[Network]\n# 注释\nEnabled=True\nDeviceName= 书房 Linux \n";
        let c = parse(t);
        assert!(c.enabled);
        assert!(c.clipboard);
        assert_eq!(c.device_name, "书房 Linux");
        assert!(!parse("[Fuzzy]\nEnabled=True\n").enabled);
        assert!(!parse("[Network]\n# Enabled=True\n").enabled);
    }
}

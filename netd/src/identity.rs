//! 匿名设备身份：注册一次后落盘复用（与 开发时的 Python 参考实现（未公开） 的 JSON 格式兼容）。

use std::io::Write;
use std::path::{Path, PathBuf};

use md5::{Digest, Md5};
use rand::Rng;
use serde::{Deserialize, Serialize};

use crate::crypto;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Identity {
    pub platform: u32,
    pub uin: u64,
    pub device_code: String,
    pub app_ver: String,
    pub system_ver: String,
    pub device_name: String,
    #[serde(default = "d_channel")]
    pub channel: u32,
    #[serde(default = "d_one")]
    pub package_type: u32,
    #[serde(default = "d_scene")]
    pub scene: u32,
    #[serde(default)]
    pub os_type: u32,
    #[serde(default)]
    pub uuid: String,
    #[serde(default)]
    pub cached_server_pub: String,
}

fn d_channel() -> u32 {
    3
}
fn d_one() -> u32 {
    1
}
fn d_scene() -> u32 {
    2
}

/// Linux 端以 Windows 客户端（平台 6）的身份接入：协议与 Windows 版一致。
pub const PLATFORM_WINDOWS: u32 = 6;
const WIN_APP_VER: &str = "3.0.1.35";
const WIN_SYS_VER: &str = "10";

const ALNUM: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

/// 设备码：前缀 + 名字（右对齐补 0 到 17 位）+ 12 位随机 + MD5(前面部分 + 字母表)，共 64 字符。
pub fn device_code(platform: u32, name: &str) -> String {
    let prefix = match platform {
        1 => "AND",
        2 => "IOS",
        5 => "MAC",
        6 => "WIN",
        _ => "HAR",
    };
    // 设备码只放 ASCII 字母数字，长度固定 17
    let clean: String = name.chars().filter(|c| c.is_ascii_alphanumeric()).take(17).collect();
    let mut head = format!("{prefix}{clean:0>17}");
    let mut r = rand::thread_rng();
    for _ in 0..12 {
        head.push(ALNUM[r.gen_range(0..ALNUM.len())] as char);
    }
    let digest = hex::encode_upper(Md5::digest(format!("{head}{}", crypto::sign_key(platform)).as_bytes()));
    head + &digest
}

fn uuid4() -> String {
    let mut b: [u8; 16] = rand::thread_rng().gen();
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    let h = hex::encode(b);
    format!("{}-{}-{}-{}-{}", &h[0..8], &h[8..12], &h[12..16], &h[16..20], &h[20..32])
}

impl Identity {
    pub fn fresh(device_name: &str) -> Identity {
        Identity {
            platform: PLATFORM_WINDOWS,
            uin: 0,
            device_code: device_code(PLATFORM_WINDOWS, device_name),
            app_ver: WIN_APP_VER.into(),
            system_ver: WIN_SYS_VER.into(),
            device_name: device_name.into(),
            channel: 3,
            package_type: 1,
            scene: 2,
            os_type: PLATFORM_WINDOWS,
            uuid: uuid4(),
            cached_server_pub: String::new(),
        }
    }
}

pub fn data_dir() -> PathBuf {
    if let Ok(d) = std::env::var("MINIME_NETD_DATA") {
        return PathBuf::from(d);
    }
    let base = std::env::var("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"));
    base.join("minime/net")
}

pub fn identity_path() -> PathBuf {
    data_dir().join("identity.json")
}

pub fn load(path: &Path) -> Option<Identity> {
    let text = std::fs::read_to_string(path).ok()?;
    let mut id: Identity = serde_json::from_str(&text).ok()?;
    if id.os_type == 0 {
        id.os_type = id.platform;
    }
    Some(id)
}

/// 原子写 + 0600：身份等同于这台设备的凭证。
pub fn save(path: &Path, id: &Identity) -> std::io::Result<()> {
    use std::os::unix::fs::OpenOptionsExt;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("json.tmp");
    {
        let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
        f.write_all(serde_json::to_string_pretty(id)?.as_bytes())?;
        f.write_all(b"\n")?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_code_shape() {
        let c = device_code(6, "Linux PC");
        assert_eq!(c.len(), 64);
        assert!(c.starts_with("WIN0000000000LinuxPC"));
        let head = &c[..32];
        let want = hex::encode_upper(Md5::digest(format!("{head}{}", crate::crypto::sign_key(6)).as_bytes()));
        assert_eq!(&c[32..], want);
        assert_eq!(device_code(6, "我的电脑").len(), 64);
    }

    #[test]
    fn python_json_compat() {
        let j = r#"{"platform": 1, "uin": 8848, "device_code": "x", "app_ver": "4.0.1",
                    "system_ver": "14", "device_name": "Pixel 7", "channel": 3, "package_type": 1,
                    "scene": 2, "os_type": 1, "uuid": "u", "cached_server_pub": ""}"#;
        let id: Identity = serde_json::from_str(j).unwrap();
        assert_eq!(id.uin, 8848);
    }
}

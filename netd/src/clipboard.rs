//! Wayland 剪贴板：用 wl-clipboard（wl-paste --watch 监听、wl-copy 写入）。
//! KDE/Wlroots 都支持 data-control 协议，不需要窗口焦点。

use std::fmt;
use std::os::unix::fs::FileTypeExt;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, SystemTime};

use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::UnixStream;
use tokio::process::Command;
use tokio::sync::mpsc;

/// 文本直发（type 1）的上限。手机实测会把 146 KB 的长文本直接用 type 1 发，所以不必走 COS。
pub const MAX_TEXT: usize = 256 << 10;

/// 密码管理器标记的内容不同步（KeePassXC 等会设这个 MIME 类型）。
const SECRET_HINT: &str = "x-kde-passwordManagerHint";

#[derive(Debug)]
pub enum WatchError {
    NoWayland,
    NotInstalled,
    Io(std::io::Error),
}

impl fmt::Display for WatchError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NoWayland => f.write_str("no wayland display"),
            Self::NotInstalled => f.write_str("wl-paste not installed"),
            Self::Io(e) => e.fmt(f),
        }
    }
}

static CACHED_DISPLAY: OnceLock<Mutex<Option<String>>> = OnceLock::new();

fn cached_display() -> &'static Mutex<Option<String>> {
    CACHED_DISPLAY.get_or_init(|| Mutex::new(None))
}

fn wayland_socket_path(runtime_dir: Option<&Path>, display: &str) -> Option<PathBuf> {
    if display.is_empty() || display.contains('\0') {
        return None;
    }
    if display.starts_with('/') {
        Some(PathBuf::from(display))
    } else {
        runtime_dir.map(|d| d.join(display))
    }
}

async fn wayland_socket_live(runtime_dir: Option<&Path>, display: &str) -> bool {
    let Some(path) = wayland_socket_path(runtime_dir, display) else {
        return false;
    };
    if !std::fs::metadata(&path).map(|m| m.file_type().is_socket()).unwrap_or(false) {
        return false;
    }
    matches!(
        tokio::time::timeout(Duration::from_millis(200), UnixStream::connect(&path)).await,
        Ok(Ok(_))
    )
}

fn parse_systemd_wayland_display(stdout: &str) -> Option<String> {
    stdout.lines().find_map(|line| {
        let val = line.strip_prefix("WAYLAND_DISPLAY=")?.trim_end_matches('\r');
        (!val.is_empty() && !val.contains('\0')).then(|| val.to_string())
    })
}

async fn systemd_wayland_display() -> Option<String> {
    let out = tokio::time::timeout(
        Duration::from_secs(1),
        Command::new("systemctl")
            .args(["--user", "show-environment"])
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .kill_on_drop(true)
            .output(),
    )
    .await
    .ok()?
    .ok()?;
    out.status
        .success()
        .then(|| parse_systemd_wayland_display(&String::from_utf8_lossy(&out.stdout)))?
}

fn scan_wayland_sockets(dir: &Path) -> Vec<String> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut found: Vec<(Option<SystemTime>, String)> = entries
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().into_string().ok()?;
            if !name.starts_with("wayland-") || name.ends_with(".lock") {
                return None;
            }
            let meta = std::fs::metadata(e.path()).ok()?;
            meta.file_type().is_socket().then(|| (meta.modified().ok(), name))
        })
        .collect();
    found.sort_by(|a, b| b.0.cmp(&a.0).then_with(|| a.1.cmp(&b.1)));
    found.into_iter().map(|(_, name)| name).collect()
}

async fn probe_and_cache(rdir: Option<&Path>, disp: &str) -> bool {
    if !wayland_socket_live(rdir, disp).await {
        return false;
    }
    *cached_display().lock().unwrap() = Some(disp.to_string());
    true
}

/// 动态解析可用的 Wayland 显示名：适应开机抢跑和合成器崩溃重启后切换 socket 的情况。
async fn resolve_wayland_display() -> Option<String> {
    let runtime_dir = std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|s| !s.is_empty())
        .map(PathBuf::from);
    let rdir = runtime_dir.as_deref();
    let cached = cached_display().lock().unwrap().clone();
    if let Some(d) = cached {
        if wayland_socket_live(rdir, &d).await {
            return Some(d);
        }
    }
    if let Ok(d) = std::env::var("WAYLAND_DISPLAY") {
        if probe_and_cache(rdir, &d).await {
            return Some(d);
        }
    }
    if let Some(d) = systemd_wayland_display().await {
        if probe_and_cache(rdir, &d).await {
            return Some(d);
        }
    }
    if let Some(dir) = rdir {
        for d in scan_wayland_sockets(dir) {
            if probe_and_cache(rdir, &d).await {
                return Some(d);
            }
        }
    }
    *cached_display().lock().unwrap() = None;
    None
}

/// 启动监听：剪贴板每变一次，通道收到一个 ()。子进程随接收端一起结束。
pub async fn watch() -> Result<mpsc::UnboundedReceiver<()>, WatchError> {
    let disp = resolve_wayland_display().await.ok_or(WatchError::NoWayland)?;
    let mut child = Command::new("wl-paste")
        .env("WAYLAND_DISPLAY", &disp)
        .args(["--watch", "echo", "x"])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .kill_on_drop(true)
        .spawn()
        .map_err(|e| {
            if e.kind() == std::io::ErrorKind::NotFound {
                WatchError::NotInstalled
            } else {
                WatchError::Io(e)
            }
        })?;
    let stdout = child.stdout.take().expect("piped stdout");
    let (tx, rx) = mpsc::unbounded_channel();
    tokio::spawn(async move {
        let _child = child; // 持有到任务结束，drop 时杀掉
        let mut lines = BufReader::new(stdout).lines();
        while let Ok(Some(_)) = lines.next_line().await {
            if tx.send(()).is_err() {
                break;
            }
        }
    });
    Ok(rx)
}

async fn run(display: &str, args: &[&str]) -> Option<Vec<u8>> {
    let out = tokio::time::timeout(
        Duration::from_secs(2),
        Command::new("wl-paste")
            .env("WAYLAND_DISPLAY", display)
            .args(args)
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .kill_on_drop(true)
            .output(),
    )
    .await
    .ok()?
    .ok()?;
    out.status.success().then_some(out.stdout)
}

/// 图片上限（COS 有自己的上限，sub 60 会返回）。
pub const MAX_IMAGE: usize = 20 << 20;

pub enum Content {
    Text(String),
    Image { mime: &'static str, data: Vec<u8> },
    /// 密码、空、过大、不支持的类型等不同步的内容
    Skip(&'static str),
}

const IMAGE_TYPES: [&str; 4] = ["image/png", "image/jpeg", "image/gif", "image/webp"];

/// 读当前剪贴板。`want_image` 为假时只看文本。
pub async fn read(want_image: bool) -> Content {
    let Some(disp) = resolve_wayland_display().await else {
        return Content::Skip("no wayland");
    };
    let Some(types) = run(&disp, &["--list-types"]).await else { return Content::Skip("empty") };
    let types = String::from_utf8_lossy(&types);
    let has = |m: &str| types.lines().any(|t| t.trim() == m);
    if has(SECRET_HINT) {
        return Content::Skip("password manager");
    }
    // 截图等纯图片内容；复制文件时会同时有 text/uri-list，那种按文本处理
    if want_image && !has("text/uri-list") {
        if let Some(mime) = IMAGE_TYPES.iter().copied().find(|m| has(m)) {
            let Some(data) = run(&disp, &["--type", mime]).await else { return Content::Skip("read failed") };
            if data.is_empty() || data.len() > MAX_IMAGE {
                return Content::Skip("image empty or too large");
            }
            return Content::Image { mime, data };
        }
    }
    let mime = if has("text/plain;charset=utf-8") {
        "text/plain;charset=utf-8"
    } else if has("text/plain") || has("UTF8_STRING") {
        "text/plain"
    } else {
        return Content::Skip("not text");
    };
    let Some(data) = run(&disp, &["--no-newline", "--type", mime]).await else { return Content::Skip("read failed") };
    // 二次校验：防止 --list-types 与实际读取之间切进了密码管理器内容（TOCTOU）
    if let Some(after) = run(&disp, &["--list-types"]).await {
        let after = String::from_utf8_lossy(&after);
        if after.lines().any(|t| t.trim() == SECRET_HINT) {
            return Content::Skip("password manager");
        }
    }
    if data.is_empty() {
        return Content::Skip("empty");
    }
    if data.len() > MAX_TEXT {
        return Content::Skip("too large");
    }
    match String::from_utf8(data) {
        Ok(s) => Content::Text(s),
        Err(_) => Content::Skip("not utf-8"),
    }
}

/// 按文件头判断图片类型（只接受常见格式）。
pub fn image_mime(data: &[u8]) -> Option<&'static str> {
    if data.starts_with(b"\x89PNG\r\n\x1a\n") {
        Some("image/png")
    } else if data.starts_with(&[0xff, 0xd8, 0xff]) {
        Some("image/jpeg")
    } else if data.starts_with(b"GIF8") {
        Some("image/gif")
    } else if data.len() > 12 && &data[0..4] == b"RIFF" && &data[8..12] == b"WEBP" {
        Some("image/webp")
    } else {
        None
    }
}

async fn copy(mime: &str, data: &[u8]) -> std::io::Result<()> {
    let disp = resolve_wayland_display()
        .await
        .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::NotConnected, "no wayland display"))?;
    let mut child = Command::new("wl-copy")
        .env("WAYLAND_DISPLAY", &disp)
        .args(["--type", mime])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .kill_on_drop(true)
        .spawn()?;
    if let Some(mut stdin) = child.stdin.take() {
        stdin.write_all(data).await?;
    }
    // wl-copy 读完输入就 fork 到后台持有剪贴板，前台进程马上退出
    match tokio::time::timeout(Duration::from_secs(3), child.wait()).await {
        Ok(Ok(status)) if status.success() => Ok(()),
        Ok(Ok(status)) => Err(std::io::Error::other(format!("wl-copy exited with {status}"))),
        Ok(Err(e)) => Err(e),
        Err(_) => Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "wl-copy timeout")),
    }
}

pub async fn write_image(mime: &str, data: &[u8]) -> std::io::Result<()> {
    copy(mime, data).await
}

pub async fn write_text(text: &str) -> std::io::Result<()> {
    copy("text/plain;charset=utf-8", text.as_bytes()).await
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::net::UnixListener;

    #[test]
    fn parse_systemd_env_extracts_wayland_display() {
        assert_eq!(parse_systemd_wayland_display("WAYLAND_DISPLAY=wayland-1\n"), Some("wayland-1".into()));
        assert_eq!(parse_systemd_wayland_display("WAYLAND_DISPLAY=\n"), None);
        assert_eq!(parse_systemd_wayland_display("DISPLAY=:0\nHOME=/home/u\n"), None);
        assert_eq!(
            parse_systemd_wayland_display("DISPLAY=:0\nWAYLAND_DISPLAY=wayland-2\r\nXDG_SESSION_TYPE=wayland\n"),
            Some("wayland-2".into())
        );
    }

    #[tokio::test]
    async fn scan_and_probe_wayland_sockets() {
        let dir = std::env::temp_dir().join(format!("minime-wl-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();

        let dead = dir.join("wayland-0");
        drop(UnixListener::bind(&dead).unwrap());
        let live_listener = UnixListener::bind(dir.join("wayland-1")).unwrap();
        std::fs::write(dir.join("wayland-1.lock"), b"").unwrap();

        let scanned = scan_wayland_sockets(&dir);
        assert!(!scanned.iter().any(|n| n == "wayland-1.lock"));
        assert!(scanned.iter().any(|n| n == "wayland-0"));
        assert!(scanned.iter().any(|n| n == "wayland-1"));

        assert!(!wayland_socket_live(Some(&dir), "wayland-0").await);
        assert!(wayland_socket_live(Some(&dir), "wayland-1").await);

        drop(live_listener);
        let _ = std::fs::remove_dir_all(&dir);
    }
}

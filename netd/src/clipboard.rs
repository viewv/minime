//! Wayland 剪贴板：用 wl-clipboard（wl-paste --watch 监听、wl-copy 写入）。
//! KDE/Wlroots 都支持 data-control 协议，不需要窗口焦点。

use std::process::Stdio;
use std::time::Duration;

use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::Command;
use tokio::sync::mpsc;

/// 文本直发（type 1）的上限。手机实测会把 146 KB 的长文本直接用 type 1 发，所以不必走 COS。
pub const MAX_TEXT: usize = 256 << 10;

/// 密码管理器标记的内容不同步（KeePassXC 等会设这个 MIME 类型）。
const SECRET_HINT: &str = "x-kde-passwordManagerHint";

/// 启动监听：剪贴板每变一次，通道收到一个 ()。子进程随接收端一起结束。
pub fn watch() -> std::io::Result<mpsc::UnboundedReceiver<()>> {
    let mut child = Command::new("wl-paste")
        .args(["--watch", "echo", "x"])
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .kill_on_drop(true)
        .spawn()?;
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

async fn run(args: &[&str]) -> Option<Vec<u8>> {
    let out = tokio::time::timeout(
        Duration::from_secs(2),
        Command::new("wl-paste").args(args).stderr(Stdio::null()).kill_on_drop(true).output(),
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
    let Some(types) = run(&["--list-types"]).await else { return Content::Skip("empty") };
    let types = String::from_utf8_lossy(&types);
    let has = |m: &str| types.lines().any(|t| t.trim() == m);
    if has(SECRET_HINT) {
        return Content::Skip("password manager");
    }
    // 截图等纯图片内容；复制文件时会同时有 text/uri-list，那种按文本处理
    if want_image && !has("text/uri-list") {
        if let Some(mime) = IMAGE_TYPES.iter().copied().find(|m| has(m)) {
            let Some(data) = run(&["--type", mime]).await else { return Content::Skip("read failed") };
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
    let Some(data) = run(&["--no-newline", "--type", mime]).await else { return Content::Skip("read failed") };
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
    let mut child = Command::new("wl-copy")
        .args(["--type", mime])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;
    if let Some(mut stdin) = child.stdin.take() {
        stdin.write_all(data).await?;
    }
    // wl-copy 读完输入就 fork 到后台持有剪贴板，前台进程马上退出
    let _ = tokio::time::timeout(Duration::from_secs(3), child.wait()).await;
    Ok(())
}

pub async fn write_image(mime: &str, data: &[u8]) -> std::io::Result<()> {
    copy(mime, data).await
}

pub async fn write_text(text: &str) -> std::io::Result<()> {
    copy("text/plain;charset=utf-8", text.as_bytes()).await
}

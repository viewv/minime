//! 和 minime-netd 的控制 socket 通信（一行命令，回若干行 JSON）。
//! 全部在后台线程里做，界面线程只读共享状态 / 收通道消息。

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{json, Value};

pub fn socket_path() -> PathBuf {
    if let Ok(p) = std::env::var("MINIME_NETD_SOCKET") {
        if !p.is_empty() {
            return PathBuf::from(p);
        }
    }
    let dir = std::env::var("XDG_RUNTIME_DIR")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "/tmp".into());
    PathBuf::from(dir).join("minime-netd.sock")
}

fn not_running() -> Value {
    json!({"type": "status", "running": false, "connected": false, "reason": "服务未运行"})
}

/// 发一条命令，把每行回复交给 `each`；返回 Ok(()) 表示连上了服务。
pub fn request(cmd: &str, timeout: Option<Duration>, mut each: impl FnMut(Value)) -> std::io::Result<()> {
    let mut s = UnixStream::connect(socket_path())?;
    s.set_read_timeout(timeout)?;
    s.write_all(format!("{cmd}\n").as_bytes())?;
    for line in BufReader::new(s).lines() {
        let line = line?;
        if let Ok(v) = serde_json::from_str::<Value>(&line) {
            each(v);
        }
    }
    Ok(())
}

/// 后台每 2 秒（或被 kick 时立刻）刷新一次状态。
pub fn spawn_status_poller() -> (Arc<Mutex<Value>>, mpsc::Sender<()>) {
    let shared = Arc::new(Mutex::new(not_running()));
    let (kick_tx, kick_rx) = mpsc::channel::<()>();
    let out = shared.clone();
    std::thread::spawn(move || loop {
        let mut got = None;
        let _ = request("status", Some(Duration::from_secs(5)), |v| got = Some(v));
        *out.lock().unwrap() = got.unwrap_or_else(not_running);
        match kick_rx.recv_timeout(Duration::from_secs(2)) {
            Ok(()) | Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    });
    (shared, kick_tx)
}

/// 一次关联：在后台线程里跑 `pair [码]`，事件逐条送回。
/// 返回的 UnixStream 克隆用于取消（shutdown 后服务端会释放匹配码）。
pub fn spawn_pair(code: Option<String>, tx: mpsc::Sender<Value>) -> Option<UnixStream> {
    spawn_stream(
        match code {
            Some(c) => format!("pair {c}"),
            None => "pair".into(),
        },
        tx,
    )
}

/// 长时间的命令（pair、migrate）：后台线程里逐条收事件；关掉返回的连接即取消。
pub fn spawn_stream(cmd: String, tx: mpsc::Sender<Value>) -> Option<UnixStream> {
    let s = match UnixStream::connect(socket_path()) {
        Ok(s) => s,
        Err(_) => {
            let _ = tx.send(json!({"type": "error", "message": "网络服务未运行"}));
            return None;
        }
    };
    let cancel = s.try_clone().ok();
    std::thread::spawn(move || {
        let mut s = s;
        if s.write_all(format!("{cmd}\n").as_bytes()).is_err() {
            let _ = tx.send(json!({"type": "error", "message": "发送命令失败，网络服务连接已断开"}));
            return;
        }
        let mut done = false;
        for line in BufReader::new(s).lines().map_while(Result::ok) {
            if let Ok(v) = serde_json::from_str::<Value>(&line) {
                if matches!(v["type"].as_str(), Some("paired" | "migrated" | "error")) {
                    done = true;
                }
                if tx.send(v).is_err() {
                    return;
                }
            }
        }
        if !done {
            let _ = tx.send(json!({"type": "error", "message": "与网络服务的连接已断开"}));
        }
    });
    cancel
}

/// 在后台线程里执行一条短命令（unpair 等），完成后 kick 状态刷新。
pub fn spawn_command(cmd: String, tx: mpsc::Sender<Value>, kick: mpsc::Sender<()>) {
    std::thread::spawn(move || {
        let r = request(&cmd, Some(Duration::from_secs(30)), |v| {
            let _ = tx.send(v);
        });
        if r.is_err() {
            let _ = tx.send(json!({"type": "error", "message": "网络服务未运行"}));
        }
        let _ = kick.send(());
    });
}

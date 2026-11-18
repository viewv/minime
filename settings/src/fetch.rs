//! 首次运行引导：在后台跑 `minime-fetch … --json`，把它的事件（每行一个 JSON）送回界面线程。
//! 官方文件（引擎、整句模型、语音模型）由每个用户从腾讯官方地址下载，MinIME 不分发。

use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{mpsc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

/// 三项官方文件：(id, 名称, 说明, 下载大小)
pub const ITEMS: [(&str, &str, &str, &str); 3] = [
    ("engine", "输入法引擎和词库", "必需。来自微信输入法 Windows 安装包，解包后约 270 MB", "约 218 MB"),
    ("e2e", "整句模型", "推荐。整句候选用的模型，来自 macOS 安装包（只取需要的 3 个文件）", "约 78 MB"),
    ("voice", "离线语音模型", "可选。按住右 Alt 说话用，完全在本机识别", "约 103 MB"),
];

fn data_home() -> PathBuf {
    std::env::var("XDG_DATA_HOME")
        .ok()
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"))
}

/// 和 minime-fetch 里的判断一致
pub fn installed(id: &str) -> bool {
    let d = data_home().join("minime");
    match id {
        "engine" => {
            let e = d.join("official/win/3.0.1.35");
            e.join("libwxhld.dll").is_file() && e.join("BuildIn/index.json").is_file()
        }
        "e2e" => ["index.json", "mpd_e2e.bin", "decgnr.bin"].iter().all(|f| d.join("official/mac/imeData").join(f).is_file()),
        "voice" => std::fs::read_dir(d.join("asr/model"))
            .map(|r| r.flatten().any(|e| e.file_name().to_string_lossy().ends_with(".xnet")))
            .unwrap_or(false),
        _ => false,
    }
}

/// 引擎服务实际用的引擎目录：引擎启动时把加载的词库路径写进 dicts/core_state.tsv（…/<引擎目录>/BuildIn/xx.bin）。
/// 开发环境的服务直接用仓库里的 vendor/，不在 minime-fetch 的下载目录里。
pub fn engine_dir_in_use() -> Option<PathBuf> {
    let text = std::fs::read_to_string(data_home().join("minime/dicts/core_state.tsv")).ok()?;
    text.lines().find_map(|l| {
        let p = PathBuf::from(l.split('\t').nth(4)?);
        let dir = p.parent()?.parent()?.to_path_buf();
        dir.join("libwxhld.dll").is_file().then_some(dir)
    })
}

fn engined_service_env() -> String {
    static CACHE: Mutex<Option<(Instant, String)>> = Mutex::new(None);
    if let Ok(g) = CACHE.lock() {
        if let Some((at, val)) = &*g {
            if at.elapsed() < Duration::from_secs(5) {
                return val.clone();
            }
        }
    }
    let env = Command::new("systemctl")
        .args(["--user", "show", "minime-engined.service", "-p", "Environment", "--value"])
        .output()
        .ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default();
    if let Ok(mut g) = CACHE.lock() {
        *g = Some((Instant::now(), env.clone()));
    }
    env
}

/// 引擎服务用的整句模型目录，和 engined 的选法一致：服务环境里的 MINIME_E2E_DIR → 下载目录 → <引擎目录>/BuildIn
pub fn e2e_dir_in_use() -> Option<PathBuf> {
    let has = |d: &PathBuf| d.join("decgnr.bin").is_file() && d.join("mpd_e2e.bin").is_file();
    let env = engined_service_env();
    if let Some(d) = env.split_whitespace().find_map(|kv| kv.strip_prefix("MINIME_E2E_DIR=")) {
        let d = PathBuf::from(d);
        return has(&d).then_some(d);
    }
    let d = data_home().join("minime/official/mac/imeData");
    if has(&d) {
        return Some(d);
    }
    engine_dir_in_use().map(|e| e.join("BuildIn")).filter(has)
}

/// 能不能用：minime-fetch 下载过，或者引擎服务正在用别处的文件（开发环境）。
/// 返回 (能用, 别处的目录——不是下载来的时候给出)
pub fn ready(id: &str) -> (bool, Option<PathBuf>) {
    if installed(id) {
        return (true, None);
    }
    let other = match id {
        "engine" => engine_dir_in_use(),
        "e2e" => e2e_dir_in_use(),
        _ => None,
    };
    (other.is_some(), other)
}

/// 找 minime-fetch：$MINIME_FETCH → PATH → 开发时仓库里的 scripts/（settings/target/release/ 往上三级）
pub fn fetch_bin() -> Option<PathBuf> {
    static BIN: OnceLock<Option<PathBuf>> = OnceLock::new();
    BIN.get_or_init(|| {
        if let Some(p) = std::env::var_os("MINIME_FETCH") {
            return Some(PathBuf::from(p));
        }
        if let Some(path) = std::env::var_os("PATH") {
            for dir in std::env::split_paths(&path) {
                let p = dir.join("minime-fetch");
                if p.is_file() {
                    return Some(p);
                }
            }
        }
        let exe = std::env::current_exe().ok()?;
        let p = exe.parent()?.parent()?.parent()?.parent()?.join("scripts/minime-fetch");
        p.is_file().then_some(p)
    })
    .clone()
}

/// 开始一次下载（`items` 为空时只做配置：`minime-fetch setup`）。
/// 返回子进程（取消时 kill）；事件从 `tx` 送回，进程结束时补一条 `{"ev":"exit","code":…}`。
pub fn spawn(items: &[&str], tx: mpsc::Sender<Value>) -> Result<Child, String> {
    let bin = fetch_bin().ok_or("没有找到 minime-fetch（MinIME 的软件包装好了吗？）")?;
    let mut cmd = Command::new(&bin);
    if items.is_empty() {
        cmd.arg("setup");
    } else {
        cmd.args(items);
    }
    cmd.arg("--json");
    // 调试用：例如 MINIME_FETCH_ARGS=--no-setup
    if let Ok(extra) = std::env::var("MINIME_FETCH_ARGS") {
        cmd.args(extra.split_whitespace());
    }
    // 自己一个进程组：取消时连它启动的 7z / curl / minime-asrd 一起结束
    std::os::unix::process::CommandExt::process_group(&mut cmd, 0);
    let mut child = cmd
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("运行 {} 失败：{e}", bin.display()))?;
    let out = child.stdout.take().unwrap();
    let err = child.stderr.take().unwrap();
    let etx = tx.clone();
    // stderr：没走 JSON 的输出（Python 的异常、7z 的报错），当日志显示
    std::thread::spawn(move || {
        for line in BufReader::new(err).lines().map_while(Result::ok) {
            if !line.trim().is_empty() {
                let _ = etx.send(json!({"ev": "log", "msg": line}));
            }
        }
    });
    std::thread::spawn(move || {
        for line in BufReader::new(out).lines().map_while(Result::ok) {
            let v = serde_json::from_str::<Value>(&line).unwrap_or_else(|_| json!({"ev": "log", "msg": line}));
            if tx.send(v).is_err() {
                return;
            }
        }
        let _ = tx.send(json!({"ev": "exit"}));
    });
    Ok(child)
}

/// 取消：先发 SIGTERM 给整个进程组让 minime-fetch 清理临时目录，超时再强杀
pub fn cancel(mut child: Child) {
    std::thread::spawn(move || {
        let pgid = format!("-{}", child.id());
        let _ = Command::new("kill").args(["-TERM", "--", &pgid]).status();
        for _ in 0..30 {
            if matches!(child.try_wait(), Ok(Some(_))) {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        let _ = Command::new("kill").args(["-KILL", "--", &pgid]).status();
        let _ = child.kill();
        let _ = child.wait();
    });
}

/// 一项的进度
#[derive(Clone, Default)]
pub struct ItemState {
    /// "" 未开始 / download / verify / extract / run / done / skip
    pub stage: String,
    pub done: u64,
    pub total: u64,
    pub speed: u64,
}

#[derive(Default)]
pub struct Job {
    pub running: bool,
    /// engine, e2e, voice, setup
    pub items: [ItemState; 4],
    /// 最近的日志（最多 6 行）
    pub logs: Vec<String>,
    pub error: Option<String>,
    /// 结束了而且成功
    pub ok: bool,
    pub finished: bool,
}

pub fn slot(id: &str) -> Option<usize> {
    match id {
        "engine" => Some(0),
        "e2e" => Some(1),
        "voice" => Some(2),
        "setup" => Some(3),
        _ => None,
    }
}

impl Job {
    pub fn apply(&mut self, v: &Value) {
        let item = v["item"].as_str().unwrap_or("");
        match v["ev"].as_str().unwrap_or("") {
            "stage" => {
                if let Some(i) = slot(item) {
                    self.items[i].stage = v["stage"].as_str().unwrap_or("").to_string();
                }
            }
            "progress" => {
                if let Some(i) = slot(item) {
                    let it = &mut self.items[i];
                    it.done = v["done"].as_u64().unwrap_or(0);
                    it.total = v["total"].as_u64().unwrap_or(0);
                    it.speed = v["speed"].as_u64().unwrap_or(0);
                }
            }
            "log" => {
                let m = v["msg"].as_str().unwrap_or("").trim().to_string();
                if !m.is_empty() && m != "完成。" {
                    self.logs.push(m);
                    if self.logs.len() > 6 {
                        self.logs.remove(0);
                    }
                }
            }
            "error" => self.error = Some(v["msg"].as_str().unwrap_or("下载失败").to_string()),
            "done" => self.ok = v["ok"].as_bool().unwrap_or(false) && self.error.is_none(),
            "exit" => {
                self.running = false;
                self.finished = true;
                if !self.ok && self.error.is_none() {
                    self.error = Some("下载中断了".into());
                }
            }
            _ => {}
        }
    }
}

pub fn mb(b: u64) -> f64 {
    b as f64 / (1u64 << 20) as f64
}

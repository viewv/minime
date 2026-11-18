//! minime-asrd —— 离线语音服务：按住说话、本地识别，一个字节都不出本机。
//!
//! 识别用 [wechat-asr-gpu](https://github.com/Enter-tainer/wechat-asr-gpu)（微信输入法离线 ASR
//! 的开源复刻，MIT）；模型是官方语音包里的 `.xnet`，不随仓库分发，用 `minime-asrd fetch-model`
//! 从官方 CDN 下载、校验 md5、解包。录音用 PipeWire 的 `pw-record`（退回 `parec`），16 kHz 单声道。
//!
//! 协议：`$XDG_RUNTIME_DIR/minime-asrd.sock`（0600），一次连接一次说话，按行收发：
//!
//! ```text
//! 客户端 → start                服务 → ready              （麦克风已打开）
//!                                服务 → partial <文字>     （说话中的中间结果，可能多次）
//! 客户端 → stop                 服务 → final <文字>       （整段重新解码的定稿），然后断开
//! 客户端 → cancel / 断开         丢弃
//!                                服务 → error <原因>
//! ```
//!
//! 同一时间只有一次说话：新的 start 会取消旧的。
//!
//! 按需启动：装成 systemd 的 `minime-asrd.socket`，第一次连接时才启动服务，空闲 10 分钟退出
//! （监听 socket 一直由 systemd 持有，退出期间的连接会排队并再次拉起服务）。
//! 直接运行（不经 socket 激活）时自己 bind，常驻不退出。

use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use md5::{Digest, Md5};

/// 官方语音包（2026-07 版，见 docs/voice.md）
const MODEL_URL: &str =
    "https://download.z.weixin.qq.com/publish_words/v1/ime_asr_decoder_model/20260710220526250_ime_asr_decoder_model.apk";
const MODEL_MD5: &str = "28386481eb94ff73b28ee3949c0cf690";
const MODEL_SIZE: u64 = 102_920_639;

/// 一次说话最长这么久，到点自动结束（流式分块解码，可支持长段输入）
const MAX_UTTERANCE: Duration = Duration::from_secs(300);
/// 中间结果的刷新间隔
const PARTIAL_EVERY: Duration = Duration::from_millis(300);
/// 每次从录音进程读 100 ms（16 kHz × 2 字节）
const CHUNK: usize = 3200;

macro_rules! log {
    ($($t:tt)*) => { eprintln!("[asrd] {}", format!($($t)*)) };
}

fn data_home() -> PathBuf {
    std::env::var("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"))
}

fn model_dir() -> PathBuf {
    if let Ok(p) = std::env::var("MINIME_ASR_MODEL") {
        return PathBuf::from(p);
    }
    let new = data_home().join("minime/asr/model");
    let old = data_home().join("wetype/asr/model");
    // 改名前的数据目录（engined / netd 启动时会整个迁过来；还没迁时先用旧的）
    if !new.exists() && old.exists() {
        return old;
    }
    new
}

fn socket_path() -> PathBuf {
    if let Ok(p) = std::env::var("MINIME_ASRD_SOCKET") {
        return PathBuf::from(p);
    }
    PathBuf::from(std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into())).join("minime-asrd.sock")
}

fn model_installed(dir: &Path) -> bool {
    dir.join("dict.decoder.utf8.txt").is_file()
        && std::fs::read_dir(dir)
            .map(|r| r.flatten().any(|e| e.file_name().to_string_lossy().ends_with(".xnet")))
            .unwrap_or(false)
}

// ------------------------------------------------------------------ 模型下载

fn fetch_model() -> Result<(), String> {
    let dir = model_dir();
    if model_installed(&dir) {
        println!("模型已安装：{}", dir.display());
        return Ok(());
    }
    let parent = dir.parent().ok_or("bad model dir")?;
    std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let apk = parent.join("ime_asr_decoder_model.apk");
    if std::fs::metadata(&apk).map(|m| m.len() > MODEL_SIZE).unwrap_or(false) {
        let _ = std::fs::remove_file(&apk);
    }
    let ok_size = std::fs::metadata(&apk).map(|m| m.len() == MODEL_SIZE).unwrap_or(false);
    // minime-fetch --json 调用时：报总大小和阶段，curl 不打进度条（minime-fetch 按文件长度算进度）
    let json = std::env::var_os("MINIME_FETCH_JSON").is_some();
    if !ok_size {
        if json {
            println!("{{\"ev\":\"total\",\"bytes\":{MODEL_SIZE}}}");
        } else {
            println!("下载官方语音包（{:.0} MB）……", MODEL_SIZE as f64 / 1e6);
        }
        let st = Command::new("curl")
            .args(if json { ["-sfL", "--retry", "3", "-C", "-", "-o"] } else { ["-fL", "--retry", "3", "-C", "-", "-o"] })
            .arg(&apk)
            .arg(MODEL_URL)
            .status()
            .map_err(|e| format!("运行 curl 失败：{e}"))?;
        if !st.success() {
            if matches!(st.code(), Some(22 | 33)) || std::fs::metadata(&apk).map(|m| m.len() >= MODEL_SIZE).unwrap_or(false) {
                let _ = std::fs::remove_file(&apk);
            }
            return Err("下载失败".into());
        }
    }
    if json {
        println!("{{\"ev\":\"verify\"}}");
    } else {
        print!("校验 md5……");
    }
    let _ = std::io::stdout().flush();
    let mut f = std::fs::File::open(&apk).map_err(|e| e.to_string())?;
    let mut h = Md5::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    let md5 = hex::encode(h.finalize());
    if md5 != MODEL_MD5 {
        let _ = std::fs::remove_file(&apk);
        return Err(format!("md5 不符（{md5}），已删除，请重试"));
    }
    if json {
        println!("{{\"ev\":\"extract\"}}");
    } else {
        println!("通过");
    }
    wechat_asr_gpu::pack::unpack(&apk, &dir).map_err(|e| format!("解包失败：{e}"))?;
    let _ = std::fs::remove_file(&apk);
    println!("模型已安装：{}", dir.display());
    Ok(())
}

// ------------------------------------------------------------------ 录音

fn spawn_recorder() -> Result<Child, String> {
    let tries: [(&str, &[&str]); 2] = [
        ("pw-record", &["--rate", "16000", "--channels", "1", "--format", "s16", "--raw", "-"]),
        ("parec", &["--raw", "--rate=16000", "--channels=1", "--format=s16le", "--latency-msec=50"]),
    ];
    let mut last = String::new();
    for (bin, args) in tries {
        match Command::new(bin).args(args).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null()).spawn() {
            Ok(c) => return Ok(c),
            Err(e) => last = format!("{bin}: {e}"),
        }
    }
    Err(format!("打不开麦克风（{last}）"))
}

// ------------------------------------------------------------------ 一次说话

fn one_line(s: &str) -> String {
    s.replace(['\n', '\r'], " ")
}

type SharedModel = Arc<Mutex<Option<Arc<wechat_asr_gpu::Model>>>>;

struct SessionCtl {
    cancel: AtomicBool,
    rec_pid: AtomicU32,
}

fn kill_pid(pid: u32, sig: i32) {
    if pid == 0 {
        return;
    }
    extern "C" {
        fn kill(pid: i32, sig: i32) -> i32;
    }
    unsafe {
        kill(pid as i32, sig);
    }
}

fn session(conn: UnixStream, ctl: Arc<SessionCtl>, shared_model: SharedModel) {
    let closer = conn.try_clone().ok();
    run_session(conn, &ctl, shared_model);
    // 收命令的线程还拿着这条连接：显式关掉，客户端才读到结束
    if let Some(c) = closer {
        let _ = c.shutdown(std::net::Shutdown::Both);
    }
}

fn run_session(conn: UnixStream, ctl: &Arc<SessionCtl>, shared_model: SharedModel) {
    let cancel = &ctl.cancel;
    let mut out = match conn.try_clone() {
        Ok(c) => c,
        Err(_) => return,
    };
    let mut rd = BufReader::new(conn);
    let mut line = String::new();
    if (&mut rd).take((64 << 10) + 1).read_line(&mut line).unwrap_or(0) == 0 || line.len() > (64 << 10) || line.trim() != "start" {
        let _ = writeln!(out, "error 用法：start / stop / cancel");
        return;
    }
    let send = |out: &mut UnixStream, s: String| out.write_all(format!("{s}\n").as_bytes()).is_ok();

    let dir = model_dir();
    if !model_installed(&dir) {
        send(&mut out, "error 语音模型未安装（运行 minime-asrd fetch-model）".into());
        return;
    }
    let model = {
        let mut g = shared_model.lock().unwrap();
        if let Some(m) = g.as_ref() {
            Arc::clone(m)
        } else {
            match wechat_asr_gpu::load(&dir) {
                Ok(m) => {
                    *g = Some(Arc::clone(&m));
                    m
                }
                Err(e) => {
                    send(&mut out, one_line(&format!("error 模型加载失败：{e}")));
                    return;
                }
            }
        }
    };
    let mut rec = match spawn_recorder() {
        Ok(c) => c,
        Err(e) => {
            send(&mut out, format!("error {e}"));
            return;
        }
    };
    let rec_pid = rec.id();
    ctl.rec_pid.store(rec_pid, Ordering::SeqCst);
    if cancel.load(Ordering::SeqCst) {
        ctl.rec_pid.store(0, Ordering::SeqCst);
        let _ = rec.kill();
        let _ = rec.wait();
        return;
    }
    let mut audio = rec.stdout.take().expect("piped");
    if !send(&mut out, "ready".into()) {
        ctl.rec_pid.store(0, Ordering::SeqCst);
        let _ = rec.kill();
        let _ = rec.wait();
        return;
    }

    // 客户端的 stop / cancel / 断开在单独的线程里收；同时发信号打断阻塞在 audio.read 的录音进程
    let stop = Arc::new(AtomicBool::new(false));
    {
        let (stop, ctl) = (stop.clone(), ctl.clone());
        std::thread::spawn(move || {
            let mut l = String::new();
            loop {
                l.clear();
                let n = (&mut rd).take((64 << 10) + 1).read_line(&mut l);
                if l.len() > (64 << 10) {
                    ctl.cancel.store(true, Ordering::SeqCst);
                    kill_pid(ctl.rec_pid.load(Ordering::SeqCst), 9);
                    break;
                }
                match n {
                    Ok(0) | Err(_) => {
                        ctl.cancel.store(true, Ordering::SeqCst);
                        kill_pid(ctl.rec_pid.load(Ordering::SeqCst), 9);
                        break;
                    }
                    Ok(_) => match l.trim() {
                        "stop" => {
                            stop.store(true, Ordering::SeqCst);
                            kill_pid(ctl.rec_pid.load(Ordering::SeqCst), 15);
                            break;
                        }
                        "cancel" => {
                            ctl.cancel.store(true, Ordering::SeqCst);
                            kill_pid(ctl.rec_pid.load(Ordering::SeqCst), 9);
                            break;
                        }
                        _ => {}
                    },
                }
            }
        });
    }

    let t0 = Instant::now();
    let mut stream = wechat_asr_gpu::Stream::new(model);
    let mut buf = vec![0u8; CHUNK];
    let mut pcm: Vec<i16> = Vec::with_capacity(CHUNK / 2);
    let mut odd: Option<u8> = None;
    let mut last_partial = Instant::now();
    let mut shown = String::new();
    let mut samples = 0usize;
    let finished = loop {
        if cancel.load(Ordering::SeqCst) {
            break false;
        }
        if stop.load(Ordering::SeqCst) || t0.elapsed() > MAX_UTTERANCE {
            break true;
        }
        let n = match audio.read(&mut buf) {
            Ok(0) | Err(_) => break !cancel.load(Ordering::SeqCst),
            Ok(n) => n,
        };
        // 字节流按 i16 小端拼（read 可能在半个样本处截断）
        pcm.clear();
        let mut bytes = &buf[..n];
        if let Some(b0) = odd.take() {
            pcm.push(i16::from_le_bytes([b0, bytes[0]]));
            bytes = &bytes[1..];
        }
        for c in bytes.chunks(2) {
            if c.len() == 2 {
                pcm.push(i16::from_le_bytes([c[0], c[1]]));
            } else {
                odd = Some(c[0]);
            }
        }
        samples += pcm.len();
        stream.push(&pcm);
        if last_partial.elapsed() >= PARTIAL_EVERY && stream.fresh() >= 20 {
            last_partial = Instant::now();
            let p = stream.partial();
            if p != shown {
                shown = p;
                if !send(&mut out, one_line(&format!("partial {shown}"))) {
                    break false;
                }
            }
        }
    };
    ctl.rec_pid.store(0, Ordering::SeqCst);
    let _ = rec.kill();
    let _ = rec.wait();
    if !finished {
        log!("cancelled after {:.1}s", samples as f64 / 16000.0);
        return;
    }
    let t1 = Instant::now();
    let text = stream.finish();
    log!(
        "{:.1}s audio, final decode {} ms, {} chars",
        samples as f64 / 16000.0,
        t1.elapsed().as_millis(),
        text.chars().count()
    );
    send(&mut out, one_line(&format!("final {text}")));
}

/// 空闲这么久（没有说话）就退出；systemd socket 激活下，下次按语音键会自动再启动
const IDLE_EXIT: Duration = Duration::from_secs(10 * 60);

static ACTIVE: AtomicU64 = AtomicU64::new(0);
static LAST_USE: Mutex<Option<Instant>> = Mutex::new(None);

fn touch() {
    *LAST_USE.lock().unwrap() = Some(Instant::now());
}

/// systemd socket 激活（`minime-asrd.socket`）传进来的监听 socket：LISTEN_PID 是本进程、LISTEN_FDS=1 → fd 3
fn activated_listener() -> Option<UnixListener> {
    use std::os::fd::FromRawFd;
    let pid: u32 = std::env::var("LISTEN_PID").ok()?.parse().ok()?;
    let n: u32 = std::env::var("LISTEN_FDS").ok()?.parse().ok()?;
    if pid != std::process::id() || n != 1 {
        return None;
    }
    std::env::remove_var("LISTEN_PID");
    std::env::remove_var("LISTEN_FDS");
    // SAFETY: systemd 约定第一个传入的 fd 是 3，且只交给本进程
    Some(unsafe { UnixListener::from_raw_fd(3) })
}

fn daemon() -> i32 {
    let path = socket_path();
    let activated = activated_listener();
    let on_demand = activated.is_some();
    let listener = match activated {
        Some(l) => l,
        None => {
            if UnixStream::connect(&path).is_ok() {
                log!("already running at {}", path.display());
                return 1;
            }
            let _ = std::fs::remove_file(&path);
            match UnixListener::bind(&path) {
                Ok(l) => {
                    use std::os::unix::fs::PermissionsExt;
                    let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600));
                    l
                }
                Err(e) => {
                    log!("bind {}: {e}", path.display());
                    return 1;
                }
            }
        }
    };
    touch();
    if on_demand {
        // 按需启动：空闲 IDLE_EXIT 后退出，监听 socket 留在 systemd 手里
        // MINIME_ASRD_IDLE_SECS 可改空闲时间（测试用）
        let limit = std::env::var("MINIME_ASRD_IDLE_SECS")
            .ok()
            .and_then(|v| v.parse().ok())
            .map(Duration::from_secs)
            .unwrap_or(IDLE_EXIT);
        let tick = (limit / 4).clamp(Duration::from_secs(1), Duration::from_secs(30));
        std::thread::spawn(move || loop {
            std::thread::sleep(tick);
            let idle = LAST_USE.lock().unwrap().map(|t| t.elapsed()).unwrap_or_default();
            if ACTIVE.load(Ordering::SeqCst) == 0 && idle > limit {
                log!("idle for {}s, exiting (socket stays with systemd)", idle.as_secs());
                std::process::exit(0);
            }
        });
    }
    let shared_model: SharedModel = Arc::new(Mutex::new(None));
    let dir = model_dir();
    if model_installed(&dir) {
        // 启动时加载并保留在内存里（mmap，约 0.1 s），空闲 10 分钟随进程退出释放
        match wechat_asr_gpu::load(&dir) {
            Ok(m) => {
                *shared_model.lock().unwrap() = Some(m);
                log!("model ready ({})", dir.display());
            }
            Err(e) => log!("model load failed: {e}"),
        }
    } else {
        log!("model not installed ({}); run: minime-asrd fetch-model", dir.display());
    }
    log!("listening on {}{}", path.display(), if on_demand { "（按需启动，空闲 10 分钟退出）" } else { "" });

    // 同一时间只有一次说话：新连接取消并立即终止旧会话的录音进程
    let current: Arc<Mutex<Option<Arc<SessionCtl>>>> = Arc::new(Mutex::new(None));
    let serial = AtomicU64::new(0);
    for conn in listener.incoming().flatten() {
        // 只接受同一用户
        let uid_ok = {
            use std::os::unix::fs::MetadataExt;
            let me = std::fs::metadata("/proc/self").map(|m| m.uid()).ok();
            peer_uid(&conn) == me
        };
        if !uid_ok {
            continue;
        }
        let ctl = Arc::new(SessionCtl { cancel: AtomicBool::new(false), rec_pid: AtomicU32::new(0) });
        if let Some(old) = current.lock().unwrap().replace(ctl.clone()) {
            old.cancel.store(true, Ordering::SeqCst);
            kill_pid(old.rec_pid.load(Ordering::SeqCst), 9);
        }
        serial.fetch_add(1, Ordering::Relaxed);
        ACTIVE.fetch_add(1, Ordering::SeqCst);
        touch();
        let shared_model = shared_model.clone();
        std::thread::spawn(move || {
            session(conn, ctl, shared_model);
            touch();
            ACTIVE.fetch_sub(1, Ordering::SeqCst);
        });
    }
    0
}

fn peer_uid(s: &UnixStream) -> Option<u32> {
    use std::os::fd::AsRawFd;
    #[repr(C)]
    struct Ucred {
        pid: i32,
        uid: u32,
        gid: u32,
    }
    extern "C" {
        fn getsockopt(fd: i32, level: i32, name: i32, val: *mut Ucred, len: *mut u32) -> i32;
    }
    const SOL_SOCKET: i32 = 1;
    const SO_PEERCRED: i32 = 17;
    let mut c = Ucred { pid: 0, uid: u32::MAX, gid: 0 };
    let mut len = std::mem::size_of::<Ucred>() as u32;
    let r = unsafe { getsockopt(s.as_raw_fd(), SOL_SOCKET, SO_PEERCRED, &mut c, &mut len) };
    (r == 0).then_some(c.uid)
}

fn parse_wav_pcm(data: &[u8]) -> Result<&[u8], &'static str> {
    if data.len() < 12 || &data[0..4] != b"RIFF" || &data[8..12] != b"WAVE" {
        return Err("不是有效的 RIFF/WAVE 文件");
    }
    let mut off = 12usize;
    let mut fmt_ok = false;
    let mut pcm_bytes = None;
    while off + 8 <= data.len() {
        let id = &data[off..off + 4];
        let sz = u32::from_le_bytes(data[off + 4..off + 8].try_into().unwrap()) as usize;
        off += 8;
        let end = off.saturating_add(sz).min(data.len());
        if id == b"fmt " {
            if end - off < 16 {
                return Err("WAV fmt 块不完整");
            }
            let fmt = u16::from_le_bytes([data[off], data[off + 1]]);
            let ch = u16::from_le_bytes([data[off + 2], data[off + 3]]);
            let rate = u32::from_le_bytes(data[off + 4..off + 8].try_into().unwrap());
            let bits = u16::from_le_bytes([data[off + 14], data[off + 15]]);
            if fmt != 1 || ch != 1 || rate != 16000 || bits != 16 {
                return Err("只支持 16 kHz 单声道 16 位 PCM WAV");
            }
            fmt_ok = true;
        } else if id == b"data" {
            pcm_bytes = Some(&data[off..end]);
        }
        off = end + (sz & 1);
    }
    if !fmt_ok {
        return Err("缺少 WAV fmt 块");
    }
    pcm_bytes.ok_or("缺少 WAV data 块")
}

/// 调试：识别一个 16 kHz 单声道 16 位 WAV（不经过麦克风）
fn file(path: &str) -> i32 {
    let data = match std::fs::read(path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("{path}: {e}");
            return 1;
        }
    };
    let raw = match parse_wav_pcm(&data) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("{path}: {e}");
            return 1;
        }
    };
    let pcm: Vec<i16> = raw.chunks_exact(2).map(|c| i16::from_le_bytes([c[0], c[1]])).collect();
    let m = match wechat_asr_gpu::load(&model_dir()) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("模型加载失败：{e}");
            return 1;
        }
    };
    let t = Instant::now();
    let text = wechat_asr_gpu::transcribe(m, &pcm, 1);
    println!("{text}");
    eprintln!("{:.1}s 音频，{} ms", pcm.len() as f64 / 16000.0, t.elapsed().as_millis());
    0
}

/// 调试：在终端里说一句（回车结束）
fn say() -> i32 {
    let mut s = match UnixStream::connect(socket_path()) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("连不上 minime-asrd：{e}（systemctl --user start minime-asrd）");
            return 1;
        }
    };
    let _ = s.write_all(b"start\n");
    let mut w = s.try_clone().unwrap();
    std::thread::spawn(move || {
        let mut l = String::new();
        let _ = std::io::stdin().read_line(&mut l);
        let _ = w.write_all(b"stop\n");
    });
    for l in BufReader::new(s).lines().map_while(Result::ok) {
        match l.split_once(' ').unwrap_or((l.as_str(), "")) {
            ("ready", _) => eprintln!("说吧，回车结束……"),
            ("partial", t) => eprintln!("… {t}"),
            ("final", t) => {
                println!("{t}");
                return 0;
            }
            ("error", e) => {
                eprintln!("错误：{e}");
                return 1;
            }
            _ => {}
        }
    }
    1
}

fn main() {
    // 识别线程数（库默认 min(核数, 4)）；短句 4 线程已经远快于实时
    if std::env::var("WTL_THREADS").is_err() {
        std::env::set_var("WTL_THREADS", "4");
    }
    let args: Vec<String> = std::env::args().skip(1).collect();
    let rc = match args.first().map(String::as_str) {
        None | Some("daemon") => daemon(),
        Some("fetch-model") => match fetch_model() {
            Ok(()) => 0,
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        Some("file") if args.len() > 1 => file(&args[1]),
        Some("say") => say(),
        _ => {
            eprintln!("用法：minime-asrd [daemon] | fetch-model | say | file <a.wav>");
            2
        }
    };
    std::process::exit(rc);
}

#[cfg(test)]
mod tests {
    use super::parse_wav_pcm;

    fn make_wav(ch: u16, rate: u32, bits: u16, pcm: &[u8], extra: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(b"RIFF\0\0\0\0WAVE");
        v.extend_from_slice(b"fmt ");
        v.extend_from_slice(&16u32.to_le_bytes());
        v.extend_from_slice(&1u16.to_le_bytes());
        v.extend_from_slice(&ch.to_le_bytes());
        v.extend_from_slice(&rate.to_le_bytes());
        v.extend_from_slice(&(rate * u32::from(ch) * u32::from(bits) / 8).to_le_bytes());
        v.extend_from_slice(&(ch * bits / 8).to_le_bytes());
        v.extend_from_slice(&bits.to_le_bytes());
        v.extend_from_slice(b"data");
        v.extend_from_slice(&(pcm.len() as u32).to_le_bytes());
        v.extend_from_slice(pcm);
        if pcm.len() & 1 == 1 {
            v.push(0);
        }
        v.extend_from_slice(extra);
        v
    }

    #[test]
    fn wav_parser_checks_fmt_and_data_len() {
        let w = make_wav(1, 16000, 16, &[1, 2, 3, 4], b"LIST\x04\0\0\0abcd");
        assert_eq!(parse_wav_pcm(&w).unwrap(), &[1, 2, 3, 4]);
        let bad = make_wav(2, 16000, 16, &[1, 2, 3, 4], b"");
        assert!(parse_wav_pcm(&bad).is_err());
    }
}

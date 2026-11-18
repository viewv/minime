//! minime-netd —— MinIME 原生网络服务（跨设备剪贴板）。
//!
//! 默认不联网：只有 minime.conf 的 [Network] Enabled=True 时才连服务器。
//!
//!   minime-netd                    守护进程（systemd 用户服务）
//!   minime-netd status             状态
//!   minime-netd pair               出 6 位码，在手机上输入
//!   minime-netd pair <手机的码>     输入手机上显示的码
//!   minime-netd unpair <uin|all>   解除某台设备 / 全部
//!   minime-netd send <文本>         手动发一条剪贴板（调试用）
//!
//! 控制 socket（$XDG_RUNTIME_DIR/minime-netd.sock）：发一行命令，回若干行 JSON 事件，
//! 每行一个对象，`type` 字段区分：status / code / info / paired / ok / error。
//! 加 `--json` 时命令行直接输出这些 JSON（给设置界面用）。

use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::mpsc;

use minime_netd::clipboard::{self, Content};
use minime_netd::config::{self, Config};
use minime_netd::hotwords;
use minime_netd::identity::{self, Identity};
use minime_netd::session::{Clip, Group, Res, Session, CLIP_IMAGE, CLIP_IMAGE_ORI, CLIP_TEXT, CLIP_TEXT_COS};

const HEARTBEAT: Duration = Duration::from_secs(25);
const CONFIG_POLL: Duration = Duration::from_secs(2);
const BACKOFF_MAX: Duration = Duration::from_secs(60);

macro_rules! log {
    ($($t:tt)*) => { eprintln!("[netd] {}", format!($($t)*)) };
}

type Reply = mpsc::UnboundedSender<Value>;

fn err(msg: impl std::fmt::Display) -> Value {
    json!({"type": "error", "message": msg.to_string()})
}
fn info(msg: impl std::fmt::Display) -> Value {
    json!({"type": "info", "message": msg.to_string()})
}

enum Ctl {
    Status,
    Pair(Option<String>),
    Unpair(String),
    Send(String),
    /// 主动拉一次组里当前的剪贴板并应用（调试 / 手动补拉）
    Fetch,
    /// 常用语列表（本地文件，连接时与服务端同步）
    HotWords,
    /// 整体替换常用语：参数是 JSON 数组 [{"key","text","id"?}]
    HotWordsSet(String),
    /// 改本机设备名（sub 79）
    Rename(String),
    /// 换机助手：出码等手机扫，双方互换用户数据（见 migrate.rs）
    Migrate,
    /// 调试：`raw <cmd> <sub> <hex>` 发一条 BaseReq 业务请求，回 hex（探协议用）
    Raw(String),
}

fn socket_path() -> PathBuf {
    if let Ok(p) = std::env::var("MINIME_NETD_SOCKET") {
        return PathBuf::from(p);
    }
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    PathBuf::from(dir).join("minime-netd.sock")
}

// ------------------------------------------------------------------ 控制通道

fn parse_ctl(line: &str) -> Option<Ctl> {
    let line = line.trim_end_matches(['\r', '\n']);
    let (cmd, arg) = line.split_once(' ').map(|(a, b)| (a, b.trim())).unwrap_or((line, ""));
    match cmd {
        "status" => Some(Ctl::Status),
        "pair" => Some(Ctl::Pair(Some(arg.to_string()).filter(|s| !s.is_empty()))),
        "unpair" => Some(Ctl::Unpair(arg.to_string())),
        "send" => Some(Ctl::Send(arg.to_string())),
        "fetch" => Some(Ctl::Fetch),
        "hotwords" => Some(Ctl::HotWords),
        "hotwords-set" => Some(Ctl::HotWordsSet(arg.to_string())),
        "rename" => Some(Ctl::Rename(arg.to_string())),
        "migrate" => Some(Ctl::Migrate),
        "raw" => Some(Ctl::Raw(arg.to_string())),
        _ => None,
    }
}

const MAX_CTL_LINE: usize = 1 << 20;

async fn read_line_capped(rd: &mut BufReader<tokio::net::unix::OwnedReadHalf>, out: &mut String) -> std::io::Result<usize> {
    use tokio::io::AsyncReadExt;
    out.clear();
    let n = rd.take((MAX_CTL_LINE + 1) as u64).read_line(out).await?;
    if out.len() > MAX_CTL_LINE {
        return Err(std::io::Error::other("line too long"));
    }
    Ok(n)
}

async fn serve_client(stream: UnixStream, ctl: mpsc::UnboundedSender<(Ctl, Reply)>) {
    let (rd, mut wr) = stream.into_split();
    let mut rd = BufReader::new(rd);
    let mut line = String::new();
    if read_line_capped(&mut rd, &mut line).await.unwrap_or(0) == 0 {
        return;
    }
    let (tx, mut rx) = mpsc::unbounded_channel::<Value>();
    match parse_ctl(&line) {
        Some(c) => {
            let _ = ctl.send((c, tx));
        }
        None => {
            let _ = wr.write_all(format!("{}\n", err("unknown command")).as_bytes()).await;
            return;
        }
    }
    // 事件按行流式写回，处理方丢掉发送端就结束；客户端断开（取消关联/迁移）时立刻丢掉
    // 接收端，长任务通过 reply.is_closed() 马上知道
    let mut rest = String::new();
    loop {
        tokio::select! {
            v = rx.recv() => match v {
                Some(v) => {
                    if wr.write_all(format!("{v}\n").as_bytes()).await.is_err() {
                        break;
                    }
                }
                None => break,
            },
            n = read_line_capped(&mut rd, &mut rest) => {
                if n.unwrap_or(0) == 0 {
                    break;
                }
                rest.clear();
            }
        }
    }
}

fn spawn_control(ctl: mpsc::UnboundedSender<(Ctl, Reply)>) -> std::io::Result<()> {
    let path = socket_path();
    let _ = std::fs::remove_file(&path);
    let listener = UnixListener::bind(&path)?;
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
    tokio::spawn(async move {
        loop {
            if let Ok((s, _)) = listener.accept().await {
                tokio::spawn(serve_client(s, ctl.clone()));
            }
        }
    });
    Ok(())
}

/// 把 JSON 事件渲染成给人看的文本。
fn render(v: &Value) -> Vec<String> {
    let s = |k: &str| v[k].as_str().unwrap_or("").to_string();
    match v["type"].as_str().unwrap_or("") {
        "status" => {
            let mut out = vec![
                format!("network: {}", if v["enabled"].as_bool() == Some(true) { "enabled" } else { "disabled" }),
                format!(
                    "connected: {}",
                    if v["connected"].as_bool() == Some(true) { "yes".to_string() } else { format!("no ({})", s("reason")) }
                ),
            ];
            if v["connected"].as_bool() == Some(true) {
                out.push(format!("device: {} (uin {})", s("device"), v["uin"]));
                out.push(format!(
                    "clipboard sync: {}, images: {}",
                    if v["clipboard"].as_bool() == Some(true) { "on" } else { "off" },
                    if v["images"].as_bool() == Some(true) { "on" } else { "off" }
                ));
                let peers = v["peers"].as_array().cloned().unwrap_or_default();
                if peers.is_empty() {
                    out.push("paired: no（minime-netd pair）".into());
                } else {
                    out.push(format!("group: {}", v["group"]));
                    for p in peers {
                        out.push(format!(
                            "peer: {} (uin {}, platform {}, {})",
                            p["name"].as_str().unwrap_or(""),
                            p["uin"],
                            p["platform"],
                            p["version"].as_str().unwrap_or("")
                        ));
                    }
                    out.push(format!("clips sent/received: {}/{}", v["sent"], v["received"]));
                }
            } else if v["enabled"].as_bool() != Some(true) {
                out.push(format!("在设置里打开「联网」，或在 {} 的 [Network] 段写 Enabled=True", s("config")));
            }
            out
        }
        "code" => vec![format!("code: {}", s("code")), format!("在手机上输入这个码完成关联（{} 分钟内有效）……", v["ttl"].as_u64().unwrap_or(300).div_ceil(60))],
        "paired" => vec![format!("paired, group {}", v["group"])],
        "migrate" => match s("stage").as_str() {
            "qr" => {
                // 终端里直接画二维码（有 qrencode 时）
                let url = s("url");
                let qr = std::process::Command::new("qrencode").args(["-t", "ansiutf8", "-m", "1", &url]).output();
                let mut out = match qr {
                    Ok(o) if o.status.success() => vec![String::from_utf8_lossy(&o.stdout).trim_end().to_string()],
                    _ => vec![],
                };
                out.push(format!("用手机「微信输入法 → 换机助手 → 扫一扫」扫码：{url}"));
                out
            }
            "scanned" => vec!["已扫码，开始同步……".into()],
            "downloading" => vec!["下载手机的数据……".into()],
            "imported" => vec!["已导入手机的个人词库".into()],
            "uploading" => vec!["上传本机的数据……".into()],
            "uploaded" => vec!["已上传".into()],
            other => vec![other.to_string()],
        },
        "raw" => vec![s("hex")],
        "migrated" => vec![format!(
            "迁移完成：导入 {} 字节、上传 {} 字节{}",
            v["imported"].as_u64().unwrap_or(0),
            v["uploaded"].as_u64().unwrap_or(0),
            v["backup"].as_str().map(|b| format!("；导入前的备份：{b}")).unwrap_or_default()
        )],
        "error" => vec![format!("error: {}", s("message"))],
        _ => vec![s("message")],
    }
}

async fn client_main(args: &[String]) -> i32 {
    let json_out = args.iter().any(|a| a == "--json");
    let line = args.iter().filter(|a| *a != "--json").cloned().collect::<Vec<_>>().join(" ");
    let mut s = match UnixStream::connect(socket_path()).await {
        Ok(s) => s,
        Err(e) => {
            if json_out {
                println!("{}", json!({"type": "status", "enabled": false, "connected": false, "running": false, "reason": "service not running", "config": config::path()}));
            } else {
                eprintln!("连不上 minime-netd（{}）：{e}\n先启动服务：systemctl --user start minime-netd", socket_path().display());
            }
            return 1;
        }
    };
    if s.write_all(format!("{line}\n").as_bytes()).await.is_err() {
        return 1;
    }
    let mut lines = BufReader::new(s).lines();
    let mut rc = 0;
    while let Ok(Some(l)) = lines.next_line().await {
        let v: Value = serde_json::from_str(&l).unwrap_or_else(|_| info(&l));
        if v["type"] == "error" {
            rc = 1;
        }
        if json_out {
            println!("{v}");
        } else {
            for t in render(&v) {
                println!("{t}");
            }
        }
    }
    rc
}

// ------------------------------------------------------------------ 会话

enum Exit {
    ConfigChanged,
    Lost(String),
}

/// 跨会话保留的状态（断线重连时不丢）。
#[derive(Default)]
struct Persist {
    /// 已经见过的剪贴板版本：重连后用来补拉错过的那一条
    clip_version: Option<u64>,
    sent: u64,
    received: u64,
    /// 上次让引擎同步个人词库的时间
    dict_sync_at: Option<Instant>,
}

struct State<'a> {
    session: Arc<Session>,
    cfg: Config,
    group: Group,
    /// 最近一次同步过的剪贴板指纹（文本本身或图片 md5），用来防回声
    last_clip: Option<String>,
    /// 刚写入远端图片后忽略短时间内桌面转码衍生出的新 PNG（防回声循环）
    ignore_image_echo_until: Option<Instant>,
    persist: &'a mut Persist,
    http: reqwest::Client,
}

fn fingerprint(c: &Content) -> Option<String> {
    match c {
        Content::Text(t) => Some(t.clone()),
        Content::Image { data, .. } => Some(format!("img:{}", minime_netd::crypto::md5_upper(data))),
        Content::Skip(_) => None,
    }
}

impl State<'_> {
    fn paired(&self) -> bool {
        self.group.group_id != 0 && self.peers().next().is_some()
    }
    fn peers(&self) -> impl Iterator<Item = &minime_netd::session::Device> {
        let me = self.session.uin();
        self.group.devices.iter().filter(move |d| d.uin != me)
    }
    fn status(&self) -> Value {
        let peers: Vec<Value> = self
            .peers()
            .map(|d| json!({"uin": d.uin, "name": d.name, "platform": d.platform, "version": d.version}))
            .collect();
        json!({
            "type": "status", "enabled": true, "connected": true, "running": true,
            "device": self.cfg.device_name, "uin": self.session.uin(),
            "clipboard": self.cfg.clipboard, "images": self.cfg.images,
            "group": self.group.group_id, "peers": peers,
            "sent": self.persist.sent, "received": self.persist.received,
            "config": config::path(),
        })
    }
}

async fn refresh_group(st: &mut State<'_>) -> bool {
    match st.session.group(1, 0).await {
        Ok(g) => {
            let was = st.paired();
            st.group = g;
            if st.paired() != was {
                log!("paired={} group={}", st.paired(), st.group.group_id);
                return st.paired();
            }
        }
        Err(e) => log!("group refresh failed: {e}"),
    }
    false
}

/// 只接受腾讯云 COS / qq.com 的 https 链接，防止被推一个任意 URL 让我们去下载。
fn trusted_image_url(url: &str) -> bool {
    let Some(rest) = url.strip_prefix("https://") else { return false };
    let host = rest.split(['/', '?', ':']).next().unwrap_or("");
    host.ends_with(".myqcloud.com") || host.ends_with(".qq.com")
}

/// 图片 COS 引用（TypeImageCos 的 data，2026-11-15 真机实测）：
/// `{"version":1,"key":"<base64 16 字节>","md5":"<明文 md5 小写>","path":"<COS 下载链接>"}`。
/// 文件内容是 AES-128-CTR 加密的，key 解 base64 后既是密钥也是初始计数器。
#[derive(serde::Serialize, serde::Deserialize)]
struct CosImageRef {
    version: u32,
    key: String,
    md5: String,
    path: String,
}

fn ctr_key(b64: &str) -> Option<[u8; 16]> {
    use base64::Engine as _;
    base64::engine::general_purpose::STANDARD.decode(b64).ok()?.try_into().ok()
}

/// 解密手机发来的图片，校验 md5。
fn decrypt_cos_image(cipher: &[u8], r: &CosImageRef) -> Res<Vec<u8>> {
    let key = ctr_key(&r.key).ok_or("bad image key")?;
    let plain = minime_netd::crypto::aes_ctr(cipher, &key, &key)?;
    if !r.md5.is_empty() && !minime_netd::crypto::md5_upper(&plain).eq_ignore_ascii_case(&r.md5) {
        return Err("image md5 mismatch".into());
    }
    Ok(plain)
}

/// 加密图片并生成引用（path 由调用方在上传后填）。
fn encrypt_cos_image(plain: &[u8]) -> (Vec<u8>, CosImageRef) {
    use base64::Engine as _;
    use rand::RngCore;
    let mut key = [0u8; 16];
    rand::thread_rng().fill_bytes(&mut key);
    let cipher = minime_netd::crypto::aes_ctr(plain, &key, &key).expect("16-byte key");
    let r = CosImageRef {
        version: 1,
        key: base64::engine::general_purpose::STANDARD.encode(key),
        md5: minime_netd::crypto::md5_upper(plain).to_ascii_lowercase(),
        path: String::new(),
    };
    (cipher, r)
}

async fn download(http: &reqwest::Client, url: &str) -> Res<Vec<u8>> {
    if !trusted_image_url(url) {
        return Err(format!("untrusted image url host: {url:.60}").into());
    }
    let mut r = http.get(url).send().await?.error_for_status()?;
    let mut data = Vec::new();
    while let Some(chunk) = r.chunk().await? {
        data.extend_from_slice(&chunk);
        if data.len() > clipboard::MAX_IMAGE {
            return Err("image too large".into());
        }
    }
    Ok(data)
}

async fn on_local_clip(st: &mut State<'_>) {
    if !st.cfg.clipboard || !st.paired() {
        return;
    }
    let c = clipboard::read(st.cfg.images).await;
    let Some(fp) = fingerprint(&c) else { return };
    if st.last_clip.as_deref() == Some(fp.as_str()) {
        return;
    }
    // 刚从手机写入图片后，Klipper 等桌面组件可能自动转码出 image/png 触发新通知：忽略这轮回声并记住新指纹
    if matches!(c, Content::Image { .. }) {
        if let Some(until) = st.ignore_image_echo_until.take() {
            if Instant::now() <= until {
                st.last_clip = Some(fp);
                return;
            }
        }
    }
    let gid = st.group.group_id;
    let r = match c {
        Content::Text(text) => st.session.send_clip_text(gid, &text).await.map(|v| (v, format!("{} chars", text.chars().count()))),
        Content::Image { mime, data } => async {
            let (upload, obj, limit_kb) = st.session.cos_upload_token().await?;
            if limit_kb > 0 && data.len() as u64 > limit_kb * 1024 {
                return Err(format!("image {} KB over limit {limit_kb} KB", data.len() / 1024).into());
            }
            let _ = mime;
            let (cipher, mut r) = encrypt_cos_image(&data);
            st.http.put(&upload).header("Content-Type", "application/octet-stream").body(cipher).send().await?.error_for_status()?;
            r.path = obj;
            let v = st.session.send_clip_cos(gid, CLIP_IMAGE, &serde_json::to_string(&r)?).await?;
            Res::Ok((v, format!("image {} KB", data.len() / 1024)))
        }
        .await,
        Content::Skip(_) => return,
    };
    match r {
        Ok((ver, what)) => {
            st.last_clip = Some(fp);
            st.persist.sent += 1;
            st.persist.clip_version = Some(ver);
            log!("clipboard -> peer ({what}, version {ver})");
        }
        Err(e) => log!("clipboard send failed: {e}"),
    }
}

/// 把对端的剪贴板写进本机。
async fn apply_remote(st: &mut State<'_>, clip: Clip) {
    st.persist.clip_version = Some(st.persist.clip_version.unwrap_or(0).max(clip.version));
    if clip.from_uin == st.session.uin() || !st.cfg.clipboard {
        return;
    }
    log!("peer clipboard: type {} ({} bytes, extra_info {} bytes, version {})", clip.kind, clip.data.len(), clip.extra_len, clip.version);
    let text = match clip.kind {
        CLIP_TEXT => Some(String::from_utf8_lossy(&clip.data).into_owned()),
        CLIP_TEXT_COS => {
            // 推测与图片相同：JSON 引用 + CTR 加密；不是 JSON 时按裸 URL 处理
            let r = match serde_json::from_slice::<CosImageRef>(&clip.data) {
                Ok(r) => download(&st.http, &r.path).await.and_then(|c| decrypt_cos_image(&c, &r)),
                Err(_) => download(&st.http, &String::from_utf8_lossy(&clip.data)).await,
            };
            match r {
                Ok(d) => Some(String::from_utf8_lossy(&d).into_owned()),
                Err(e) => return log!("long text fetch failed: {e}"),
            }
        }
        _ => None,
    };
    if let Some(text) = text {
        if st.last_clip.as_deref() == Some(text.as_str()) {
            return;
        }
        match clipboard::write_text(&text).await {
            Ok(()) => {
                st.last_clip = Some(text.clone());
                st.persist.received += 1;
                log!("clipboard <- peer ({} chars, version {})", text.chars().count(), clip.version);
            }
            Err(e) => log!("wl-copy failed: {e}"),
        }
        return;
    }
    if clip.kind != CLIP_IMAGE && clip.kind != CLIP_IMAGE_ORI {
        return log!("peer sent clipboard type {} (ignored)", clip.kind);
    }
    if !st.cfg.images {
        return log!("peer sent an image (image sync is off)");
    }
    let data = if clip.kind == CLIP_IMAGE {
        match serde_json::from_slice::<CosImageRef>(&clip.data) {
            // 官方客户端：JSON 引用 + 加密文件
            Ok(r) => match download(&st.http, &r.path).await.and_then(|c| decrypt_cos_image(&c, &r)) {
                Ok(d) => d,
                Err(e) => return log!("image fetch failed: {e}"),
            },
            // 早期自研客户端：裸 URL、明文文件
            Err(_) => match download(&st.http, &String::from_utf8_lossy(&clip.data)).await {
                Ok(d) => d,
                Err(e) => return log!("image download failed: {e}"),
            },
        }
    } else {
        clip.data.clone()
    };
    let Some(mime) = clipboard::image_mime(&data) else {
        return log!("peer image: unknown format (head {})", hex::encode(&data[..data.len().min(16)]));
    };
    match clipboard::write_image(mime, &data).await {
        Ok(()) => {
            st.last_clip = Some(format!("img:{}", minime_netd::crypto::md5_upper(&data)));
            st.ignore_image_echo_until = Some(Instant::now() + Duration::from_secs(3));
            st.persist.received += 1;
            log!("clipboard <- peer (image {} KB, version {})", data.len() / 1024, clip.version);
        }
        Err(e) => log!("wl-copy failed: {e}"),
    }
}

async fn on_push(st: &mut State<'_>, frame: &[u8], refresh: &mpsc::UnboundedSender<()>) {
    let push = match st.session.decode_push(frame) {
        Ok(Some(p)) => p,
        Ok(None) => return,
        Err(e) => return log!("bad push: {e}"),
    };
    match push.event {
        4 => {
            if let Some(clip) = push.clip {
                apply_remote(st, clip).await;
            }
        }
        // 1 绑定确认 / 2 绑定成功 / 6 解绑 / 10 绑定列表变化
        1 | 2 | 6 | 10 => {
            log!("push event {} -> refresh group", push.event);
            let _ = refresh.send(());
        }
        // 7 SyncClipboardMsgVersion：只通知版本号（实测手机复制图片时只推这个，
        // 不推 event 4），要自己用 sub 34 filter=2 拉内容
        7 => match st.session.group(2, 0).await {
            Ok(g) => match g.clip {
                Some(clip) if clip.version > st.persist.clip_version.unwrap_or(0) => apply_remote(st, clip).await,
                Some(_) => {}
                None => log!("event 7 but group has no clipboard"),
            },
            Err(e) => log!("clipboard fetch after event 7 failed: {e}"),
        },
        // 5 SyncUserHotWords：对端改了常用语，带完整数据
        5 => {
            if let Some((v, data)) = push.hot_words {
                apply_hot_words(v, &data);
            }
        }
        // 其他事件（个人词库更新等，事件号未确认）：让引擎自己去同步一次
        e => {
            log!("push event {e} -> engine user dict sync");
            engine_dict_sync(st, "push").await;
        }
    }
}

/// 拉服务端 AB 配置（4814/28），引擎实验写进 ab_engine.tsv，engined 发现变化后重灌给引擎。
/// 只是取配置（空 body），不上传任何内容。
async fn fetch_ab(s: &Session) {
    let body = match s.api(4814, 28, &[], Duration::from_secs(10)).await {
        Ok(b) => b,
        Err(e) => return log!("ab config: {e}"),
    };
    match minime_netd::ab::parse(&body) {
        Ok((engine, client)) => match minime_netd::ab::save(&engine) {
            Ok(changed) => log!(
                "ab config: {} engine / {} client keys{}",
                engine.len(),
                client.len(),
                if changed { " (updated)" } else { "" }
            ),
            Err(e) => log!("ab config save: {e}"),
        },
        Err(e) => log!("ab config parse: {e}"),
    }
}

/// 定期让引擎同步个人词库（另外连上时、收到未知推送时也会同步）
const DICT_SYNC_EVERY: Duration = Duration::from_secs(30 * 60);

/// 个人词库同步由引擎自己做（UserDictSyncer，经兼容层发 4814/41、42、44 和 COS）：
/// 这里只把同步组信息告诉它。只给个人词库这一位（4），常用语仍由 netd 自己同步，免得两边打架。
async fn engine_dict_sync(st: &mut State<'_>, why: &str) {
    st.persist.dict_sync_at = Some(Instant::now());
    if !st.cfg.user_dict || !st.paired() || st.group.func_switch & 4 == 0 {
        return;
    }
    let line = format!("group {} 4 0 0", st.group.group_id);
    match minime_netd::migrate::engined_ctl(&line).await {
        Ok(()) => log!("user dict sync -> engine ({why})"),
        Err(e) => log!("user dict sync ({why}): {e}"),
    }
}

fn now_ms() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

/// 连上（或收到推送）时：服务端版本和本地不同就以服务端为准；本地有未上传的改动就先上传。
async fn sync_hot_words(st: &mut State<'_>) {
    if !st.paired() {
        return;
    }
    let mut local = hotwords::load();
    if !hotwords::path().with_extension("tsv").exists() {
        let _ = hotwords::save(&local);
    }
    if local.pending {
        let data = hotwords::encode(&local.items, st.session.uin(), now_ms());
        match st.session.send_hot_words(st.group.group_id, local.version, &data).await {
            Ok(v) => {
                local.version = v;
                local.pending = false;
                let _ = hotwords::save(&local);
                log!("hot words uploaded ({} items, version {v})", local.items.len());
            }
            Err(e) => log!("hot words upload failed: {e}"),
        }
        return;
    }
    match st.session.group(4, 0).await {
        Ok(g) => {
            if let Some((v, data)) = g.hot_words {
                if v != local.version {
                    apply_hot_words(v, &data);
                }
            }
        }
        Err(e) => log!("hot words fetch failed: {e}"),
    }
}

fn apply_hot_words(version: u64, data: &[u8]) {
    match hotwords::decode(data) {
        Ok(items) => {
            let n = items.len();
            let h = hotwords::HotWords { version, items, pending: false };
            match hotwords::save(&h) {
                Ok(()) => log!("hot words updated ({n} items, version {version})"),
                Err(e) => log!("cannot save hot words: {e}"),
            }
        }
        Err(e) => log!("bad hot words data: {e}"),
    }
}

/// 设置程序整体替换常用语：保留已有条目的 id/创建时间，新条目生成 id；关联了就立即上传。
async fn set_hot_words(st: &mut State<'_>, arg: &str) -> Res<hotwords::HotWords> {
    #[derive(serde::Deserialize)]
    struct In {
        key: String,
        text: String,
        #[serde(default)]
        id: String,
    }
    let wanted: Vec<In> = serde_json::from_str(arg)?;
    let mut h = hotwords::load();
    let now = now_ms();
    let mut items = Vec::new();
    for (i, w) in wanted.into_iter().enumerate() {
        let key = w.key.trim().to_ascii_lowercase();
        let text = w.text.trim().to_string();
        if key.is_empty() || text.is_empty() {
            continue;
        }
        let old = h.items.iter().find(|x| !w.id.is_empty() && x.id == w.id).cloned();
        items.push(match old {
            Some(mut o) => {
                o.key = key;
                o.text = text;
                o
            }
            None => hotwords::new_item(&key, &text, now + i as u64),
        });
    }
    h.items = items;
    h.pending = true;
    hotwords::save(&h)?;
    if st.paired() {
        let data = hotwords::encode(&h.items, st.session.uin(), now);
        let v = st.session.send_hot_words(st.group.group_id, h.version, &data).await?;
        h.version = v;
        h.pending = false;
        hotwords::save(&h)?;
        log!("hot words uploaded ({} items, version {v})", h.items.len());
    }
    Ok(h)
}

/// 出码方：显示自己的码，等对方输入后确认关联。
async fn pair_show_code(s: Arc<Session>, reply: Reply) -> Res<u64> {
    let (code, ttl) = s.match_code().await?;
    let _ = reply.send(json!({"type": "code", "code": code, "ttl": ttl}));
    let deadline = Instant::now() + Duration::from_secs(ttl);
    while Instant::now() < deadline {
        tokio::time::sleep(Duration::from_secs(2)).await;
        if reply.is_closed() {
            let _ = s.release_code(&code).await;
            return Err("cancelled".into());
        }
        let (st, bind_id) = s.match_code_status(&code).await?;
        match st {
            1 if !bind_id.is_empty() => {
                let _ = reply.send(info("设备匹配中..."));
                return s.real_bind(&bind_id).await;
            }
            2 => return Err("匹配码已失效".into()),
            _ => {}
        }
    }
    let _ = s.release_code(&code).await;
    Err("匹配码已过期".into())
}

/// 输码方：输入对方（手机）显示的码，等对方确认。
async fn pair_enter_code(s: Arc<Session>, peer_code: String, reply: Reply) -> Res<u64> {
    let (self_code, _) = s.match_code().await?;
    if let Err(e) = s.bind_code(&peer_code, &self_code).await {
        let _ = s.release_code(&self_code).await;
        return Err(e);
    }
    let _ = reply.send(info("请在另一台设备上确认关联"));
    let deadline = Instant::now() + Duration::from_secs(120);
    while Instant::now() < deadline {
        tokio::time::sleep(Duration::from_secs(2)).await;
        if reply.is_closed() {
            let _ = s.release_code(&self_code).await;
            return Err("cancelled".into());
        }
        match s.group(1, 0).await {
            Ok(g) if g.group_id != 0 && g.devices.iter().any(|d| d.uin != s.uin()) => return Ok(g.group_id),
            Ok(_) => {}
            Err(e) => {
                let _ = s.release_code(&self_code).await;
                return Err(e);
            }
        }
    }
    let _ = s.release_code(&self_code).await;
    Err("超时，对方没有确认".into())
}

async fn handle_ctl(st: &mut State<'_>, c: Ctl, reply: Reply, refresh: &mpsc::UnboundedSender<()>) {
    match c {
        Ctl::Status => {
            let _ = reply.send(st.status());
        }
        Ctl::Send(text) => {
            if !st.paired() {
                let _ = reply.send(err("not paired"));
                return;
            }
            st.last_clip = Some(text.clone());
            let _ = reply.send(match st.session.send_clip_text(st.group.group_id, &text).await {
                Ok(v) => {
                    st.persist.sent += 1;
                    json!({"type": "ok", "version": v})
                }
                Err(e) => err(e),
            });
        }
        Ctl::HotWords => {
            let h = hotwords::load();
            let _ = reply.send(json!({"type": "hotwords", "version": h.version, "pending": h.pending, "items": h.items}));
        }
        Ctl::HotWordsSet(arg) => {
            let _ = reply.send(match set_hot_words(st, &arg).await {
                Ok(h) => json!({"type": "hotwords", "version": h.version, "pending": h.pending, "items": h.items}),
                Err(e) => err(e),
            });
        }
        Ctl::Rename(name) => {
            let name = name.trim().chars().take(32).collect::<String>();
            if name.is_empty() {
                let _ = reply.send(err("用法：rename <设备名>"));
                return;
            }
            let _ = reply.send(match st.session.update_device_info(Some(&name), &[]).await {
                Ok(0) => {
                    st.cfg.device_name = name.clone();
                    st.session.set_device_name(&name);
                    refresh_group(st).await;
                    json!({"type": "ok", "renamed": name})
                }
                Ok(code) => err(format!("update_device_info err_code={code}")),
                Err(e) => err(e),
            });
        }
        Ctl::Fetch => {
            let _ = reply.send(match st.session.group(2, 0).await {
                Ok(g) => match g.clip {
                    Some(clip) => {
                        let v = json!({"type": "ok", "version": clip.version, "kind": clip.kind, "from": clip.from_uin});
                        st.last_clip = None;
                        apply_remote(st, clip).await;
                        v
                    }
                    None => err("group has no clipboard"),
                },
                Err(e) => err(e),
            });
        }
        Ctl::Unpair(which) => {
            let targets: Vec<u64> = if which == "all" {
                st.peers().map(|d| d.uin).collect()
            } else {
                match which.parse::<u64>() {
                    Ok(u) if st.peers().any(|d| d.uin == u) => vec![u],
                    _ => {
                        let _ = reply.send(err("用法：unpair <uin|all>（uin 见 status）"));
                        return;
                    }
                }
            };
            for p in targets {
                let _ = reply.send(match st.session.unbind(p).await {
                    Ok(0) => json!({"type": "ok", "unbound": p}),
                    Ok(code) => err(format!("unbind {p}: err_code={code}")),
                    Err(e) => err(e),
                });
            }
            refresh_group(st).await;
        }
        Ctl::Raw(arg) => {
            let p: Vec<&str> = arg.split_whitespace().collect();
            let parsed = match p.as_slice() {
                [c, sb] => c.parse::<u32>().ok().zip(sb.parse::<u32>().ok()).map(|(c, sb)| (c, sb, Vec::new())),
                [c, sb, h] => c.parse::<u32>().ok().zip(sb.parse::<u32>().ok()).zip(hex::decode(h).ok()).map(|((c, sb), b)| (c, sb, b)),
                _ => None,
            };
            let _ = reply.send(match parsed {
                Some((c, sb, body)) => match st.session.api(c, sb, &body, Duration::from_secs(10)).await {
                    Ok(r) => json!({"type": "raw", "hex": hex::encode(r)}),
                    Err(e) => err(e),
                },
                None => err("用法：raw <cmd> <sub> [hex]"),
            });
        }
        Ctl::Migrate => {
            let s = st.session.clone();
            let http = st.http.clone();
            tokio::spawn(async move {
                let r = minime_netd::migrate::run(s, http, reply.clone()).await;
                let _ = reply.send(match r {
                    Ok(v) => v,
                    Err(e) => err(e),
                });
            });
        }
        Ctl::Pair(code) => {
            // 关联要轮询几分钟，放到单独的任务里，主循环继续收推送
            let s = st.session.clone();
            let refresh = refresh.clone();
            tokio::spawn(async move {
                let r = match code {
                    None => pair_show_code(s, reply.clone()).await,
                    Some(c) => pair_enter_code(s, c, reply.clone()).await,
                };
                let _ = reply.send(match r {
                    Ok(gid) => {
                        let _ = refresh.send(());
                        json!({"type": "paired", "group": gid})
                    }
                    Err(e) => err(e),
                });
            });
        }
    }
}

async fn run_session(
    cfg: &Config,
    mut cfg_mtime: Option<std::time::SystemTime>,
    ctl_rx: &mut mpsc::UnboundedReceiver<(Ctl, Reply)>,
    persist: &mut Persist,
    shared: &EngineNet,
) -> Exit {
    let id_path = identity::identity_path();
    let saved = identity::load(&id_path);
    let mut ident = saved.clone().unwrap_or_else(|| Identity::fresh(&cfg.device_name));
    ident.device_name = cfg.device_name.clone();
    let (session, mut push_rx) = match Session::connect(ident).await {
        Ok(v) => v,
        Err(e) => return Exit::Lost(format!("connect: {e}")),
    };
    let setup = async {
        session.sync_timestamp().await?;
        session.handshake().await?;
        session.ensure_uin().await?;
        session.subscribe().await?;
        Res::Ok(())
    };
    if let Err(e) = setup.await {
        session.close();
        return Exit::Lost(format!("setup: {e}"));
    }
    let now = session.identity();
    let changed = match &saved {
        None => true,
        Some(s) => s.uin != now.uin || s.cached_server_pub != now.cached_server_pub || s.device_name != now.device_name,
    };
    if changed {
        if let Err(e) = identity::save(&id_path, &now) {
            log!("cannot save identity {}: {e}", id_path.display());
        }
        if saved.is_none() {
            log!("registered anonymous device, uin {}", now.uin);
        }
    }
    let http = reqwest::Client::builder().timeout(Duration::from_secs(30)).build().unwrap_or_default();
    *shared.lock().unwrap() = Some(EngineCtx {
        session: session.clone(),
        cloud: cfg.cloud,
        user_dict: cfg.user_dict,
        http: reqwest::Client::builder().timeout(Duration::from_secs(120)).build().unwrap_or_default(),
    });
    let mut st = State {
        session: session.clone(),
        cfg: cfg.clone(),
        group: Group::default(),
        last_clip: None,
        ignore_image_echo_until: None,
        persist,
        http,
    };
    refresh_group(&mut st).await;
    // 设备名同步到组里（sub 79）：设置里改名后会重连，走到这里就生效
    if st.paired() {
        let me = session.uin();
        let shown = st.group.devices.iter().find(|d| d.uin == me).map(|d| d.name.clone()).unwrap_or_default();
        if shown != cfg.device_name {
            match session.update_device_info(Some(&cfg.device_name), &[]).await {
                Ok(0) => {
                    log!("device name updated: {shown:?} -> {:?}", cfg.device_name);
                    refresh_group(&mut st).await;
                }
                Ok(code) => log!("device rename err_code={code}"),
                Err(e) => log!("device rename failed: {e}"),
            }
        }
    }
    sync_hot_words(&mut st).await;
    engine_dict_sync(&mut st, "connect").await;
    fetch_ab(&session).await;
    log!("connected (uin {}, paired={})", session.uin(), st.paired());

    let mut clip_rx = if cfg.clipboard {
        match clipboard::watch() {
            Ok(rx) => Some(rx),
            Err(e) => {
                log!("wl-paste unavailable ({e}); clipboard sync disabled");
                None
            }
        }
    } else {
        None
    };
    // 启动时剪贴板里已有的内容不发：只同步之后的变化
    st.last_clip = fingerprint(&clipboard::read(cfg.images).await);
    // 断线期间对端发过剪贴板：补拉那一条（首次启动只记版本，不应用旧内容）
    if st.paired() {
        match session.group(2, 0).await {
            Ok(g) => {
                if let Some(clip) = g.clip {
                    match st.persist.clip_version {
                        Some(v) if clip.version > v => {
                            log!("catching up clipboard version {} (had {v})", clip.version);
                            apply_remote(&mut st, clip).await;
                        }
                        _ => st.persist.clip_version = Some(clip.version),
                    }
                }
            }
            Err(e) => log!("clipboard catch-up failed: {e}"),
        }
    }
    let (refresh_tx, mut refresh_rx) = mpsc::unbounded_channel::<()>();
    let mut hb = tokio::time::interval(HEARTBEAT);
    hb.tick().await;
    let mut cfg_tick = tokio::time::interval(CONFIG_POLL);

    let exit = loop {
        tokio::select! {
            f = push_rx.recv() => match f {
                Some(frame) => on_push(&mut st, &frame, &refresh_tx).await,
                None => break Exit::Lost("connection closed".into()),
            },
            res = async { match clip_rx.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => match res {
                Some(()) => on_local_clip(&mut st).await,
                None => {
                    log!("wl-paste --watch exited; clipboard watch disabled");
                    clip_rx = None;
                }
            },
            Some(()) = refresh_rx.recv() => {
                if refresh_group(&mut st).await {
                    sync_hot_words(&mut st).await;
                    engine_dict_sync(&mut st, "paired").await;
                }
            }
            Some((c, reply)) = ctl_rx.recv() => handle_ctl(&mut st, c, reply, &refresh_tx).await,
            _ = hb.tick() => {
                if let Err(e) = session.heartbeat().await {
                    break Exit::Lost(format!("heartbeat: {e}"));
                }
                // 对端在手机上增删设备时不一定有推送（实测删除设备没收到 event 6），
                // 和官方客户端一样定期拉组信息
                if refresh_group(&mut st).await {
                    sync_hot_words(&mut st).await;
                    engine_dict_sync(&mut st, "paired").await;
                }
                if st.persist.dict_sync_at.map(|t| t.elapsed() > DICT_SYNC_EVERY).unwrap_or(true) {
                    engine_dict_sync(&mut st, "timer").await;
                }
            }
            _ = cfg_tick.tick() => {
                let m = config::mtime();
                if m != cfg_mtime {
                    cfg_mtime = m;
                    if config::load().0 != *cfg {
                        break Exit::ConfigChanged;
                    }
                }
            }
        }
    };
    *shared.lock().unwrap() = None;
    session.close();
    exit
}

// ------------------------------------------------------------------ 引擎网络兼容层

/// 当前会话和放行范围，给 engined 的 ime_net 兼容层用（src/netcompat.c）
#[derive(Clone)]
struct EngineCtx {
    session: Arc<Session>,
    /// 云输入：引擎的请求全部放行（上报类除外）
    cloud: bool,
    /// 同步个人词库：只放行组同步类请求和 COS 上传下载
    user_dict: bool,
    http: reqwest::Client,
}
type EngineNet = Arc<std::sync::Mutex<Option<EngineCtx>>>;

/// 兼容层的 COS 任务（不是服务器业务号）：body = url \0 本地文件
const COS_UPLOAD: i32 = -100;
const COS_DOWNLOAD: i32 = -101;
/// 引擎个人词库同步用到的 4814 子命令：34 组信息、41 上传增量、42 拉增量、44 全量信息、60 COS 凭证……
const USER_DICT_SUBS: &[i32] = &[34, 41, 42, 43, 44, 45, 46, 47, 48, 49, 60];

/// 引擎的 COS 上传 / 下载（预签名链接，只允许腾讯云 COS 的地址）
async fn engine_cos(http: &reqwest::Client, upload: bool, body: &[u8]) -> Res<usize> {
    let mut it = body.splitn(2, |b| *b == 0);
    let url = String::from_utf8_lossy(it.next().unwrap_or_default()).to_string();
    let path = PathBuf::from(String::from_utf8_lossy(it.next().unwrap_or_default()).to_string());
    if !trusted_image_url(&url) {
        return Err(format!("refused COS url host: {}", url.split('?').next().unwrap_or("")).into());
    }
    if upload {
        let data = tokio::fs::read(&path).await?;
        let n = data.len();
        http.put(&url).header("Content-Type", "application/octet-stream").body(data).send().await?.error_for_status()?;
        Ok(n)
    } else {
        let data = download(http, &url).await?;
        use std::os::unix::fs::OpenOptionsExt;
        let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&path)?;
        std::io::Write::write_all(&mut f, &data)?;
        Ok(data.len())
    }
}

fn engine_net_path() -> PathBuf {
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    PathBuf::from(dir).join("minime-netd-net.sock")
}

/// 二进制帧（小端）：请求 u32 len | u32 task | i32 cmd | i32 sub | u32 timeout_ms | body；
/// 响应 u32 len | u32 task | i32 err | body。
fn spawn_engine_net(shared: EngineNet) -> std::io::Result<()> {
    use std::os::unix::fs::PermissionsExt;
    let path = engine_net_path();
    let _ = std::fs::remove_file(&path);
    let listener = UnixListener::bind(&path)?;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
    tokio::spawn(async move {
        loop {
            let Ok((s, _)) = listener.accept().await else { continue };
            let shared = shared.clone();
            tokio::spawn(async move {
                use tokio::io::AsyncReadExt;
                let (mut rd, wr) = s.into_split();
                let wr = Arc::new(tokio::sync::Mutex::new(wr));
                loop {
                    let mut hdr = [0u8; 20];
                    if rd.read_exact(&mut hdr).await.is_err() {
                        break;
                    }
                    let f = |i: usize| u32::from_le_bytes(hdr[i..i + 4].try_into().unwrap());
                    let (len, task, cmd, sub, timeout) = (f(0), f(4), f(8) as i32, f(12) as i32, f(16));
                    if len < 16 || len > (32 << 20) {
                        break;
                    }
                    let mut body = vec![0u8; (len - 16) as usize];
                    if rd.read_exact(&mut body).await.is_err() {
                        break;
                    }
                    let (shared, wr) = (shared.clone(), wr.clone());
                    tokio::spawn(async move {
                        let (err, resp) = engine_task(&shared, cmd, sub, &body, timeout).await;
                        let mut out = Vec::with_capacity(12 + resp.len());
                        out.extend_from_slice(&(8 + resp.len() as u32).to_le_bytes());
                        out.extend_from_slice(&task.to_le_bytes());
                        out.extend_from_slice(&err.to_le_bytes());
                        out.extend_from_slice(&resp);
                        let _ = wr.lock().await.write_all(&out).await;
                    });
                }
            });
        }
    });
    Ok(())
}

/// 引擎发的请求里 uin 是 0：顶层 field 1 = varint 0（41/42/34……），或者顶层 field 1 是子消息、
/// 子消息开头是 varint 0（43 UpdateUserDictFullSyncInfo）。补成本机 uin；不是这两种形状返回 None。
fn fill_uin(body: &[u8], uin: u64) -> Option<Vec<u8>> {
    use minime_netd::pb::Pb;
    if body.starts_with(&[0x08, 0x00]) {
        let mut b = Pb::new().varint(1, uin).into_vec();
        b.extend_from_slice(&body[2..]);
        return Some(b);
    }
    if body.first() != Some(&0x0a) {
        return None;
    }
    // 读子消息长度（varint）
    let (mut len, mut i, mut shift) = (0usize, 1usize, 0);
    loop {
        let c = *body.get(i)?;
        len |= ((c & 0x7f) as usize) << shift;
        i += 1;
        if c < 0x80 {
            break;
        }
        shift += 7;
        if shift > 28 {
            return None;
        }
    }
    let inner = body.get(i..i + len)?;
    if !inner.starts_with(&[0x08, 0x00]) {
        return None;
    }
    let mut new_inner = Pb::new().varint(1, uin).into_vec();
    new_inner.extend_from_slice(&inner[2..]);
    let mut b = Pb::new().bytes(1, &new_inner).into_vec();
    b.extend_from_slice(&body[i + len..]);
    Some(b)
}

async fn engine_task(shared: &EngineNet, cmd: i32, sub: i32, body: &[u8], timeout_ms: u32) -> (i32, Vec<u8>) {
    // 上屏内容上报、日志上报：不发（engined 那边也会拦，这里再挡一次）
    if (cmd == 4814 && (sub == 17 || sub == 91)) || cmd == 4959 {
        log!("engine task {cmd}/{sub}: blocked (report)");
        return (-1, Vec::new());
    }
    let cur = shared.lock().unwrap().clone();
    let Some(ctx) = cur else { return (-2, Vec::new()) };
    let is_cos = cmd == COS_UPLOAD || cmd == COS_DOWNLOAD;
    let dict_ok = ctx.user_dict && (is_cos || (cmd == 4814 && USER_DICT_SUBS.contains(&sub)));
    // 云输入只放行云拼音（4329）；4814 下的其他子命令（91 等，可能是上屏/AI 类上报）一律不发，
    // 只在日志里留个头便于识别
    let cloud_ok = ctx.cloud && cmd == 4329;
    if !cloud_ok && !dict_ok {
        if ctx.cloud {
            log!("engine task {cmd}/{sub}: blocked (not allow-listed), {} B: {}", body.len(), hex::encode(&body[..body.len().min(48)]));
        }
        return (-3, Vec::new());
    }
    if is_cos {
        let t0 = Instant::now();
        let up = cmd == COS_UPLOAD;
        return match engine_cos(&ctx.http, up, body).await {
            Ok(n) => {
                log!("engine COS {}: {n} B, {} ms", if up { "upload" } else { "download" }, t0.elapsed().as_millis());
                (0, Vec::new())
            }
            Err(e) => {
                log!("engine COS {}: {e}", if up { "upload" } else { "download" });
                (-1, Vec::new())
            }
        };
    }
    let s = ctx.session;
    let to = Duration::from_millis(timeout_ms.clamp(1000, 60000) as u64);
    let t0 = Instant::now();
    // 引擎组同步类请求（34 组信息、41/42 个人词库……）的 field 1 是 uin，引擎自己不知道（填 0），
    // 官方客户端由网络层补上；这里同样用 netd 的匿名身份补
    let patched;
    let body = match (cmd == 4814).then(|| fill_uin(body, s.uin())).flatten() {
        Some(b) => {
            patched = b;
            &patched[..]
        }
        None => body,
    };
    let r = if cmd == 4814 || cmd == 4777 {
        s.api(cmd as u32, sub as u32, body, to).await
    } else {
        s.api_raw(cmd as u32, sub as u32, body, to).await
    };
    match r {
        Ok(v) => {
            log!("engine task {cmd}/{sub}: {} B -> {} B, {} ms", body.len(), v.len(), t0.elapsed().as_millis());
            if std::env::var("MINIME_NETD_DUMP_ENGINE").is_ok() {
                log!("  req  {}", hex::encode(&body[..body.len().min(96)]));
                log!("  resp {}", hex::encode(&v[..v.len().min(160)]));
            }
            (0, v)
        }
        Err(e) => {
            log!("engine task {cmd}/{sub}: {e}");
            (-1, Vec::new())
        }
    }
}

// ------------------------------------------------------------------ 主循环

fn idle_reply(c: Ctl, reply: Reply, cfg: &Config, why: &str) {
    let _ = reply.send(match c {
        Ctl::Status => json!({
            "type": "status", "enabled": cfg.enabled, "connected": false, "running": true,
            "reason": why, "device": cfg.device_name, "clipboard": cfg.clipboard, "images": cfg.images,
            "config": config::path(),
        }),
        _ => err(format!("not connected ({why})")),
    });
}

async fn daemon() -> i32 {
    let _ = rustls::crypto::ring::default_provider().install_default();
    let (ctl_tx, mut ctl_rx) = mpsc::unbounded_channel::<(Ctl, Reply)>();
    if let Err(e) = spawn_control(ctl_tx) {
        log!("cannot listen on {}: {e}", socket_path().display());
        return 1;
    }
    log!("started (config {}, data {})", config::path().display(), identity::data_dir().display());
    let engine_net: EngineNet = Arc::new(std::sync::Mutex::new(None));
    if let Err(e) = spawn_engine_net(engine_net.clone()) {
        log!("cannot listen on engine net socket: {e}");
    }
    let mut persist = Persist::default();
    let mut backoff = Duration::from_secs(1);
    loop {
        let (cfg, cfg_mtime) = config::load();
        if !cfg.enabled {
            // 不联网：只回答控制命令，等配置打开
            loop {
                tokio::select! {
                    Some((c, r)) = ctl_rx.recv() => idle_reply(c, r, &cfg, "network disabled"),
                    _ = tokio::time::sleep(CONFIG_POLL) => if config::mtime() != cfg_mtime { break },
                }
            }
            continue;
        }
        if !minime_netd::crypto::keys_ready() {
            // 协议常量从官方 ime_net.dll 里读；还没有用 minime-fetch 下载时先等着，下载后自动连上
            log!("official files not found (run minime-fetch); waiting");
            let until = Instant::now() + Duration::from_secs(30);
            while Instant::now() < until {
                tokio::select! {
                    Some((c, r)) = ctl_rx.recv() => idle_reply(c, r, &cfg, "official files missing: run minime-fetch"),
                    _ = tokio::time::sleep(CONFIG_POLL) => if config::mtime() != cfg_mtime { break },
                }
            }
            continue;
        }
        let started = Instant::now();
        match run_session(&cfg, cfg_mtime, &mut ctl_rx, &mut persist, &engine_net).await {
            Exit::ConfigChanged => {
                log!("config changed");
                backoff = Duration::from_secs(1);
                continue;
            }
            Exit::Lost(why) => {
                if started.elapsed() > Duration::from_secs(120) {
                    backoff = Duration::from_secs(1);
                }
                log!("{why}; reconnect in {}s", backoff.as_secs());
                let until = Instant::now() + backoff;
                let m0 = config::mtime();
                while Instant::now() < until {
                    tokio::select! {
                        Some((c, r)) = ctl_rx.recv() => idle_reply(c, r, &cfg, "reconnecting"),
                        _ = tokio::time::sleep_until(until.into()) => {}
                        _ = tokio::time::sleep(CONFIG_POLL) => if config::mtime() != m0 { break },
                    }
                }
                backoff = (backoff * 2).min(BACKOFF_MAX);
            }
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    for m in minime_netd::legacy::migrate() {
        eprintln!("[netd] 改名迁移：{m}");
    }
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio runtime");
    let rc = rt.block_on(async {
        match args.iter().find(|a| *a != "--json").map(|s| s.as_str()) {
            None | Some("run") if !args.iter().any(|a| a == "--json") => daemon().await,
            Some("status" | "pair" | "unpair" | "send" | "fetch" | "hotwords" | "hotwords-set" | "rename" | "migrate" | "raw") => client_main(&args).await,
            _ => {
                eprintln!("用法: minime-netd [run | [--json] status | pair [码] | unpair <uin|all> | send <文本>]");
                2
            }
        }
    });
    std::process::exit(rc);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uin_fill() {
        assert_eq!(fill_uin(&[0x08, 0x00, 0x10, 0x06], 300).unwrap(), vec![0x08, 0xac, 0x02, 0x10, 0x06]);
        // 嵌套：{1: {1: 0, 2: 5}, 2: 7}
        let b = [0x0a, 0x04, 0x08, 0x00, 0x10, 0x05, 0x10, 0x07];
        assert_eq!(fill_uin(&b, 300).unwrap(), vec![0x0a, 0x05, 0x08, 0xac, 0x02, 0x10, 0x05, 0x10, 0x07]);
        assert!(fill_uin(&[0x08, 0x05], 300).is_none());
        assert!(fill_uin(&[0x0a, 0x02, 0x08, 0x05], 300).is_none());
    }

    #[test]
    fn url_filter() {
        assert!(trusted_image_url("https://wxkb-1258344707.cos.ap-shanghai.myqcloud.com/a.png"));
        assert!(!trusted_image_url("http://x.myqcloud.com/a"));
        assert!(!trusted_image_url("https://evil.com/x.myqcloud.com"));
        assert!(!trusted_image_url("https://myqcloud.com.evil.com/"));
    }
}

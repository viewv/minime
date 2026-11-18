//! 一条 WebSocket 连接上的会话：帧封装、请求/响应路由、推送、业务请求。
//!
//! 对照 开发时的 Python 参考实现（未公开）。断线由上层处理：会话对象只代表一次连接，
//! 断了就整体丢弃、重建（新的 ECDH 密钥、重新握手）。

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use futures_util::{SinkExt, StreamExt};
use rand::Rng;
use tokio::sync::{mpsc, oneshot};
use tokio_tungstenite::tungstenite::{client::IntoClientRequest, Message};

use crate::crypto::{self, EcdhKey};
use crate::identity::Identity;
use crate::pb::{Msg, Pb};

pub type Res<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;

pub const HOST: &str = "wetype.weixin.qq.com";
const SUBPROTOCOL: &str = "wxws_pb";

pub const CMD_SYNC: u32 = 4814;
pub const CMD_SYNC_START: u32 = 8074;
const CMD_HANDSHAKE: u64 = 0x7FFF_FFFE;
const CMD_GEN_UIN: u64 = 0x7FFF_FFFD;

const COMPRESS_NONE: u64 = 0;
const COMPRESS_SNAPPY: u64 = 2;
const COMPRESS_MIN_SIZE: usize = 301;

pub const TASK_TIMEOUT: Duration = Duration::from_secs(20);

/// ClipboardType（multi_platform_sync.proto）：
/// 1 TypeTextOri 文本直发 / 2 TypeTextCos 长文本走 COS（data 是链接）/
/// 5 TypeImageOri 图片字节直发 / 6 TypeImageCos 图片走 COS（data 是链接）。
pub const CLIP_TEXT: u64 = 1;
pub const CLIP_TEXT_COS: u64 = 2;
pub const CLIP_IMAGE_ORI: u64 = 5;
pub const CLIP_IMAGE: u64 = 6;

#[derive(Clone, Copy, PartialEq)]
enum SignKind {
    Dh,
    Uin,
    Api,
}

struct Keys {
    aes: Vec<u8>,
    token: String,
}

pub struct Session {
    ident: Mutex<Identity>,
    out: mpsc::UnboundedSender<Message>,
    pending: Mutex<HashMap<u64, oneshot::Sender<Vec<u8>>>>,
    keys: Mutex<Keys>,
    ecdh: EcdhKey,
    static_key: &'static [u8],
    sign_key: &'static str,
    ts_offset: AtomicI64,
    task: AtomicU64,
    closed: AtomicBool,
    reader_abort: Mutex<Option<tokio::task::AbortHandle>>,
}

/// 推送事件（cmd 8074 订阅后服务端主动发来的 notify 帧）。
#[derive(Debug, Clone)]
pub struct Push {
    pub event: u64,
    pub group_id: u64,
    pub clip: Option<Clip>,
    /// event 5 PushUserHotWords{1 group_id, 2 version, 3 data}
    pub hot_words: Option<(u64, Vec<u8>)>,
}

#[derive(Debug, Clone)]
pub struct Clip {
    pub version: u64,
    pub kind: u64,
    pub data: Vec<u8>,
    pub from_uin: u64,
    pub expiration: u64,
    pub extra_len: usize,
}

#[derive(Debug, Clone, Default)]
pub struct Device {
    pub uin: u64,
    pub name: String,
    pub platform: u64,
    pub version: String,
}

#[derive(Debug, Clone, Default)]
pub struct Group {
    pub err_code: u64,
    pub group_id: u64,
    pub func_switch: u64,
    pub devices: Vec<Device>,
    pub clip: Option<Clip>,
    /// 常用语（UserHotWords{1 version, 2 data}），filter 含 4 时才有
    pub hot_words: Option<(u64, Vec<u8>)>,
    /// GetGroupInfoRsp.device_remark_name(3)
    pub remark: String,
}

fn now_ms() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

fn rand_trace() -> String {
    let mut r = rand::thread_rng();
    (0..16).map(|_| (b'a' + r.gen_range(0..26)) as char).collect()
}

fn parse_clip(m: &Msg) -> Clip {
    Clip {
        version: m.int(1),
        kind: m.int(2),
        // 服务端回包把内容放在 binary_data(6)；请求里是 data(3)。两个都看。
        data: if m.has(6) { m.bytes(6).to_vec() } else { m.bytes(3).to_vec() },
        from_uin: m.int(7),
        expiration: m.int(5),
        extra_len: m.bytes(8).len(),
    }
}

const MAX_SNAPPY_UNPACK: usize = 32 << 20;

fn snap_decompress_capped(src: &[u8]) -> Res<Vec<u8>> {
    let n = snap::raw::decompress_len(src)?;
    if n > MAX_SNAPPY_UNPACK {
        return Err(format!("snappy payload too large ({n} bytes)").into());
    }
    Ok(snap::raw::Decoder::new().decompress_vec(src)?)
}

impl Session {
    /// 建立 WebSocket 连接。返回会话和推送接收端；推送接收端关闭 = 连接断了。
    pub async fn connect(ident: Identity) -> Res<(Arc<Session>, mpsc::UnboundedReceiver<Vec<u8>>)> {
        let mut req = format!("wss://{HOST}/").into_client_request()?;
        req.headers_mut().insert("Sec-WebSocket-Protocol", SUBPROTOCOL.parse()?);
        let (ws, _) = tokio::time::timeout(Duration::from_secs(15), tokio_tungstenite::connect_async(req))
            .await
            .map_err(|_| "connect timeout")??;
        let (mut sink, mut stream) = ws.split();
        let (out_tx, mut out_rx) = mpsc::unbounded_channel::<Message>();
        let (push_tx, push_rx) = mpsc::unbounded_channel::<Vec<u8>>();

        let platform = ident.platform;
        let static_key = crypto::static_aes_key(platform);
        let s = Arc::new(Session {
            ident: Mutex::new(ident),
            out: out_tx,
            pending: Mutex::new(HashMap::new()),
            keys: Mutex::new(Keys { aes: static_key.to_vec(), token: String::new() }),
            ecdh: EcdhKey::generate(),
            static_key,
            sign_key: crypto::sign_key(platform),
            ts_offset: AtomicI64::new(0),
            task: AtomicU64::new(0),
            closed: AtomicBool::new(false),
            reader_abort: Mutex::new(None),
        });

        tokio::spawn(async move {
            while let Some(m) = out_rx.recv().await {
                if sink.send(m).await.is_err() {
                    break;
                }
            }
            let _ = sink.close().await;
        });

        let weak = Arc::downgrade(&s);
        let reader_handle = tokio::spawn(async move {
            while let Some(frame) = stream.next().await {
                let data = match frame {
                    Ok(Message::Binary(d)) => d,
                    Ok(Message::Close(_)) | Err(_) => break,
                    Ok(_) => continue,
                };
                let Some(s) = weak.upgrade() else { break };
                let Ok(outer) = Msg::parse(&data) else { continue };
                if outer.bytes(3) == b"notify" {
                    let _ = push_tx.send(data);
                    continue;
                }
                let task = outer.int(2);
                let tx = s.pending.lock().unwrap().remove(&task);
                if let Some(tx) = tx {
                    let _ = tx.send(data);
                }
            }
            if let Some(s) = weak.upgrade() {
                s.closed.store(true, Ordering::SeqCst);
                s.pending.lock().unwrap().clear(); // 等待者收到 RecvError
            }
            drop(push_tx);
        });
        *s.reader_abort.lock().unwrap() = Some(reader_handle.abort_handle());
        Ok((s, push_rx))
    }

    /// 改本端设备名（下一次绑定/握手时生效）。
    pub fn set_device_name(&self, name: &str) {
        self.ident.lock().unwrap().device_name = name.to_string();
    }

    pub fn identity(&self) -> Identity {
        self.ident.lock().unwrap().clone()
    }

    pub fn uin(&self) -> u64 {
        self.ident.lock().unwrap().uin
    }

    pub fn is_closed(&self) -> bool {
        self.closed.load(Ordering::SeqCst)
    }

    pub fn close(&self) {
        self.closed.store(true, Ordering::SeqCst);
        self.pending.lock().unwrap().clear();
        let _ = self.out.send(Message::Close(None));
        if let Some(abort) = self.reader_abort.lock().unwrap().take() {
            // 给对端留短暂时间回 Close，超时则强行中止读任务避免网络黑洞永久泄漏
            tokio::spawn(async move {
                tokio::time::sleep(Duration::from_secs(1)).await;
                abort.abort();
            });
        }
    }

    fn server_ms(&self) -> i64 {
        now_ms() + self.ts_offset.load(Ordering::Relaxed)
    }

    #[allow(clippy::too_many_arguments)]
    async fn send(
        &self,
        path: &str,
        task: u64,
        body: &[u8],
        kind: SignKind,
        cmd: u32,
        sub: u32,
        aes: &[u8],
        with_token: bool,
        compression: u64,
        timeout: Duration,
    ) -> Res<(u64, HashMap<String, String>, Vec<u8>)> {
        if self.is_closed() {
            return Err("connection closed".into());
        }
        let (os_type, app_ver, sys_ver, pkg, scene, uin) = {
            let i = self.ident.lock().unwrap();
            (i.os_type.to_string(), i.app_ver.clone(), i.system_ver.clone(), i.package_type, i.scene.to_string(), i.uin)
        };
        let token = self.keys.lock().unwrap().token.clone();
        let md5 = crypto::md5_upper(body);
        let ts = self.server_ms().to_string();
        let trace = rand_trace();
        let task_s = task.to_string();
        let sig = match kind {
            SignKind::Dh => crypto::signature(self.sign_key, &[&os_type, &app_ver, &scene, &ts, &md5, &trace, &task_s]),
            SignKind::Uin => {
                crypto::signature(self.sign_key, &[&os_type, &app_ver, &scene, &ts, &md5, &token, &trace, &task_s])
            }
            SignKind::Api => crypto::signature(
                self.sign_key,
                &[&os_type, &app_ver, &scene, &cmd.to_string(), &sub.to_string(), &ts, &md5, &uin.to_string(), &trace, &task_s],
            ),
        };

        let mut headers: Vec<(&str, String)> = vec![("Kb-Uin", if kind == SignKind::Uin { "0".into() } else { uin.to_string() })];
        if with_token && !token.is_empty() {
            headers.push(("Kb-GenUinToken", token));
        }
        if kind == SignKind::Api {
            let suffix = String::from_utf8_lossy(&aes[aes.len().saturating_sub(4)..]).into_owned();
            headers.push(("Kb-SharedKeySuffix", suffix));
            headers.push(("Kb-CmdId", cmd.to_string()));
            headers.push(("Kb-SubCmdId", sub.to_string()));
        }
        headers.extend([
            ("Kb-DeviceCodeRestrictionV2", "1".to_string()),
            ("Kb-OsType", os_type.clone()),
            ("Kb-Version", app_ver),
            ("Kb-SystemVersion", sys_ver),
            ("Kb-PackageType", pkg.to_string()),
            ("Use_DebugNet", "0".into()),
            ("Kb-TimeStamp", ts),
            ("Kb-BodyMd5", md5),
            ("Kb-TraceId", trace),
            ("Kb-TaskId", task_s),
            ("Kb-Scene", scene),
            ("Kb-Sign", sig),
            ("Content-Length", body.len().to_string()),
            ("Kb-CompressionType", compression.to_string()),
            ("Content-Type", "application/octet-stream".into()),
            ("HOST", HOST.into()),
        ]);

        let mut inner = Pb::new().str(1, "POST").str(3, path).str(4, "");
        for (k, v) in &headers {
            inner = inner.msg(5, &Pb::new().str(1, k).str(2, v));
        }
        inner = inner.bytes(6, body);
        let frame = Pb::new().varint(1, 0).varint(2, 0).varint(3, task).varint(4, 0).msg(5, &inner);

        let (tx, rx) = oneshot::channel();
        {
            let mut p = self.pending.lock().unwrap();
            if self.is_closed() {
                return Err("connection closed".into());
            }
            p.insert(task, tx);
        }
        if self.out.send(Message::Binary(frame.into_vec())).is_err() {
            self.pending.lock().unwrap().remove(&task);
            return Err("connection closed".into());
        }
        let data = match tokio::time::timeout(timeout, rx).await {
            Ok(Ok(d)) => d,
            Ok(Err(_)) => return Err("connection closed".into()),
            Err(_) => {
                self.pending.lock().unwrap().remove(&task);
                return Err(format!("task {task} ({path} {cmd}/{sub}) timeout").into());
            }
        };
        let outer = Msg::parse(&data)?;
        let resp = outer.sub(4)?;
        let mut hdrs = HashMap::new();
        for hv in resp.all_bytes(4) {
            let hh = Msg::parse(hv)?;
            hdrs.insert(hh.str(1), hh.str(2));
        }
        Ok((resp.int(2), hdrs, resp.bytes(5).to_vec()))
    }

    /// `/timestamp`：取服务器时钟偏移（签名里的时间戳要用服务器时间）。
    pub async fn sync_timestamp(&self) -> Res<i64> {
        let local = now_ms();
        let (status, _, body) = self
            .send("/timestamp", 0, b"", SignKind::Api, 0, 0, self.static_key, false, 0, TASK_TIMEOUT)
            .await?;
        if status != 200 || body.is_empty() {
            return Err(format!("timestamp failed: {status}").into());
        }
        // /timestamp 实测返回明文 protobuf（长度非 16 倍数），兼容可能加密的回包并校验时间戳合理性
        let plain = match crypto::aes_decrypt(&body, self.static_key) {
            Ok(p) => p,
            Err(_) => body,
        };
        let server = Msg::parse(&plain)?.int(1) as i64;
        if server < 1_500_000_000_000 {
            return Err("invalid server timestamp".into());
        }
        let off = server - local;
        self.ts_offset.store(off, Ordering::Relaxed);
        Ok(off)
    }

    /// `/oauth_pubkey_v2`：ECDH 握手，之后的请求都用共享密钥加密。
    pub async fn handshake(&self) -> Res<()> {
        let i = self.identity();
        let plain = Pb::new()
            .str(1, &self.ecdh.public_hex())
            .str(2, &i.cached_server_pub)
            .varint(3, (i.uin == 0) as u64)
            .str(4, &i.device_code)
            .varint(5, i.uin)
            .str(6, &i.uuid)
            .str(7, "")
            .str(8, &i.device_name)
            .str(9, "");
        let body = crypto::aes_encrypt(&plain.0, self.static_key)?;
        let (status, _, rbody) = self
            .send("/oauth_pubkey_v2", CMD_HANDSHAKE, &body, SignKind::Dh, 0, 0, self.static_key, false, 0, TASK_TIMEOUT)
            .await?;
        if status != 200 {
            return Err(format!("handshake failed: HTTP {status}").into());
        }
        let rsp = Msg::parse(&crypto::aes_decrypt(&rbody, self.static_key)?)?;
        if rsp.int(1) != 0 {
            return Err(format!("handshake err_code={}", rsp.int(1)).into());
        }
        let server_pub = rsp.str(2);
        let aes = self.ecdh.aes_key(&server_pub)?;
        {
            let mut k = self.keys.lock().unwrap();
            k.aes = aes;
            k.token = rsp.str(4);
        }
        self.ident.lock().unwrap().cached_server_pub = server_pub;
        Ok(())
    }

    /// `/gen_uin_v2`：匿名注册（只在身份还没有 UIN 时调用）。
    pub async fn ensure_uin(&self) -> Res<u64> {
        let i = self.identity();
        if i.uin != 0 {
            return Ok(i.uin);
        }
        let aes = self.keys.lock().unwrap().aes.clone();
        let req = Pb::new().str(1, &i.device_code).str(2, &i.cached_server_pub).str(4, &i.uuid).str(5, "");
        let body = crypto::aes_encrypt(&req.0, &aes)?;
        let (status, _, rbody) = self
            .send("/gen_uin_v2", CMD_GEN_UIN, &body, SignKind::Uin, 0, 0, &aes, true, 0, TASK_TIMEOUT)
            .await?;
        if status != 200 {
            return Err(format!("gen_uin failed: HTTP {status}").into());
        }
        let rsp = Msg::parse(&crypto::aes_decrypt(&rbody, &aes)?)?;
        if rsp.int(1) != 0 {
            return Err(format!("gen_uin err_code={}", rsp.int(1)).into());
        }
        let uin = rsp.int(2);
        self.ident.lock().unwrap().uin = uin;
        Ok(uin)
    }

    fn base_req(&self, payload: &[u8], sub: u32) -> Vec<u8> {
        let i = self.ident.lock().unwrap();
        let item = |k: &str, v: &str| Pb::new().str(1, k).str(2, v);
        let hdr = Pb::new()
            .msg(1, &item("vid", &i.uin.to_string()))
            .msg(1, &item("app_ver", &i.app_ver))
            .msg(1, &item("platform", &i.platform.to_string()))
            .msg(1, &item("device", &i.device_name))
            .msg(1, &item("channelid", &i.channel.to_string()))
            .msg(1, &item("client_timestamp", &now_ms().to_string()));
        let mut m = Pb::new().varint(2, i.uin);
        if sub != 0 {
            m = m.varint(3, sub as u64);
        }
        m.msg(4, &hdr).bytes(5, payload).into_vec()
    }

    /// `/api_v2` 业务请求：BaseReq 信封 + 可选 Snappy + AES，返回 BaseRsp 里的 body。
    pub async fn api(&self, cmd: u32, sub: u32, payload: &[u8], timeout: Duration) -> Res<Vec<u8>> {
        let task = self.task.fetch_add(1, Ordering::SeqCst) + 1;
        let mut plain = self.base_req(payload, sub);
        let mut ctype = COMPRESS_NONE;
        if plain.len() >= COMPRESS_MIN_SIZE {
            if let Ok(packed) = snap::raw::Encoder::new().compress_vec(&plain) {
                plain = packed;
                ctype = COMPRESS_SNAPPY;
            }
        }
        let aes = self.keys.lock().unwrap().aes.clone();
        let body = crypto::aes_encrypt(&plain, &aes)?;
        let (status, hdrs, rbody) =
            self.send("/api_v2", task, &body, SignKind::Api, cmd, sub, &aes, false, ctype, timeout).await?;
        if status != 200 {
            return Err(format!("api {cmd}/{sub} -> HTTP {status} {:?}", hdrs.get("Kb-ErrCode")).into());
        }
        if rbody.is_empty() {
            return Ok(Vec::new());
        }
        // 响应是先压缩后加密：先解密再解压
        let mut plain = crypto::aes_decrypt(&rbody, &aes)?;
        if hdrs.get("Kb-CompressionType").map(|s| s.as_str()) == Some("2") {
            plain = snap_decompress_capped(&plain)?;
        }
        let rsp = Msg::parse(&plain)?;
        let inner = rsp.bytes(6).to_vec();
        if rsp.int(3) == COMPRESS_SNAPPY && !inner.is_empty() {
            return snap_decompress_capped(&inner);
        }
        Ok(inner)
    }

    /// 不包 BaseReq 的业务请求：ime_net 对 4814/4777 以外的 cmd（云 4329 等）就是这样发的。
    /// 返回解密（必要时解压）后的原始响应，不再拆 BaseRsp——交给引擎自己解析。
    pub async fn api_raw(&self, cmd: u32, sub: u32, payload: &[u8], timeout: Duration) -> Res<Vec<u8>> {
        let task = self.task.fetch_add(1, Ordering::SeqCst) + 1;
        let mut plain = payload.to_vec();
        let mut ctype = COMPRESS_NONE;
        if plain.len() >= COMPRESS_MIN_SIZE {
            if let Ok(packed) = snap::raw::Encoder::new().compress_vec(&plain) {
                plain = packed;
                ctype = COMPRESS_SNAPPY;
            }
        }
        let aes = self.keys.lock().unwrap().aes.clone();
        let body = crypto::aes_encrypt(&plain, &aes)?;
        let (status, hdrs, rbody) =
            self.send("/api_v2", task, &body, SignKind::Api, cmd, sub, &aes, false, ctype, timeout).await?;
        if status != 200 {
            return Err(format!("api {cmd}/{sub} -> HTTP {status}").into());
        }
        if rbody.is_empty() {
            return Ok(Vec::new());
        }
        let mut plain = crypto::aes_decrypt(&rbody, &aes)?;
        if hdrs.get("Kb-CompressionType").map(|s| s.as_str()) == Some("2") {
            plain = snap_decompress_capped(&plain)?;
        }
        Ok(plain)
    }

    async fn sync(&self, sub: u32, payload: Pb) -> Res<Msg> {
        Ok(Msg::parse(&self.api(CMD_SYNC, sub, &payload.0, TASK_TIMEOUT).await?)?)
    }

    fn me(&self) -> (u64, u64, String, String) {
        let i = self.ident.lock().unwrap();
        (i.uin, i.platform as u64, i.device_name.clone(), i.app_ver.clone())
    }

    fn device_info(&self) -> Pb {
        let (_, platform, name, ver) = self.me();
        Pb::new().str(1, &name).varint(2, platform).str(3, &ver).varint(5, 1)
    }

    /// 心跳（4814/1 空包），同时用来探测连接是否还活着。
    pub async fn heartbeat(&self) -> Res<()> {
        self.api(CMD_SYNC, 1, b"", Duration::from_secs(8)).await.map(|_| ())
    }

    /// 订阅推送（cmd 8074 sub 1）。
    pub async fn subscribe(&self) -> Res<()> {
        self.api(CMD_SYNC_START, 1, b"", TASK_TIMEOUT).await.map(|_| ())
    }

    /// sub 30 GetMatchCode：出 6 位匹配码，返回 (码, 剩余有效秒数)。
    /// 响应 `{1 err_code, 2 match_code, 3 expire_at(Unix 秒), 4 expiration(秒，实测 300)}`。
    pub async fn match_code(&self) -> Res<(String, u64)> {
        let (uin, platform, ..) = self.me();
        let r = self.sync(30, Pb::new().varint(1, uin).varint(2, platform).msg(3, &self.device_info())).await?;
        if r.int(1) != 0 {
            return Err(format!("match_code err_code={}", r.int(1)).into());
        }
        let now = (self.server_ms() / 1000).max(0) as u64;
        let ttl = match (r.int(4), r.int(3)) {
            (e, _) if e > 0 && e <= 3600 => e,
            (_, at) if at > now && at - now <= 3600 => at - now,
            _ => 300,
        };
        Ok((r.str(2), ttl))
    }

    /// sub 31 BindMatchCode：用对方的码申请绑定，返回 bind_id。
    pub async fn bind_code(&self, peer_code: &str, self_code: &str) -> Res<String> {
        let (uin, platform, ..) = self.me();
        let r = self
            .sync(
                31,
                Pb::new()
                    .varint(1, uin)
                    .varint(2, platform)
                    .str(3, peer_code)
                    .str(4, self_code)
                    .msg(5, &self.device_info()),
            )
            .await?;
        if r.int(1) != 0 {
            return Err(format!("bind_code err_code={}", r.int(1)).into());
        }
        Ok(r.str(2))
    }

    /// sub 32 RealBindMatchCode：出码方确认对方的 bind_id，得到 group_id。
    pub async fn real_bind(&self, bind_id: &str) -> Res<u64> {
        let (uin, platform, ..) = self.me();
        let r = self.sync(32, Pb::new().varint(1, uin).varint(2, platform).str(3, bind_id)).await?;
        if r.int(1) != 0 {
            return Err(format!("real_bind err_code={} {}", r.int(1), r.str(4)).into());
        }
        Ok(r.int(2))
    }

    /// sub 40 GetMatchCodeStatus：0 空闲 / 1 有人申请绑定（带 bind_id）/ 2 失效。
    pub async fn match_code_status(&self, self_code: &str) -> Res<(u64, String)> {
        let (uin, platform, ..) = self.me();
        let r = self.sync(40, Pb::new().varint(1, uin).varint(2, platform).str(3, self_code)).await?;
        Ok((r.int(1), r.str(2)))
    }

    /// sub 33 ReleaseMatchCodeOrBind：取消匹配码。
    pub async fn release_code(&self, self_code: &str) -> Res<()> {
        let (uin, platform, ..) = self.me();
        self.sync(33, Pb::new().varint(1, uin).varint(2, platform).str(3, self_code).str(4, "")).await?;
        Ok(())
    }

    /// sub 79 UpdateDeviceInfo：改本机设备名（user_device_name），
    /// 以及给其它设备起备注名（device_remark_names）。device_token 可空。
    pub async fn update_device_info(&self, name: Option<&str>, remarks: &[(u64, String)]) -> Res<u64> {
        let mut m = Pb::new().str(1, "");
        if let Some(n) = name {
            m = m.str(2, n);
        }
        for (uin, r) in remarks {
            m = m.msg(3, &Pb::new().varint(1, *uin).str(2, r));
        }
        let r = self.sync(79, m).await?;
        Ok(r.int(1))
    }

    /// sub 37 SendUserHotWords：上传常用语（整份），返回服务端版本号。
    pub async fn send_hot_words(&self, group_id: u64, version: u64, data: &[u8]) -> Res<u64> {
        let (uin, platform, ..) = self.me();
        let hw = Pb::new().varint(1, version).bytes(2, data);
        let r = self.sync(37, Pb::new().varint(1, uin).varint(2, platform).msg(3, &hw).varint(4, group_id)).await?;
        if r.int(1) != 0 {
            return Err(format!("send_hot_words err_code={}", r.int(1)).into());
        }
        Ok(r.int(2))
    }

    /// sub 35 Unbind：把某台设备移出组。
    pub async fn unbind(&self, peer_uin: u64) -> Res<u64> {
        let (uin, platform, ..) = self.me();
        let r = self.sync(35, Pb::new().varint(1, uin).varint(2, platform).varint(3, peer_uin)).await?;
        Ok(r.int(1))
    }

    /// sub 34 GetGroupInfo。filter 位掩码：1 设备，2 剪贴板，4 热词。
    pub async fn group(&self, filter: u64, clip_version: u64) -> Res<Group> {
        let (uin, ..) = self.me();
        let r = self
            .sync(34, Pb::new().varint(1, uin).varint(2, 1).varint(3, filter).varint(4, clip_version).varint(5, 0))
            .await?;
        let mut out = Group { err_code: r.int(2), ..Default::default() };
        if !r.has(1) {
            return Ok(out);
        }
        let g = r.sub(1)?;
        out.group_id = g.int(1);
        out.func_switch = g.int(5);
        for dv in g.all_bytes(2) {
            let item = Msg::parse(dv)?;
            let info = item.sub(2)?;
            out.devices.push(Device { uin: item.int(1), name: info.str(1), platform: info.int(2), version: info.str(4) });
        }
        if g.has(3) {
            out.clip = Some(parse_clip(&g.sub(3)?));
        }
        if g.has(4) {
            let hw = g.sub(4)?;
            out.hot_words = Some((hw.int(1), hw.bytes(2).to_vec()));
        }
        out.remark = r.str(3);
        Ok(out)
    }

    /// sub 36 SendClipboardMsg：发文本剪贴板，返回服务端分配的版本号。
    pub async fn send_clip_text(&self, group_id: u64, text: &str) -> Res<u64> {
        let (uin, platform, ..) = self.me();
        let trace = hex::encode(rand::thread_rng().gen::<[u8; 8]>());
        let msg = Pb::new().varint(2, CLIP_TEXT).str(3, text).varint(7, uin).str(9, &trace);
        let r = self
            .sync(
                36,
                Pb::new().varint(1, uin).varint(2, platform).msg(3, &msg).varint(4, group_id).varint(5, now_ms() as u64),
            )
            .await?;
        if r.int(1) != 0 {
            return Err(format!("send_clipboard err_code={}", r.int(1)).into());
        }
        Ok(r.int(2))
    }

    /// sub 60 ApplyCosToken：biz 1=图片，method 2=PUT。返回 (上传 URL, 对象 URL, 大小上限 KB)。
    pub async fn cos_upload_token(&self) -> Res<(String, String, u64)> {
        let r = self.sync(60, Pb::new().varint(1, 1).varint(3, 2)).await?;
        let upload = r.str(5);
        if upload.is_empty() {
            return Err("cos token: no upload url".into());
        }
        let obj = if r.has(7) {
            r.str(7)
        } else {
            format!("https://{}.cos.{}.myqcloud.com/{}", r.str(1), r.str(3), r.str(2))
        };
        Ok((upload, obj, r.int(6)))
    }

    /// sub 36，COS 引用：kind 是 CLIP_IMAGE（6）或 CLIP_TEXT_COS（2），data 是下载 URL。
    pub async fn send_clip_cos(&self, group_id: u64, kind: u64, url: &str) -> Res<u64> {
        let (uin, platform, ..) = self.me();
        let trace = hex::encode(rand::thread_rng().gen::<[u8; 8]>());
        let msg = Pb::new().varint(2, kind).str(3, url).varint(7, uin).str(9, &trace);
        let r = self
            .sync(
                36,
                Pb::new().varint(1, uin).varint(2, platform).msg(3, &msg).varint(4, group_id).varint(5, now_ms() as u64),
            )
            .await?;
        if r.int(1) != 0 {
            return Err(format!("send_clipboard(image) err_code={}", r.int(1)).into());
        }
        Ok(r.int(2))
    }

    /// 解一个 notify 帧。
    pub fn decode_push(&self, frame: &[u8]) -> Res<Option<Push>> {
        let outer = Msg::parse(frame)?;
        let payload = outer.sub(4)?.bytes(5).to_vec();
        if payload.is_empty() {
            return Ok(None);
        }
        let aes = self.keys.lock().unwrap().aes.clone();
        let info = Msg::parse(&crypto::aes_decrypt(&payload, &aes)?)?;
        let body = info.sub(4)?;
        let mut p = Push { event: info.int(1), group_id: body.int(1), clip: None, hot_words: None };
        if p.event == 4 {
            p.clip = Some(parse_clip(&body.sub(2)?));
        }
        if p.event == 5 {
            p.hot_words = Some((body.int(2), body.bytes(3).to_vec()));
        }
        Ok(Some(p))
    }
}

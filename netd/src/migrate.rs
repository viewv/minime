//! 换机助手（设备迁移）：本机出码，手机「换机助手 → 扫一扫」，双方互换用户数据。
//!
//! 协议见 docs/cross-device.md 附录 E、docs/cross-device.md。不需要关联设备，也不需要登录。
//!
//! ```text
//! 50 ×2（出码，用第二个 id）→ 51 轮询 ──被扫──> state 2（出现上传槽 field3 + 会话密钥 field5）
//!   → state 3（出现下载槽 field4：手机的包）→ 下载、导入 → 上传本机的包 → 53 → 57 → 54
//! ```
//!
//! 迁移包是 ZIP：`export_engine_file.bin`（引擎用户数据，按会话密钥 AES-128-ECB 加密）
//! + `migration_config.json`（手机的设置）。引擎数据的导出/导入交给正在运行的
//! minime-engined（控制通道 `$XDG_RUNTIME_DIR/minime/ctl.sock`），这样学到的词是最新的，
//! 导入后也马上生效。
//!
//! 安全措施：
//! - 导入前先把本机用户数据导出一份备份（`~/.local/share/minime/net/migration/backup-*.bin`
//!   + 同名 `.key`），出问题可以用 `minime-netd`/engined 的 import 恢复；
//! - 被扫后立即上传（双方各自上传，不互等）。包里的设置用上次迁移缓存的手机设置
//!   （手机会应用它），没有缓存就只传引擎数据；不会用 Linux 的设置覆盖手机。

use std::io::{Cursor, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::UnixStream;
use tokio::sync::mpsc::UnboundedSender;

use crate::identity;
use crate::pb::{Msg, Pb};
use crate::session::{Res, Session};

const CMD_SYNC: u32 = 4814;
const SUB_GEN: u32 = 50;
const SUB_INFO: u32 = 51;
const SUB_UPLOADED: u32 = 53;
const SUB_FINISH: u32 = 54;
/// 出码方在扫码前作废二维码（state → 5）；扫码后取消用 56
const SUB_RELEASE: u32 = 55;
const SUB_CLOSE: u32 = 56;
const SUB_DOWNLOADED: u32 = 57;
const RPC_TIMEOUT: Duration = Duration::from_secs(8);
const POLL: Duration = Duration::from_millis(1500);
/// 等扫码的时间（二维码对应的上传/下载链接有效期是 15 分钟）
const SCAN_WAIT: Duration = Duration::from_secs(10 * 60);
const TOTAL: Duration = Duration::from_secs(25 * 60);

const ENGINE_FILE: &str = "export_engine_file.bin";
const CONFIG_FILE: &str = "migration_config.json";

macro_rules! log {
    ($($t:tt)*) => { eprintln!("[netd] migrate: {}", format!($($t)*)) };
}

#[derive(Debug, Default)]
struct Info {
    err: u64,
    state: u64,
    put: String,
    get: String,
    key: String,
    alive: u64,
}

fn stage(reply: &UnboundedSender<Value>, stage: &str, extra: Value) {
    let mut v = json!({"type": "migrate", "stage": stage});
    if let (Some(o), Value::Object(e)) = (v.as_object_mut(), extra) {
        o.extend(e);
    }
    let _ = reply.send(v);
}

fn now_s() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

pub fn dir() -> PathBuf {
    identity::data_dir().join("migration")
}

fn ensure_dir() -> Res<PathBuf> {
    use std::os::unix::fs::PermissionsExt;
    let d = dir();
    std::fs::create_dir_all(&d)?;
    std::fs::set_permissions(&d, std::fs::Permissions::from_mode(0o700))?;
    Ok(d)
}

fn write_private(path: &Path, data: &[u8]) -> Res<()> {
    use std::os::unix::fs::OpenOptionsExt;
    let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(path)?;
    f.write_all(data)?;
    Ok(())
}

/// 引擎写出的文件按默认 umask 是 0644；个人词库只给本人读
fn private(path: &Path) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
}

// ------------------------------------------------------------------ 协议

/// sub 50 `GenerateMigrationQrCodeReq{1: deviceName}` → `{2: migrationId}`
/// （早先的 Python 探针在这里传的是握手 token，碰巧也行；已有 uin 的身份握手不给 token）
async fn gen_id(s: &Session) -> Res<String> {
    let mut name = s.identity().device_name;
    if name.is_empty() {
        name = "Linux".into();
    }
    let body = s.api(CMD_SYNC, SUB_GEN, &Pb::new().str(1, &name).into_vec(), RPC_TIMEOUT).await?;
    let m = Msg::parse(&body)?;
    if m.int(1) != 0 {
        return Err(format!("出码失败（err {}）", m.int(1)).into());
    }
    Ok(m.str(2))
}

async fn info(s: &Session, id: &str) -> Res<Info> {
    let body = s.api(CMD_SYNC, SUB_INFO, &Pb::new().str(1, id).into_vec(), RPC_TIMEOUT).await?;
    let m = Msg::parse(&body)?;
    Ok(Info { err: m.int(1), state: m.int(2), put: m.str(3), get: m.str(4), key: m.str(5), alive: m.int(6) })
}

async fn op(s: &Session, sub: u32, id: &str) -> Res<u64> {
    let body = s.api(CMD_SYNC, sub, &Pb::new().str(1, id).into_vec(), RPC_TIMEOUT).await?;
    Ok(if body.is_empty() { 0 } else { Msg::parse(&body)?.int(1) })
}

// ------------------------------------------------------------------ 引擎（engined 控制通道）

fn ctl_path() -> PathBuf {
    if let Ok(p) = std::env::var("MINIME_ENGINED_CTL") {
        return PathBuf::from(p);
    }
    match std::env::var("XDG_RUNTIME_DIR") {
        Ok(rt) if !rt.is_empty() => PathBuf::from(rt).join("minime/ctl.sock"),
        _ => {
            extern "C" {
                fn getuid() -> u32;
            }
            let uid = unsafe { getuid() };
            PathBuf::from(format!("/tmp/minime-{uid}/ctl.sock"))
        }
    }
}

/// 给 engined 的控制通道发一行命令，回 `ok` 算成功。
pub async fn engined_ctl(line: &str) -> Res<()> {
    let mut s = UnixStream::connect(ctl_path())
        .await
        .map_err(|e| format!("连不上输入法引擎服务（minime-engined）：{e}"))?;
    s.write_all(format!("{line}\n").as_bytes()).await?;
    let mut resp = String::new();
    tokio::time::timeout(Duration::from_secs(60), BufReader::new(s).read_line(&mut resp))
        .await
        .map_err(|_| "引擎服务没有响应")??;
    match resp.trim() {
        "ok" => Ok(()),
        l => Err(l.trim_start_matches("err ").to_string().into()),
    }
}

/// 让 engined 导出/导入用户数据：`verb` = export | import。
pub async fn engine(verb: &str, path: &Path, key: &str) -> Res<()> {
    if key.len() != 32 || !key.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("无效的迁移会话密钥".into());
    }
    let p = path.to_string_lossy();
    if p.contains([' ', '\n', '\r']) {
        return Err("迁移路径包含非法空白字符".into());
    }
    engined_ctl(&format!("{verb} {p} {key}"))
        .await
        .map_err(|e| format!("引擎 {verb} 失败：{e}").into())
}

// ------------------------------------------------------------------ 迁移包

/// 解开手机的包：(设置, 引擎数据)
fn unpack(zip: &[u8]) -> Res<(Option<Vec<u8>>, Option<Vec<u8>>)> {
    const MAX_ENTRY: usize = 64 << 20;
    let mut z = zip::ZipArchive::new(Cursor::new(zip))?;
    let mut get = |name: &str| -> Res<Option<Vec<u8>>> {
        match z.by_name(name) {
            Ok(f) => {
                if f.size() > MAX_ENTRY as u64 {
                    return Err(format!("{name} 过大").into());
                }
                let mut v = Vec::new();
                f.take((MAX_ENTRY + 1) as u64).read_to_end(&mut v)?;
                if v.len() > MAX_ENTRY {
                    return Err(format!("{name} 解压后过大").into());
                }
                Ok(Some(v))
            }
            Err(zip::result::ZipError::FileNotFound) => Ok(None),
            Err(e) => Err(e.into()),
        }
    };
    let cfg = get(CONFIG_FILE)?;
    let eng = get(ENGINE_FILE)?;
    Ok((cfg, eng))
}

fn pack(engine: &[u8], cfg: Option<&[u8]>) -> Res<Vec<u8>> {
    let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let opt = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
    w.start_file(ENGINE_FILE, opt)?;
    w.write_all(engine)?;
    if let Some(c) = cfg {
        w.start_file(CONFIG_FILE, opt)?;
        w.write_all(c)?;
    }
    Ok(w.finish()?.into_inner())
}

/// 手机设置里有几项 Linux 也认的：只统计给界面看，暂不改本机配置。
fn config_summary(cfg: &[u8]) -> Value {
    let v: Value = serde_json::from_slice(cfg).unwrap_or(Value::Null);
    json!({
        "platform": v["platform"].as_str().unwrap_or(""),
        "fuzzy": v["mainSettingInfo"]["isEnableFuzzyPinyin"].as_bool(),
    })
}

// ------------------------------------------------------------------ 流程

pub async fn run(s: Arc<Session>, http: reqwest::Client, reply: UnboundedSender<Value>) -> Res<Value> {
    let d = ensure_dir()?;
    let ts = now_s();
    let first = gen_id(&s).await?;
    let second = gen_id(&s).await?;
    let id = if second.is_empty() { first } else { second };
    if id.is_empty() {
        return Err("服务端没有返回迁移编号".into());
    }
    let url = format!("https://weread.qq.com/wetype/devicetransfer?deviceid={id}");
    log!("qr ready");
    stage(&reply, "qr", json!({"url": url, "ttl": SCAN_WAIT.as_secs()}));

    let start = Instant::now();
    let mut scanned: Option<Instant> = None;
    let (mut put, mut key) = (String::new(), String::new());
    let mut phone_cfg: Option<Vec<u8>> = None;
    let mut imported = false;
    let mut uploaded = false;
    let mut finishing: Option<Instant> = None;
    let mut result = json!({"type": "migrated"});
    let mut last_state = u64::MAX;

    let close = |s: Arc<Session>, id: String, scanned: bool| async move {
        let sub = if scanned { SUB_CLOSE } else { SUB_RELEASE };
        let code = op(&s, sub, &id).await;
        log!("{sub} -> {code:?}");
    };

    loop {
        if reply.is_closed() {
            close(s.clone(), id.clone(), scanned.is_some()).await;
            log!("cancelled");
            return Err("cancelled".into());
        }
        if start.elapsed() > TOTAL || (scanned.is_none() && start.elapsed() > SCAN_WAIT) {
            close(s.clone(), id.clone(), scanned.is_some()).await;
            return Err(if scanned.is_none() { "二维码已过期" } else { "迁移超时" }.into());
        }
        let i = match info(&s, &id).await {
            Ok(i) => i,
            Err(e) => {
                log!("poll: {e}");
                tokio::time::sleep(Duration::from_secs(2)).await;
                continue;
            }
        };
        if i.state != last_state {
            log!("state {} (err {}, peer alive {})", i.state, i.err, i.alive);
            last_state = i.state;
        }
        if !i.put.is_empty() {
            put = i.put.clone();
        }
        if i.key.len() == 32 && i.key.bytes().all(|b| b.is_ascii_hexdigit()) {
            key = i.key.clone();
        }
        if finishing.is_none() && matches!(i.state, 5 | 6) && scanned.is_some() {
            return Err("对方取消了迁移".into());
        }
        if i.state >= 2 && scanned.is_none() {
            scanned = Some(Instant::now());
            stage(&reply, "scanned", json!({}));
        }

        // 手机的包：下载 → 备份本机数据 → 导入
        if !imported && !i.get.is_empty() && key.len() == 32 {
            stage(&reply, "downloading", json!({}));
            let data = match http.get(&i.get).send().await.and_then(|r| r.error_for_status()) {
                Ok(r) => r.bytes().await?.to_vec(),
                Err(e) => {
                    log!("download: {e}");
                    tokio::time::sleep(Duration::from_secs(2)).await;
                    continue;
                }
            };
            write_private(&d.join(format!("phone-{ts}.zip")), &data)?;
            let (cfg, eng) = unpack(&data)?;
            if let Some(c) = &cfg {
                write_private(&d.join("last_config.json"), c)?;
                result["phone"] = config_summary(c);
            }
            phone_cfg = cfg;
            if let Some(eng) = eng {
                let backup = d.join(format!("backup-{ts}.bin"));
                engine("export", &backup, &key).await?;
                private(&backup);
                write_private(&d.join(format!("backup-{ts}.key")), key.as_bytes())?;
                let tmp = d.join(format!("import-{ts}.bin"));
                write_private(&tmp, &eng)?;
                let r = engine("import", &tmp, &key).await;
                let _ = std::fs::remove_file(&tmp);
                r?;
                log!("imported {} bytes of engine data (backup {})", eng.len(), backup.display());
                result["imported"] = json!(eng.len());
                result["backup"] = json!(backup);
            }
            imported = true;
            stage(&reply, "imported", json!({"bytes": data.len()}));
        }

        // 本机的包：导出（会话密钥加密）+ 手机自己的设置原样回传
        // 被扫后立刻上传：双方各自上传，实测手机约 6 秒后传上来，不必等它
        if !uploaded && !put.is_empty() && key.len() == 32 {
            stage(&reply, "uploading", json!({}));
            let tmp = d.join(format!("export-{ts}.bin"));
            engine("export", &tmp, &key).await?;
            private(&tmp);
            let eng = std::fs::read(&tmp);
            let _ = std::fs::remove_file(&tmp);
            let eng = eng?;
            let cfg = phone_cfg.clone().or_else(|| std::fs::read(d.join("last_config.json")).ok());
            if cfg.is_none() {
                log!("no phone settings to echo back; uploading engine data only");
            }
            let zip = pack(&eng, cfg.as_deref())?;
            http.put(&put)
                .header("Content-Type", "application/octet-stream")
                .body(zip.clone())
                .send()
                .await?
                .error_for_status()?;
            let code = op(&s, SUB_UPLOADED, &id).await?;
            log!("uploaded {} bytes (53 -> {code})", zip.len());
            result["uploaded"] = json!(eng.len());
            uploaded = true;
            stage(&reply, "uploaded", json!({"bytes": zip.len()}));
        }

        if uploaded && imported && finishing.is_none() {
            let code = op(&s, SUB_DOWNLOADED, &id).await?;
            log!("57 -> {code}");
            finishing = Some(Instant::now());
        }
        if let Some(t) = finishing {
            // 对端也做完了（4/6/7），或者等了一会儿：结束会话
            if matches!(i.state, 4 | 6 | 7) || t.elapsed() > Duration::from_secs(15) {
                let code = op(&s, SUB_FINISH, &id).await.unwrap_or(u64::MAX);
                log!("54 -> {code}, state {}", i.state);
                return Ok(result);
            }
        }
        tokio::time::sleep(POLL).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pack_roundtrip() {
        let z = pack(b"engine", Some(b"{\"platform\":\"Android\"}")).unwrap();
        let (c, e) = unpack(&z).unwrap();
        assert_eq!(e.unwrap(), b"engine");
        assert_eq!(config_summary(&c.unwrap())["platform"], "Android");
        let (c, e) = unpack(&pack(b"x", None).unwrap()).unwrap();
        assert!(c.is_none() && e.is_some());
    }
}

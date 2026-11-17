//! minime-settings —— MinIME 设置界面（GPUI）。
//!
//! 版式参照微信输入法 macOS/Windows 设置面板：左侧渐变侧栏 + 绿色选中项，
//! 右侧浅灰底上的白色圆角卡片、分隔线行、绿色开关。图标用 Lucide（ISC），
//! 不使用腾讯的图标和商标。
//!
//! 配置写 ~/.config/fcitx5/conf/minime.conf（与 fcitx5 面板同一份），
//! 跨设备功能通过 minime-netd 的控制 socket。

mod conf;
mod dicts;
mod netd;

use std::borrow::Cow;
use std::os::unix::net::UnixStream;
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use gpui_kit::component::button::*;
use gpui_kit::component::input::{Input, InputState, OtpEvent, OtpInput, OtpState};
use gpui_kit::component::switch::Switch;
use gpui_kit::component::*;
use gpui_kit::prelude::FluentBuilder;
use gpui_kit::*;
use serde_json::Value;

// ------------------------------------------------------------------ 配色

const GREEN: u32 = 0x07C160;
const TEXT: u32 = 0x1A1A1A;
const TEXT2: u32 = 0x8A8F99;

/// 选一个本机真实存在的中文字体族。
///
/// GPUI 在字体族不存在时仍会给出字形（回退字体），但排版测量用的是"主字体"的
/// 度量——测出来比实际字形窄，短标签就被折成一字一行（`输入` → 竖排）。
/// 所以按常见发行版的字体名依次尝试，都没有才退回 sans-serif。
fn pick_ui_font(cx: &App) -> SharedString {
    const CANDS: [&str; 9] = [
        "Noto Sans CJK SC",
        "Noto Sans SC",
        "Source Han Sans SC",
        "HarmonyOS Sans SC",
        "WenQuanYi Micro Hei",
        "WenQuanYi Zen Hei",
        "Microsoft YaHei",
        "PingFang SC",
        "sans-serif",
    ];
    let installed = cx.text_system().all_font_names();
    for c in CANDS {
        if installed.iter().any(|n| n == c) {
            return SharedString::from(c);
        }
    }
    SharedString::from("sans-serif")
}
const DIVIDER: u32 = 0xEEEFF1;
const PAGE_BG: u32 = 0xF5F6F8;
const RED: u32 = 0xE5484D;

// ------------------------------------------------------------------ 资源

struct AppAssets;

const ICONS: &[(&str, &[u8])] = &[
    ("icons/keyboard.svg", include_bytes!("../assets/icons/keyboard.svg")),
    ("icons/box.svg", include_bytes!("../assets/icons/box.svg")),
    ("icons/monitor-smartphone.svg", include_bytes!("../assets/icons/monitor-smartphone.svg")),
    ("icons/info.svg", include_bytes!("../assets/icons/info.svg")),
    ("icons/smartphone.svg", include_bytes!("../assets/icons/smartphone.svg")),
    ("icons/laptop.svg", include_bytes!("../assets/icons/laptop.svg")),
    ("icons/link.svg", include_bytes!("../assets/icons/link.svg")),
    ("icons/circle-check.svg", include_bytes!("../assets/icons/circle-check.svg")),
];

impl AssetSource for AppAssets {
    fn load(&self, path: &str) -> Result<Option<Cow<'static, [u8]>>> {
        if let Some((_, b)) = ICONS.iter().find(|(p, _)| *p == path) {
            return Ok(Some(Cow::Borrowed(b)));
        }
        gpui_kit::assets::Assets.load(path)
    }
    fn list(&self, path: &str) -> Result<Vec<SharedString>> {
        let mut v = gpui_kit::assets::Assets.list(path)?;
        v.extend(ICONS.iter().filter(|(p, _)| p.starts_with(path)).map(|(p, _)| SharedString::from(*p)));
        Ok(v)
    }
}

fn icon(path: &'static str) -> Icon {
    Icon::empty().path(path)
}

// ------------------------------------------------------------------ 状态

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Input,
    Phrases,
    Dicts,
    Models,
    Devices,
    About,
}

enum Pairing {
    Idle,
    /// 本机出码，等对方输入
    ShowCode { code: String, until: Instant },
    /// 输入对方的匹配码（六格输入框），带上一次失败的原因
    EnterCode(Option<String>),
    /// 进行中的提示（"请在另一台设备上确认关联" 等）
    Busy(String),
    Done(String, Instant),
    Failed(String),
}

/// 换机助手（设备迁移）
enum Migrate {
    Idle,
    /// 二维码（模块矩阵，true = 黑）和失效时间
    Qr { modules: Vec<bool>, width: usize, until: Instant },
    Busy(String),
    Done(String),
    Failed(String),
}

fn qr_modules(text: &str) -> Option<(Vec<bool>, usize)> {
    let code = qrcode::QrCode::with_error_correction_level(text, qrcode::EcLevel::M).ok()?;
    let w = code.width();
    Some((code.to_colors().into_iter().map(|c| c == qrcode::Color::Dark).collect(), w))
}

const FUZZY: [(&str, &str); 14] = [
    ("ZZh", "z ⇄ zh"),
    ("CCh", "c ⇄ ch"),
    ("SSh", "s ⇄ sh"),
    ("NL", "n ⇄ l"),
    ("RL", "r ⇄ l"),
    ("HF", "h ⇄ f"),
    ("GK", "g ⇄ k"),
    ("AnAng", "an ⇄ ang"),
    ("EnEng", "en ⇄ eng"),
    ("InIng", "in ⇄ ing"),
    ("IanIang", "ian ⇄ iang"),
    ("UanUang", "uan ⇄ uang"),
    ("HuiFei", "hui ⇄ fei"),
    ("HuangWang", "huang ⇄ wang"),
];

struct Settings {
    page: Page,
    /// 本机真实存在的中文字体族（见 pick_ui_font）
    ui_font: SharedString,
    ini: conf::Ini,
    save_error: Option<String>,
    status: Arc<Mutex<Value>>,
    kick: mpsc::Sender<()>,
    net: Value,
    pairing: Pairing,
    pair_rx: Option<mpsc::Receiver<Value>>,
    pair_cancel: Option<UnixStream>,
    migrate: Migrate,
    migrate_rx: Option<mpsc::Receiver<Value>>,
    migrate_cancel: Option<UnixStream>,
    cmd_rx: mpsc::Receiver<Value>,
    cmd_tx: mpsc::Sender<Value>,
    toast: Option<(String, bool, Instant)>,
    confirm_unpair: Option<u64>,
    name_input: Entity<InputState>,
    otp: Entity<OtpState>,
    hot: minime_netd::hotwords::HotWords,
    hot_mtime: Option<std::time::SystemTime>,
    hot_key: Entity<InputState>,
    hot_text: Entity<InputState>,
    /// 正在编辑的条目 id（None = 新增）
    hot_edit: Option<String>,
    hot_error: Option<String>,
    /// 当前这次关联是不是“输入对方的码”（失败后回到输入框）
    entering: bool,
    cells: Vec<dicts::Cell>,
    core_state: Vec<dicts::CoreState>,
    core_conf: dicts::CoreConf,
    catalog: Vec<dicts::Pack>,
    dict_mtime: Vec<Option<std::time::SystemTime>>,
    open_system: bool,
    open_other: bool,
    /// 点了「重启引擎」：(时间, 当时 core_state.tsv 的 mtime)；引擎重新写状态文件就算重启成功
    restarting: Option<(Instant, Option<std::time::SystemTime>)>,
    dict_error: Option<String>,
    /// 后台服务的状态（systemctl is-active），在「关于」页每两秒刷新一次
    svc: [String; 3],
    svc_at: Option<Instant>,
}

/// 「关于 → 服务」里列出的服务：(systemd 单元, 名称, 说明)
const SERVICES: [(&str, &str, &str); 3] = [
    ("minime-engined.service", "输入法引擎", "候选不出、打字没反应时重启；正在输入的内容会丢"),
    ("minime-netd.service", "网络服务", "跨设备同步、换机助手、云输入不工作时重启"),
    ("minime-asrd.socket", "离线语音", "按需启动：重启只是结束正在运行的识别进程，下次说话时重新启动"),
];

impl Settings {
    fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let ini = conf::Ini::load();
        let ui_font = pick_ui_font(cx);
        let name = ini.get("Network", "DeviceName").unwrap_or_else(|| "Linux".into());
        let name_input = cx.new(|cx| {
            let mut s = InputState::new(window, cx).placeholder("Linux");
            s.set_value(name.clone(), window, cx);
            s
        });
        let otp = cx.new(|cx| OtpState::new(6, window, cx));
        let hot_key = cx.new(|cx| InputState::new(window, cx).placeholder("快捷字母，如 dz"));
        let hot_text = cx.new(|cx| InputState::new(window, cx).placeholder("输入快捷字母时出现的短语"));
        // 六位填满就提交，同时清空输入框（失败时回到空的输入框）
        cx.subscribe_in(&otp, window, |this, otp, ev: &OtpEvent, window, cx| {
            if let OtpEvent::Complete = ev {
                let code: String = otp.read(cx).value().chars().filter(|c| c.is_ascii_digit()).collect();
                otp.update(cx, |s, cx| s.set_value("", window, cx));
                if code.len() == 6 {
                    this.start_pair(Some(code), cx);
                } else {
                    this.pairing = Pairing::EnterCode(Some("匹配码是 6 位数字".into()));
                    cx.notify();
                }
            }
        })
        .detach();
        let (status, kick) = netd::spawn_status_poller();
        let (cmd_tx, cmd_rx) = mpsc::channel();
        let net = status.lock().unwrap().clone();
        cx.spawn(async move |this, cx| loop {
            cx.background_executor().timer(Duration::from_millis(250)).await;
            if this.update(cx, |this, cx| this.tick(cx)).is_err() {
                break;
            }
        })
        .detach();
        let page = match std::env::args().skip_while(|a| a != "--page").nth(1).as_deref() {
            Some("devices") => Page::Devices,
            Some("about") => Page::About,
            Some("phrases") => Page::Phrases,
            Some("dicts") => Page::Dicts,
            Some("models") => Page::Models,
            _ => Page::Input,
        };
        // --enter-code：直接打开“输入匹配码”（从托盘/命令行跳转用）
        let enter = std::env::args().any(|a| a == "--enter-code");
        if enter {
            otp.update(cx, |s, cx| s.focus(window, cx));
        }
        Settings {
            page: if enter { Page::Devices } else { page },
            ui_font,
            ini,
            save_error: None,
            net,
            status,
            kick,
            pairing: if enter { Pairing::EnterCode(None) } else { Pairing::Idle },
            pair_rx: None,
            pair_cancel: None,
            cmd_rx,
            cmd_tx,
            toast: None,
            confirm_unpair: None,
            name_input,
            otp,
            entering: false,
            hot: minime_netd::hotwords::load(),
            hot_mtime: hot_mtime(),
            hot_key,
            hot_text,
            hot_edit: None,
            hot_error: None,
            cells: dicts::load_cells(),
            core_state: dicts::load_core_state(),
            core_conf: dicts::load_core_conf(),
            catalog: dicts::load_catalog(),
            dict_mtime: dict_mtimes(),
            open_system: false,
            open_other: false,
            restarting: None,
            dict_error: None,
            svc: Default::default(),
            svc_at: None,
            migrate: Migrate::Idle,
            migrate_rx: None,
            migrate_cancel: None,
        }
    }

    /// 定时：同步服务状态、收关联/命令事件。
    fn tick(&mut self, cx: &mut Context<Self>) {
        let mut dirty = false;
        let now = self.status.lock().unwrap().clone();
        if now != self.net {
            self.net = now;
            dirty = true;
        }
        let mut pair_events = Vec::new();
        if let Some(rx) = &self.pair_rx {
            while let Ok(v) = rx.try_recv() {
                pair_events.push(v);
            }
        }
        for v in pair_events {
            dirty = true;
            match v["type"].as_str().unwrap_or("") {
                "code" => {
                    self.pairing = Pairing::ShowCode {
                        code: v["code"].as_str().unwrap_or("").to_string(),
                        until: Instant::now() + Duration::from_secs(v["ttl"].as_u64().unwrap_or(300)),
                    }
                }
                "info" => self.pairing = Pairing::Busy(v["message"].as_str().unwrap_or("").to_string()),
                "paired" => {
                    self.pairing = Pairing::Done("关联成功".into(), Instant::now());
                    self.pair_rx = None;
                    self.pair_cancel = None;
                    let _ = self.kick.send(());
                }
                "error" => {
                    let m = v["message"].as_str().unwrap_or("关联失败").to_string();
                    self.pairing = if self.entering { Pairing::EnterCode(Some(m)) } else { Pairing::Failed(m) };
                    self.pair_rx = None;
                    self.pair_cancel = None;
                }
                _ => {}
            }
        }
        let mut mig_events = Vec::new();
        if let Some(rx) = &self.migrate_rx {
            while let Ok(v) = rx.try_recv() {
                mig_events.push(v);
            }
        }
        for v in mig_events {
            dirty = true;
            self.on_migrate_event(&v);
        }
        if let Migrate::Qr { until, .. } = &self.migrate {
            if Instant::now() >= *until {
                self.cancel_migrate();
                self.migrate = Migrate::Failed("二维码已过期".into());
            }
            dirty = true; // 倒计时
        }
        if let Some((t, m0)) = self.restarting {
            let m = std::fs::metadata(dicts::dir().join("core_state.tsv")).and_then(|m| m.modified()).ok();
            if m != m0 {
                self.restarting = None;
                self.reload_dicts();
                let msg = if self.core_pending() { "引擎已重启，但还有改动没生效" } else { "引擎已重启，改动已生效" };
                self.toast = Some((msg.into(), !self.core_pending(), Instant::now()));
                dirty = true;
            } else if t.elapsed() > Duration::from_secs(25) {
                self.restarting = None;
                self.toast = Some(("引擎 25 秒内没有重启成功（journalctl --user -u minime-engined）".into(), false, Instant::now()));
                dirty = true;
            } else {
                dirty = true; // 转圈提示
            }
        }
        if self.page == Page::About && self.svc_at.map_or(true, |t| t.elapsed() >= Duration::from_secs(2)) {
            self.svc_at = Some(Instant::now());
            let st = service_states();
            if st != self.svc {
                self.svc = st;
                dirty = true;
            }
        }
        let dm = dict_mtimes();
        if dm != self.dict_mtime {
            self.dict_mtime = dm;
            self.reload_dicts();
            dirty = true;
        }
        let m = hot_mtime();
        if m != self.hot_mtime {
            self.hot_mtime = m;
            self.hot = minime_netd::hotwords::load();
            dirty = true;
        }
        while let Ok(v) = self.cmd_rx.try_recv() {
            dirty = true;
            let ok = v["type"] != "error";
            let msg = if !ok {
                v["message"].as_str().unwrap_or("失败").to_string()
            } else if v["type"] == "hotwords" {
                if v["pending"].as_bool() == Some(true) { "已保存，关联设备后同步".into() } else { "已保存并同步".into() }
            } else {
                "已解除关联".to_string()
            };
            self.toast = Some((msg, ok, Instant::now()));
        }
        if let Pairing::Done(_, t) = &self.pairing {
            if t.elapsed() > Duration::from_secs(4) {
                self.pairing = Pairing::Idle;
                dirty = true;
            }
        }
        if matches!(self.pairing, Pairing::ShowCode { .. }) {
            dirty = true; // 倒计时
        }
        if let Some((_, _, t)) = &self.toast {
            if t.elapsed() > Duration::from_secs(4) {
                self.toast = None;
                dirty = true;
            }
        }
        if dirty {
            cx.notify();
        }
    }

    fn save(&mut self, cx: &mut Context<Self>) {
        self.save_error = self.ini.save().err().map(|e| format!("保存失败：{e}"));
        let _ = self.kick.send(());
        cx.notify();
    }

    fn set_bool(&mut self, section: &str, key: &str, v: bool, cx: &mut Context<Self>) {
        self.ini.set_bool(section, key, v);
        self.save(cx);
    }

    fn page_size(&self) -> i64 {
        self.ini.get("", "PageSize").and_then(|s| s.parse().ok()).unwrap_or(9)
    }

    fn start_migrate(&mut self, cx: &mut Context<Self>) {
        self.cancel_migrate();
        let (tx, rx) = mpsc::channel();
        self.migrate_cancel = netd::spawn_stream("migrate".into(), tx);
        self.migrate_rx = Some(rx);
        self.migrate = Migrate::Busy("正在生成二维码…".into());
        cx.notify();
    }

    fn cancel_migrate(&mut self) {
        if let Some(s) = self.migrate_cancel.take() {
            let _ = s.shutdown(std::net::Shutdown::Both);
        }
        self.migrate_rx = None;
        self.migrate = Migrate::Idle;
    }

    fn on_migrate_event(&mut self, v: &Value) {
        let busy = |m: &str| Migrate::Busy(m.to_string());
        match v["type"].as_str().unwrap_or("") {
            "migrate" => {
                self.migrate = match v["stage"].as_str().unwrap_or("") {
                    "qr" => match qr_modules(v["url"].as_str().unwrap_or("")) {
                        Some((modules, width)) => Migrate::Qr {
                            modules,
                            width,
                            until: Instant::now() + Duration::from_secs(v["ttl"].as_u64().unwrap_or(600)),
                        },
                        None => Migrate::Failed("二维码生成失败".into()),
                    },
                    "scanned" => busy("已扫码，等待手机的数据…"),
                    "downloading" => busy("正在下载手机的数据…"),
                    "imported" => busy("已导入手机的个人词库，正在上传本机的数据…"),
                    "uploading" => busy("正在上传本机的数据…"),
                    "uploaded" => busy("已上传，等待手机完成…"),
                    _ => return,
                }
            }
            "migrated" => {
                let kb = |k: &str| v[k].as_u64().map(|b| format!("{:.1} KB", b as f64 / 1024.0));
                let mut parts = Vec::new();
                if let Some(s) = kb("imported") {
                    parts.push(format!("导入手机词库 {s}"));
                }
                if let Some(s) = kb("uploaded") {
                    parts.push(format!("发送本机词库 {s}"));
                }
                self.migrate = Migrate::Done(if parts.is_empty() { "迁移完成".into() } else { format!("迁移完成：{}", parts.join("，")) });
                self.migrate_rx = None;
                self.migrate_cancel = None;
            }
            "error" => {
                let m = v["message"].as_str().unwrap_or("迁移失败").to_string();
                self.migrate = Migrate::Failed(if m == "cancelled" { "已取消".into() } else { m });
                self.migrate_rx = None;
                self.migrate_cancel = None;
            }
            _ => {}
        }
    }

    fn migrate_card(&self, cx: &mut Context<Self>) -> Div {
        let cancel = |id: &'static str| {
            Button::new(id).label("取消").small().ghost().on_click(cx.listener(|this, _: &ClickEvent, _, cx| {
                this.cancel_migrate();
                cx.notify();
            }))
        };
        let body: AnyElement = match &self.migrate {
            Migrate::Qr { modules, width, until } => {
                let left = until.saturating_duration_since(Instant::now()).as_secs();
                const M: f32 = 5.0;
                let mut grid = div().flex().flex_col().p(px(M * 3.)).bg(rgb(0xFFFFFF)).rounded(px(8.));
                for r in 0..*width {
                    let mut line = div().flex();
                    for c in 0..*width {
                        let dark = modules[r * width + c];
                        line = line.child(div().w(px(M)).h(px(M)).when(dark, |d| d.bg(rgb(0x111111))));
                    }
                    grid = grid.child(line);
                }
                div()
                    .py(px(18.))
                    .flex()
                    .flex_col()
                    .gap(px(14.))
                    .items_center()
                    .child(div().text_size(px(15.)).text_color(rgb(TEXT)).child("用手机扫描二维码"))
                    .child(div().border_1().border_color(rgb(0xE5E5E5)).rounded(px(10.)).child(grid))
                    .child(
                        div()
                            .text_size(px(12.))
                            .text_color(rgb(TEXT2))
                            .child("手机打开微信输入法 → 换机助手 → 扫一扫"),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(12.))
                            .child(div().text_size(px(12.)).text_color(rgb(TEXT2)).child(format!("{} 分 {:02} 秒后失效", left / 60, left % 60)))
                            .child(cancel("mig-cancel")),
                    )
                    .into_any_element()
            }
            Migrate::Busy(msg) => div()
                .py(px(22.))
                .flex()
                .items_center()
                .gap(px(12.))
                .child(div().text_size(px(14.)).text_color(rgb(TEXT)).child(msg.clone()))
                .child(cancel("mig-cancel2"))
                .into_any_element(),
            Migrate::Idle | Migrate::Done(_) | Migrate::Failed(_) => {
                let note = match &self.migrate {
                    Migrate::Done(m) => Some((m.clone(), GREEN)),
                    Migrate::Failed(m) => Some((m.clone(), RED)),
                    _ => None,
                };
                div()
                    .flex()
                    .flex_col()
                    .child(row(
                        "从手机迁移",
                        Some("手机和本机互相同步个人词库，无需先关联。导入前自动备份本机词库，不改动手机的设置".into()),
                        Button::new("mig-start").label("生成二维码").outline().on_click(cx.listener(|this, _: &ClickEvent, _, cx| this.start_migrate(cx))),
                        note.is_none(),
                    ))
                    .when_some(note, |d, (m, c)| d.child(div().py(px(10.)).text_size(px(13.)).text_color(rgb(c)).child(m)))
                    .into_any_element()
            }
        };
        card().child(body)
    }

    fn start_pair(&mut self, code: Option<String>, cx: &mut Context<Self>) {
        self.cancel_pair();
        let (tx, rx) = mpsc::channel();
        self.pair_cancel = netd::spawn_pair(code.clone(), tx);
        self.pair_rx = Some(rx);
        self.entering = code.is_some();
        self.pairing = Pairing::Busy(if code.is_some() { "正在申请关联…".into() } else { "正在获取匹配码…".into() });
        cx.notify();
    }

    fn cancel_pair(&mut self) {
        if let Some(s) = self.pair_cancel.take() {
            let _ = s.shutdown(std::net::Shutdown::Both);
        }
        self.pair_rx = None;
        self.entering = false;
        self.pairing = Pairing::Idle;
    }

    fn net_enabled(&self) -> bool {
        self.ini.get_bool("Network", "Enabled", false)
    }
}

// ------------------------------------------------------------------ 组件

/// 五笔词库（词库名, 版本名），顺序就是引擎挑选的优先级
const WUBI_DICTS: [(&str, &str); 3] = [("swbd86.bin", "86 版"), ("swbd98.bin", "98 版"), ("swbdnc.bin", "新世纪")];

fn card() -> Div {
    div().w_full().bg(rgb(0xFFFFFF)).rounded(px(12.)).px(px(20.)).flex().flex_col()
}

fn section_title(t: &'static str) -> Div {
    div().pt(px(8.)).pb(px(2.)).px(px(4.)).text_size(px(13.)).text_color(rgb(TEXT2)).child(t)
}

/// 一行：标题 + 说明（可选） + 右侧控件；`last` 的行不画分隔线。
/// 离线语音服务的状态（说明文字, 颜色）
fn asr_status() -> (String, u32) {
    let rt = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    // 只看 socket 在不在（systemd 按需启动时由它持有）；不能去连，连一下就会把服务拉起来
    let running = std::path::Path::new(&format!("{rt}/minime-asrd.sock")).exists();
    let data = std::env::var("XDG_DATA_HOME")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::path::PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"));
    let model = std::fs::read_dir(data.join("minime/asr/model"))
        .map(|r| r.flatten().any(|e| e.file_name().to_string_lossy().ends_with(".xnet")))
        .unwrap_or(false);
    match (running, model) {
        (true, true) => ("按住 Alt 不放说话，松开上屏；短按和 Alt 组合键照常使用，Esc 取消。完全本地识别，不联网".into(), TEXT2),
        (_, false) => ("语音模型还没有下载：运行 minime-fetch voice（从腾讯官方下载约 100 MB）".into(), RED),
        (false, true) => ("语音服务没有启用：systemctl --user enable --now minime-asrd.socket".into(), RED),
    }
}

fn row(title: impl Into<SharedString>, desc: Option<SharedString>, right: impl IntoElement, last: bool) -> Div {
    row_c(title, desc, TEXT2, right, last)
}

fn row_c(title: impl Into<SharedString>, desc: Option<SharedString>, desc_color: u32, right: impl IntoElement, last: bool) -> Div {
    div()
        .w_full()
        .min_h(px(56.))
        .py(px(10.))
        .flex()
        .items_center()
        .justify_between()
        .gap(px(16.))
        .when(!last, |d| d.border_b_1().border_color(rgb(DIVIDER)))
        // 文字一列可收缩、换行；右侧控件保持原大小，不会被长说明挤出卡片
        .child(
            div()
                .flex_1()
                .min_w(px(0.))
                .flex()
                .flex_col()
                .gap(px(2.))
                .child(div().text_size(px(15.)).text_color(rgb(TEXT)).child(title.into()))
                .when_some(desc, |d, s| d.child(div().text_size(px(12.)).line_height(px(18.)).text_color(rgb(desc_color)).child(s))),
        )
        .child(div().flex_shrink_0().child(right))
}

impl Settings {
    fn nav_item(&self, id: &'static str, page: Page, ico: &'static str, label: &'static str, cx: &mut Context<Self>) -> impl IntoElement {
        let selected = self.page == page;
        div()
            .id(id)
            .h(px(44.))
            .px(px(14.))
            .rounded(px(10.))
            .flex()
            .items_center()
            .gap(px(12.))
            .cursor_pointer()
            .text_size(px(15.))
            .when(selected, |d| d.bg(rgb(GREEN)).text_color(rgb(0xFFFFFF)))
            .when(!selected, |d| d.text_color(rgb(0x333333)).hover(|s| s.bg(rgba(0xFFFFFF99))))
            .child(icon(ico).size(px(18.)).text_color(if selected { rgb(0xFFFFFF) } else { rgb(0x333333) }))
            // 标签必须单行：字体缺失时 GPUI 的测量宽度会小于实际字形宽度，
            // 短标签会被折成一字一行（侧边栏竖排的 bug）。
            .child(div().flex_shrink_0().whitespace_nowrap().child(label))
            .on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                this.page = page;
                cx.notify();
            }))
    }

    fn sidebar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .w(px(232.))
            .h_full()
            .flex_none()
            .flex()
            .flex_col()
            .gap(px(6.))
            .px(px(12.))
            .pt(px(22.))
            .bg(linear_gradient(
                160.,
                linear_color_stop(rgb(0xD3F1EE), 0.),
                linear_color_stop(rgb(0xE6EEFA), 1.),
            ))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(10.))
                    .px(px(8.))
                    .pb(px(18.))
                    .child(
                        div()
                            .size(px(32.))
                            .rounded_full()
                            .bg(linear_gradient(135., linear_color_stop(rgb(0x2DD889), 0.), linear_color_stop(rgb(0x07B05A), 1.)))
                            .flex()
                            .items_center()
                            .justify_center()
                            .child(icon("icons/keyboard.svg").size(px(18.)).text_color(rgb(0xFFFFFF))),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .child(div().text_size(px(17.)).font_weight(FontWeight::SEMIBOLD).text_color(rgb(TEXT)).child("MinIME"))
                            .child(div().text_size(px(11.)).text_color(rgb(TEXT2)).child("Linux 设置")),
                    ),
            )
            .child(self.nav_item("nav-input", Page::Input, "icons/keyboard.svg", "输入", cx))
            .child(self.nav_item("nav-phr", Page::Phrases, "icons/box.svg", "常用语", cx))
            .child(self.nav_item("nav-dict", Page::Dicts, "icons/book-open.svg", "词库", cx))
            .child(self.nav_item("nav-model", Page::Models, "icons/cpu.svg", "模型", cx))
            .child(self.nav_item("nav-dev", Page::Devices, "icons/monitor-smartphone.svg", "跨设备", cx))
            .child(self.nav_item("nav-about", Page::About, "icons/info.svg", "关于", cx))
    }

    fn switch_row(
        &self,
        id: &'static str,
        section: &'static str,
        key: &'static str,
        default: bool,
        title: &'static str,
        desc: Option<&'static str>,
        last: bool,
        cx: &mut Context<Self>,
    ) -> Div {
        let on = self.ini.get_bool(section, key, default);
        row(
            title,
            desc.map(SharedString::from),
            Switch::new(id).checked(on).on_click(cx.listener(move |this, v: &bool, _, cx| this.set_bool(section, key, *v, cx))),
            last,
        )
    }

    /// 中英文标点切换快捷键：几个常用的预设（fcitx5 的 KeyList 存成 [PunctToggleKey] 0=…）
    fn punct_key_picker(&self, cx: &mut Context<Self>) -> Div {
        const OPTS: [(&str, &str); 3] = [("Control+period", "Ctrl+."), ("Control+Alt+period", "Ctrl+Alt+."), ("", "不使用")];
        self.key_picker("PunctToggleKey", "Control+period", &OPTS, cx)
    }

    /// 语音输入快捷键（[VoiceKey] 0=…）
    fn voice_key_picker(&self, cx: &mut Context<Self>) -> Div {
        const OPTS: [(&str, &str); 3] = [("Control+Alt+space", "Ctrl+Alt+空格"), ("Control+Shift+space", "Ctrl+Shift+空格"), ("", "不使用")];
        self.key_picker("VoiceKey", "Control+Alt+space", &OPTS, cx)
    }

    /// 表情候选（顶层 EmojiMode=Emoji|Kaomoji|None）
    fn emoji_picker(&self, cx: &mut Context<Self>) -> Div {
        const OPTS: [(&str, &str); 3] = [("Emoji", "Emoji 😄"), ("Kaomoji", "颜文字 (≧ω≦)"), ("None", "不显示")];
        let cur = self.ini.get("", "EmojiMode").unwrap_or_else(|| "Emoji".into());
        let mut d = div().flex().items_center().gap(px(6.));
        for (i, (val, label)) in OPTS.iter().enumerate() {
            let val: &'static str = val;
            let b = Button::new(SharedString::from(format!("em-{i}"))).label(*label).small();
            let b = if cur == val { b.primary() } else { b.outline() };
            d = d.child(b.on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                this.ini.set("", "EmojiMode", val);
                this.save(cx);
            })));
        }
        d
    }

    /// 一组按钮选一个顶层字符串选项（如 InputScheme=Quanpin|Shuangpin|Wubi）
    fn choice_picker(&self, key: &'static str, default: &'static str, opts: &'static [(&'static str, &'static str)], cx: &mut Context<Self>) -> Div {
        let cur = self.ini.get("", key).unwrap_or_else(|| default.into());
        let mut d = div().flex().flex_wrap().items_center().gap(px(6.));
        for (i, (val, label)) in opts.iter().enumerate() {
            let val: &'static str = val;
            let b = Button::new(SharedString::from(format!("{key}-{i}"))).label(*label).small();
            let b = if cur == val { b.primary() } else { b.outline() };
            d = d.child(b.on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                this.ini.set("", key, val);
                this.save(cx);
            })));
        }
        d
    }

    /// 当前用的五笔版本：引擎按 86、98、新世纪的顺序用第一个加载了的 swbd 词库
    fn wubi_version(&self) -> &'static str {
        WUBI_DICTS.iter().find(|(n, _)| !self.core_conf.off.contains(*n)).map(|(n, _)| *n).unwrap_or(WUBI_DICTS[0].0)
    }

    /// 五笔版本：只留一个 swbd 词库，然后重启引擎（词库只在引擎启动时加载）
    fn wubi_version_picker(&self, cx: &mut Context<Self>) -> Div {
        let cur = self.wubi_version();
        let mut d = div().flex().items_center().gap(px(6.));
        for (i, (name, label)) in WUBI_DICTS.iter().enumerate() {
            let name: &'static str = name;
            let b = Button::new(SharedString::from(format!("wbv-{i}"))).label(*label).small();
            let b = if cur == name { b.primary() } else { b.outline() };
            d = d.child(b.on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                if this.wubi_version() == name && WUBI_DICTS.iter().filter(|(n, _)| !this.core_conf.off.contains(*n)).count() == 1 {
                    return;
                }
                for (n, _) in WUBI_DICTS.iter() {
                    if *n == name {
                        this.core_conf.off.remove(*n);
                    } else {
                        this.core_conf.off.insert(n.to_string());
                    }
                }
                this.save_core_conf(cx);
                this.restart_engine(cx);
                this.toast = Some(("正在重启引擎，换五笔版本".into(), true, Instant::now()));
            })));
        }
        d
    }

    fn scheme_card(&self, cx: &mut Context<Self>) -> Div {
        const SCHEMES: [(&str, &str); 3] = [("Quanpin", "全拼"), ("Shuangpin", "双拼"), ("Wubi", "五笔")];
        const SP: [(&str, &str); 7] = [
            ("Xiaohe", "小鹤"),
            ("Ziranma", "自然码"),
            ("Microsoft", "微软"),
            ("Sogou", "搜狗"),
            ("Ziguang", "紫光"),
            ("PinyinJiaJia", "拼音加加"),
            ("ABC", "智能 ABC"),
        ];
        let scheme = self.ini.get("", "InputScheme").unwrap_or_else(|| "Quanpin".into());
        let desc = match scheme.as_str() {
            "Shuangpin" => "双拼下不出整句模型候选，也不走云输入（这两项只认全拼）",
            "Wubi" => "五笔下不出整句模型候选，也不走云输入",
            _ => "全拼支持整句模型候选和云输入",
        };
        let mut c = card().child(row("输入方案", Some(desc.into()), self.choice_picker("InputScheme", "Quanpin", &SCHEMES, cx), scheme == "Quanpin"));
        if scheme == "Shuangpin" {
            c = c.child(
                div()
                    .w_full()
                    .py(px(12.))
                    .flex()
                    .flex_col()
                    .gap(px(8.))
                    .child(div().text_size(px(15.)).text_color(rgb(TEXT)).child("双拼方案"))
                    .child(self.choice_picker("ShuangpinScheme", "Xiaohe", &SP, cx)),
            );
        }
        if scheme == "Wubi" {
            c = c
                .child(row("五笔版本", Some("换版本会重启引擎".into()), self.wubi_version_picker(cx), false))
                .child(self.switch_row("sw-wb-top", "", "WubiTopCommit", true, "第五码顶字上屏", Some("打满四码后再按键，自动上屏第一个候选"), false, cx))
                .child(self.switch_row("sw-wb-uniq", "", "WubiUniqueCommit", false, "四码唯一自动上屏", Some("四码只有一个候选时直接上屏"), false, cx))
                .child(self.switch_row("sw-wb-py", "", "WubiPinyin", false, "拼音混输", Some("不会五笔编码的字可以直接打拼音"), true, cx));
        }
        c
    }

    /// 长按说话的键（顶层 VoiceHoldKey=RightAlt|LeftAlt|None）
    fn voice_hold_picker(&self, cx: &mut Context<Self>) -> Div {
        const OPTS: [(&str, &str); 3] = [("RightAlt", "右 Alt"), ("LeftAlt", "左 Alt"), ("None", "不使用")];
        let cur = self.ini.get("", "VoiceHoldKey").unwrap_or_else(|| "RightAlt".into());
        let mut d = div().flex().items_center().gap(px(6.));
        for (i, (val, label)) in OPTS.iter().enumerate() {
            let val: &'static str = val;
            let b = Button::new(SharedString::from(format!("vh-{i}"))).label(*label).small();
            let b = if cur == val { b.primary() } else { b.outline() };
            d = d.child(b.on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                this.ini.set("", "VoiceHoldKey", val);
                this.save(cx);
            })));
        }
        d
    }

    /// 快捷键预设（fcitx5 的 KeyList 存成 [段] 0=…）
    fn key_picker(&self, section: &'static str, default: &str, opts: &'static [(&'static str, &'static str)], cx: &mut Context<Self>) -> Div {
        let cur = self.ini.get(section, "0").unwrap_or_else(|| default.into());
        let mut d = div().flex().items_center().gap(px(6.));
        for (i, (val, label)) in opts.iter().enumerate() {
            let val: &'static str = val;
            let b = Button::new(SharedString::from(format!("{section}-{i}"))).label(*label).small();
            let b = if cur == val { b.primary() } else { b.outline() };
            d = d.child(b.on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                this.ini.set(section, "0", val);
                this.save(cx);
            })));
        }
        if !opts.iter().any(|(v, _)| *v == cur) {
            d = d.child(div().text_size(px(12.)).text_color(rgb(TEXT2)).child(format!("当前：{cur}")));
        }
        d
    }

    fn page_input(&self, cx: &mut Context<Self>) -> Div {
        let n = self.page_size();
        let stepper = div()
            .flex()
            .items_center()
            .gap(px(10.))
            .child(Button::new("ps-dec").label("−").small().outline().disabled(n <= 3).on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                let v = (this.page_size() - 1).max(3);
                this.ini.set("", "PageSize", &v.to_string());
                this.save(cx);
            })))
            .child(div().w(px(20.)).text_center().text_size(px(15.)).text_color(rgb(TEXT)).child(n.to_string()))
            .child(Button::new("ps-inc").label("+").small().outline().disabled(n >= 9).on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                let v = (this.page_size() + 1).min(9);
                this.ini.set("", "PageSize", &v.to_string());
                this.save(cx);
            })));

        let mut fuzzy = div().w_full().grid().grid_cols(2).gap_x(px(24.)).py(px(8.));
        for (i, (key, label)) in FUZZY.iter().enumerate() {
            let on = self.ini.get_bool("Fuzzy", key, false);
            let key: &'static str = key;
            fuzzy = fuzzy.child(
                div()
                    .h(px(44.))
                    .flex()
                    .items_center()
                    .justify_between()
                    .border_b_1()
                    .border_color(rgb(if i + 2 >= FUZZY.len() { 0xFFFFFF } else { DIVIDER }))
                    .child(div().text_size(px(14.)).text_color(rgb(TEXT)).child(*label))
                    .child(
                        Switch::new(SharedString::from(format!("fz-{key}")))
                            .checked(on)
                            .on_click(cx.listener(move |this, v: &bool, _, cx| this.set_bool("Fuzzy", key, *v, cx))),
                    ),
            );
        }

        let wubi = self.ini.get("", "InputScheme").as_deref() == Some("Wubi");
        div()
            .flex()
            .flex_col()
            .gap(px(12.))
            .child(self.scheme_card(cx))
            .child(
                card()
                    .child(self.switch_row("sw-e2e", "", "WholeSentence", true, "整句候选", Some("用整句模型给出整句候选，长句输入更准"), false, cx))
                    .child(self.switch_row("sw-trad", "", "Traditional", false, "繁体输出", Some("候选和上屏的文字使用繁体"), false, cx))
                    .child(row("每页候选数", None, stepper, true)),
            )
            .child(card().child(self.switch_row(
                "sw-shift",
                "",
                "ShiftToggle",
                false,
                "单按 Shift 切换中英文",
                Some("关闭时用 fcitx5 的切换快捷键"),
                true,
                cx,
            )))
            .child(
                card()
                    .child(self.switch_row("sw-apunct", "", "AsciiPunct", false, "使用英文标点", Some("中文模式下也输出半角标点"), false, cx))
                    .child(row("中英文标点切换", Some("打字时随时切换；其他组合键在 fcitx5 配置里设".into()), self.punct_key_picker(cx), true)),
            )
            .child(
                card()
                    .child(self.switch_row(
                        "sw-sp-abc",
                        "",
                        "AutoSpaceAbc",
                        false,
                        "中文和英文之间自动加空格",
                        Some("例如「用 iPhone 打字」；标点前后不加"),
                        false,
                        cx,
                    ))
                    .child(self.switch_row(
                        "sw-sp-123",
                        "",
                        "AutoSpace123",
                        false,
                        "中文和数字之间自动加空格",
                        Some("例如「买了 3 个」"),
                        true,
                        cx,
                    )),
            )
            .child(card().child(row(
                "表情候选",
                Some("打「哈哈」「点赞」时在候选里给出表情，只能二选一".into()),
                self.emoji_picker(cx),
                true,
            )))
            .child(section_title("语音输入"))
            .child({
                let (desc, color) = asr_status();
                card()
                    .child(row_c("长按说话", Some(desc.into()), color, self.voice_hold_picker(cx), false))
                    .child(row(
                        "快捷键",
                        Some("按住说话，或按一下开始、再按一下或回车结束".into()),
                        self.voice_key_picker(cx),
                        true,
                    ))
            })
            .when(!wubi, |d| d.child(section_title("模糊拼音")).child(card().child(fuzzy)))
    }

    fn net_status_text(&self) -> (SharedString, u32) {
        let v = &self.net;
        if v["running"].as_bool() != Some(true) {
            return ("网络服务未运行（systemctl --user enable --now minime-netd）".into(), RED);
        }
        if !self.net_enabled() {
            return ("已关闭，输入法完全离线".into(), TEXT2);
        }
        if v["connected"].as_bool() == Some(true) {
            ("已连接".into(), GREEN)
        } else {
            let reason = v["reason"].as_str().unwrap_or("");
            (format!("正在连接…{}", if reason.is_empty() { String::new() } else { format!("（{reason}）") }).into(), TEXT2)
        }
    }

    fn device_rows(&self, cx: &mut Context<Self>) -> Div {
        let peers = self.net["peers"].as_array().cloned().unwrap_or_default();
        let mut list = card();
        if peers.is_empty() {
            return list.child(
                div()
                    .py(px(22.))
                    .text_size(px(14.))
                    .text_color(rgb(TEXT2))
                    .child("还没有关联设备。关联后，两边复制的内容会自动同步。"),
            );
        }
        let n = peers.len();
        for (i, p) in peers.iter().enumerate() {
            let uin = p["uin"].as_u64().unwrap_or(0);
            let platform = p["platform"].as_u64().unwrap_or(0);
            let (ico, plat) = match platform {
                1 => ("icons/smartphone.svg", "Android"),
                2 => ("icons/smartphone.svg", "iOS"),
                5 => ("icons/laptop.svg", "Mac"),
                6 => ("icons/laptop.svg", "Windows"),
                _ => ("icons/smartphone.svg", "设备"),
            };
            let confirming = self.confirm_unpair == Some(uin);
            let btn = Button::new(SharedString::from(format!("unpair-{uin}")))
                .label(if confirming { "确认解除" } else { "解除" })
                .small()
                .when(confirming, |b| b.danger())
                .when(!confirming, |b| b.outline())
                .on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                    if this.confirm_unpair == Some(uin) {
                        this.confirm_unpair = None;
                        netd::spawn_command(format!("unpair {uin}"), this.cmd_tx.clone(), this.kick.clone());
                    } else {
                        this.confirm_unpair = Some(uin);
                    }
                    cx.notify();
                }));
            list = list.child(
                div()
                    .w_full()
                    .h(px(64.))
                    .flex()
                    .items_center()
                    .gap(px(14.))
                    .when(i + 1 < n, |d| d.border_b_1().border_color(rgb(DIVIDER)))
                    .child(
                        div()
                            .size(px(36.))
                            .rounded(px(10.))
                            .bg(rgb(0xEAF8F0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .child(icon(ico).size(px(18.)).text_color(rgb(GREEN))),
                    )
                    .child(
                        div()
                            .flex_1()
                            .flex()
                            .flex_col()
                            .gap(px(2.))
                            .child(div().text_size(px(15.)).text_color(rgb(TEXT)).child(p["name"].as_str().unwrap_or("").to_string()))
                            .child(
                                div()
                                    .text_size(px(12.))
                                    .text_color(rgb(TEXT2))
                                    .child(format!("{plat} · 微信输入法 {}", p["version"].as_str().unwrap_or(""))),
                            ),
                    )
                    .child(btn),
            );
        }
        list
    }

    fn pair_card(&self, cx: &mut Context<Self>) -> Div {
        let body: AnyElement = match &self.pairing {
            Pairing::ShowCode { code, until } => {
                let left = until.saturating_duration_since(Instant::now()).as_secs();
                let mut digits = div().flex().gap(px(10.));
                for ch in code.chars() {
                    digits = digits.child(
                        div()
                            .w(px(46.))
                            .h(px(58.))
                            .rounded(px(10.))
                            .bg(rgb(0xF2FBF6))
                            .border_1()
                            .border_color(rgb(0xBDEBD2))
                            .flex()
                            .items_center()
                            .justify_center()
                            .text_size(px(28.))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(GREEN))
                            .child(ch.to_string()),
                    );
                }
                div()
                    .py(px(18.))
                    .flex()
                    .flex_col()
                    .gap(px(14.))
                    .items_center()
                    .child(div().text_size(px(15.)).text_color(rgb(TEXT)).child("在手机上输入本机匹配码"))
                    .child(digits)
                    .child(
                        div()
                            .text_size(px(12.))
                            .text_color(rgb(TEXT2))
                            .child("手机打开微信输入法 → 跨设备粘贴和同步 → 关联我的其他设备，输入上面的匹配码"),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(12.))
                            .child(div().text_size(px(12.)).text_color(rgb(TEXT2)).child(format!("{} 分 {:02} 秒后失效", left / 60, left % 60)))
                            .child(Button::new("pair-cancel").label("取消").small().ghost().on_click(cx.listener(|this, _: &ClickEvent, _, cx| {
                                this.cancel_pair();
                                cx.notify();
                            }))),
                    )
                    .into_any_element()
            }
            Pairing::EnterCode(error) => div()
                .py(px(18.))
                .flex()
                .flex_col()
                .gap(px(14.))
                .items_center()
                .child(div().text_size(px(15.)).text_color(rgb(TEXT)).child("输入另一台设备上显示的匹配码"))
                .child(div().child(OtpInput::new(&self.otp).large()))
                .child(
                    div()
                        .text_size(px(12.))
                        .text_color(rgb(TEXT2))
                        .child("手机打开微信输入法 → 跨设备粘贴和同步 → 关联我的其他设备，查看本机匹配码；输满 6 位自动关联"),
                )
                .when_some(error.clone(), |d, m| d.child(div().text_size(px(13.)).text_color(rgb(RED)).child(m)))
                .child(div().child(Button::new("enter-cancel").label("取消").small().ghost().on_click(cx.listener(|this, _: &ClickEvent, window, cx| {
                    this.otp.update(cx, |s, cx| s.set_value("", window, cx));
                    this.cancel_pair();
                    cx.notify();
                }))))
                .into_any_element(),
            Pairing::Busy(msg) => div()
                .py(px(22.))
                .flex()
                .items_center()
                .gap(px(12.))
                .child(div().text_size(px(14.)).text_color(rgb(TEXT)).child(msg.clone()))
                .child(Button::new("pair-cancel2").label("取消").small().ghost().on_click(cx.listener(|this, _: &ClickEvent, _, cx| {
                    this.cancel_pair();
                    cx.notify();
                })))
                .into_any_element(),
            Pairing::Done(msg, _) => div()
                .py(px(22.))
                .flex()
                .items_center()
                .gap(px(10.))
                .child(icon("icons/circle-check.svg").size(px(20.)).text_color(rgb(GREEN)))
                .child(div().text_size(px(15.)).text_color(rgb(GREEN)).child(msg.clone()))
                .into_any_element(),
            Pairing::Idle | Pairing::Failed(_) => {
                let failed = if let Pairing::Failed(m) = &self.pairing { Some(m.clone()) } else { None };
                div()
                    .flex()
                    .flex_col()
                    .child(row(
                        "显示本机匹配码",
                        Some("在手机上输入这个码完成关联".into()),
                        Button::new("pair-show").label("关联设备").primary().on_click(cx.listener(|this, _: &ClickEvent, _, cx| {
                            this.start_pair(None, cx)
                        })),
                        false,
                    ))
                    .child(row(
                        "输入其他设备的匹配码",
                        Some("另一台设备显示匹配码时使用".into()),
                        Button::new("pair-enter").label("输入匹配码").outline().on_click(cx.listener(|this, _: &ClickEvent, window, cx| {
                            this.cancel_pair();
                            this.pairing = Pairing::EnterCode(None);
                            this.otp.update(cx, |s, cx| {
                                s.set_value("", window, cx);
                                s.focus(window, cx);
                            });
                            cx.notify();
                        })),
                        failed.is_none(),
                    ))
                    .when_some(failed, |d, m| d.child(div().py(px(10.)).text_size(px(13.)).text_color(rgb(RED)).child(m)))
                    .into_any_element()
            }
        };
        card().child(body)
    }

    fn page_devices(&self, cx: &mut Context<Self>) -> Div {
        let enabled = self.net_enabled();
        let connected = self.net["connected"].as_bool() == Some(true);
        let (status, color) = self.net_status_text();
        let uin = self.net["uin"].as_u64();
        div()
            .flex()
            .flex_col()
            .gap(px(12.))
            .child(
                card().child(row_c(
                    "跨设备粘贴和同步",
                    Some(status),
                    color,
                    Switch::new("sw-net").checked(enabled).on_click(cx.listener(|this, v: &bool, _, cx| {
                        if !*v {
                            this.cancel_pair();
                            this.cancel_migrate();
                        }
                        this.set_bool("Network", "Enabled", *v, cx)
                    })),
                    true,
                ))
            )
            .when(!enabled, |d| {
                d.child(
                    div()
                        .px(px(4.))
                        .text_size(px(13.))
                        .text_color(rgb(TEXT2))
                        .child("以下功能需联网后使用。打开后会以匿名设备身份连接微信输入法服务器，不需要登录微信。"),
                )
            })
            .when(enabled, |d| {
                d.child(section_title("我的设备"))
                    .child(self.device_rows(cx))
                    .when(connected, |d| d.child(section_title("关联新设备")).child(self.pair_card(cx)))
                    .when(connected, |d| d.child(section_title("换机助手")).child(self.migrate_card(cx)))
                    .child(section_title("同步内容"))
                    .child(
                        card()
                            .child(self.switch_row(
                                "sw-clip",
                                "Network",
                                "ClipboardSync",
                                true,
                                "跨设备粘贴",
                                Some("在一台设备上复制的文字，另一台设备可直接粘贴"),
                                false,
                                cx,
                            ))
                            .child(self.switch_row(
                                "sw-img",
                                "Network",
                                "ImageSync",
                                false,
                                "跨设备发送图片",
                                Some("图片经腾讯云存储中转，下载链接泄露即可被他人查看"),
                                false,
                                cx,
                            ))
                            .child(self.switch_row(
                                "sw-udict",
                                "Network",
                                "UserDictSync",
                                false,
                                "同步个人词库",
                                Some("关联设备之间同步打字学到的词；词库经腾讯云存储中转，全量包上传较慢"),
                                true,
                                cx,
                            )),
                    )
                    .child(section_title("云输入（实验）"))
                    .child(card().child(self.switch_row(
                        "sw-cloud",
                        "Network",
                        "CloudInput",
                        false,
                        "云输入",
                        Some("打字时把拼音和上文发给服务器，换回云端候选（标 ☁）"),
                        true,
                        cx,
                    )))
                    .child(section_title("本机"))
                    .child(
                        card()
                            .child(row(
                                "设备名称",
                                Some("显示在其他设备的「我的设备」里，保存后同步到已关联的设备".into()),
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(8.))
                                    .child(div().w(px(180.)).child(Input::new(&self.name_input)))
                                    .child(Button::new("name-save").label("保存").outline().on_click(cx.listener(|this, _: &ClickEvent, _, cx| {
                                        let v = this.name_input.read(cx).value().trim().to_string();
                                        if !v.is_empty() {
                                            this.ini.set("Network", "DeviceName", &v);
                                            this.save(cx);
                                            this.toast = Some(("设备名称已保存".into(), true, Instant::now()));
                                        }
                                    }))),
                                uin.is_none(),
                            ))
                            .when_some(uin, |d, u| d.child(row("设备 ID", None, div().text_size(px(13.)).text_color(rgb(TEXT2)).child(u.to_string()), true))),
                    )
            })
    }

    /// 重启一个后台服务（不等结果；状态在下一次刷新里看得到）
    fn restart_service(&mut self, unit: &'static str, name: &'static str, cx: &mut Context<Self>) {
        if unit == "minime-engined.service" {
            self.restart_engine(cx); // 复用词库页的重启：等引擎重新写状态文件再提示
            return;
        }
        // 离线语音由 socket 按需拉起：停掉正在跑的识别进程即可，socket 保持监听
        let args: &[&str] = if unit == "minime-asrd.socket" {
            &["--user", "stop", "minime-asrd.service"]
        } else {
            &["--user", "restart", unit]
        };
        let ok = std::process::Command::new("systemctl")
            .args(args)
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        self.toast = Some(if ok { (format!("已重启{name}"), true, Instant::now()) } else { (format!("{name}重启失败（journalctl --user -u {unit}）"), false, Instant::now()) });
        self.svc_at = None;
        cx.notify();
    }

    fn services_card(&self, cx: &mut Context<Self>) -> Div {
        let mut c = card();
        for (i, (unit, name, desc)) in SERVICES.iter().enumerate() {
            let (unit, name): (&'static str, &'static str) = (unit, name);
            let state = self.svc[i].as_str();
            let (label, color) = match state {
                "active" => ("运行中", GREEN),
                "activating" | "reloading" => ("启动中", TEXT2),
                "" => ("…", TEXT2),
                "inactive" => ("没有运行", RED),
                "failed" => ("出错了", RED),
                _ => ("没有安装", RED),
            };
            let busy = unit == "minime-engined.service" && self.restarting.is_some();
            let right = div()
                .flex()
                .items_center()
                .gap(px(10.))
                .child(div().text_size(px(13.)).text_color(rgb(color)).child(if busy { "重启中…" } else { label }))
                .child(
                    Button::new(SharedString::from(format!("svc-{i}")))
                        .label("重启")
                        .small()
                        .outline()
                        .disabled(busy || label == "没有安装")
                        .on_click(cx.listener(move |this, _: &ClickEvent, _, cx| this.restart_service(unit, name, cx))),
                );
            // 引擎没在运行、也没有用 minime-fetch 下载过官方文件：提示先下载
            let desc: SharedString = if unit == "minime-engined.service"
                && state != "active"
                && !official_engine_present()
            {
                "还没有下载官方引擎：在终端运行 minime-fetch（从腾讯官方下载），再点重启".into()
            } else {
                SharedString::from(*desc)
            };
            c = c.child(row(name, Some(desc), right, i + 1 == SERVICES.len()));
        }
        c
    }

    fn page_about(&self, cx: &mut Context<Self>) -> Div {
        let item = |k: &'static str, v: String, last: bool| row(k, None, div().text_size(px(13.)).text_color(rgb(TEXT2)).child(v), last);
        div()
            .flex()
            .flex_col()
            .gap(px(12.))
            .child(section_title("服务"))
            .child(self.services_card(cx))
            .child(section_title("版本"))
            .child(
                card()
                    .child(item("设置程序", format!("minime-settings {}", env!("CARGO_PKG_VERSION")), false))
                    .child(item("配置文件", conf::path().display().to_string(), false))
                    .child(item("网络服务", netd::socket_path().display().to_string(), true)),
            )
            .child(
                div()
                    .px(px(4.))
                    .text_size(px(12.))
                    .text_color(rgb(TEXT2))
                    .child("这是在 Linux 上运行微信输入法引擎的社区项目，与腾讯无关。图标来自 Lucide（ISC 许可）。"),
            )
    }
}

/// minime-fetch 把官方文件放在这里（~/.local/share/minime/official）
fn official_dir() -> std::path::PathBuf {
    let data = std::env::var("XDG_DATA_HOME")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::path::PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"));
    data.join("minime/official")
}

/// minime-fetch 下载的官方引擎在不在（开发环境用仓库里的 vendor/，不算）
fn official_engine_present() -> bool {
    official_dir().join("win/3.0.1.35/libwxhld.dll").exists()
}

/// 各服务的 systemctl is-active 结果（没装的单元是 "inactive" 或 "unknown"，装没装另外看单元文件）
fn service_states() -> [String; 3] {
    let out = std::process::Command::new("systemctl")
        .args(["--user", "is-active"])
        .args(SERVICES.iter().map(|s| s.0))
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default();
    let mut lines = out.lines();
    std::array::from_fn(|i| {
        let st = lines.next().unwrap_or("").trim().to_string();
        let unit = SERVICES[i].0;
        let installed = std::process::Command::new("systemctl")
            .args(["--user", "cat", unit])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        if installed { st } else { "missing".into() }
    })
}

fn dict_mtimes() -> Vec<Option<std::time::SystemTime>> {
    let m = |f: &str| std::fs::metadata(dicts::dir().join(f)).and_then(|m| m.modified()).ok();
    ["cells.tsv", "state.tsv", "core_conf.tsv", "core_state.tsv", "library/catalog.json"].iter().map(|f| m(f)).collect()
}

fn hot_mtime() -> Option<std::time::SystemTime> {
    std::fs::metadata(minime_netd::hotwords::path()).and_then(|m| m.modified()).ok()
}

impl Settings {
    /// 保存整份常用语：交给 minime-netd（会立即同步到关联设备）；服务没运行就先存本地、标记待上传。
    fn save_phrases(&mut self, items: Vec<minime_netd::hotwords::HotWord>) {
        use minime_netd::hotwords;
        let arg: Vec<Value> = items.iter().map(|h| serde_json::json!({"id": h.id, "key": h.key, "text": h.text})).collect();
        let cmd = format!("hotwords-set {}", Value::Array(arg));
        // 先在界面上生效
        self.hot.items = items.clone();
        let tx = self.cmd_tx.clone();
        std::thread::spawn(move || {
            let mut got = None;
            let r = netd::request(&cmd, Some(Duration::from_secs(20)), |v| got = Some(v));
            match (r, got) {
                (Ok(()), Some(v)) => {
                    let _ = tx.send(v);
                }
                _ => {
                    let now = std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .map(|d| d.as_millis() as u64)
                        .unwrap_or(0);
                    let mut h = hotwords::load();
                    h.items = items
                        .into_iter()
                        .enumerate()
                        .map(|(i, it)| if it.id.is_empty() { hotwords::new_item(&it.key, &it.text, now + i as u64) } else { it })
                        .collect();
                    h.pending = true;
                    let _ = tx.send(match hotwords::save(&h) {
                        Ok(()) => serde_json::json!({"type": "hotwords", "pending": true}),
                        Err(e) => serde_json::json!({"type": "error", "message": format!("保存失败：{e}")}),
                    });
                }
            }
        });
    }

    fn submit_phrase(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let key: String = self.hot_key.read(cx).value().trim().to_ascii_lowercase();
        let text: String = self.hot_text.read(cx).value().trim().to_string();
        if key.is_empty() || !key.chars().all(|c| c.is_ascii_lowercase()) || key.len() > 16 {
            self.hot_error = Some("快捷字母只能是 1–16 个英文字母".into());
            cx.notify();
            return;
        }
        if text.is_empty() {
            self.hot_error = Some("短语不能为空".into());
            cx.notify();
            return;
        }
        let mut items = self.hot.items.clone();
        if items.iter().any(|h| h.key == key && Some(&h.id) != self.hot_edit.as_ref()) {
            self.hot_error = Some(format!("快捷字母 {key} 已经用过了"));
            cx.notify();
            return;
        }
        match self.hot_edit.take() {
            Some(id) => {
                if let Some(h) = items.iter_mut().find(|h| h.id == id) {
                    h.key = key;
                    h.text = text;
                }
            }
            None => items.push(minime_netd::hotwords::HotWord { key, text, ..Default::default() }),
        }
        self.hot_error = None;
        self.hot_key.update(cx, |s, cx| s.set_value("", window, cx));
        self.hot_text.update(cx, |s, cx| s.set_value("", window, cx));
        self.save_phrases(items);
        cx.notify();
    }

    fn page_phrases(&self, cx: &mut Context<Self>) -> Div {
        let editing = self.hot_edit.is_some();
        let form = card()
            .py(px(14.))
            .gap(px(10.))
            .child(div().text_size(px(15.)).text_color(rgb(TEXT)).child(if editing { "编辑常用语" } else { "添加常用语" }))
            .child(
                div()
                    .flex()
                    .gap(px(10.))
                    .items_center()
                    .child(div().w(px(150.)).child(Input::new(&self.hot_key)))
                    .child(div().flex_1().child(Input::new(&self.hot_text)))
                    .child(Button::new("hot-submit").label(if editing { "保存" } else { "添加" }).primary().on_click(cx.listener(
                        |this, _: &ClickEvent, window, cx| this.submit_phrase(window, cx),
                    )))
                    .when(editing, |d| {
                        d.child(Button::new("hot-cancel").label("取消").ghost().on_click(cx.listener(|this, _: &ClickEvent, window, cx| {
                            this.hot_edit = None;
                            this.hot_error = None;
                            this.hot_key.update(cx, |s, cx| s.set_value("", window, cx));
                            this.hot_text.update(cx, |s, cx| s.set_value("", window, cx));
                            cx.notify();
                        })))
                    }),
            )
            .when_some(self.hot_error.clone(), |d, e| d.child(div().text_size(px(13.)).text_color(rgb(RED)).child(e)));

        let n = self.hot.items.len();
        let mut list = card();
        if n == 0 {
            list = list.child(div().py(px(22.)).text_size(px(14.)).text_color(rgb(TEXT2)).child("还没有常用语。"));
        }
        for (i, h) in self.hot.items.iter().enumerate() {
            let id = h.id.clone();
            let id2 = h.id.clone();
            let (key, text) = (h.key.clone(), h.text.clone());
            let row_el = div()
                .w_full()
                .min_h(px(56.))
                .py(px(10.))
                .flex()
                .items_center()
                .gap(px(14.))
                .when(i + 1 < n, |d| d.border_b_1().border_color(rgb(DIVIDER)))
                .child(
                    div()
                        .flex_none()
                        .min_w(px(56.))
                        .px(px(10.))
                        .py(px(4.))
                        .rounded(px(8.))
                        .bg(rgb(0xEAF8F0))
                        .text_size(px(13.))
                        .font_family("monospace")
                        .text_color(rgb(GREEN))
                        .text_center()
                        .child(key.clone()),
                )
                .child(div().flex_1().text_size(px(15.)).text_color(rgb(TEXT)).child(text.clone()))
                .child(
                    Button::new(SharedString::from(format!("hot-edit-{i}"))).label("编辑").small().ghost().on_click(cx.listener(
                        move |this, _: &ClickEvent, window, cx| {
                            this.hot_edit = Some(id.clone());
                            this.hot_error = None;
                            let (k, t) = (key.clone(), text.clone());
                            this.hot_key.update(cx, |s, cx| s.set_value(k, window, cx));
                            this.hot_text.update(cx, |s, cx| s.set_value(t, window, cx));
                            cx.notify();
                        },
                    )),
                )
                .child(
                    Button::new(SharedString::from(format!("hot-del-{i}"))).label("删除").small().outline().on_click(cx.listener(
                        move |this, _: &ClickEvent, _, cx| {
                            let items: Vec<_> = this.hot.items.iter().filter(|h| h.id != id2).cloned().collect();
                            if this.hot_edit.as_deref() == Some(id2.as_str()) {
                                this.hot_edit = None;
                            }
                            this.save_phrases(items);
                            cx.notify();
                        },
                    )),
                );
            list = list.child(row_el);
        }
        let synced = if self.hot.pending {
            "有改动还没同步到关联设备（联网后自动上传）"
        } else {
            "关联设备后，常用语会在手机和电脑之间同步"
        };
        div()
            .flex()
            .flex_col()
            .gap(px(12.))
            .child(
                div()
                    .px(px(4.))
                    .text_size(px(13.))
                    .text_color(rgb(TEXT2))
                    .child(format!("输入快捷字母（例如 sss）时，对应的短语会排在第一个候选。{synced}。")),
            )
            .child(form)
            .child(section_title("我的常用语"))
            .child(list)
    }
}

impl Settings {
    fn reload_dicts(&mut self) {
        self.cells = dicts::load_cells();
        self.core_state = dicts::load_core_state();
        self.core_conf = dicts::load_core_conf();
        self.catalog = dicts::load_catalog();
    }

    fn save_cells(&mut self, cx: &mut Context<Self>) {
        self.dict_error = dicts::save_cells(&self.cells).err().map(|e| format!("保存失败：{e}"));
        cx.notify();
    }

    fn save_core_conf(&mut self, cx: &mut Context<Self>) {
        self.dict_error = dicts::save_core_conf(&self.core_conf).err().map(|e| format!("保存失败：{e}"));
        cx.notify();
    }

    /// 核心词库的配置比引擎上次启动时新：要重启引擎才生效
    fn core_pending(&self) -> bool {
        // 按内容比：当前配置和引擎上次启动实际用的配置不一样，就是有改动待生效
        !self.core_state.is_empty() && self.core_conf != dicts::conf_from_state(&self.core_state)
    }

    fn discard_core_changes(&mut self, cx: &mut Context<Self>) {
        self.core_conf = dicts::conf_from_state(&self.core_state);
        self.save_core_conf(cx);
        self.toast = Some(("已放弃改动".into(), true, Instant::now()));
    }

    fn restart_engine(&mut self, cx: &mut Context<Self>) {
        let ok = std::process::Command::new("systemctl")
            .args(["--user", "restart", "minime-engined"])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .is_ok();
        if ok {
            let m = std::fs::metadata(dicts::dir().join("core_state.tsv")).and_then(|m| m.modified()).ok();
            self.restarting = Some((Instant::now(), m));
        } else {
            self.toast = Some(("无法运行 systemctl".into(), false, Instant::now()));
        }
        cx.notify();
    }

    fn import_dicts(&mut self, cx: &mut Context<Self>) {
        let rx = cx.prompt_for_paths(PathPromptOptions {
            files: true,
            directories: false,
            multiple: true,
            prompt: Some("导入细胞词库".into()),
        });
        cx.spawn(async move |this, cx| {
            let Ok(Ok(Some(paths))) = rx.await else { return };
            let _ = this.update(cx, |this, cx| {
                for p in paths {
                    match dicts::import(&p) {
                        Ok(dst) => {
                            let path = dst.to_string_lossy().into_owned();
                            if !this.cells.iter().any(|c| c.path == path) {
                                let kind = dicts::guess_kind(dicts::file_name(&path));
                                this.cells.push(dicts::Cell { enabled: true, kind, path, loaded: None });
                            }
                        }
                        Err(e) => this.dict_error = Some(format!("导入失败：{e}")),
                    }
                }
                this.save_cells(cx);
                this.toast = Some(("已导入，马上生效".into(), true, Instant::now()));
            });
        })
        .detach();
    }

    fn page_models(&self, _cx: &mut Context<Self>) -> Div {
        let model_title = |name: &str| {
            let (t, _) = dicts::model_info(name);
            let short = name.trim_end_matches(".bin");
            if t.is_empty() { short.to_string() } else { format!("{t}（{short}）") }
        };
        // 整句模型：库房里的 e2e 包
        let mut e2e: Vec<&dicts::Pack> = self.catalog.iter().filter(|p| dicts::is_e2e(&p.name)).collect();
        e2e.sort_by_key(|p| (p.name.starts_with("e2enr"), p.name.clone()));
        let on = self.ini.get_bool("", "WholeSentence", true);
        let n = e2e.len();
        let mut e2e_card = card();
        for (i, p) in e2e.iter().enumerate() {
            let used = !p.name.starts_with("e2enr");
            let mut meta = vec![dicts::model_info(&p.name).1.to_string()];
            meta.push(format!("v{} · {} · {}", p.version, dicts::platforms_text(&p.platforms), dicts::size_text(p.size)));
            if p.deprecated {
                meta.push("官方已下架".into());
            }
            let status = if used {
                if on { ("使用中", GREEN) } else { ("整句候选已关", TEXT2) }
            } else {
                ("暂不支持", TEXT2)
            };
            e2e_card = e2e_card.child(dict_row("icons/cpu.svg", model_title(&p.name), meta, Some(status), div(), i + 1 < n));
        }
        if n == 0 {
            // 库房里没有（普通用户都是这样）：显示 minime-fetch 下载的模型
            let dir = official_dir().join("mac/imeData");
            let files: Vec<(&str, u64)> = ["decgnr.bin", "mpd_e2e.bin"]
                .iter()
                .filter_map(|f| std::fs::metadata(dir.join(f)).ok().map(|m| (*f, m.len())))
                .collect();
            if files.len() == 2 {
                for (i, (f, size)) in files.iter().enumerate() {
                    let meta = vec![dicts::model_info(f).1.to_string(), format!("macOS 版 3.0.1 · {}", dicts::size_text(*size))];
                    let status = if on { ("使用中", GREEN) } else { ("整句候选已关", TEXT2) };
                    e2e_card = e2e_card.child(dict_row("icons/cpu.svg", model_title(f), meta, Some(status), div(), i + 1 < files.len()));
                }
            } else {
                e2e_card = e2e_card.child(
                    div().py(px(18.)).text_size(px(13.)).text_color(rgb(TEXT2)).child("还没有下载整句模型：在终端运行 minime-fetch e2e，然后重启引擎。"),
                );
            }
        }

        // 引擎内置的模型（必需）
        let mut builtin: Vec<&dicts::CoreState> =
            self.core_state.iter().filter(|c| c.state != 3 && dicts::is_model_pack(&c.name)).collect();
        const ORDER: [&str; 6] = ["lmcnmac.bin", "nlrgpt2_nnlm.bin", "mdltrpc.bin", "hpmac_base.bin", "md.bin", "prism_v2.bin"];
        builtin.sort_by_key(|c| ORDER.iter().position(|x| *x == c.name).unwrap_or(ORDER.len()));
        let bn = builtin.len();
        let mut core = card();
        for (i, c) in builtin.iter().enumerate() {
            let mut meta = vec![];
            let d = dicts::model_info(&c.name).1;
            if !d.is_empty() {
                meta.push(d.to_string());
            }
            meta.push(format!("v{} · {}", c.version, dicts::size_text(dicts::file_size(&c.path))));
            core = core.child(dict_row(
                "icons/cpu.svg",
                model_title(&c.name),
                meta,
                None,
                div(),
                i + 1 < bn,
            ));
        }

        // 其他平台的模型（实验，追加给引擎）
        let builtin_names: Vec<&str> = self.core_state.iter().map(|c| c.name.as_str()).collect();
        let others: Vec<&dicts::Pack> = self
            .catalog
            .iter()
            .filter(|p| {
                !p.builtin && p.type_id.is_some() && dicts::is_model_pack(&p.name) && !dicts::is_e2e(&p.name)
                    && !builtin_names.contains(&p.name.as_str())
            })
            .collect();
        let on_n = others.len();
        let mut other_card = card();
        for (i, p) in others.iter().enumerate() {
            let mut meta = vec![];
            let d = dicts::model_info(&p.name).1;
            if !d.is_empty() {
                meta.push(d.to_string());
            }
            meta.push(format!("v{} · {} · {}", p.version, dicts::platforms_text(&p.platforms), dicts::size_text(p.size)));
            other_card = other_card.child(dict_row("icons/smartphone.svg", model_title(&p.name), meta, None, div(), i + 1 < on_n));
        }

        let page = div().flex().flex_col().gap(px(12.)).child(
            div()
                .px(px(4.))
                .text_size(px(13.))
                .text_color(rgb(TEXT2))
                .child("模型负责给候选打分和排序。整句模型由「输入」页的整句候选开关控制。"),
        );
        page.child(section_title("整句模型"))
            .child(e2e_card)
            .child(section_title("引擎内置模型"))
            .child(core)
            .child(section_title("其他平台的模型（桌面引擎每类只用一个，这些暂时只是展示）"))
            .child(other_card)
    }

    fn page_dicts(&self, cx: &mut Context<Self>) -> Div {
        // ---------- 专题词库（细胞词库）：已配置的 + 库房里还没加的
        struct CellRow {
            path: String,
            kind: u32,
            enabled: bool,
            loaded: Option<bool>,
            extra: String,
            removable: bool,
        }
        let imported_dir = dicts::dir().join("imported").to_string_lossy().into_owned();
        let mut rows: Vec<CellRow> = self
            .cells
            .iter()
            .filter(|c| !dicts::file_name(&c.path).contains("cell_dict.ssrc."))
            .map(|c| {
                let pk = self.catalog.iter().find(|p| p.file == c.path);
                CellRow {
                    path: c.path.clone(),
                    kind: c.kind,
                    enabled: c.enabled,
                    loaded: c.loaded,
                    extra: pk.map(|p| format!("v{} · {}", p.version, dicts::platforms_text(&p.platforms))).unwrap_or_default(),
                    removable: c.path.starts_with(&imported_dir),
                }
            })
            .collect();
        for p in &self.catalog {
            let is_cell = p.dict_type == "DictTypeCellDict" || p.name.starts_with("cell_dict.");
            let same = rows.iter().any(|r| dicts::cell_info(&r.path).0 == dicts::cell_info(&p.file).0);
            if is_cell && !p.builtin && !same && !p.name.starts_with("cell_dict.ssrc.") {
                rows.push(CellRow {
                    path: p.file.clone(),
                    kind: p.type_id.unwrap_or_else(|| dicts::guess_kind(&p.name)),
                    enabled: false,
                    loaded: None,
                    extra: format!("v{} · {}", p.version, dicts::platforms_text(&p.platforms)),
                    removable: false,
                });
            }
        }
        let n = rows.len();
        let mut cells = card();
        for (i, r) in rows.into_iter().enumerate() {
            let (title, desc) = dicts::cell_info(&r.path);
            let (st, st_color) = match (r.enabled, r.loaded) {
                (true, Some(true)) => ("已加载", GREEN),
                (true, Some(false)) => ("加载失败", RED),
                (true, None) => ("加载中…", TEXT2),
                (false, _) => ("未启用", TEXT2),
            };
            let path = r.path.clone();
            let kind = r.kind;
            let right = div()
                .flex()
                .items_center()
                .gap(px(10.))
                .when(r.removable, |d| {
                    let path = path.clone();
                    d.child(Button::new(SharedString::from(format!("cell-rm-{i}"))).label("移除").small().ghost().on_click(cx.listener(
                        move |this, _: &ClickEvent, _, cx| {
                            this.cells.retain(|c| c.path != path);
                            let _ = std::fs::remove_file(&path);
                            this.save_cells(cx);
                        },
                    )))
                })
                .child(Switch::new(SharedString::from(format!("cell-{i}"))).checked(r.enabled).on_click(cx.listener(
                    move |this, v: &bool, _, cx| {
                        match this.cells.iter_mut().find(|c| c.path == path) {
                            Some(c) => {
                                c.enabled = *v;
                                c.loaded = None;
                            }
                            None => this.cells.push(dicts::Cell { enabled: *v, kind, path: path.clone(), loaded: None }),
                        }
                        this.save_cells(cx);
                    },
                )));
            let mut meta = vec![desc, dicts::size_text(dicts::file_size(&r.path))];
            if !r.extra.is_empty() {
                meta.push(r.extra.clone());
            }
            cells = cells.child(dict_row("icons/book-open.svg", title, meta, Some((st, st_color)), right, i + 1 < n));
        }
        if n == 0 {
            cells = cells.child(div().py(px(22.)).text_size(px(14.)).text_color(rgb(TEXT2)).child("还没有专题词库。"));
        }

        // ---------- 核心词库：按分组
        let latest = |name: &str, ver: u32| {
            self.catalog
                .iter()
                .filter(|p| !p.builtin && p.name == name && p.version > ver)
                .max_by_key(|p| p.version)
                .cloned()
        };
        let mut groups: Vec<(dicts::Group, Vec<(String, Vec<String>, AnyElement)>)> = vec![
            (dicts::Group::Scheme, vec![]),
            (dicts::Group::Extra, vec![]),
            (dicts::Group::Voice, vec![]),
            (dicts::Group::System, vec![]),
        ];
        let builtin: Vec<&dicts::CoreState> = self.core_state.iter().filter(|c| c.state != 3).collect();
        for (i, c) in builtin.iter().enumerate() {
            if dicts::is_model_pack(&c.name) {
                continue; // 模型放在「模型」页
            }
            let (g, title, desc) = dicts::core_info(&c.name);
            let short = c.name.trim_end_matches(".bin").to_string();
            let title = if title.is_empty() { short.clone() } else { format!("{title}（{short}）") };
            let enabled = !self.core_conf.off.contains(&c.name);
            // 关掉的包状态文件里没有路径，从库房目录里找内置那份
            let size = if c.path.is_empty() {
                self.catalog.iter().find(|p| p.builtin && p.name == c.name).map(|p| p.size).unwrap_or(0)
            } else {
                dicts::file_size(&c.path)
            };
            let mut meta = vec![];
            if !desc.is_empty() {
                meta.push(desc.to_string());
            }
            meta.push(format!("v{}", c.version));
            if size > 0 {
                meta.push(dicts::size_text(size));
            }
            if c.state == 2 {
                meta.push("已换成新版".into());
            }
            let name = c.name.clone();
            let right: AnyElement = if g == dicts::Group::System {
                div().into_any_element()
            } else {
                let mut r = div().flex().items_center().gap(px(10.));
                if let Some(newer) = latest(&c.name, c.version) {
                    let use_new = self.core_conf.replace.get(&c.name) == Some(&newer.file);
                    let (name2, file) = (c.name.clone(), newer.file.clone());
                    r = r.child(
                        Button::new(SharedString::from(format!("core-new-{i}")))
                            .label(if use_new { format!("用回内置") } else { format!("换成新版 v{}", newer.version) })
                            .small()
                            .outline()
                            .on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                                if this.core_conf.replace.get(&name2) == Some(&file) {
                                    this.core_conf.replace.remove(&name2);
                                } else {
                                    this.core_conf.replace.insert(name2.clone(), file.clone());
                                }
                                this.save_core_conf(cx);
                            })),
                    );
                }
                r.child(Switch::new(SharedString::from(format!("core-{i}"))).checked(enabled).on_click(cx.listener(
                    move |this, v: &bool, _, cx| {
                        if *v {
                            this.core_conf.off.remove(&name);
                        } else {
                            this.core_conf.off.insert(name.clone());
                        }
                        this.save_core_conf(cx);
                    },
                )))
                .into_any_element()
            };
            if let Some(gr) = groups.iter_mut().find(|(gg, _)| *gg == g) {
                gr.1.push((title, meta, right));
            }
        }

        // ---------- 其他平台的包（实验）
        let builtin_names: Vec<&str> = self.core_state.iter().map(|c| c.name.as_str()).collect();
        let mut others: Vec<&dicts::Pack> = self
            .catalog
            .iter()
            .filter(|p| {
                !p.builtin
                    && p.type_id.is_some()
                    && !(p.dict_type == "DictTypeCellDict" || p.name.starts_with("cell_dict."))
                    && !dicts::is_model_pack(&p.name)
                    && !builtin_names.contains(&p.name.as_str())
            })
            .collect();
        others.sort_by_key(|p| (dicts::other_info(&p.name).0.is_empty(), p.name.clone()));
        let other_n = others.len();
        let shown = if self.open_other { other_n } else { other_n.min(6) };
        let mut other_card = card();
        for (i, p) in others.iter().take(shown).enumerate() {
            let (t, d) = dicts::other_info(&p.name);
            let short = p.name.trim_end_matches(".bin").to_string();
            let title = if t.is_empty() { short.clone() } else { format!("{t}（{short}）") };
            let mut meta = vec![];
            if !d.is_empty() {
                meta.push(d.to_string());
            }
            meta.push(format!("v{} · {} · {}", p.version, dicts::platforms_text(&p.platforms), dicts::size_text(p.size)));
            if p.deprecated {
                meta.push("官方已下架".into());
            }
            let on = self.core_conf.add.contains_key(&p.file);
            let (file, tid) = (p.file.clone(), p.type_id.unwrap_or(0));
            let sw = Switch::new(SharedString::from(format!("other-{i}"))).checked(on).on_click(cx.listener(
                move |this, v: &bool, _, cx| {
                    if *v {
                        this.core_conf.add.insert(file.clone(), tid);
                    } else {
                        this.core_conf.add.remove(&file);
                    }
                    this.save_core_conf(cx);
                },
            ));
            other_card = other_card.child(dict_row("icons/smartphone.svg", title, meta, None, sw, i + 1 < shown || other_n > 6));
        }
        if other_n > 6 {
            other_card = other_card.child(div().py(px(6.)).child(
                Button::new("other-toggle")
                    .label(if self.open_other { "收起".to_string() } else { format!("展开全部 {other_n} 个") })
                    .small()
                    .ghost()
                    .on_click(cx.listener(|this, _: &ClickEvent, _, cx| {
                        this.open_other = !this.open_other;
                        cx.notify();
                    })),
            ));
        }
        if other_n == 0 {
            other_card = other_card.child(
                div()
                    .py(px(18.))
                    .text_size(px(13.))
                    .text_color(rgb(TEXT2))
                    .child("目前没有其他平台的词库。"),
            );
        }

        let mut page = div()
            .flex()
            .flex_col()
            .gap(px(12.))
            .child(
                div()
                    .px(px(4.))
                    .text_size(px(13.))
                    .text_color(rgb(TEXT2))
                    .child("专题词库开关立即生效；核心词库和其他平台的词库改动后要重启引擎。模型在「模型」页。"),
            )
            .when_some(self.dict_error.clone(), |d, e| d.child(div().px(px(4.)).text_size(px(13.)).text_color(rgb(RED)).child(e)));
        page = page
            .child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(section_title("专题词库"))
                    .child(Button::new("cell-import").label("导入词库文件…").small().outline().on_click(cx.listener(
                        |this, _: &ClickEvent, _, cx| this.import_dicts(cx),
                    ))),
            )
            .child(cells);
        let titles = [
            (dicts::Group::Scheme, "输入方案"),
            (dicts::Group::Extra, "扩展词库"),
            (dicts::Group::Voice, "语音"),
            (dicts::Group::System, "系统组件"),
        ];
        for (g, raw) in groups {
            if raw.is_empty() {
                continue;
            }
            let cnt = raw.len();
            let items: Vec<AnyElement> = raw
                .into_iter()
                .enumerate()
                .map(|(i, (t, m, r))| dict_row("icons/book-open.svg", t, m, None, r, i + 1 < cnt).into_any_element())
                .collect();
            let t = titles.iter().find(|(x, _)| *x == g).map(|(_, t)| *t).unwrap_or("");
            if g == dicts::Group::System {
                page = page.child(
                    div()
                        .flex()
                        .items_center()
                        .justify_between()
                        .child(section_title(t))
                        .child(
                            Button::new("sys-toggle")
                                .label(if self.open_system { "收起".to_string() } else { format!("展开 {cnt} 个") })
                                .small()
                                .ghost()
                                .on_click(cx.listener(|this, _: &ClickEvent, _, cx| {
                                    this.open_system = !this.open_system;
                                    cx.notify();
                                })),
                        ),
                );
                if self.open_system {
                    page = page.child(card().children(items));
                }
            } else {
                page = page.child(section_title(t)).child(card().children(items));
            }
        }
        page.child(section_title("其他平台的词库（实验，追加给引擎；桌面版可能用不上）")).child(other_card)
    }
}

/// 词库列表的一行：图标、标题、灰色说明（多段用 · 分隔）、可选状态、右侧控件。
fn dict_row(
    ico: &'static str,
    title: String,
    meta: Vec<String>,
    status: Option<(&'static str, u32)>,
    right: impl IntoElement,
    divider: bool,
) -> Div {
    let mut line = div().flex().flex_wrap().gap(px(6.)).text_size(px(12.)).text_color(rgb(TEXT2));
    for (i, m) in meta.into_iter().enumerate() {
        if i > 0 {
            line = line.child("·");
        }
        line = line.child(m);
    }
    if let Some((s, c)) = status {
        line = line.child("·").child(div().text_color(rgb(c)).child(s));
    }
    div()
        .w_full()
        .min_h(px(60.))
        .py(px(10.))
        .flex()
        .items_center()
        .gap(px(14.))
        .when(divider, |d| d.border_b_1().border_color(rgb(DIVIDER)))
        .child(
            div()
                .flex_none()
                .size(px(34.))
                .rounded(px(10.))
                .bg(rgb(0xEAF8F0))
                .flex()
                .items_center()
                .justify_center()
                .child(icon(ico).size(px(17.)).text_color(rgb(GREEN))),
        )
        .child(
            div()
                .flex_1()
                .min_w(px(0.))
                .flex()
                .flex_col()
                .gap(px(2.))
                .child(div().text_size(px(15.)).text_color(rgb(TEXT)).child(title))
                .child(line),
        )
        .child(right)
}

impl Render for Settings {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let (title, content) = match self.page {
            Page::Input => ("输入", self.page_input(cx)),
            Page::Devices => ("跨设备", self.page_devices(cx)),
            Page::About => ("关于", self.page_about(cx)),
            Page::Phrases => ("常用语", self.page_phrases(cx)),
            Page::Dicts => ("词库", self.page_dicts(cx)),
            Page::Models => ("模型", self.page_models(cx)),
        };
        let toast = self.toast.clone();
        let save_error = self.save_error.clone();
        // 词库 / 模型页：有待生效的改动（或正在重启）时，底部浮一个胶囊
        let pill: Option<AnyElement> = if matches!(self.page, Page::Dicts | Page::Models)
            && (self.core_pending() || self.restarting.is_some())
        {
            let inner = if self.restarting.is_some() {
                div()
                    .flex()
                    .items_center()
                    .gap(px(10.))
                    .child(div().size(px(8.)).rounded_full().bg(rgb(GREEN)))
                    .child(div().text_size(px(14.)).text_color(rgb(TEXT)).child("正在重启引擎…"))
            } else {
                div()
                    .flex()
                    .items_center()
                    .gap(px(12.))
                    .child(div().size(px(8.)).rounded_full().bg(rgb(0xF5A524)))
                    .child(div().text_size(px(14.)).text_color(rgb(TEXT)).child("有改动，重启引擎后生效"))
                    .child(Button::new("pill-discard").label("放弃").small().ghost().on_click(cx.listener(
                        |this, _: &ClickEvent, _, cx| this.discard_core_changes(cx),
                    )))
                    .child(Button::new("pill-restart").label("重启引擎").small().primary().on_click(cx.listener(
                        |this, _: &ClickEvent, _, cx| this.restart_engine(cx),
                    )))
            };
            Some(
                div()
                    .absolute()
                    .bottom(px(22.))
                    .left(px(232.))
                    .right_0()
                    .flex()
                    .justify_center()
                    .child(
                        div()
                            .px(px(18.))
                            .py(px(8.))
                            .rounded(px(24.))
                            .bg(rgb(0xFFFFFF))
                            .border_1()
                            .border_color(rgb(0xE3E5E8))
                            .shadow_lg()
                            .child(inner),
                    )
                    .into_any_element(),
            )
        } else {
            None
        };
        let toast_bottom = if pill.is_some() { 84. } else { 24. };
        div()
            .size_full()
            .flex()
            .bg(rgb(PAGE_BG))
            .font_family(self.ui_font.clone())
            .child(self.sidebar(cx))
            .child(
                div()
                    .id("content")
                    .flex_1()
                    .min_w(px(0.))
                    .h_full()
                    .overflow_y_scroll()
                    .px(px(28.))
                    .pt(px(22.))
                    .pb(px(if pill.is_some() { 96. } else { 28. }))
                    .child(div().pb(px(14.)).text_size(px(20.)).font_weight(FontWeight::MEDIUM).text_color(rgb(TEXT)).child(title))
                    .when_some(save_error, |d, e| d.child(div().pb(px(10.)).text_size(px(13.)).text_color(rgb(RED)).child(e)))
                    .child(content.w_full()),
            )
            .when_some(pill, |d, p| d.child(p))
            .when_some(toast, |d, (msg, ok, _)| {
                d.child(
                    div().absolute().bottom(px(toast_bottom)).left(px(232.)).right_0().flex().justify_center().child(
                        div()
                            .px(px(16.))
                            .py(px(9.))
                            .rounded(px(18.))
                            .bg(rgba(0x1A1A1AE6))
                            .text_size(px(13.))
                            .text_color(rgb(if ok { 0xFFFFFF } else { 0xFFB4B4 }))
                            .child(msg),
                    ),
                )
            })
    }
}

fn main() {
    let _ = minime_netd::legacy::migrate(); // 旧版 wetype 目录和配置改名过来（引擎没先跑时）
    gpui_kit::application().with_assets(AppAssets).run(move |cx| {
        gpui_kit::init(cx);
        Theme::change(ThemeMode::Light, None, cx);
        {
            let t = Theme::global_mut(cx);
            // 颜色和组件 token 两边都要改（组件读 tokens）
            macro_rules! set {
                ($($f:ident = $c:expr),+ $(,)?) => {$(
                    let c: Hsla = rgb($c).into();
                    t.$f = c;
                    t.tokens.$f = c.into();
                )+};
            }
            set!(
                primary = GREEN,
                primary_hover = 0x06AD56,
                primary_active = 0x059A4C,
                primary_foreground = 0xFFFFFF,
                button_primary = GREEN,
                button_primary_hover = 0x06AD56,
                button_primary_active = 0x059A4C,
                button_primary_foreground = 0xFFFFFF,
                ring = 0x7EDCA9,
                switch = 0xE1E3E6,
            );
        }
        cx.on_window_closed(|cx, _| {
            if cx.windows().is_empty() {
                cx.quit();
            }
        })
        .detach();
        let bounds = Bounds::centered(None, size(px(900.), px(660.)), cx);
        gpui_kit::open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                titlebar: Some(TitlebarOptions { title: Some("MinIME 设置".into()), ..Default::default() }),
                app_id: Some("minime-settings".into()),
                window_min_size: Some(size(px(760.), px(520.))),
                ..Default::default()
            },
            cx,
            |window, cx| cx.new(|cx| Settings::new(window, cx)),
        )
        .expect("open window");
        cx.activate(true);
    });
}

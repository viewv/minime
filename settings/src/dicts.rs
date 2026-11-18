//! 词库配置：读写 engined 的 $XDG_DATA_HOME/minime/dicts/ 下的文件。
//!
//! - cells.tsv       细胞词库：`启用(0/1)<TAB>type<TAB>路径`，我们写，engined 热加载
//! - state.tsv       细胞词库加载结果：`1 成功 / -1 失败 / 0 未启用<TAB>type<TAB>路径`
//! - core_conf.tsv   核心词库覆盖：`off<TAB>名字` / `replace<TAB>名字<TAB>路径` / `add<TAB>typeId<TAB>路径`，
//!                   我们写，engined 启动时读（要重启引擎）
//! - core_state.tsv  核心词库实际加载情况：`状态<TAB>typeId<TAB>版本<TAB>名字<TAB>路径`
//!                   （1 内置 0 关闭 2 替换 3 追加）
//! - library/catalog.json  本机词库库房（其他平台的词库；将来 netd 下载的包也放这里）

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

pub fn dir() -> PathBuf {
    let base = std::env::var("XDG_DATA_HOME")
        .ok()
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"));
    base.join("minime/dicts")
}

fn read_lines(p: &Path, min: usize) -> Vec<Vec<String>> {
    std::fs::read_to_string(p)
        .unwrap_or_default()
        .lines()
        .map(|l| l.split('\t').map(|s| s.to_string()).collect::<Vec<_>>())
        .filter(|v| v.len() >= min)
        .collect()
}

fn write_atomic(name: &str, text: &str) -> std::io::Result<()> {
    let d = dir();
    std::fs::create_dir_all(&d)?;
    let tmp = d.join(format!("{name}.tmp"));
    std::fs::write(&tmp, text)?;
    std::fs::rename(tmp, d.join(name))
}

pub fn file_name(path: &str) -> &str {
    path.rsplit('/').next().unwrap_or(path)
}

pub fn size_text(bytes: u64) -> String {
    if bytes >= 1 << 20 {
        format!("{:.1} MB", bytes as f64 / (1 << 20) as f64)
    } else {
        format!("{} KB", (bytes + 1023) / 1024)
    }
}

pub fn file_size(path: &str) -> u64 {
    std::fs::metadata(path).map(|m| m.len()).unwrap_or(0)
}

// ------------------------------------------------------------------ 细胞词库

#[derive(Clone, Debug, PartialEq)]
pub struct Cell {
    pub enabled: bool,
    pub kind: u32,
    pub path: String,
    /// engined 的加载结果：Some(true) 成功，Some(false) 失败，None 未启用/未知
    pub loaded: Option<bool>,
}

pub fn load_cells() -> Vec<Cell> {
    let state = read_lines(&dir().join("state.tsv"), 3);
    read_lines(&dir().join("cells.tsv"), 3)
        .into_iter()
        .map(|v| {
            let path = v[2].clone();
            let loaded = state.iter().find(|s| s[2] == path).and_then(|s| match s[0].as_str() {
                "1" => Some(true),
                "-1" => Some(false),
                _ => None,
            });
            Cell { enabled: v[0] == "1", kind: v[1].parse().unwrap_or(0), path, loaded }
        })
        .collect()
}

pub fn save_cells(cells: &[Cell]) -> std::io::Result<()> {
    let mut text = String::new();
    for c in cells {
        text.push_str(&format!("{}\t{}\t{}\n", c.enabled as u8, c.kind, c.path));
    }
    write_atomic("cells.tsv", &text)
}

/// 细胞词库类型（服务端清单的 typeId）：wrw=1，ssrc=7，其余 0。
pub fn guess_kind(name: &str) -> u32 {
    if name.contains("cell_dict.wrw.") {
        1
    } else if name.contains("cell_dict.ssrc.") {
        7
    } else {
        0
    }
}

/// 导入：原子复制到 dicts/imported/ 下，返回新路径。
pub fn import(src: &Path) -> std::io::Result<PathBuf> {
    let d = dir().join("imported");
    std::fs::create_dir_all(&d)?;
    let name = src.file_name().ok_or_else(|| std::io::Error::other("no file name"))?;
    let dst = d.join(name);
    if dst != src {
        let tmp = d.join(format!("{}.tmp", name.to_string_lossy()));
        std::fs::copy(src, &tmp)?;
        std::fs::rename(&tmp, &dst)?;
    }
    Ok(dst)
}

// ------------------------------------------------------------------ 库房

#[derive(Clone, Debug, serde::Deserialize)]
pub struct Pack {
    pub name: String,
    #[serde(rename = "typeId")]
    pub type_id: Option<u32>,
    #[serde(rename = "dictType", default)]
    pub dict_type: String,
    #[serde(default)]
    pub version: u32,
    #[serde(default)]
    #[allow(dead_code)]
    pub md5: String,
    #[serde(default)]
    pub size: u64,
    #[serde(default)]
    pub platforms: Vec<String>,
    pub file: String,
    #[serde(default)]
    pub builtin: bool,
    /// 服务端清单标了 del（官方已下架，CDN 还能下）
    #[serde(default)]
    pub deprecated: bool,
}

pub fn load_catalog() -> Vec<Pack> {
    std::fs::read_to_string(dir().join("library/catalog.json"))
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

pub fn platforms_text(p: &[String]) -> String {
    p.iter()
        .map(|x| match x.as_str() {
            "windows" => "Windows",
            "mac" => "Mac",
            "android" => "安卓",
            "ios" => "iOS",
            o => o,
        })
        .collect::<Vec<_>>()
        .join(" / ")
}

// ------------------------------------------------------------------ 核心词库

#[derive(Clone, Debug, PartialEq)]
pub struct CoreState {
    /// 1 内置 0 关闭 2 替换 3 追加
    pub state: u8,
    pub type_id: u32,
    pub version: u32,
    pub name: String,
    pub path: String,
}

pub fn load_core_state() -> Vec<CoreState> {
    read_lines(&dir().join("core_state.tsv"), 4)
        .into_iter()
        .map(|v| CoreState {
            state: v[0].parse().unwrap_or(1),
            type_id: v[1].parse().unwrap_or(0),
            version: v[2].parse().unwrap_or(0),
            name: v[3].clone(),
            path: v.get(4).cloned().unwrap_or_default(),
        })
        .collect()
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct CoreConf {
    pub off: BTreeSet<String>,
    pub replace: BTreeMap<String, String>,
    /// 路径 -> typeId
    pub add: BTreeMap<String, u32>,
}

impl CoreConf {
    /// 去掉已关闭词库上的 replace（engined 会跳过关闭的词库，不写替换路径到 core_state.tsv）
    pub fn effective(&self) -> CoreConf {
        let mut c = self.clone();
        c.replace.retain(|k, _| !c.off.contains(k));
        c
    }
}

/// 由引擎实际加载的结果反推出当时用的配置（用来判断「有没有待生效的改动」和「放弃改动」）
pub fn conf_from_state(state: &[CoreState]) -> CoreConf {
    let mut c = CoreConf::default();
    for s in state {
        match s.state {
            0 => {
                c.off.insert(s.name.clone());
            }
            2 => {
                c.replace.insert(s.name.clone(), s.path.clone());
            }
            3 => {
                c.add.insert(s.path.clone(), s.type_id);
            }
            _ => {}
        }
    }
    c
}

pub fn load_core_conf() -> CoreConf {
    let mut c = CoreConf::default();
    for v in read_lines(&dir().join("core_conf.tsv"), 2) {
        match (v[0].as_str(), v.get(2)) {
            ("off", _) => {
                c.off.insert(v[1].clone());
            }
            ("replace", Some(p)) => {
                c.replace.insert(v[1].clone(), p.clone());
            }
            ("add", Some(p)) => {
                c.add.insert(p.clone(), v[1].parse().unwrap_or(0));
            }
            _ => {}
        }
    }
    c
}

pub fn save_core_conf(c: &CoreConf) -> std::io::Result<()> {
    let mut t = String::new();
    for n in &c.off {
        t.push_str(&format!("off\t{n}\n"));
    }
    for (n, p) in &c.replace {
        t.push_str(&format!("replace\t{n}\t{p}\n"));
    }
    for (p, id) in &c.add {
        t.push_str(&format!("add\t{id}\t{p}\n"));
    }
    write_atomic("core_conf.tsv", &t)
}

// ------------------------------------------------------------------ 分类与说明

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Group {
    /// 五笔等输入方案，可关
    Scheme,
    /// 扩展词库，可关
    Extra,
    /// 语音相关，可关
    Voice,
    /// 系统组件，不能关
    System,
}

/// 内置核心词库：(分组, 中文名, 说明)
pub fn core_info(name: &str) -> (Group, &'static str, &'static str) {
    let n = name.trim_end_matches(".bin");
    match n {
        "swbd86" => (Group::Scheme, "五笔 86", "五笔字型 86 版编码"),
        "swbd98" => (Group::Scheme, "五笔 98", "五笔字型 98 版编码"),
        "swbdnc" => (Group::Scheme, "五笔新世纪", "五笔新世纪版编码"),
        "slwd_pc_second_level" => (Group::Extra, "二级词库", "主词库之外的长尾词（约 50 MB）"),
        "ckvdmac" => (Group::Extra, "大词库", "大规模词条补充（约 39 MB）"),
        "bwcnenmac" => (Group::Extra, "中英混合词库", "中英混输时的英文词"),
        "bwcjpmac_jianpin" => (Group::Extra, "简拼词库", "声母简拼（nh → 你好）"),
        "noun_tag_dict" => (Group::Extra, "名词标签", "人名、地名等名词的标签"),
        "tts" => (Group::Voice, "语音前端表（tts）", "语音输入用的字符表"),
        "stt" => (Group::Voice, "语音前端表（stt）", "语音输入用的索引表"),
        "bwcnmac" => (Group::System, "中文主词库", ""),
        "bwcnmacv8" => (Group::System, "中文主词库（v8）", ""),
        "lmcnmac" => (Group::System, "中文语言模型", ""),
        "nlrgpt2_nnlm" => (Group::System, "神经网络语言模型", ""),
        "hpmac_base" => (Group::System, "超参数", "引擎的超参数（hyper_params）"),
        "editpinyin" => (Group::System, "拼音纠错表", ""),
        "bccn" => (Group::System, "常用词表", ""),
        "pcc" => (Group::System, "专业词表", "机械、证券、函数等领域词"),
        "wkvc2e.pc" => (Group::System, "词 → emoji", "打「红包」「晚安」时出对应的 emoji"),
        "wwkvc2e" => (Group::System, "词 → emoji（补充）", ""),
        "mdltrpc" => (Group::System, "排序模型", "LightGBM 排序（learning to rank）"),
        "md" => (Group::System, "模型参数表", ""),
        "wsd" => (Group::System, "词频表", ""),
        "ln2py" => (Group::System, "拼音音节表", ""),
        "prism_v2" => (Group::System, "prism 小模型", ""),
        "sepd" => (Group::System, "拼写结尾预测", "预测拼音还没打完的部分（SpellingEndPredictDict）"),
        _ => (Group::System, "", ""),
    }
}

/// 其他平台的包：中文名和说明（不认识的返回 None）。
pub fn other_info(name: &str) -> (&'static str, &'static str) {
    match name.trim_end_matches(".bin") {
        // 联想（上屏后推荐下一个词；引擎的 AssociateKVDict 一族）
        "akv" => ("联想词库", "上屏后推荐下一个词（谢谢 → 你、晚安 → 好梦）"),
        "ae" => ("表情联想", "微信表情名（[旺柴] [捂脸] [玫瑰]）的联想"),
        "pskvd" => ("诗词联想", "名句上下句（春风……、子曰……）"),
        "tcd" => ("纠错词典", "成语等的错字纠正（TextCorrectDict）"),
        "wkvc2e" => ("词 → emoji（手机版）", "和桌面 wkvc2e.pc 同类"),
        // 词库与模型的手机版
        "bwcn" => ("中文主词库（手机版）", "和桌面版同类，词条略有不同"),
        "bwcnv8" => ("中文主词库 v8（手机版）", ""),
        "bwcnen" => ("中英混合词库（手机版）", ""),
        "bwcjp_jianpin" => ("简拼词库（安卓）", ""),
        "lmcn" => ("中文语言模型（手机版）", ""),
        "c4g" => ("中文 4-gram 模型", "手机端的 n-gram 语言模型（约 7 MB）"),
        "nlrgptand_nnlm" => ("神经网络语言模型（安卓）", ""),
        "slwd_second_level" => ("二级词库（安卓）", ""),
        "mdltr" => ("排序模型（手机版）", "LightGBM 排序"),
        "amd" => ("排序模型（iOS）", ""),
        "wltm" => ("TFLite 模型", "手机端的神经网络模型"),
        "ssd" => ("音节表（手机）", ""),
        "sw" => ("笔画组词表", "笔画键盘的组词（StrokeWordsDict）"),
        "sd" => ("笔画词库", "笔画键盘的词库（StrokesDict）"),
        "rsd" => ("笔画反查表", "汉字 → 笔画序列，每半字节一笔（ReverseStrokeDict）"),
        "awb" => ("五笔联想", "五笔输入后的联想（AssociateWbDict）"),
        // 英文键盘
        "bwen" => ("英文词库", "手机英文键盘的词库，将来做英文输入纠错用"),
        "wpen" => ("英文预测词库", "手机英文键盘的联想"),
        "wlwmd" => ("英文词形表", "词根、词缀（-tion、-ing）"),
        "enfont" => ("英文花体字", "happy、new year 等的花体写法"),
        "ene2e" => ("英文整句模型（iOS）", ""),
        // 其他键盘
        "editwordt9" => ("九宫格纠错表", "九宫格键盘用"),
        "wtid" => ("九宫格按键表", "九宫格按键序列（推测）"),
        "hwd_hand_write" => ("手写识别", "手写输入模型"),
        "ap" => ("拼音 trie（手机）", "Ime trie 结构"),
        "ucw" => ("iOS 小表", "用途未知"),
        "atp" | "elc" => ("配置小表", "几十字节的开关表"),
        _ => ("", ""),
    }
}

pub fn cell_info(path: &str) -> (String, String) {
    let n = file_name(path);
    let n = n
        .split_once('_')
        .filter(|(h, _)| h.len() == 8 && h.bytes().all(|b| b.is_ascii_hexdigit()))
        .map(|(_, t)| t)
        .unwrap_or(n); // 库房文件带 md5 前缀
    let known: &[(&str, &str, &str)] = &[
        ("cell_dict.hf.", "高频短语", "聊天里常用的口语短语（如「阳了」）"),
        ("cell_dict.sc.", "符号", "用拼音打出 ♠ ♥ → 等符号"),
        ("cell_dict.wrw.", "微信读书", "书名、作者和文学词汇"),
        ("cell_dict.ssrc.", "ssrc（空表）", "Ime trie 结构但几乎没有词条，像是服务端给某个功能留的占位，加载了也没变化"),
        ("cell_dict.newhot.", "云端新热词", "服务端按账号下发的新词"),
    ];
    for (k, t, d) in known {
        if n.starts_with(k) {
            return (t.to_string(), d.to_string());
        }
    }
    (n.to_string(), "导入的细胞词库".to_string())
}

/// 整句模型（e2e）：由 engined 的 e2e 模块直接加载，不走引擎的词库接口
pub fn is_e2e(name: &str) -> bool {
    matches!(name.trim_end_matches(".bin"), "decgnr" | "e2enr" | "mpd_e2e" | "e2enr_win_expt")
}

/// 模型（而不是词表）：语言模型、神经网络、排序、手写、整句等
pub fn is_model_pack(name: &str) -> bool {
    is_e2e(name)
        || matches!(
            name.trim_end_matches(".bin"),
            "lmcnmac" | "lmcn" | "nlrgpt2_nnlm" | "nlrgptand_nnlm" | "hpmac_base" | "hp_base" | "mdltrpc" | "mdltr"
                | "amd" | "md" | "prism_v2" | "c4g" | "wltm" | "ene2e" | "hwd_hand_write"
        )
}

/// 模型的中文名和说明
pub fn model_info(name: &str) -> (&'static str, &'static str) {
    match name.trim_end_matches(".bin") {
        "decgnr" => ("整句模型", "GPT-2 结构，按拼音约束解码出整句候选（我们的 Vulkan / CPU 实现）"),
        "mpd_e2e" => ("整句模型的词表", "整句模型用的分词表"),
        "e2enr" | "e2enr_win_expt" => ("中英混合整句模型（Ch_En_Mix）", "encoder + predictor + decoder 结构，我们的推理代码还不支持"),
        "lmcnmac" => ("中文语言模型", "n-gram 语言模型，给候选打分"),
        "lmcn" => ("中文语言模型（手机版）", ""),
        "c4g" => ("中文 4-gram 模型（手机）", ""),
        "nlrgpt2_nnlm" => ("神经网络语言模型", "小型神经网络，给候选重排"),
        "nlrgptand_nnlm" => ("神经网络语言模型（安卓）", ""),
        "mdltrpc" => ("排序模型", "LightGBM learning to rank，决定候选顺序"),
        "mdltr" => ("排序模型（手机版）", ""),
        "amd" => ("排序模型（iOS）", ""),
        "hpmac_base" => ("超参数", "引擎的超参数（hyper_params）"),
        "hp_base" => ("超参数（手机版）", ""),
        "md" => ("模型参数表", ""),
        "prism_v2" => ("prism 小模型", ""),
        "wltm" => ("TFLite 模型（手机）", "手机端的神经网络模型"),
        "ene2e" => ("英文整句模型（iOS）", ""),
        "hwd_hand_write" => ("手写识别模型", "手写输入用"),
        _ => ("", ""),
    }
}

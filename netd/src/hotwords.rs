//! 常用语（用户热词）：同步组里的 UserHotWords.data。
//!
//! 2026-11-15 真机样本解出的格式（官方没有对应的 .proto，字段名是我们起的）：
//!
//! ```text
//! HotWordSync { 1: 格式版本(=1), 2: repeated Item, 3: 最后修改者 uin, 4: 修改时间(ms) }
//! Item        { 1: id(创建时间 ms 的十进制字符串), 2: key(快捷字母，如 "sss"),
//!               3: text(短语), 4: 创建时间(ms), 5: 0, 9: 0, 10: 1 }
//! ```
//!
//! 输入 key（如 sss）就能出 text 这条候选。字段 5/9/10 含义未知，原样保留。

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::pb::{Msg, Pb, Val};

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct HotWord {
    pub id: String,
    pub key: String,
    pub text: String,
    #[serde(default)]
    pub ctime: u64,
    /// 未知的整数字段（字段号, 值），编码时原样写回
    #[serde(default)]
    pub extra: Vec<(u32, u64)>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct HotWords {
    /// 服务端版本号（UserHotWords.version）
    pub version: u64,
    pub items: Vec<HotWord>,
    /// 本地改过、还没上传（没关联或离线时编辑）
    #[serde(default)]
    pub pending: bool,
}

pub fn decode(data: &[u8]) -> Result<Vec<HotWord>, crate::pb::PbError> {
    if data.is_empty() {
        return Ok(Vec::new());
    }
    let m = Msg::parse(data)?;
    let mut out = Vec::new();
    for raw in m.all_bytes(2) {
        let it = Msg::parse(raw)?;
        let extra = it
            .fields
            .iter()
            .filter_map(|(f, v)| match v {
                Val::Int(x) if *f != 4 => Some((*f, *x)),
                _ => None,
            })
            .collect();
        out.push(HotWord { id: it.str(1), key: it.str(2), text: it.str(3), ctime: it.int(4), extra });
    }
    Ok(out)
}

pub fn encode(items: &[HotWord], uin: u64, now_ms: u64) -> Vec<u8> {
    let mut m = Pb::new().varint(1, 1);
    for h in items {
        let mut it = Pb::new().str(1, &h.id).str(2, &h.key).str(3, &h.text).varint(4, h.ctime);
        let extra: Vec<(u32, u64)> = if h.extra.is_empty() { vec![(5, 0), (9, 0), (10, 1)] } else { h.extra.clone() };
        for (f, v) in extra {
            it = it.varint(f, v);
        }
        m = m.msg(2, &it);
    }
    m.varint(3, uin).varint(4, now_ms).into_vec()
}

/// 新建一条（id 与官方一致用创建时间的毫秒数）。
pub fn new_item(key: &str, text: &str, now_ms: u64) -> HotWord {
    HotWord { id: now_ms.to_string(), key: key.to_string(), text: text.to_string(), ctime: now_ms, extra: Vec::new() }
}

pub fn path() -> PathBuf {
    crate::identity::data_dir().join("hot_words.json")
}

pub fn load() -> HotWords {
    std::fs::read_to_string(path()).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

/// 原子写。engined 读这个文件（mtime 变化即重载）。
pub fn save(h: &HotWords) -> std::io::Result<()> {
    let p = path();
    if let Some(d) = p.parent() {
        std::fs::create_dir_all(d)?;
    }
    let tmp = p.with_extension("json.tmp");
    std::fs::write(&tmp, serde_json::to_string_pretty(h)?)?;
    std::fs::rename(tmp, &p)?;
    // engined 读的简化版：每行 `快捷字母<TAB>短语`，短语里的 \\ \t \n 转义
    let mut tsv = String::new();
    for it in &h.items {
        let esc = it.text.replace('\\', "\\\\").replace('\t', "\\t").replace('\n', "\\n");
        tsv.push_str(&format!("{}\t{}\n", it.key, esc));
    }
    let tsv_path = p.with_extension("tsv");
    let tmp = p.with_extension("tsv.tmp");
    std::fs::write(&tmp, tsv)?;
    std::fs::rename(tmp, tsv_path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sample_roundtrip() {
        // 格式和真机样本（version 2）一致；uin 和短语换成了编造的值
        let data = hex::decode("0801122f0a0d3137393437343835323734343512037373731a0ce7a4bae4be8be79fade8afad20d5e6e4fa9d342800480050011881809aa6eaafe301209ec9f8829e34").unwrap();
        let items = decode(&data).unwrap();
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].key, "sss");
        assert_eq!(items[0].text, "示例短语");
        assert_eq!(items[0].ctime, 1794748527445);
        assert_eq!(items[0].extra, vec![(5, 0), (9, 0), (10, 1)]);
        let again = encode(&items, 1000000000000001, 1794765628574);
        assert_eq!(again, data);
    }
}

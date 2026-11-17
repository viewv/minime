//! 服务端 AB / 实验配置（cmd 4814 sub 28，空 body）。
//!
//! 响应 `{1: Status, 2: {1: 引擎实验*, 2: 客户端实验*}}`，每个实验
//! `{1: {1,2,3, 4: 实验号}, 2: {1: 键, 2: 值}*}`。引擎实验的键都是 `mmkb_cli_engine_*`，
//! 官方客户端把它们交给 `wxime_reset_cli_ab_test_info`；客户端实验（工具栏、语音、问 AI 等）
//! 只是界面开关。
//!
//! 我们只取引擎实验，写成 `~/.local/share/minime/net/ab_engine.tsv`（键\t值），engined 发现文件
//! 变了就重新灌给引擎（docs/cross-device.md「AB 参数」）。按平台不同下发不同，本机身份是 Windows。

use std::path::PathBuf;

use crate::identity;
use crate::pb::Msg;

pub fn path() -> PathBuf {
    identity::data_dir().join("ab_engine.tsv")
}

/// 解析 4814/28 的响应：返回 (引擎实验的键值, 客户端实验的键值)。
pub fn parse(body: &[u8]) -> Result<(Vec<(String, String)>, Vec<(String, String)>), crate::pb::PbError> {
    let m = Msg::parse(body)?;
    let inner = m.sub(2)?;
    let mut engine = Vec::new();
    let mut client = Vec::new();
    for (field, out) in [(1u32, &mut engine), (2u32, &mut client)] {
        for expt in inner.all_bytes(field) {
            let e = Msg::parse(expt)?;
            for kv in e.all_bytes(2) {
                let kv = Msg::parse(kv)?;
                let k = kv.str(1);
                if !k.is_empty() {
                    out.push((k, kv.str(2)));
                }
            }
        }
    }
    Ok((engine, client))
}

/// 引擎实验写成 tsv；内容没变不动文件（engined 按修改时间重灌）。返回是否有变化。
pub fn save(engine: &[(String, String)]) -> std::io::Result<bool> {
    let mut items: Vec<_> = engine
        .iter()
        .filter(|(k, v)| k.starts_with("mmkb_cli_engine_") && !v.contains(['\t', '\n']))
        .collect();
    items.sort();
    let text: String = items.iter().map(|(k, v)| format!("{k}\t{v}\n")).collect();
    let p = path();
    if std::fs::read_to_string(&p).ok().as_deref() == Some(text.as_str()) {
        return Ok(false);
    }
    let tmp = p.with_extension("tmp");
    std::fs::write(&tmp, &text)?;
    std::fs::rename(&tmp, &p)?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pb::Pb;

    #[test]
    fn parse_both_kinds() {
        let kv = |k: &str, v: &str| Pb::new().str(1, k).str(2, v);
        let hdr = Pb::new().varint(4, 9391548);
        let e = Pb::new().msg(1, &hdr).msg(2, &kv("mmkb_cli_engine_searcher_rotate_typo", "1"));
        let c = Pb::new().msg(1, &hdr).msg(2, &kv("mmkb_cli_language_model", "true")).msg(2, &kv("mmkb_cli_x", "1"));
        let body = Pb::new().msg(1, &Pb::new().varint(1, 0)).msg(2, &Pb::new().msg(1, &e).msg(2, &c)).into_vec();
        let (eng, cli) = parse(&body).unwrap();
        assert_eq!(eng, vec![("mmkb_cli_engine_searcher_rotate_typo".into(), "1".into())]);
        assert_eq!(cli.len(), 2);
    }
}

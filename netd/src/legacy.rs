//! 改名迁移（wetype → minime，2026-11）：旧版数据在 `~/.local/share/wetype`、配置在
//! `~/.config/fcitx5/conf/wetype.conf`。新的不存在时整个改名过去，再把词库配置里写死的旧路径换成新路径。
//! engined（C）里有同样的逻辑；谁先启动谁做（rename 是原子的，另一个看到新目录已在就什么都不做）。

use std::path::{Path, PathBuf};

fn home() -> PathBuf {
    PathBuf::from(std::env::var("HOME").unwrap_or_default())
}

fn data_base() -> PathBuf {
    std::env::var("XDG_DATA_HOME").map(PathBuf::from).unwrap_or_else(|_| home().join(".local/share"))
}

fn config_base() -> PathBuf {
    std::env::var("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|_| home().join(".config"))
}

fn rewrite(file: &Path, from: &str, to: &str) {
    let Ok(text) = std::fs::read_to_string(file) else { return };
    if !text.contains(from) {
        return;
    }
    let tmp = file.with_extension("migrate.tmp");
    if std::fs::write(&tmp, text.replace(from, to)).is_ok() {
        let _ = std::fs::rename(&tmp, file);
    }
}

/// 做一次迁移；返回做了哪些（给日志用）。
pub fn migrate() -> Vec<String> {
    let mut done = Vec::new();
    let (old, new) = (data_base().join("wetype"), data_base().join("minime"));
    if !new.exists() && old.is_dir() && std::fs::rename(&old, &new).is_ok() {
        let (from, to) = (format!("{}/", old.display()), format!("{}/", new.display()));
        for f in ["dicts/core_conf.tsv", "dicts/core_state.tsv", "dicts/cells.tsv", "dicts/state.tsv", "dicts/library/catalog.json"] {
            rewrite(&new.join(f), &from, &to);
        }
        done.push(format!("数据目录 {} → {}", old.display(), new.display()));
    }
    let conf = config_base().join("fcitx5/conf");
    let (oldc, newc) = (conf.join("wetype.conf"), conf.join("minime.conf"));
    if !newc.exists() && oldc.is_file() && std::fs::rename(&oldc, &newc).is_ok() {
        done.push(format!("配置 {} → {}", oldc.display(), newc.display()));
    }
    done
}

//! minime.conf（fcitx5 INI）读写：只改动目标键，保留注释和其它内容。
//!
//! fcitx5 写这个文件时会把默认值写成注释（`# Traditional=False`），
//! 所以读取时注释行一律忽略，取不到就用默认值。

use std::path::PathBuf;

pub fn path() -> PathBuf {
    if let Ok(p) = std::env::var("MINIME_NETD_CONFIG") {
        if !p.is_empty() {
            return PathBuf::from(p);
        }
    }
    let base = std::env::var("XDG_CONFIG_HOME")
        .ok()
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".config"));
    base.join("fcitx5/conf/minime.conf")
}

#[derive(Default, Clone)]
pub struct Ini {
    lines: Vec<String>,
}

fn section_of(line: &str) -> Option<&str> {
    let t = line.trim();
    t.strip_prefix('[').and_then(|s| s.strip_suffix(']'))
}

fn key_of(line: &str) -> Option<(&str, &str)> {
    let t = line.trim();
    if t.starts_with('#') || t.starts_with(';') {
        return None;
    }
    t.split_once('=').map(|(k, v)| (k.trim(), v.trim()))
}

impl Ini {
    pub fn load() -> Ini {
        let text = std::fs::read_to_string(path()).unwrap_or_default();
        Ini::parse(&text)
    }

    pub fn parse(text: &str) -> Ini {
        Ini { lines: text.lines().map(|s| s.to_string()).collect() }
    }

    /// 段的行范围 [start, end)；顶层（无段名）用 ""。start 是段头的下一行。
    fn range(&self, section: &str) -> Option<(usize, usize)> {
        let start = if section.is_empty() {
            0
        } else {
            self.lines.iter().position(|l| section_of(l) == Some(section))? + 1
        };
        let end = self.lines[start..]
            .iter()
            .position(|l| section_of(l).is_some())
            .map(|i| start + i)
            .unwrap_or(self.lines.len());
        Some((start, end))
    }

    pub fn get(&self, section: &str, key: &str) -> Option<String> {
        let (s, e) = self.range(section)?;
        self.lines[s..e].iter().find_map(|l| key_of(l).filter(|(k, _)| *k == key).map(|(_, v)| v.to_string()))
    }

    pub fn get_bool(&self, section: &str, key: &str, default: bool) -> bool {
        match self.get(section, key).as_deref() {
            Some("True") | Some("true") => true,
            Some("False") | Some("false") => false,
            _ => default,
        }
    }

    pub fn set(&mut self, section: &str, key: &str, value: &str) {
        let line = format!("{key}={value}");
        match self.range(section) {
            Some((s, e)) => {
                if let Some(i) = (s..e).find(|&i| key_of(&self.lines[i]).map(|(k, _)| k == key).unwrap_or(false)) {
                    self.lines[i] = line;
                    return;
                }
                // 插在段内最后一个非空行之后
                let mut at = e;
                while at > s && self.lines[at - 1].trim().is_empty() {
                    at -= 1;
                }
                self.lines.insert(at, line);
            }
            None => {
                if self.lines.last().map(|l| !l.trim().is_empty()).unwrap_or(false) {
                    self.lines.push(String::new());
                }
                self.lines.push(format!("[{section}]"));
                self.lines.push(line);
            }
        }
    }

    pub fn set_bool(&mut self, section: &str, key: &str, v: bool) {
        self.set(section, key, if v { "True" } else { "False" });
    }

    pub fn has_section(&self, section: &str) -> bool {
        self.range(section).is_some()
    }

    /// 清空段内所有键值（保留 `[section]` 节头，供 fcitx5 KeyList 表达“不使用/空列表”）
    pub fn clear_section(&mut self, section: &str) {
        if let Some((s, e)) = self.range(section) {
            let mut i = e;
            while i > s {
                i -= 1;
                if key_of(&self.lines[i]).is_some() {
                    self.lines.remove(i);
                }
            }
        } else {
            if self.lines.last().map(|l| !l.trim().is_empty()).unwrap_or(false) {
                self.lines.push(String::new());
            }
            self.lines.push(format!("[{section}]"));
        }
    }

    pub fn to_text(&self) -> String {
        let mut s = self.lines.join("\n");
        s.push('\n');
        s
    }

    /// 原子写回，然后让 fcitx5 重新读取插件配置（fcitx5 没运行也没关系）。
    pub fn save(&self) -> std::io::Result<()> {
        let p = path();
        if let Some(dir) = p.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let tmp = p.with_extension("conf.tmp");
        std::fs::write(&tmp, self.to_text())?;
        std::fs::rename(&tmp, &p)?;
        let reload = std::process::Command::new("busctl")
            .args([
                "--user",
                "call",
                "org.fcitx.Fcitx5",
                "/controller",
                "org.fcitx.Fcitx.Controller1",
                "ReloadAddonConfig",
                "s",
                "minime",
            ])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .or_else(|_| {
                std::process::Command::new("dbus-send")
                    .args([
                        "--session",
                        "--type=method_call",
                        "--dest=org.fcitx.Fcitx5",
                        "/controller",
                        "org.fcitx.Fcitx.Controller1.ReloadAddonConfig",
                        "string:minime",
                    ])
                    .stdout(std::process::Stdio::null())
                    .stderr(std::process::Stdio::null())
                    .spawn()
            });
        if let Ok(mut child) = reload {
            std::thread::spawn(move || {
                let _ = child.wait();
            });
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn edit_keeps_comments() {
        let mut ini = Ini::parse("# Traditional=False\nPageSize=9\n\n[Fuzzy]\n# NL=False\nZZh=True\n");
        assert_eq!(ini.get("", "Traditional"), None);
        assert_eq!(ini.get("", "PageSize").as_deref(), Some("9"));
        assert!(ini.get_bool("Fuzzy", "ZZh", false));
        ini.set_bool("", "Traditional", true);
        ini.set_bool("Fuzzy", "NL", true);
        ini.set("Network", "Enabled", "True");
        let t = ini.to_text();
        assert_eq!(
            t,
            "# Traditional=False\nPageSize=9\nTraditional=True\n\n[Fuzzy]\n# NL=False\nZZh=True\nNL=True\n\n[Network]\nEnabled=True\n"
        );
        ini.set_bool("Fuzzy", "ZZh", false);
        assert!(!ini.get_bool("Fuzzy", "ZZh", true));
    }
}

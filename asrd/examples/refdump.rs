//! refdump —— 参考实现的对照 dump：给 NPU 移植用的逐字节验证。
//!
//!   cargo run --release --example refdump frames    # 700 帧伪随机输入 → argmax id 序列
//!   cargo run --release --example refdump wav A.wav # 真 wav → 转写文本（走完整 fbank）
//!
//! 帧序列用固定的 LCG（PCG64 风格乘法哈希），`npu/asr/reference.py --dump` 用同一
//! 公式生成同一批帧，两边 forward 的 id 必须完全一致（验证权重抽取、卷积前端、
//! 40 层 transformer 和逐行动态量化语义全部对齐）。
use std::path::PathBuf;

/// 模型目录：环境变量优先；然后找 minime（新名）和 wetype（旧名）。
fn model_dir() -> PathBuf {
    for var in ["MINIME_ASR_MODEL", "WETYPE_ASR_MODEL"] {
        if let Ok(v) = std::env::var(var) {
            return PathBuf::from(v);
        }
    }
    let base = std::env::var("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(std::env::var("HOME").unwrap()).join(".local/share"));
    for rel in ["minime/asr/model", "wetype/asr/model"] {
        let p = base.join(rel);
        if p.join("dict.decoder.utf8.txt").exists() {
            return p;
        }
    }
    base.join("minime/asr/model")
}

fn lcg_frames(n: usize, scale: f64) -> Vec<[f32; 40]> {
    let mut s: u64 = 0x243F_6A88_85A3_08D3;
    (0..n)
        .map(|_| {
            let mut f = [0f32; 40];
            for v in f.iter_mut() {
                s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                let u = (s >> 11) as f64 / (1u64 << 53) as f64;
                *v = ((u * 2.0 - 1.0) * scale) as f32;
            }
            f
        })
        .collect()
}

fn main() {
    let mode = std::env::args().nth(1).unwrap_or_else(|| "frames".into());
    let dir = model_dir();
    let m = wechat_asr_gpu::load(&dir).expect("模型加载失败");
    match mode.as_str() {
        "frames" => {
            let n: usize = std::env::args().nth(2).and_then(|s| s.parse().ok()).unwrap_or(200);
            let scale: f64 = std::env::args().nth(3).and_then(|s| s.parse().ok()).unwrap_or(100.0);
            let frames = lcg_frames(n, scale);
            let ids = m.forward(&frames);
            print!("ids=");
            for i in &ids {
                print!("{i},");
            }
            println!();
        }
        "wav" => {
            let path = std::env::args().nth(2).expect("wav 路径");
            let pcm = hound_read(&path);
            let text = wechat_asr_gpu::transcribe(m, &pcm, 1);
            println!("{text}");
        }
        _ => eprintln!("用法: refdump frames | refdump wav 文件"),
    }
}

fn hound_read(path: &str) -> Vec<i16> {
    // asrd 依赖里没有 hound，直接读 16 位 PCM wave（我们自己的 wav 都是这个格式）
    let d = std::fs::read(path).expect("读 wav");
    let body = &d[44..];
    body.chunks_exact(2)
        .map(|b| i16::from_le_bytes([b[0], b[1]]))
        .collect()
}

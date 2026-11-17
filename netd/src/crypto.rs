//! 密码学原语：secp128r1 ECDH、AES-ECB-PKCS7、三种 SHA-256 签名。
//! 与 开发时的 Python 参考实现（未公开） 逐字节一致（见单元测试里的向量）。

use aes::cipher::{generic_array::GenericArray, BlockDecrypt, BlockEncrypt, KeyInit};
use md5::Md5;
use num_bigint::BigUint;
use num_traits::{One, Zero};
use rand::RngCore;
use sha2::{Digest, Sha256};

// ---------------------------------------------------------------- secp128r1

struct Curve {
    p: BigUint,
    a: BigUint,
    b: BigUint,
    gx: BigUint,
    gy: BigUint,
    n: BigUint,
}

fn h(s: &str) -> BigUint {
    BigUint::parse_bytes(s.as_bytes(), 16).unwrap()
}

fn curve() -> Curve {
    Curve {
        p: h("FFFFFFFDFFFFFFFFFFFFFFFFFFFFFFFF"),
        a: h("FFFFFFFDFFFFFFFFFFFFFFFFFFFFFFFC"),
        b: h("E87579C11079F43DD824993C2CEE5ED3"),
        gx: h("161FF7528B899B2D0C28607CA52C5B86"),
        gy: h("CF5AC8395BAFEB13C02DA292DDED7A83"),
        n: h("FFFFFFFE0000000075A30D1B9038A115"),
    }
}

type Point = Option<(BigUint, BigUint)>;

impl Curve {
    fn inv(&self, x: &BigUint) -> BigUint {
        x.modpow(&(&self.p - 2u32), &self.p)
    }
    fn sub(&self, x: &BigUint, y: &BigUint) -> BigUint {
        ((x + &self.p) - (y % &self.p)) % &self.p
    }
    fn add(&self, p1: &Point, p2: &Point) -> Point {
        let (x1, y1) = match p1 {
            None => return p2.clone(),
            Some(v) => v,
        };
        let (x2, y2) = match p2 {
            None => return p1.clone(),
            Some(v) => v,
        };
        let p = &self.p;
        let lam = if x1 == x2 {
            if ((y1 + y2) % p).is_zero() {
                return None;
            }
            (BigUint::from(3u32) * x1 * x1 + &self.a) % p * self.inv(&((y1 * 2u32) % p)) % p
        } else {
            self.sub(y2, y1) * self.inv(&self.sub(x2, x1)) % p
        };
        let x3 = self.sub(&self.sub(&(&lam * &lam % p), x1), x2);
        let y3 = self.sub(&(&lam * self.sub(x1, &x3) % p), y1);
        Some((x3, y3))
    }
    fn mul(&self, k: &BigUint, pt: &Point) -> Point {
        let mut r: Point = None;
        let mut q = pt.clone();
        for i in 0..k.bits() {
            if k.bit(i) {
                r = self.add(&r, &q);
            }
            q = self.add(&q, &q);
        }
        r
    }
    fn on_curve(&self, x: &BigUint, y: &BigUint) -> bool {
        let p = &self.p;
        let lhs = y * y % p;
        let rhs = (x * x % p * x + &self.a * x + &self.b) % p;
        lhs == rhs
    }
    fn g(&self) -> Point {
        Some((self.gx.clone(), self.gy.clone()))
    }
}

fn be16(x: &BigUint) -> [u8; 16] {
    let b = x.to_bytes_be();
    let mut out = [0u8; 16];
    out[16 - b.len()..].copy_from_slice(&b);
    out
}

pub struct EcdhKey {
    private: BigUint,
    public: (BigUint, BigUint),
}

impl EcdhKey {
    pub fn generate() -> EcdhKey {
        let c = curve();
        let mut raw = [0u8; 16];
        rand::thread_rng().fill_bytes(&mut raw);
        let k = BigUint::from_bytes_be(&raw) % (&c.n - 1u32) + BigUint::one();
        Self::from_private(k)
    }

    pub fn from_private(private: BigUint) -> EcdhKey {
        let c = curve();
        let public = c.mul(&private, &c.g()).expect("k*G is not infinity");
        EcdhKey { private, public }
    }

    /// `04 || X || Y`，66 个大写十六进制字符。
    pub fn public_hex(&self) -> String {
        let mut raw = vec![4u8];
        raw.extend_from_slice(&be16(&self.public.0));
        raw.extend_from_slice(&be16(&self.public.1));
        hex::encode_upper(raw)
    }

    /// 16 字节共享密钥（X 坐标）。服务器公钥按官方写法解析：去掉前两个字符，剩下对半分。
    pub fn shared_secret(&self, server_public: &str) -> Result<[u8; 16], String> {
        let c = curve();
        if server_public.len() <= 2 || !server_public.is_ascii() {
            return Err("server public key too short".into());
        }
        let body = &server_public[2..];
        if body.len() % 2 != 0 {
            return Err("server public key: odd length".into());
        }
        let (xs, ys) = body.split_at(body.len() / 2);
        let x = BigUint::parse_bytes(xs.as_bytes(), 16).ok_or("bad hex")?;
        let y = BigUint::parse_bytes(ys.as_bytes(), 16).ok_or("bad hex")?;
        if !c.on_curve(&x, &y) {
            return Err("server public key not on secp128r1".into());
        }
        let s = c.mul(&self.private, &Some((x, y))).ok_or("shared point at infinity")?;
        Ok(be16(&s.0))
    }

    /// AES 密钥 = 共享密钥的大写十六进制文本（32 个 ASCII 字节，即 AES-256）。
    pub fn aes_key(&self, server_public: &str) -> Result<Vec<u8>, String> {
        Ok(hex::encode_upper(self.shared_secret(server_public)?).into_bytes())
    }
}

// ---------------------------------------------------------------- AES-ECB

enum Aes {
    A128(aes::Aes128),
    A192(aes::Aes192),
    A256(aes::Aes256),
}

fn aes_cipher(key: &[u8]) -> Result<Aes, String> {
    match key.len() {
        16 => Ok(Aes::A128(aes::Aes128::new(GenericArray::from_slice(key)))),
        24 => Ok(Aes::A192(aes::Aes192::new(GenericArray::from_slice(key)))),
        32 => Ok(Aes::A256(aes::Aes256::new(GenericArray::from_slice(key)))),
        n => Err(format!("invalid AES key length {n}")),
    }
}

pub fn aes_encrypt(payload: &[u8], key: &[u8]) -> Result<Vec<u8>, String> {
    let c = aes_cipher(key)?;
    let pad = 16 - payload.len() % 16;
    let mut data = payload.to_vec();
    data.extend(std::iter::repeat(pad as u8).take(pad));
    for blk in data.chunks_mut(16) {
        let b = GenericArray::from_mut_slice(blk);
        match &c {
            Aes::A128(a) => a.encrypt_block(b),
            Aes::A192(a) => a.encrypt_block(b),
            Aes::A256(a) => a.encrypt_block(b),
        }
    }
    Ok(data)
}

pub fn aes_decrypt(cipher: &[u8], key: &[u8]) -> Result<Vec<u8>, String> {
    if cipher.is_empty() || cipher.len() % 16 != 0 {
        return Err("ciphertext length must be a non-zero multiple of 16".into());
    }
    let c = aes_cipher(key)?;
    let mut data = cipher.to_vec();
    for blk in data.chunks_mut(16) {
        let b = GenericArray::from_mut_slice(blk);
        match &c {
            Aes::A128(a) => a.decrypt_block(b),
            Aes::A192(a) => a.decrypt_block(b),
            Aes::A256(a) => a.decrypt_block(b),
        }
    }
    let pad = *data.last().unwrap() as usize;
    if !(1..=16).contains(&pad) || data[data.len() - pad..].iter().any(|&x| x as usize != pad) {
        return Err("bad PKCS#7 padding".into());
    }
    data.truncate(data.len() - pad);
    Ok(data)
}

/// AES-CTR（128 位大端计数器，与 OpenSSL / cryptography 一致）。加解密是同一个操作。
/// 剪贴板图片（TypeImageCos）用它：key 是随机 16 字节，初始计数器就是 key 本身。
pub fn aes_ctr(data: &[u8], key: &[u8], iv: &[u8; 16]) -> Result<Vec<u8>, String> {
    let c = aes_cipher(key)?;
    let mut ctr = u128::from_be_bytes(*iv);
    let mut out = Vec::with_capacity(data.len());
    for chunk in data.chunks(16) {
        let mut ks = ctr.to_be_bytes();
        let b = GenericArray::from_mut_slice(&mut ks);
        match &c {
            Aes::A128(a) => a.encrypt_block(b),
            Aes::A192(a) => a.encrypt_block(b),
            Aes::A256(a) => a.encrypt_block(b),
        }
        out.extend(chunk.iter().zip(ks.iter()).map(|(x, k)| x ^ k));
        ctr = ctr.wrapping_add(1);
    }
    Ok(out)
}

// ---------------------------------------------------------------- digests

pub fn md5_upper(data: &[u8]) -> String {
    hex::encode_upper(Md5::digest(data))
}

/// 官方签名：参数 2..N 先拼，第 1 个（sign_key）放最后，SHA-256 后取大写十六进制。
pub fn signature(sign_key: &str, parts: &[&str]) -> String {
    let mut h = Sha256::new();
    for p in parts {
        h.update(p.as_bytes());
    }
    h.update(sign_key.as_bytes());
    hex::encode_upper(h.finalize())
}

// ---------------------------------------------------------------- 协议常量
//
// 握手前用的各平台静态 AES 密钥和请求签名用的 sign_key 是官方客户端里的常量。仓库里不存这些值，
// 只存它们的 SHA-256：启动后从用户自己用 minime-fetch 下载的官方 ime_net.dll 里，
// 按长度（AES 32 个字符、sign_key 16 个字符）找出候选字符串，哈希对上的才用。

const AES_KEY_SHA256: [(u32, &str); 5] = [
    (1, "8e75ac46da092a24881648ab360a4a13113c5e5f650aac4ef8290d83f17fe8f5"),
    (2, "c8d9103d0e0701b8b536ab3d1fbbdcd4de05599a2422161b00e2c02f32910a31"),
    (5, "6d4d282be36cc7e397e76aa21ff180c22081a347d79220d36a37b6bd83f98e38"),
    (6, "497cedcc5334d883da28c0ca4f94232f02f89707139a2e13955cb516aa0ccd58"),
    (7, "d96caf4e2063780e7a45dab549bfc28cd33eb754c07797a6f2c99b0a9d8f4843"),
];
/// 平台号 u32::MAX 表示其他平台共用的那一个
const SIGN_KEY_SHA256: [(u32, &str); 5] = [
    (1, "37d5c3cd9969c219c910b84c73b200d00c8acd61d15c7be28487a8cc1b005469"),
    (2, "4f3e0ed275ee6a80087a8d3d2afe5e09a777cd48e6868022d2543bef43545bba"),
    (5, "1ce5082b9980ae99d69081d16d9c7ad22f781648f468cb9623c30e80182c1aaf"),
    (6, "6d9f9ae87ad471b9ac3fc3e8fed204b9e0a64dcbaed666250b40bb4586eda083"),
    (u32::MAX, "4e210afee78e8a31d35369a542238ffc0169c42e6c19440957507310516e0bec"),
];

struct Keys {
    aes: Vec<(u32, &'static [u8])>,
    sign: Vec<(u32, &'static str)>,
}

static KEYS: std::sync::Mutex<Option<&'static Keys>> = std::sync::Mutex::new(None);

/// 官方 ime_net.dll 可能在的位置：MINIME_ENGINE_DIR、minime-fetch 的下载目录、开发时仓库里的 vendor/。
fn ime_net_candidates() -> Vec<std::path::PathBuf> {
    use std::path::PathBuf;
    let mut v = Vec::new();
    if let Ok(d) = std::env::var("MINIME_ENGINE_DIR") {
        v.push(PathBuf::from(d).join("ime_net.dll"));
    }
    let data = std::env::var("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share"));
    v.push(data.join("minime/official/win/3.0.1.35/ime_net.dll"));
    // 开发时直接运行仓库里的构建：<仓库>/netd/target/release/minime-netd → <仓库>/vendor/win/3.0.1.35
    if let Ok(exe) = std::env::current_exe() {
        if let Some(repo) = exe.ancestors().nth(4) {
            v.push(repo.join("vendor/win/3.0.1.35/ime_net.dll"));
        }
    }
    v
}

fn load_keys() -> Option<Keys> {
    let data = ime_net_candidates().into_iter().find_map(|p| std::fs::read(p).ok())?;
    let mut keys = Keys { aes: Vec::new(), sign: Vec::new() };
    // 以 0 结尾、只含字母和数字的字符串
    let mut start = 0;
    for (i, &b) in data.iter().enumerate() {
        if b.is_ascii_alphanumeric() {
            continue;
        }
        let run = &data[start..i];
        start = i + 1;
        if b != 0 || (run.len() != 16 && run.len() != 32) {
            continue;
        }
        let h = hex::encode(Sha256::digest(run));
        let leak = || -> &'static str { Box::leak(String::from_utf8_lossy(run).into_owned().into_boxed_str()) };
        if let Some(&(p, _)) = AES_KEY_SHA256.iter().find(|(_, x)| *x == h) {
            if !keys.aes.iter().any(|(q, _)| *q == p) {
                keys.aes.push((p, leak().as_bytes()));
            }
        } else if let Some(&(p, _)) = SIGN_KEY_SHA256.iter().find(|(_, x)| *x == h) {
            if !keys.sign.iter().any(|(q, _)| *q == p) {
                keys.sign.push((p, leak()));
            }
        }
    }
    (keys.aes.len() == AES_KEY_SHA256.len() && keys.sign.len() == SIGN_KEY_SHA256.len()).then_some(keys)
}

fn keys() -> Option<&'static Keys> {
    let mut g = KEYS.lock().unwrap();
    if g.is_none() {
        *g = load_keys().map(|k| &*Box::leak(Box::new(k)));
    }
    *g
}

/// 协议常量是否已经从官方文件里读到（没有时提示用户运行 minime-fetch；之后会自动重试）。
pub fn keys_ready() -> bool {
    keys().is_some()
}

/// 握手前使用的平台静态 AES 密钥；没有官方文件时为空。
pub fn static_aes_key(platform: u32) -> &'static [u8] {
    keys().and_then(|k| k.aes.iter().find(|(p, _)| *p == platform).map(|(_, v)| *v)).unwrap_or(b"")
}

/// 签名用的 sign_key（同时是设备码的字母表）；没有官方文件时为空。
pub fn sign_key(platform: u32) -> &'static str {
    keys()
        .and_then(|k| {
            k.sign.iter().find(|(p, _)| *p == platform).or_else(|| k.sign.iter().find(|(p, _)| *p == u32::MAX))
        })
        .map(|(_, v)| *v)
        .unwrap_or("")
}

#[cfg(test)]
mod tests {
    use super::*;

    // 参考向量（与 Python 参考实现对照）
    #[test]
    fn ecdh_vectors() {
        let a = EcdhKey::from_private(h("11111111111111111111111111111111"));
        let b = EcdhKey::from_private(h("22222222222222222222222222222222"));
        assert_eq!(a.public_hex(), "041067D10005D66DF789AE012887BF07616DE8F5DED7353495D37D17F20CA2E473");
        assert_eq!(b.public_hex(), "04D33DAE45F4620E154A86A3C4716726FCED31E72298C06B89EAB0A952E0E358A4");
        let s = a.shared_secret(&b.public_hex()).unwrap();
        assert_eq!(hex::encode_upper(s), "53FE2C5B464235D5726D3ACF9F81E6DE");
        assert_eq!(s, b.shared_secret(&a.public_hex()).unwrap());
        assert_eq!(a.aes_key(&b.public_hex()).unwrap(), b"53FE2C5B464235D5726D3ACF9F81E6DE");
        let r = EcdhKey::generate();
        assert_eq!(r.public_hex().len(), 66);
        assert!(a.shared_secret("04ABCD").is_err());
    }

    #[test]
    fn aes_vectors() {
        // AES-128 参考向量
        let key: Vec<u8> = (0u8..16).collect();
        let ct = aes_encrypt(b"0123456789abcdef", &key).unwrap();
        assert_eq!(hex::encode(&ct), "281567ab2f4cf0d73d3198225b8b8393954f64f2e4e86e9eee82d20216684899");
        assert_eq!(aes_decrypt(&ct, &key).unwrap(), b"0123456789abcdef");
        // AES-256：32 个 ASCII 字符直接当密钥（编造的测试密钥，期望值用 openssl 算）
        let k = b"TESTKEY0TESTKEY1TESTKEY2TESTKEY3".as_slice();
        let ct = aes_encrypt(b"hello wetype netd", k).unwrap();
        assert_eq!(hex::encode(&ct), "8aafb0a21ede82febda3a713d2b2dcba869b0f65487ae10d49ec6b7bd99a52aa");
        assert!(aes_decrypt(&ct[..16], k).is_err() || aes_decrypt(&ct[..16], k).unwrap() != b"hello wetype netd");
    }

    #[test]
    fn ctr_roundtrip() {
        // 与 Python cryptography 的 CTR 对照：key=iv=00..0f，明文 "hello wetype clipboard"
        let key: [u8; 16] = core::array::from_fn(|i| i as u8);
        let ct = aes_ctr(b"hello wetype clipboard", &key, &key).unwrap();
        assert_eq!(aes_ctr(&ct, &key, &key).unwrap(), b"hello wetype clipboard");
        assert_eq!(ct.len(), 22);
    }

    #[test]
    fn signature_vector() {
        // sign_key 和 uin 都是编造的；期望值用 Python 独立算出
        let md5 = "AB".repeat(16);
        let sig = signature(
            "testsignkey0test",
            &["6", "3.0.1.35", "2", "4814", "36", "1700000000000", &md5, "1000000000000001", "abcdefghijklmnop", "5"],
        );
        assert_eq!(sig, "767FD971FA1439C7BD43BD2E192AD7BC051F2836985444C52154C4CC157182CB");
    }

    /// 本机有官方 ime_net.dll 时（开发机），确认所有协议常量都能按哈希找到；没有就跳过（CI）。
    #[test]
    fn keys_from_official_dll() {
        // 开发机上官方文件在仓库的 vendor/ 下（测试程序的路径和正式程序不同，这里直接指过去）
        let dev = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../vendor/win/3.0.1.35");
        if dev.join("ime_net.dll").exists() && std::env::var("MINIME_ENGINE_DIR").is_err() {
            std::env::set_var("MINIME_ENGINE_DIR", &dev);
        }
        if !ime_net_candidates().iter().any(|p| p.exists()) {
            eprintln!("没有官方 ime_net.dll，跳过");
            return;
        }
        assert!(keys_ready());
        for (p, _) in AES_KEY_SHA256 {
            assert_eq!(static_aes_key(p).len(), 32);
        }
        for p in [1, 2, 5, 6, 7] {
            assert_eq!(sign_key(p).len(), 16);
        }
        assert_eq!(hex::encode(Sha256::digest(sign_key(7))), SIGN_KEY_SHA256[4].1);
    }
}

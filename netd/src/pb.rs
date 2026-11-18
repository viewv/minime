//! 最小 protobuf 编解码（与 protobuf-lite 线格式兼容，不需要 .proto）。

/// 消息构造器：按字段号追加，顺序即线上顺序。
#[derive(Default, Clone)]
pub struct Pb(pub Vec<u8>);

fn put_varint(out: &mut Vec<u8>, mut v: u64) {
    loop {
        let b = (v & 0x7f) as u8;
        v >>= 7;
        if v == 0 {
            out.push(b);
            return;
        }
        out.push(b | 0x80);
    }
}

impl Pb {
    pub fn new() -> Self {
        Pb(Vec::new())
    }
    pub fn varint(mut self, field: u32, v: u64) -> Self {
        put_varint(&mut self.0, (field as u64) << 3);
        put_varint(&mut self.0, v);
        self
    }
    pub fn bytes(mut self, field: u32, data: &[u8]) -> Self {
        put_varint(&mut self.0, ((field as u64) << 3) | 2);
        put_varint(&mut self.0, data.len() as u64);
        self.0.extend_from_slice(data);
        self
    }
    pub fn str(self, field: u32, s: &str) -> Self {
        self.bytes(field, s.as_bytes())
    }
    pub fn msg(self, field: u32, m: &Pb) -> Self {
        self.bytes(field, &m.0)
    }
    pub fn into_vec(self) -> Vec<u8> {
        self.0
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Val {
    Int(u64),
    Bytes(Vec<u8>),
    Fixed(Vec<u8>),
}

/// 解析后的消息：保留所有字段（含重复字段），按出现顺序。
#[derive(Debug, Default, Clone)]
pub struct Msg {
    pub fields: Vec<(u32, Val)>,
}

#[derive(Debug)]
pub struct PbError(pub &'static str);

impl std::fmt::Display for PbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "protobuf: {}", self.0)
    }
}
impl std::error::Error for PbError {}

fn read_varint(data: &[u8], i: &mut usize) -> Result<u64, PbError> {
    let mut v = 0u64;
    let mut shift = 0;
    loop {
        let b = *data.get(*i).ok_or(PbError("truncated varint"))?;
        *i += 1;
        if shift >= 64 {
            return Err(PbError("varint too long"));
        }
        v |= ((b & 0x7f) as u64) << shift;
        if b & 0x80 == 0 {
            return Ok(v);
        }
        shift += 7;
    }
}

fn take<'a>(data: &'a [u8], i: &mut usize, n: usize) -> Result<&'a [u8], PbError> {
    let end = i.checked_add(n).ok_or(PbError("length overflow"))?;
    let s = data.get(*i..end).ok_or(PbError("truncated field"))?;
    *i = end;
    Ok(s)
}

impl Msg {
    pub fn parse(data: &[u8]) -> Result<Msg, PbError> {
        let mut fields = Vec::new();
        let mut i = 0;
        while i < data.len() {
            let key = read_varint(data, &mut i)?;
            let field = (key >> 3) as u32;
            let val = match key & 7 {
                0 => Val::Int(read_varint(data, &mut i)?),
                2 => {
                    let n = read_varint(data, &mut i)? as usize;
                    Val::Bytes(take(data, &mut i, n)?.to_vec())
                }
                5 => Val::Fixed(take(data, &mut i, 4)?.to_vec()),
                1 => Val::Fixed(take(data, &mut i, 8)?.to_vec()),
                _ => return Err(PbError("unsupported wire type")),
            };
            fields.push((field, val));
        }
        Ok(Msg { fields })
    }

    pub fn int(&self, field: u32) -> u64 {
        self.fields
            .iter()
            .find_map(|(f, v)| match v {
                Val::Int(x) if *f == field => Some(*x),
                _ => None,
            })
            .unwrap_or(0)
    }

    pub fn bytes(&self, field: u32) -> &[u8] {
        self.fields
            .iter()
            .find_map(|(f, v)| match v {
                Val::Bytes(b) if *f == field => Some(b.as_slice()),
                _ => None,
            })
            .unwrap_or(&[])
    }

    pub fn has(&self, field: u32) -> bool {
        self.fields.iter().any(|(f, _)| *f == field)
    }

    pub fn str(&self, field: u32) -> String {
        String::from_utf8_lossy(self.bytes(field)).into_owned()
    }

    /// 嵌套消息（字段缺失时返回空消息，保持 protobuf 语义）。
    pub fn sub(&self, field: u32) -> Result<Msg, PbError> {
        Msg::parse(self.bytes(field))
    }

    /// 重复的 bytes 字段。
    pub fn all_bytes(&self, field: u32) -> impl Iterator<Item = &[u8]> {
        self.fields.iter().filter_map(move |(f, v)| match v {
            Val::Bytes(b) if *f == field => Some(b.as_slice()),
            _ => None,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let inner = Pb::new().varint(1, 7);
        let m = Pb::new()
            .varint(2, 42)
            .str(3, "hi")
            .msg(4, &inner)
            .varint(5, u64::MAX - 4)
            .str(3, "again");
        let p = Msg::parse(&m.0).unwrap();
        assert_eq!(p.int(2), 42);
        assert_eq!(p.str(3), "hi");
        assert_eq!(p.sub(4).unwrap().int(1), 7);
        assert_eq!(p.int(5), u64::MAX - 4);
        assert_eq!(p.all_bytes(3).count(), 2);
        assert_eq!(p.int(99), 0);
    }

    #[test]
    fn truncated() {
        assert!(Msg::parse(&[0x0a, 0x05, 1, 2]).is_err());
        assert!(Msg::parse(&[0x08, 0x80]).is_err());
    }

    #[test]
    fn python_base_req_vector() {
        // crypto_ref.build_base_req(uin=42, f3=0, headers={"vid":"v","platform":"5"}, body=b"\x08\x01")
        let item = |k: &str, v: &str| Pb::new().str(1, k).str(2, v);
        let hdr = Pb::new().msg(1, &item("vid", "v")).msg(1, &item("platform", "5"));
        let req = Pb::new().varint(2, 42).msg(4, &hdr).bytes(5, &[8, 1]);
        assert_eq!(
            hex::encode(&req.0),
            "102a22190a080a037669641201760a0d0a08706c6174666f726d1201352a020801"
        );
    }
}

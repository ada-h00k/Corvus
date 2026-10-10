//! Encrypted chat-history store.
//!
//! File layout (all integers little endian):
//!
//! ```text
//! header (105 bytes)
//!   "SCQH" | version u8 | argon2 m_kib u32 | t u32 | p u32 | salt[16]   <- authenticated (AAD)
//!   wrap_nonce[24] | wrapped_dek[32 + 16 tag]
//! records, appended one after another
//!   len u32 | nonce[24] | ciphertext+tag      (len = 24 + ciphertext.len())
//! ```
//!
//! * A random 32 byte data key (DEK) encrypts all records with XChaCha20-Poly1305.
//! * The DEK is wrapped with a key derived from the passphrase via Argon2id.
//!   Changing the passphrase therefore only rewrites the header.
//! * Every record is authenticated together with its sequence number, so
//!   reordering or removing records in the middle of the file is detected.
//!   (Removing records at the very end cannot be detected by an append-only log.)

use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use argon2::{Algorithm, Argon2, Params, Version};
use chacha20poly1305::aead::rand_core::RngCore;
use chacha20poly1305::aead::{Aead, KeyInit, OsRng, Payload};
use chacha20poly1305::{Key, XChaCha20Poly1305, XNonce};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use zeroize::Zeroizing;

const MAGIC: &[u8; 4] = b"SCQH";
const VERSION: u8 = 1;
const SALT_LEN: usize = 16;
const NONCE_LEN: usize = 24;
const KEY_LEN: usize = 32;
const TAG_LEN: usize = 16;
const AAD_LEN: usize = 4 + 1 + 12 + SALT_LEN; // magic, version, kdf params, salt
const HEADER_LEN: usize = AAD_LEN + NONCE_LEN + KEY_LEN + TAG_LEN;
const MAX_RECORD: usize = 16 * 1024 * 1024;

/// Argon2id cost parameters.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KdfParams {
    pub m_kib: u32,
    pub t: u32,
    pub p: u32,
}

impl Default for KdfParams {
    fn default() -> Self {
        // 64 MiB, 3 passes, 1 lane: well above the OWASP minimum for Argon2id.
        KdfParams { m_kib: 64 * 1024, t: 3, p: 1 }
    }
}

#[derive(Debug)]
pub enum StoreError {
    WrongPassphrase,
    Corrupt(String),
    AlreadyExists,
    Io(io::Error),
    Crypto(String),
}

impl fmt::Display for StoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            StoreError::WrongPassphrase => write!(f, "wrong passphrase"),
            StoreError::Corrupt(m) => write!(f, "history file is corrupt: {m}"),
            StoreError::AlreadyExists => write!(f, "history file already exists"),
            StoreError::Io(e) => write!(f, "I/O error: {e}"),
            StoreError::Crypto(m) => write!(f, "crypto error: {m}"),
        }
    }
}

impl std::error::Error for StoreError {}

impl From<io::Error> for StoreError {
    fn from(e: io::Error) -> Self {
        StoreError::Io(e)
    }
}

/// One logical entry of the history log.
///
/// Messages are identified by `(ts, au)`: timestamp in ms and author key.
/// Records written by older versions (without `au`/`d`) stay readable.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "t")]
pub enum Record {
    /// A chat message. `d` carries optional extras: `q` (quote) and `at` (attachments).
    #[serde(rename = "m")]
    Msg {
        c: String,
        ts: i64,
        s: String,
        x: String,
        o: bool,
        #[serde(default)]
        au: String,
        #[serde(default, skip_serializing_if = "Value::is_null")]
        d: Value,
    },
    /// Display name learnt from an incoming message.
    #[serde(rename = "n")]
    Name { c: String, n: String },
    /// Conversation deleted (only used if a log is not compacted).
    #[serde(rename = "d")]
    Del { c: String },
    /// Merge `d` into the extras of message `(ts, au)` (e.g. a late thumbnail).
    #[serde(rename = "u")]
    Upd { c: String, ts: i64, au: String, d: Value },
    /// Reaction `e` by `by` to message `(ts, au)`; empty `e` removes it.
    #[serde(rename = "r")]
    React { c: String, ts: i64, au: String, by: String, e: String },
    /// Archive state: `v` = archived here, `s` = last state seen from signal-cli.
    #[serde(rename = "a")]
    Archive { c: String, v: bool, s: bool },
}

#[derive(Clone)]
struct Header {
    params: KdfParams,
    salt: [u8; SALT_LEN],
    nonce: [u8; NONCE_LEN],
    wrapped: [u8; KEY_LEN + TAG_LEN],
}

impl Header {
    fn aad(&self) -> [u8; AAD_LEN] {
        let mut a = [0u8; AAD_LEN];
        a[..4].copy_from_slice(MAGIC);
        a[4] = VERSION;
        a[5..9].copy_from_slice(&self.params.m_kib.to_le_bytes());
        a[9..13].copy_from_slice(&self.params.t.to_le_bytes());
        a[13..17].copy_from_slice(&self.params.p.to_le_bytes());
        a[17..].copy_from_slice(&self.salt);
        a
    }

    fn to_bytes(&self) -> Vec<u8> {
        let mut v = Vec::with_capacity(HEADER_LEN);
        v.extend_from_slice(&self.aad());
        v.extend_from_slice(&self.nonce);
        v.extend_from_slice(&self.wrapped);
        v
    }

    fn parse(buf: &[u8]) -> Result<Header, StoreError> {
        if buf.len() < HEADER_LEN || &buf[..4] != MAGIC {
            return Err(StoreError::Corrupt("not a history file".into()));
        }
        if buf[4] != VERSION {
            return Err(StoreError::Corrupt(format!("unsupported version {}", buf[4])));
        }
        let rd = |o: usize| u32::from_le_bytes(buf[o..o + 4].try_into().unwrap());
        let params = KdfParams { m_kib: rd(5), t: rd(9), p: rd(13) };
        // Guard against absurd values in a crafted file.
        if params.m_kib > 1 << 20 || params.t == 0 || params.t > 64 || params.p == 0 || params.p > 16 {
            return Err(StoreError::Corrupt("implausible KDF parameters".into()));
        }
        let mut h = Header {
            params,
            salt: [0; SALT_LEN],
            nonce: [0; NONCE_LEN],
            wrapped: [0; KEY_LEN + TAG_LEN],
        };
        h.salt.copy_from_slice(&buf[17..17 + SALT_LEN]);
        h.nonce.copy_from_slice(&buf[AAD_LEN..AAD_LEN + NONCE_LEN]);
        h.wrapped.copy_from_slice(&buf[AAD_LEN + NONCE_LEN..HEADER_LEN]);
        Ok(h)
    }

    /// Create a fresh header that wraps `dek` under a key derived from `pass`.
    fn wrap_new(pass: &str, dek: &[u8; KEY_LEN], params: KdfParams) -> Result<Header, StoreError> {
        let mut salt = [0u8; SALT_LEN];
        OsRng.fill_bytes(&mut salt);
        let mut nonce = [0u8; NONCE_LEN];
        OsRng.fill_bytes(&mut nonce);
        let mut h = Header { params, salt, nonce, wrapped: [0; KEY_LEN + TAG_LEN] };
        let kek = derive_kek(pass, &h.salt, params)?;
        let cipher = XChaCha20Poly1305::new(Key::from_slice(&kek[..]));
        let aad = h.aad();
        let ct = cipher
            .encrypt(XNonce::from_slice(&h.nonce), Payload { msg: dek, aad: &aad })
            .map_err(|_| StoreError::Crypto("key wrapping failed".into()))?;
        h.wrapped.copy_from_slice(&ct);
        Ok(h)
    }

    fn unwrap_dek(&self, pass: &str) -> Result<Zeroizing<[u8; KEY_LEN]>, StoreError> {
        let kek = derive_kek(pass, &self.salt, self.params)?;
        let cipher = XChaCha20Poly1305::new(Key::from_slice(&kek[..]));
        let aad = self.aad();
        let pt = cipher
            .decrypt(XNonce::from_slice(&self.nonce), Payload { msg: &self.wrapped, aad: &aad })
            .map_err(|_| StoreError::WrongPassphrase)?;
        let pt = Zeroizing::new(pt);
        if pt.len() != KEY_LEN {
            return Err(StoreError::Corrupt("bad key length".into()));
        }
        let mut dek = Zeroizing::new([0u8; KEY_LEN]);
        dek.copy_from_slice(&pt);
        Ok(dek)
    }
}

fn derive_kek(pass: &str, salt: &[u8], p: KdfParams) -> Result<Zeroizing<[u8; KEY_LEN]>, StoreError> {
    let params = Params::new(p.m_kib, p.t, p.p, Some(KEY_LEN)).map_err(|e| StoreError::Crypto(e.to_string()))?;
    let argon = Argon2::new(Algorithm::Argon2id, Version::V0x13, params);
    let mut out = Zeroizing::new([0u8; KEY_LEN]);
    argon
        .hash_password_into(pass.as_bytes(), salt, &mut out[..])
        .map_err(|e| StoreError::Crypto(e.to_string()))?;
    Ok(out)
}

fn record_aad(seq: u64) -> [u8; 16] {
    let mut a = [0u8; 16];
    a[..8].copy_from_slice(b"SCQH-rec");
    a[8..].copy_from_slice(&seq.to_le_bytes());
    a
}

fn encrypt_record(dek: &[u8; KEY_LEN], seq: u64, rec: &Record) -> Result<Vec<u8>, StoreError> {
    let json = Zeroizing::new(serde_json::to_vec(rec).map_err(|e| StoreError::Crypto(e.to_string()))?);
    let mut nonce = [0u8; NONCE_LEN];
    OsRng.fill_bytes(&mut nonce);
    let cipher = XChaCha20Poly1305::new(Key::from_slice(dek));
    let aad = record_aad(seq);
    let ct = cipher
        .encrypt(XNonce::from_slice(&nonce), Payload { msg: &json, aad: &aad })
        .map_err(|_| StoreError::Crypto("encryption failed".into()))?;
    let mut buf = Vec::with_capacity(4 + NONCE_LEN + ct.len());
    buf.extend_from_slice(&((NONCE_LEN + ct.len()) as u32).to_le_bytes());
    buf.extend_from_slice(&nonce);
    buf.extend_from_slice(&ct);
    Ok(buf)
}

fn open_new(path: &Path) -> io::Result<File> {
    let mut o = OpenOptions::new();
    o.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        o.mode(0o600);
    }
    o.open(path)
}

fn open_append(path: &Path) -> io::Result<File> {
    OpenOptions::new().append(true).open(path)
}

/// Write via temp file + rename so a crash never leaves a half-written file.
fn write_atomic(path: &Path, data: &[u8]) -> io::Result<()> {
    let tmp = path.with_extension("tmp");
    {
        let mut f = open_new(&tmp)?;
        f.write_all(data)?;
        f.sync_all()?;
    }
    fs::rename(&tmp, path)
}

pub struct Store {
    path: PathBuf,
    file: File,
    dek: Zeroizing<[u8; KEY_LEN]>,
    header: Header,
    seq: u64,
    len: u64,
}

impl Store {
    pub fn exists(path: &Path) -> bool {
        path.is_file()
    }

    pub fn create(path: &Path, pass: &str, params: KdfParams) -> Result<Store, StoreError> {
        if path.exists() {
            return Err(StoreError::AlreadyExists);
        }
        if let Some(dir) = path.parent() {
            fs::create_dir_all(dir)?;
        }
        let mut dek = Zeroizing::new([0u8; KEY_LEN]);
        OsRng.fill_bytes(&mut dek[..]);
        let header = Header::wrap_new(pass, &dek, params)?;
        let bytes = header.to_bytes();
        write_atomic(path, &bytes)?;
        let file = open_append(path)?;
        Ok(Store { path: path.to_path_buf(), file, dek, header, seq: 0, len: bytes.len() as u64 })
    }

    /// Open and fully decrypt the log. A partially written trailing record
    /// (crash during append) is dropped; any authentication failure is an error.
    pub fn open(path: &Path, pass: &str) -> Result<(Store, Vec<Record>), StoreError> {
        let data = fs::read(path)?;
        let header = Header::parse(&data)?;
        let dek = header.unwrap_dek(pass)?;
        let cipher = XChaCha20Poly1305::new(Key::from_slice(&dek[..]));

        let mut records = Vec::new();
        let mut pos = HEADER_LEN;
        let mut seq = 0u64;
        while pos < data.len() {
            if data.len() - pos < 4 {
                break;
            }
            let len = u32::from_le_bytes(data[pos..pos + 4].try_into().unwrap()) as usize;
            if !(NONCE_LEN + TAG_LEN..=MAX_RECORD).contains(&len) {
                return Err(StoreError::Corrupt(format!("bad length of record {seq}")));
            }
            if data.len() - pos - 4 < len {
                break; // partial tail
            }
            let nonce = &data[pos + 4..pos + 4 + NONCE_LEN];
            let ct = &data[pos + 4 + NONCE_LEN..pos + 4 + len];
            let aad = record_aad(seq);
            let pt = cipher
                .decrypt(XNonce::from_slice(nonce), Payload { msg: ct, aad: &aad })
                .map_err(|_| StoreError::Corrupt(format!("record {seq} failed authentication")))?;
            let pt = Zeroizing::new(pt);
            let rec: Record = serde_json::from_slice(&pt)
                .map_err(|e| StoreError::Corrupt(format!("record {seq}: {e}")))?;
            records.push(rec);
            pos += 4 + len;
            seq += 1;
        }
        if pos < data.len() {
            OpenOptions::new().write(true).open(path)?.set_len(pos as u64)?;
        }
        let file = open_append(path)?;
        let store = Store { path: path.to_path_buf(), file, dek, header, seq, len: pos as u64 };
        Ok((store, records))
    }

    pub fn append(&mut self, rec: &Record) -> Result<(), StoreError> {
        let buf = encrypt_record(&self.dek, self.seq, rec)?;
        if let Err(e) = self.file.write_all(&buf).and_then(|_| self.file.sync_data()) {
            let _ = self.file.set_len(self.len); // never leave a torn record behind
            return Err(e.into());
        }
        self.seq += 1;
        self.len += buf.len() as u64;
        Ok(())
    }

    /// Replace the whole log (used for compaction after deleting data).
    pub fn rewrite(&mut self, records: &[Record]) -> Result<(), StoreError> {
        let mut data = self.header.to_bytes();
        for (i, r) in records.iter().enumerate() {
            data.extend_from_slice(&encrypt_record(&self.dek, i as u64, r)?);
        }
        write_atomic(&self.path, &data)?;
        self.file = open_append(&self.path)?;
        self.seq = records.len() as u64;
        self.len = data.len() as u64;
        Ok(())
    }

    /// Re-wrap the data key under a new passphrase; records stay untouched.
    pub fn change_passphrase(&mut self, old: &str, new: &str, params: KdfParams) -> Result<(), StoreError> {
        let check = self.header.unwrap_dek(old)?;
        if check[..] != self.dek[..] {
            return Err(StoreError::Corrupt("key mismatch".into()));
        }
        let header = Header::wrap_new(new, &self.dek, params)?;
        let current = fs::read(&self.path)?;
        let mut data = header.to_bytes();
        data.extend_from_slice(&current[HEADER_LEN..]);
        write_atomic(&self.path, &data)?;
        self.file = open_append(&self.path)?;
        self.header = header;
        self.len = data.len() as u64;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    const FAST: KdfParams = KdfParams { m_kib: 8, t: 1, p: 1 };
    static N: AtomicU32 = AtomicU32::new(0);

    fn tmp() -> PathBuf {
        let d = std::env::temp_dir().join(format!("sc-store-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
        fs::create_dir_all(&d).unwrap();
        d.join("h.scq")
    }

    fn msg(i: i64) -> Record {
        Record::Msg {
            c: "+491".into(),
            ts: i,
            s: "Alice".into(),
            x: format!("secret text {i}"),
            o: i % 2 == 0,
            au: "+491".into(),
            d: Value::Null,
        }
    }

    #[test]
    fn roundtrip_and_no_plaintext_on_disk() {
        let p = tmp();
        let mut s = Store::create(&p, "pw", FAST).unwrap();
        for i in 0..5 {
            s.append(&msg(i)).unwrap();
        }
        drop(s);
        let raw = fs::read(&p).unwrap();
        assert!(!raw.windows(6).any(|w| w == b"secret"));
        assert!(!raw.windows(5).any(|w| w == b"Alice"));
        let (_, recs) = Store::open(&p, "pw").unwrap();
        assert_eq!(recs, (0..5).map(msg).collect::<Vec<_>>());
    }

    #[test]
    fn wrong_passphrase() {
        let p = tmp();
        Store::create(&p, "right", FAST).unwrap();
        assert!(matches!(Store::open(&p, "wrong"), Err(StoreError::WrongPassphrase)));
    }

    #[test]
    fn create_refuses_to_overwrite() {
        let p = tmp();
        Store::create(&p, "a", FAST).unwrap();
        assert!(matches!(Store::create(&p, "b", FAST), Err(StoreError::AlreadyExists)));
    }

    #[test]
    fn tampering_is_detected() {
        let p = tmp();
        let mut s = Store::create(&p, "pw", FAST).unwrap();
        s.append(&msg(1)).unwrap();
        s.append(&msg(2)).unwrap();
        drop(s);
        let mut raw = fs::read(&p).unwrap();
        let i = HEADER_LEN + 4 + NONCE_LEN + 3;
        raw[i] ^= 1;
        fs::write(&p, &raw).unwrap();
        assert!(matches!(Store::open(&p, "pw"), Err(StoreError::Corrupt(_))));
    }

    #[test]
    fn reordering_is_detected() {
        let p = tmp();
        let mut s = Store::create(&p, "pw", FAST).unwrap();
        s.append(&msg(1)).unwrap();
        s.append(&msg(2)).unwrap();
        drop(s);
        let raw = fs::read(&p).unwrap();
        let body = &raw[HEADER_LEN..];
        let l1 = 4 + u32::from_le_bytes(body[..4].try_into().unwrap()) as usize;
        let mut swapped = raw[..HEADER_LEN].to_vec();
        swapped.extend_from_slice(&body[l1..]);
        swapped.extend_from_slice(&body[..l1]);
        fs::write(&p, &swapped).unwrap();
        assert!(matches!(Store::open(&p, "pw"), Err(StoreError::Corrupt(_))));
    }

    #[test]
    fn torn_tail_is_dropped() {
        let p = tmp();
        let mut s = Store::create(&p, "pw", FAST).unwrap();
        s.append(&msg(1)).unwrap();
        s.append(&msg(2)).unwrap();
        drop(s);
        let raw = fs::read(&p).unwrap();
        fs::write(&p, &raw[..raw.len() - 10]).unwrap();
        let (mut s, recs) = Store::open(&p, "pw").unwrap();
        assert_eq!(recs, vec![msg(1)]);
        s.append(&msg(3)).unwrap(); // log stays usable
        drop(s);
        let (_, recs) = Store::open(&p, "pw").unwrap();
        assert_eq!(recs, vec![msg(1), msg(3)]);
    }

    #[test]
    fn rewrite_compacts() {
        let p = tmp();
        let mut s = Store::create(&p, "pw", FAST).unwrap();
        for i in 0..4 {
            s.append(&msg(i)).unwrap();
        }
        s.rewrite(&[msg(9)]).unwrap();
        s.append(&msg(10)).unwrap();
        drop(s);
        let (_, recs) = Store::open(&p, "pw").unwrap();
        assert_eq!(recs, vec![msg(9), msg(10)]);
    }

    #[test]
    fn change_passphrase_keeps_data() {
        let p = tmp();
        let mut s = Store::create(&p, "old", FAST).unwrap();
        s.append(&msg(1)).unwrap();
        assert!(matches!(s.change_passphrase("nope", "new", FAST), Err(StoreError::WrongPassphrase)));
        s.change_passphrase("old", "new", FAST).unwrap();
        s.append(&msg(2)).unwrap();
        drop(s);
        assert!(matches!(Store::open(&p, "old"), Err(StoreError::WrongPassphrase)));
        let (_, recs) = Store::open(&p, "new").unwrap();
        assert_eq!(recs, vec![msg(1), msg(2)]);
    }

    #[test]
    fn old_records_without_new_fields_still_parse() {
        let old = r#"{"t":"m","c":"+49","ts":5,"s":"A","x":"hi","o":true}"#;
        let r: Record = serde_json::from_str(old).unwrap();
        assert_eq!(
            r,
            Record::Msg { c: "+49".into(), ts: 5, s: "A".into(), x: "hi".into(), o: true, au: String::new(), d: Value::Null }
        );
        // and null extras are not written back
        assert!(!serde_json::to_string(&r).unwrap().contains("\"d\""));
    }

    #[test]
    fn new_record_kinds_roundtrip_through_the_store() {
        let p = tmp();
        let recs = vec![
            Record::Msg {
                c: "+49".into(),
                ts: 10,
                s: "Bob".into(),
                x: "look".into(),
                o: false,
                au: "+49".into(),
                d: serde_json::json!({"q": {"id": 3, "a": "+1", "t": "quoted"}, "at": [{"id": "abc", "ct": "image/png"}]}),
            },
            Record::Upd { c: "+49".into(), ts: 10, au: "+49".into(), d: serde_json::json!({"at": [{"id": "abc", "th": "AAAA"}]}) },
            Record::React { c: "+49".into(), ts: 10, au: "+49".into(), by: "+1".into(), e: "👍".into() },
            Record::Archive { c: "+49".into(), v: true, s: false },
        ];
        let mut s = Store::create(&p, "pw", FAST).unwrap();
        for r in &recs {
            s.append(r).unwrap();
        }
        drop(s);
        let (_, back) = Store::open(&p, "pw").unwrap();
        assert_eq!(back, recs);
    }
}

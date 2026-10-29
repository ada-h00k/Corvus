// Safety numbers and trust, with signal-cli's default policy (trust-new-identities on-first-use):
// the first key seen for someone is TRUSTED_UNVERIFIED; a different key later is UNTRUSTED, and
// nothing is sent to it until the user verifies it (`trust`). presage itself trusts every key, so
// the decision is kept here, in <account dir>/trust.json.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use base64::prelude::*;
use presage::libsignal_service::prelude::Uuid;
use presage::libsignal_service::protocol::{Fingerprint, IdentityKey};
use serde_json::{json, Value};

/// Signal's safety number parameters (version 2: identified by ACI).
const FINGERPRINT_VERSION: u32 = 2;
const FINGERPRINT_ITERATIONS: u32 = 5200;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Level {
    TrustedUnverified,
    TrustedVerified,
    Untrusted,
}

impl Level {
    pub fn as_str(self) -> &'static str {
        match self {
            Level::TrustedUnverified => "TRUSTED_UNVERIFIED",
            Level::TrustedVerified => "TRUSTED_VERIFIED",
            Level::Untrusted => "UNTRUSTED",
        }
    }

    fn parse(s: &str) -> Option<Self> {
        match s {
            "TRUSTED_UNVERIFIED" => Some(Level::TrustedUnverified),
            "TRUSTED_VERIFIED" => Some(Level::TrustedVerified),
            _ => None,
        }
    }
}

/// The key the user accepted for one person, and how.
#[derive(Debug, Clone, PartialEq)]
struct Accepted {
    key: Vec<u8>,
    level: Level,
    added: u64,
}

pub struct TrustStore {
    path: PathBuf,
    accepted: BTreeMap<Uuid, Accepted>,
}

impl TrustStore {
    pub fn load(account_dir: &Path) -> Self {
        let path = account_dir.join("trust.json");
        let mut accepted = BTreeMap::new();
        if let Ok(Value::Object(map)) = std::fs::read(&path).map_err(drop).and_then(|d| serde_json::from_slice(&d).map_err(drop)) {
            for (uuid, entry) in map {
                let (Ok(uuid), Some(key), Some(level)) = (
                    Uuid::parse_str(&uuid),
                    entry["key"].as_str().and_then(|k| BASE64_STANDARD.decode(k).ok()),
                    entry["level"].as_str().and_then(Level::parse),
                ) else {
                    continue;
                };
                accepted.insert(uuid, Accepted { key, level, added: entry["added"].as_u64().unwrap_or(0) });
            }
        }
        Self { path, accepted }
    }

    fn save(&self) {
        let map: serde_json::Map<String, Value> = self
            .accepted
            .iter()
            .map(|(uuid, a)| {
                (uuid.to_string(), json!({"key": BASE64_STANDARD.encode(&a.key), "level": a.level.as_str(), "added": a.added}))
            })
            .collect();
        let tmp = self.path.with_extension("json.tmp");
        let written = std::fs::write(&tmp, Value::Object(map).to_string()).and_then(|()| std::fs::rename(&tmp, &self.path));
        if let Err(e) = written {
            tracing::warn!(%e, "cannot save trust decisions");
        }
    }

    /// The trust level of `key` for `uuid`; a first key is accepted (and remembered) as unverified.
    pub fn level(&mut self, uuid: Uuid, key: &[u8], now: u64) -> (Level, u64) {
        match self.accepted.get(&uuid) {
            Some(a) if a.key == key => (a.level, a.added),
            Some(_) => (Level::Untrusted, now),
            None => {
                self.accepted.insert(uuid, Accepted { key: key.to_vec(), level: Level::TrustedUnverified, added: now });
                self.save();
                (Level::TrustedUnverified, now)
            }
        }
    }

    pub fn accept(&mut self, uuid: Uuid, key: &[u8], level: Level, now: u64) {
        self.accepted.insert(uuid, Accepted { key: key.to_vec(), level, added: now });
        self.save();
    }
}

pub struct SafetyNumber {
    pub display: String,
    pub scannable: String,
}

pub fn safety_number(own_aci: Uuid, own_key: &IdentityKey, remote: Uuid, remote_key: &IdentityKey) -> Option<SafetyNumber> {
    let fp = Fingerprint::new(
        FINGERPRINT_VERSION,
        FINGERPRINT_ITERATIONS,
        own_aci.as_bytes(),
        own_key,
        remote.as_bytes(),
        remote_key,
    )
    .ok()?;
    Some(SafetyNumber {
        display: fp.display_string().ok()?,
        scannable: BASE64_STANDARD.encode(fp.scannable.serialize().ok()?),
    })
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn digits(text: &str) -> String {
    text.chars().filter(char::is_ascii_digit).collect()
}

/// The error signal-cli gives for a send to a key that was not accepted; corvus looks for
/// "untrusted identity" in it.
pub fn untrusted_error(who: &str) -> String {
    format!("Failed to send message: Untrusted Identity for \"{who}\"")
}

#[cfg(test)]
mod tests {
    use super::*;
    use presage::libsignal_service::protocol::IdentityKeyPair;

    fn tmp() -> PathBuf {
        let dir = std::env::temp_dir().join(format!("corvus-signal-trust-{}-{:?}", std::process::id(), std::thread::current().id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn first_use_then_change_then_verify() {
        let dir = tmp();
        let alice = Uuid::from_u128(5);
        let mut store = TrustStore::load(&dir);
        assert_eq!(store.level(alice, b"k1", 10).0, Level::TrustedUnverified);
        assert_eq!(store.level(alice, b"k1", 20), (Level::TrustedUnverified, 10));
        assert_eq!(store.level(alice, b"k2", 30).0, Level::Untrusted);
        // A change is not accepted by looking at it again.
        assert_eq!(store.level(alice, b"k2", 40).0, Level::Untrusted);
        store.accept(alice, b"k2", Level::TrustedVerified, 50);
        // Kept across restarts.
        let mut again = TrustStore::load(&dir);
        assert_eq!(again.level(alice, b"k2", 60), (Level::TrustedVerified, 50));
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn safety_numbers_are_symmetric_60_digits() {
        let mut rng = rand::rng();
        let (a, b) = (IdentityKeyPair::generate(&mut rng), IdentityKeyPair::generate(&mut rng));
        let (ua, ub) = (Uuid::from_u128(1), Uuid::from_u128(2));
        let ours = safety_number(ua, a.identity_key(), ub, b.identity_key()).unwrap();
        let theirs = safety_number(ub, b.identity_key(), ua, a.identity_key()).unwrap();
        assert_eq!(digits(&ours.display).len(), 60);
        assert_eq!(ours.display, theirs.display);
        assert!(untrusted_error("+49").to_lowercase().contains("untrusted identity"));
    }
}

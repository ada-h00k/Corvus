// On-disk layout: <data dir>/accounts/<aci>/{store.db3,attachments/} plus accounts.json (number -> aci).
// The data dir is $CORVUS_SIGNAL_DIR, else $XDG_DATA_HOME/corvus-signal, else ~/.local/share/corvus-signal.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context as _};
use presage::libsignal_service::prelude::phonenumber::{self, PhoneNumber};
use presage::model::identity::OnNewIdentity;
use presage_store_sqlite::SqliteStore;

pub fn data_dir(config: Option<&Path>) -> PathBuf {
    if let Some(dir) = config {
        return dir.to_path_buf();
    }
    if let Some(dir) = std::env::var_os("CORVUS_SIGNAL_DIR") {
        return dir.into();
    }
    let base = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .unwrap_or_else(|| home().join(".local/share"));
    base.join("corvus-signal")
}

fn home() -> PathBuf {
    std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Creates a directory readable only by the user (key material lives below it).
pub fn private_dir(path: &Path) -> anyhow::Result<()> {
    fs::create_dir_all(path).with_context(|| format!("cannot create {}", path.display()))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}

pub fn e164(number: &PhoneNumber) -> String {
    number.format().mode(phonenumber::Mode::E164).to_string()
}

pub struct Accounts {
    dir: PathBuf,
}

impl Accounts {
    pub fn new(dir: PathBuf) -> Self {
        Self { dir }
    }

    fn index_path(&self) -> PathBuf {
        self.dir.join("accounts.json")
    }

    /// number -> account directory name
    pub fn list(&self) -> BTreeMap<String, String> {
        fs::read(self.index_path())
            .ok()
            .and_then(|data| serde_json::from_slice(&data).ok())
            .unwrap_or_default()
    }

    fn save(&self, index: &BTreeMap<String, String>) -> anyhow::Result<()> {
        private_dir(&self.dir)?;
        let tmp = self.dir.join("accounts.json.tmp");
        fs::write(&tmp, serde_json::to_vec_pretty(index)?)?;
        fs::rename(tmp, self.index_path())?;
        Ok(())
    }

    pub fn account_dir(&self, name: &str) -> PathBuf {
        self.dir.join("accounts").join(name)
    }

    /// The directory of a linked account, by phone number or ACI.
    pub fn find(&self, account: &str) -> anyhow::Result<PathBuf> {
        let index = self.list();
        let name = index
            .get(account)
            .cloned()
            .or_else(|| index.values().find(|v| v.as_str() == account).cloned());
        match name {
            Some(name) => Ok(self.account_dir(&name)),
            None => bail!("User {account} is not registered."),
        }
    }

    /// A fresh directory for linking; it is renamed by `register` once linking succeeded.
    pub fn staging_dir(&self) -> anyhow::Result<PathBuf> {
        private_dir(&self.dir)?;
        let accounts = self.dir.join("accounts");
        private_dir(&accounts)?;
        // Leftovers of interrupted links hold keys of devices that were never linked.
        for entry in fs::read_dir(&accounts)?.flatten() {
            if entry.file_name().to_string_lossy().starts_with(".link-") {
                let _ = fs::remove_dir_all(entry.path());
            }
        }
        let dir = accounts.join(format!(".link-{}", std::process::id()));
        private_dir(&dir)?;
        Ok(dir)
    }

    /// Moves a linked staging directory into place and records the account.
    /// Linking the same number again replaces the old (now unusable) device data.
    pub fn register(&self, staging: &Path, number: &str, aci: &str) -> anyhow::Result<()> {
        let target = self.account_dir(aci);
        if target.exists() {
            fs::remove_dir_all(&target)?;
        }
        fs::rename(staging, &target)?;
        let mut index = self.list();
        index.retain(|_, v| v != aci);
        index.insert(number.to_string(), aci.to_string());
        self.save(&index)
    }
}

pub async fn open_store(account_dir: &Path) -> anyhow::Result<SqliteStore> {
    private_dir(account_dir)?;
    let url = format!("sqlite://{}", account_dir.join("store.db3").display());
    // Like signal-cli's default (trust on first use): new identities are accepted and marked unverified.
    SqliteStore::open(&url, OnNewIdentity::Trust)
        .await
        .with_context(|| format!("cannot open store in {}", account_dir.display()))
}

pub fn attachments_dir(account_dir: &Path) -> PathBuf {
    account_dir.join("attachments")
}

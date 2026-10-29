// Account-level commands: block/unblock and accepting message requests (storage service),
// updateProfile and updateAccount (username).

use std::collections::HashMap;

use base64::prelude::*;
use presage::libsignal_service::profile_name::ProfileName;
use presage::libsignal_service::prelude::Uuid;
use presage::manager::ProfileAvatar;
use presage::store::ContentsStore;
use serde_json::{json, Value as Json};
use tracing::warn;

use crate::daemon::{now_ms, Daemon};
use crate::rpc::{Params, RpcError, RpcResult};
use crate::storage::{self, Change, Target, Value};

/// Entropy and server id from "https://signal.me/#eu/<base64url(entropy || handle)>".
fn username_link_parts(link: &str) -> Option<(Vec<u8>, Vec<u8>)> {
    let payload = link.split_once("#eu/")?.1;
    let bytes = BASE64_URL_SAFE_NO_PAD.decode(payload.trim_end_matches('=')).ok()?;
    (bytes.len() == 48).then(|| (bytes[..32].to_vec(), bytes[32..].to_vec()))
}

impl Daemon {
    /// Reads blocked, hidden and archived state from the storage service.
    pub(crate) async fn refresh_storage(&self) {
        let service = match self.manager.storage_service().await {
            Ok(Some(service)) => service,
            Ok(None) => return, // no account entropy pool from the phone: nothing to read
            Err(e) => return warn!(%e, "cannot reach the storage service"),
        };
        let state = match storage::read_state(&service).await {
            Ok(state) => state,
            Err(e) => return warn!(%e, "cannot read the storage service"),
        };
        // Current phones keep the contacts here (a contact sync request gets an empty answer):
        // numbers, names and profile keys come from the records.
        let mut store = self.store().clone();
        for (uuid, person) in &state.people {
            if let Some(key) = person.profile_key {
                let _ = store.upsert_profile_key(uuid, presage::libsignal_service::prelude::ProfileKey::create(key)).await;
            }
        }
        {
            let mut dir = self.dir.borrow_mut();
            for (uuid, person) in &state.people {
                let entry = dir.people.entry(*uuid).or_default();
                if entry.number.is_none() {
                    entry.number = person.number.clone();
                }
                if entry.name.is_none() {
                    entry.name = person.system_name.clone().or_else(|| person.profile_name.clone());
                }
            }
        }
        *self.storage_state.borrow_mut() = state;
    }

    /// Address book name and profile name the storage service has for `uuid`.
    pub(crate) fn storage_names(&self, uuid: Uuid) -> (Option<String>, Option<String>) {
        let state = self.storage_state.borrow();
        state.people.get(&uuid).map(|p| (p.system_name.clone(), p.profile_name.clone())).unwrap_or_default()
    }

    async fn write_storage(&self, changes: &[Change]) -> Result<(), RpcError> {
        let service = self
            .manager
            .storage_service()
            .await?
            .ok_or_else(|| RpcError::failed("No storage service access for this device; link it again"))?;
        let numbers: HashMap<Uuid, String> = self
            .dir
            .borrow()
            .people
            .iter()
            .filter_map(|(uuid, p)| Some((*uuid, p.number.clone()?)))
            .collect();
        let device_id = u32::from(self.manager.device_id());
        storage::write_changes(&service, device_id, changes, &numbers).await?;
        // The phone reads the storage service again when told to.
        let _guard = self.send_lock.lock().await;
        let own = presage::libsignal_service::protocol::Aci::from(self.self_aci);
        if let Err(e) = self.manager.clone().send_message(own, storage::fetch_latest_sync(), now_ms()).await {
            warn!(%e, "cannot tell the other devices about the storage change");
        }
        Ok(())
    }

    /// Targets of block/unblock: "recipient" (people, or groups given by id) and "groupId".
    async fn block_targets(&self, p: &Params) -> Result<Vec<Target>, RpcError> {
        let mut out = Vec::new();
        for who in p.strings("recipient") {
            match self.group_key(&who).await {
                Ok(key) if !who.starts_with('+') => out.push(Target::Group(key)),
                _ => out.push(Target::Contact(self.resolve_aci(&who).await?)),
            }
        }
        for id in p.strings("groupId") {
            out.push(Target::Group(self.group_key(&id).await?));
        }
        if out.is_empty() {
            return Err(RpcError::invalid("no recipient or group given"));
        }
        Ok(out)
    }

    pub(crate) async fn set_blocked(&self, p: &Params, blocked: bool) -> RpcResult {
        let targets = self.block_targets(p).await?;
        let changes: Vec<Change> = targets
            .iter()
            .map(|t| Change {
                target: *t,
                field: match t {
                    Target::Group(_) => storage::GROUP_BLOCKED,
                    _ => storage::CONTACT_BLOCKED,
                },
                value: Value::Bool(blocked),
            })
            .collect();
        self.write_storage(&changes).await?;
        let mut state = self.storage_state.borrow_mut();
        for t in targets {
            match (t, blocked) {
                (Target::Contact(u), true) => drop(state.blocked.insert(u)),
                (Target::Contact(u), false) => drop(state.blocked.remove(&u)),
                (Target::Group(k), true) => drop(state.blocked_groups.insert(k)),
                (Target::Group(k), false) => drop(state.blocked_groups.remove(&k)),
                (Target::Account, _) => {}
            }
        }
        Ok(json!({}))
    }

    /// signal-cli's plain removeContact: the contact is no longer hidden (a message request
    /// accepted, in corvus' terms). Other details are left alone.
    pub(crate) async fn remove_contact(&self, p: &Params) -> RpcResult {
        let uuid = self.resolve_aci(p.required_str("recipient")?).await?;
        if self.storage_state.borrow().hidden.contains(&uuid) {
            self.write_storage(&[Change {
                target: Target::Contact(uuid),
                field: storage::CONTACT_HIDDEN,
                value: Value::Bool(false),
            }])
            .await?;
            self.storage_state.borrow_mut().hidden.remove(&uuid);
        }
        Ok(json!({}))
    }

    pub(crate) fn is_blocked(&self, uuid: Uuid) -> bool {
        self.storage_state.borrow().blocked.contains(&uuid)
    }

    pub(crate) fn is_group_blocked(&self, key: &[u8; 32]) -> bool {
        self.storage_state.borrow().blocked_groups.contains(key)
    }

    pub(crate) async fn update_profile(&self, p: &Params) -> RpcResult {
        let mut m = self.manager.clone();
        // Only the given fields change, like signal-cli: the rest is taken from the current profile.
        let current = m.retrieve_profile().await?;
        let mut name = current.name.unwrap_or(ProfileName { given_name: String::new(), family_name: None });
        if let Some(given) = p.str("givenName") {
            name.given_name = given.to_owned();
            name.family_name = p.str("familyName").filter(|f| !f.is_empty()).map(str::to_owned);
        } else if let Some(family) = p.str("familyName") {
            name.family_name = (!family.is_empty()).then(|| family.to_owned());
        }
        if name.given_name.trim().is_empty() {
            return Err(RpcError::invalid("the profile name must not be empty"));
        }
        let about = p.str("about").map(str::to_owned).or(current.about);
        let emoji = p.str("aboutEmoji").map(str::to_owned).or(current.about_emoji);
        let avatar = if p.bool("removeAvatar") {
            ProfileAvatar::Remove
        } else if let Some(path) = p.str("avatar") {
            ProfileAvatar::Set(tokio::fs::read(path).await.map_err(|e| RpcError::failed(format!("{path}: {e}")))?)
        } else {
            ProfileAvatar::Keep
        };
        let _guard = self.send_lock.lock().await;
        m.set_profile(name, about, emoji, avatar).await?;
        Ok(json!({}))
    }

    pub(crate) async fn update_account(&self, p: &Params) -> RpcResult {
        let Some(nickname) = p.str("username") else {
            return Ok(json!({}));
        };
        let (username, link) = {
            let _guard = self.send_lock.lock().await;
            self.manager.clone().set_username(nickname).await?
        };
        let link = link.to_string();
        // Signal's apps keep the username in the account record too; the phone checks it.
        if let Some((entropy, server_id)) = username_link_parts(&link) {
            let changes = [
                Change { target: Target::Account, field: storage::ACCOUNT_USERNAME, value: Value::Bytes(username.clone().into_bytes()) },
                Change {
                    target: Target::Account,
                    field: storage::ACCOUNT_USERNAME_LINK,
                    value: Value::Bytes(storage::username_link_message(&entropy, &server_id)),
                },
            ];
            if let Err(e) = self.write_storage(&changes).await {
                warn!(error = e.message, "username set, but not noted in the storage service");
            }
        }
        self.storage_state.borrow_mut().username = Some(username.clone());
        Ok(json!({"username": username, "usernameLink": link}))
    }

    pub(crate) fn contact_flags(&self, uuid: Uuid) -> Json {
        let state = self.storage_state.borrow();
        json!({
            "isBlocked": state.blocked.contains(&uuid),
            "isHidden": state.hidden.contains(&uuid),
            "isArchived": state.archived.contains(&uuid),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn username_links() {
        let payload = BASE64_URL_SAFE_NO_PAD.encode([[7u8; 32].as_slice(), [9u8; 16].as_slice()].concat());
        let (entropy, server_id) = username_link_parts(&format!("https://signal.me/#eu/{payload}")).unwrap();
        assert_eq!(entropy, vec![7; 32]);
        assert_eq!(server_id, vec![9; 16]);
        assert!(username_link_parts("https://signal.me/#eu/AAAA").is_none());
        assert!(username_link_parts("https://example.org").is_none());
    }
}

// The jsonRpc daemon: a receive loop that emits signal-cli "receive" notifications, and the
// methods corvus calls. Everything runs on one thread (presage's store futures are !Send).

use std::cell::RefCell;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use base64::prelude::*;
use futures::{pin_mut, StreamExt};
use presage::libsignal_service::content::{Content, ContentBody};
use presage::libsignal_service::prelude::{ProfileKey, Uuid};
use presage::libsignal_service::protocol::{
    Aci, DeviceId, IdentityKey, IdentityKeyStore, Pni, ProtocolAddress, ServiceId,
};
use presage::libsignal_service::sender::AttachmentSpec;
use presage::manager::Registered;
use presage::model::groups::Group;
use presage::model::messages::Received;
use presage::proto::data_message::{quote, Delete, Flags, Quote, Reaction};
use presage::proto::receipt_message::Type as ReceiptType;
use presage::proto::sync_message::Content as SyncContent;
use presage::proto::typing_message::Action;
use presage::proto::{
    AttachmentPointer, DataMessage, EditMessage, GroupContextV2, Preview, ReceiptMessage, TypingMessage,
};
use presage::store::{ContentsStore, Store};
use presage::Manager;
use presage_store_sqlite::SqliteStore;
use serde_json::{json, Value};
use tokio::sync::{mpsc, Mutex};
use tracing::{debug, info, warn};

use crate::envelope::{self, Directory, Person};
use crate::groupops::ExtrasCache;
use crate::groups;
use crate::identities::{self, Level, TrustStore};
use crate::rpc::{self, Params, RpcError, RpcResult};
use crate::store::{attachments_dir, e164, private_dir};
use crate::textstyle;

type Mgr = Manager<SqliteStore, Registered>;

pub struct Daemon {
    pub(crate) manager: Mgr,
    out: mpsc::UnboundedSender<Value>,
    attachments: PathBuf,
    pub(crate) account: String,
    pub(crate) self_aci: Uuid,
    pub(crate) dir: RefCell<Directory>,
    /// group id (signal-cli base64) -> master key
    group_keys: RefCell<HashMap<String, [u8; 32]>>,
    trust: RefCell<TrustStore>,
    pub(crate) group_extras: RefCell<ExtrasCache>,
    pub(crate) storage_state: RefCell<crate::storage::State>,
    /// Sends are serialised: concurrent sends to one recipient would race on its session state.
    pub(crate) send_lock: Mutex<()>,
}

pub(crate) enum Target {
    Contact(ServiceId),
    Group([u8; 32]),
}

pub(crate) fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

impl Daemon {
    pub async fn new(manager: Mgr, account_dir: &Path, out: mpsc::UnboundedSender<Value>) -> anyhow::Result<Rc<Self>> {
        // corvus decides about link previews itself and passes them with `send` (previewUrl …);
        // presage would otherwise fetch every linked page, revealing the user's IP address to it.
        manager.set_automatic_link_previews(false);
        let data = manager.registration_data();
        let account = e164(&data.phone_number);
        let self_aci = Uuid::from(data.service_ids.aci());
        let attachments = attachments_dir(account_dir);
        private_dir(&attachments)?;
        let daemon = Rc::new(Self {
            manager,
            out,
            trust: RefCell::new(TrustStore::load(account_dir)),
            group_extras: Default::default(),
            storage_state: Default::default(),
            attachments,
            account,
            self_aci,
            dir: Default::default(),
            group_keys: Default::default(),
            send_lock: Mutex::new(()),
        });
        daemon.reload_directory().await;
        daemon.reload_groups().await;
        Ok(daemon)
    }

    pub(crate) fn store(&self) -> &SqliteStore {
        self.manager.store()
    }

    // ---- people and groups ----------------------------------------------------------------

    pub(crate) async fn profile_name(&self, uuid: Uuid) -> Option<String> {
        let key = self.store().profile_key(&ServiceId::Aci(uuid.into())).await.ok()??;
        let profile = self.store().profile(uuid, key).await.ok()??;
        let name = profile.name?;
        let full = match name.family_name {
            Some(family) if !family.is_empty() => format!("{} {}", name.given_name, family),
            _ => name.given_name,
        };
        (!full.trim().is_empty()).then_some(full)
    }

    /// Refreshes the people from the contacts; people known otherwise (who wrote, group members)
    /// are kept.
    pub(crate) async fn reload_directory(&self) {
        let mut people = self.dir.borrow().people.clone();
        match self.store().contacts().await {
            Ok(contacts) => {
                for contact in contacts.flatten() {
                    people.insert(
                        contact.uuid,
                        Person {
                            number: contact.phone_number.as_ref().map(e164),
                            name: (!contact.name.is_empty()).then(|| contact.name.clone()),
                        },
                    );
                }
            }
            Err(e) => warn!(%e, "cannot read contacts"),
        }
        for (uuid, person) in people.iter_mut() {
            if person.name.is_none() {
                person.name = self.profile_name(*uuid).await;
            }
        }
        // Group members (and others with a fetched profile) get their profile names too.
        if let Ok(groups) = self.groups().await {
            for (_, group) in groups {
                for member in group.members {
                    let uuid = Uuid::from(member.aci);
                    if !people.contains_key(&uuid) {
                        let name = self.profile_name(uuid).await;
                        people.insert(uuid, Person { number: None, name });
                    }
                }
            }
        }
        people.insert(self.self_aci, Person { number: Some(self.account.clone()), name: None });
        self.dir.borrow_mut().people = people;
    }

    /// Makes sure a sender we have not seen before gets a name (profile fetched over the network once).
    async fn ensure_person(&self, uuid: Uuid) {
        if self.dir.borrow().people.contains_key(&uuid) {
            return;
        }
        let mut name = self.profile_name(uuid).await;
        if name.is_none() {
            if let Ok(Some(key)) = self.store().profile_key(&ServiceId::Aci(uuid.into())).await {
                let mut m = self.manager.clone();
                if let Ok(profile) = m.retrieve_profile_by_uuid(uuid, key).await {
                    name = profile.name.map(|n| n.given_name);
                }
            }
        }
        self.dir.borrow_mut().people.insert(uuid, Person { number: None, name });
    }

    pub(crate) async fn groups(&self) -> Result<Vec<([u8; 32], Group)>, RpcError> {
        Ok(self.store().groups().await?.flatten().collect())
    }

    pub(crate) async fn reload_groups(&self) {
        let Ok(groups) = self.groups().await else { return };
        let mut keys = self.group_keys.borrow_mut();
        keys.clear();
        for (key, _) in groups {
            if let Some(id) = envelope::group_id(&key) {
                keys.insert(id, key);
            }
        }
    }

    pub(crate) async fn group_key(&self, id: &str) -> Result<[u8; 32], RpcError> {
        if let Some(key) = self.group_keys.borrow().get(id) {
            return Ok(*key);
        }
        self.reload_groups().await;
        self.group_keys.borrow().get(id).copied().ok_or_else(|| RpcError::failed(format!("Unknown group {id}")))
    }

    pub(crate) async fn group_context(&self, key: [u8; 32]) -> GroupContextV2 {
        let revision = self.store().group(key).await.ok().flatten().map(|g| g.revision);
        GroupContextV2 { master_key: Some(key.to_vec()), revision: Some(revision.unwrap_or(0)), group_change: None }
    }

    /// A known phone number, an ACI uuid, or "PNI:<uuid>"; nothing is looked up.
    pub(crate) fn resolve_local(&self, who: &str) -> Result<ServiceId, RpcError> {
        if who.starts_with('+') {
            let dir = self.dir.borrow();
            return dir
                .people
                .iter()
                .find(|(_, p)| p.number.as_deref() == Some(who))
                .map(|(uuid, _)| ServiceId::Aci(Aci::from(*uuid)))
                .ok_or_else(|| RpcError::failed(format!("Unknown recipient {who}")));
        }
        if let Some(pni) = who.strip_prefix("PNI:") {
            let uuid = Uuid::parse_str(pni).map_err(|_| RpcError::invalid(format!("invalid recipient '{who}'")))?;
            return Ok(ServiceId::Pni(Pni::from(uuid)));
        }
        let uuid = Uuid::parse_str(who).map_err(|_| RpcError::invalid(format!("invalid recipient '{who}'")))?;
        Ok(ServiceId::Aci(Aci::from(uuid)))
    }

    /// Like signal-cli: also a phone number nobody synced (contact discovery) or a username
    /// ("name.42"). What is found is remembered.
    pub(crate) async fn resolve(&self, who: &str) -> Result<ServiceId, RpcError> {
        if let Ok(sid) = self.resolve_local(who) {
            return Ok(sid);
        }
        let mut m = self.manager.clone();
        if who.starts_with('+') {
            let found = m.discover_contacts_by_phone_number([who]).await?;
            let Some(sid) = found.into_iter().find_map(|(_, sid)| sid) else {
                return Err(RpcError::failed(format!("Unregistered user \"{who}\"")));
            };
            self.dir.borrow_mut().people.entry(sid.raw_uuid()).or_default().number = Some(who.to_owned());
            return Ok(sid);
        }
        if who.contains('.') {
            let Some(aci) = m.lookup_username(who).await? else {
                return Err(RpcError::failed(format!("Username {who} not found")));
            };
            self.dir.borrow_mut().people.entry(Uuid::from(aci)).or_default();
            return Ok(ServiceId::Aci(aci));
        }
        Err(RpcError::invalid(format!("invalid recipient '{who}'")))
    }

    pub(crate) async fn resolve_aci(&self, who: &str) -> Result<Uuid, RpcError> {
        match self.resolve(who).await? {
            ServiceId::Aci(aci) => Ok(aci.into()),
            ServiceId::Pni(_) => Err(RpcError::invalid(format!("'{who}' is not an ACI"))),
        }
    }

    /// signal-cli's targets: "recipient"/"recipients" and "groupId"/"groupIds".
    pub(crate) async fn targets(&self, p: &Params) -> Result<Vec<Target>, RpcError> {
        let mut out = Vec::new();
        for who in p.strings("recipient").into_iter().chain(p.strings("recipients")) {
            out.push(Target::Contact(self.resolve(&who).await?));
        }
        for id in p.strings("groupId").into_iter().chain(p.strings("groupIds")) {
            out.push(Target::Group(self.group_key(&id).await?));
        }
        if out.is_empty() {
            return Err(RpcError::invalid("no recipient or group given"));
        }
        Ok(out)
    }

    pub(crate) fn address_json(&self, sid: &ServiceId) -> Value {
        let uuid = sid.raw_uuid();
        json!({"uuid": uuid.to_string(), "number": self.dir.borrow().get(&uuid).number})
    }

    /// Sends one body to every target. Group messages get the group context set here.
    pub(crate) async fn send_to(&self, targets: &[Target], body: ContentBody, timestamp: u64) -> RpcResult {
        let _guard = self.send_lock.lock().await;
        let mut results = Vec::new();
        for target in targets {
            let mut m = self.manager.clone();
            match target {
                Target::Contact(sid) => {
                    // Like signal-cli: nothing goes to a changed key the user has not accepted.
                    if self.trust_level(sid.raw_uuid()).await.map(|t| t.1) == Some(Level::Untrusted) {
                        let who = self.dir.borrow().get(&sid.raw_uuid()).number.unwrap_or_else(|| sid.raw_uuid().to_string());
                        return Err(RpcError::failed(identities::untrusted_error(&who)));
                    }
                    m.send_message(*sid, body.clone(), timestamp).await?;
                    results.push(json!({"recipientAddress": self.address_json(sid), "type": "SUCCESS"}));
                }
                Target::Group(key) => {
                    let mut body = body.clone();
                    let ctx = self.group_context(*key).await;
                    match &mut body {
                        ContentBody::DataMessage(dm) => dm.group_v2 = Some(ctx),
                        ContentBody::EditMessage(EditMessage { data_message: Some(dm), .. }) => dm.group_v2 = Some(ctx),
                        _ => {}
                    }
                    // Members with an unaccepted key are left out and reported, as signal-cli does.
                    let members: Vec<Aci> = match self.store().group(*key).await? {
                        Some(group) => group.members.iter().map(|m| m.aci).filter(|a| Uuid::from(*a) != self.self_aci).collect(),
                        None => Vec::new(),
                    };
                    let mut untrusted = Vec::new();
                    for aci in &members {
                        if self.trust_level(Uuid::from(*aci)).await.map(|t| t.1) == Some(Level::Untrusted) {
                            untrusted.push(*aci);
                        }
                    }
                    m.send_message_to_group_except(key, body, timestamp, &untrusted).await?;
                    for aci in members {
                        let sid = ServiceId::Aci(aci);
                        let kind = if untrusted.contains(&aci) { "IDENTITY_FAILURE" } else { "SUCCESS" };
                        results.push(json!({"recipientAddress": self.address_json(&sid), "type": kind}));
                    }
                }
            }
        }
        Ok(json!({"timestamp": timestamp, "results": results}))
    }

    // ---- identities ------------------------------------------------------------------------

    async fn identity_key(&self, uuid: Uuid) -> Option<IdentityKey> {
        let address = ProtocolAddress::new(uuid.to_string(), DeviceId::new(1).ok()?);
        self.store().aci_protocol_store().get_identity(&address).await.ok().flatten()
    }

    /// The current key of `uuid` with its trust level and since when (None: no session yet).
    async fn trust_level(&self, uuid: Uuid) -> Option<(IdentityKey, Level, u64)> {
        let key = self.identity_key(uuid).await?;
        let (level, added) = self.trust.borrow_mut().level(uuid, &key.serialize(), now_ms());
        Some((key, level, added))
    }

    async fn identity_json(&self, uuid: Uuid) -> Option<Value> {
        let (key, level, added) = self.trust_level(uuid).await?;
        let own = self.store().aci_protocol_store().get_identity_key_pair().await.ok()?;
        let number = identities::safety_number(self.self_aci, own.identity_key(), uuid, &key)?;
        Some(json!({
            "number": self.dir.borrow().get(&uuid).number,
            "uuid": uuid.to_string(),
            "fingerprint": identities::hex(&key.serialize()),
            "safetyNumber": number.display,
            "scannableSafetyNumber": number.scannable,
            "trustLevel": level.as_str(),
            "addedTimestamp": added,
        }))
    }

    async fn list_identities(&self, p: &Params) -> RpcResult {
        let uuids: Vec<Uuid> = match p.str("number") {
            Some(who) => vec![self.resolve(who).await?.raw_uuid()],
            None => {
                let mut all: Vec<Uuid> = self.dir.borrow().people.keys().copied().collect();
                for (_, group) in self.groups().await? {
                    all.extend(group.members.iter().map(|m| Uuid::from(m.aci)));
                }
                all.sort();
                all.dedup();
                all
            }
        };
        let mut out = Vec::new();
        for uuid in uuids.into_iter().filter(|u| *u != self.self_aci) {
            if let Some(identity) = self.identity_json(uuid).await {
                out.push(identity);
            }
        }
        Ok(Value::Array(out))
    }

    async fn trust(&self, p: &Params) -> RpcResult {
        let uuid = self.resolve_aci(p.required_str("recipient")?).await?;
        let Some((key, _, _)) = self.trust_level(uuid).await else {
            return Err(RpcError::failed("No identity known for this recipient"));
        };
        if p.bool("trustAllKnownKeys") {
            self.trust.borrow_mut().accept(uuid, &key.serialize(), Level::TrustedUnverified, now_ms());
            return Ok(json!({}));
        }
        let given = identities::digits(p.required_str("verifiedSafetyNumber")?);
        let own = self.store().aci_protocol_store().get_identity_key_pair().await?;
        let actual = identities::safety_number(self.self_aci, own.identity_key(), uuid, &key)
            .map(|n| identities::digits(&n.display))
            .unwrap_or_default();
        if given.is_empty() || given != actual {
            return Err(RpcError::failed("Failed to verify safety number"));
        }
        self.trust.borrow_mut().accept(uuid, &key.serialize(), Level::TrustedVerified, now_ms());
        Ok(json!({}))
    }

    // ---- receiving -------------------------------------------------------------------------

    pub async fn receive_loop(self: Rc<Self>) {
        let mut delay = Duration::from_secs(1);
        let mut requested_contacts = false;
        loop {
            let mut m = self.manager.clone();
            match m.receive_messages().await {
                Ok(messages) => {
                    pin_mut!(messages);
                    while let Some(item) = messages.next().await {
                        delay = Duration::from_secs(1);
                        match item {
                            Received::QueueEmpty => {
                                debug!("message queue drained");
                                let no_contacts = self.store().contacts().await.map(|c| c.count() == 0).unwrap_or(false);
                                if !requested_contacts && no_contacts {
                                    // Fresh link: ask the primary device for its contacts.
                                    requested_contacts = true;
                                    if let Err(e) = self.manager.clone().request_contacts().await {
                                        warn!(%e, "cannot request contacts");
                                    }
                                }
                            }
                            Received::Contacts => {
                                info!("contacts synchronised");
                                self.reload_directory().await;
                                self.refresh_storage().await;
                            }
                            Received::DecryptionError(sender) => {
                                warn!(sender = %sender.service_id_string(), "cannot decrypt a message");
                            }
                            Received::Content(content) => {
                                if self.handle_content(&content).await {
                                    self.clone().acknowledge(&content);
                                }
                            }
                        }
                    }
                    warn!("message stream ended, reconnecting");
                }
                Err(e) => warn!(%e, "cannot open message stream"),
            }
            tokio::time::sleep(delay).await;
            delay = (delay * 2).min(Duration::from_secs(60));
        }
    }

    /// Announces a received message; false if it was ignored (blocked sender, control message).
    async fn handle_content(&self, content: &Content) -> bool {
        let sender = content.metadata.sender.raw_uuid();
        if sender == self.self_aci {
            // The phone changed the storage service (blocked someone, archived a chat, …).
            if let ContentBody::SynchronizeMessage(sync) = &content.body {
                if matches!(sync.content, Some(SyncContent::FetchLatest(_))) {
                    self.refresh_storage().await;
                    return false;
                }
            }
        } else if self.is_blocked(sender) || self.group_of(&content.body).is_some_and(|k| self.is_group_blocked(&k)) {
            // Like signal-cli: messages from blocked people and groups are ignored.
            debug!("ignoring a message from a blocked contact or group");
            return false;
        }
        self.ensure_person(sender).await;
        if sender != self.self_aci {
            self.trust_level(sender).await; // remembers a first key (trust on first use)
        }
        let mut pointers = Vec::new();
        match &content.body {
            ContentBody::DataMessage(dm) => {
                collect_pointers(dm, &mut pointers);
                if dm.group_v2.is_some() {
                    self.reload_groups().await;
                }
            }
            ContentBody::EditMessage(EditMessage { data_message: Some(dm), .. }) => collect_pointers(dm, &mut pointers),
            ContentBody::SynchronizeMessage(sync) => {
                if let Some(SyncContent::Sent(sent)) = &sync.content {
                    if let Some(dm) = &sent.message {
                        collect_pointers(dm, &mut pointers);
                    }
                    if let Some(EditMessage { data_message: Some(dm), .. }) = &sent.edit_message {
                        collect_pointers(dm, &mut pointers);
                    }
                }
            }
            _ => {}
        }
        // Like signal-cli: attachments are downloaded before the message is announced.
        for pointer in pointers {
            self.download(&pointer, None).await;
        }
        for (pointer, name) in sticker_pointers(&content.body) {
            self.download(&pointer, Some(name)).await;
        }
        let envelope = envelope::to_envelope(&self.dir.borrow(), content);
        if let Some(envelope) = envelope {
            self.emit(rpc::notification("receive", json!({"envelope": envelope, "account": self.account})));
        }
        true
    }

    /// Like signal-cli: a delivery receipt for every message someone sent us, in the background.
    fn acknowledge(self: Rc<Self>, content: &Content) {
        let sender = content.metadata.sender;
        if sender.raw_uuid() == self.self_aci {
            return;
        }
        let timestamp = match &content.body {
            ContentBody::DataMessage(dm) => dm.timestamp,
            ContentBody::EditMessage(EditMessage { data_message: Some(dm), .. }) => dm.timestamp,
            _ => None,
        }
        .unwrap_or_else(|| content.metadata.client_timestamp.timestamp_millis() as u64);
        tokio::task::spawn_local(async move {
            let receipt = ReceiptMessage { r#type: Some(ReceiptType::Delivery as i32), timestamp: vec![timestamp] };
            if let Err(e) = self.send_to(&[Target::Contact(sender)], ContentBody::ReceiptMessage(receipt), now_ms()).await {
                debug!(error = e.message, "cannot send a delivery receipt");
            }
        });
    }

    /// The master key of the group a message belongs to.
    fn group_of(&self, body: &ContentBody) -> Option<[u8; 32]> {
        let ctx = match body {
            ContentBody::DataMessage(dm) => dm.group_v2.as_ref(),
            ContentBody::EditMessage(EditMessage { data_message: Some(dm), .. }) => dm.group_v2.as_ref(),
            ContentBody::TypingMessage(t) => {
                let id = BASE64_STANDARD.encode(t.group_id.as_deref()?);
                return self.group_keys.borrow().get(&id).copied();
            }
            _ => None,
        };
        ctx?.master_key.as_deref()?.try_into().ok()
    }

    pub(crate) fn attachments_path(&self, name: &str) -> PathBuf {
        self.attachments.join(name)
    }

    /// Saves an attachment under its id, or under `name` (received stickers).
    async fn download(&self, pointer: &AttachmentPointer, name: Option<String>) {
        let Some(id) = name.or_else(|| envelope::attachment_id(pointer)) else { return };
        let path = self.attachments.join(&id);
        if path.exists() {
            return;
        }
        match self.manager.get_attachment(pointer).await {
            Ok(data) => {
                if let Err(e) = write_private(&path, &data).await {
                    warn!(%e, id, "cannot save attachment");
                }
            }
            Err(e) => warn!(%e, id, "cannot download attachment"),
        }
    }

    fn emit(&self, value: Value) {
        let _ = self.out.send(value);
    }

    // ---- methods ---------------------------------------------------------------------------

    pub async fn call(&self, method: &str, p: &Params) -> RpcResult {
        match method {
            "version" => Ok(json!({"version": crate::VERSION})),
            "listContacts" => self.list_contacts(p).await,
            "listGroups" => self.list_groups(p).await,
            "send" => self.send(p).await,
            "sendReaction" => self.send_reaction(p).await,
            "remoteDelete" => self.remote_delete(p).await,
            "sendReceipt" => self.send_receipt(p).await,
            "sendTyping" => self.send_typing(p).await,
            "getAttachment" => self.get_attachment(p).await,
            "getAvatar" => self.get_avatar(p).await,
            "updateContact" => self.update_contact(p).await,
            "listIdentities" => self.list_identities(p).await,
            "updateGroup" => self.update_group(p).await,
            "quitGroup" => self.quit_group(p).await,
            "joinGroup" => self.join_group(p).await,
            "listStickerPacks" => self.list_sticker_packs().await,
            "addStickerPack" => self.add_sticker_pack(p).await,
            "getSticker" => self.get_sticker(p).await,
            "block" => self.set_blocked(p, true).await,
            "unblock" => self.set_blocked(p, false).await,
            "removeContact" => self.remove_contact(p).await,
            "updateProfile" => self.update_profile(p).await,
            "updateAccount" => self.update_account(p).await,
            "trust" => self.trust(p).await,
            other => Err(RpcError::unsupported(other)),
        }
    }

    async fn list_contacts(&self, p: &Params) -> RpcResult {
        let wanted: Vec<Uuid> = p
            .strings("recipient")
            .into_iter()
            .chain(p.strings("recipients"))
            .filter_map(|w| self.resolve_local(&w).ok().map(|s| s.raw_uuid()))
            .collect();
        let mut out = Vec::new();
        if wanted.contains(&self.self_aci) {
            let profile = self.manager.clone().retrieve_profile().await.ok();
            let (given, family) = profile.and_then(|p| p.name).map(|n| (Some(n.given_name), n.family_name)).unwrap_or_default();
            out.push(json!({
                "number": self.account, "uuid": self.self_aci.to_string(), "name": "",
                "username": self.storage_state.borrow().username,
                "profile": {"givenName": given, "familyName": family},
                "givenName": given, "familyName": family,
                "isBlocked": false, "isHidden": false, "isArchived": false, "messageExpirationTime": 0,
            }));
        }
        let mut listed = std::collections::HashSet::new();
        for contact in self.store().contacts().await?.flatten() {
            if contact.uuid == self.self_aci || (!wanted.is_empty() && !wanted.contains(&contact.uuid)) {
                continue;
            }
            listed.insert(contact.uuid);
            let profile_name = self.profile_name(contact.uuid).await;
            let mut entry = json!({
                "number": contact.phone_number.as_ref().map(e164),
                "uuid": contact.uuid.to_string(),
                "name": contact.name,
                "profileName": profile_name,
                "messageExpirationTime": contact.expire_timer,
            });
            if let (Value::Object(entry), Value::Object(flags)) = (&mut entry, self.contact_flags(contact.uuid)) {
                entry.extend(flags);
            }
            out.push(entry);
        }
        // Like signal-cli, everyone known is listed: also group members and people who wrote,
        // with the profile name where the profile is known.
        let mut others: Vec<Uuid> = self.dir.borrow().people.keys().copied().collect();
        others.extend(self.storage_state.borrow().people.keys().copied());
        for (_, group) in self.groups().await? {
            others.extend(group.members.iter().map(|m| Uuid::from(m.aci)));
        }
        others.sort();
        others.dedup();
        for uuid in others {
            if uuid == self.self_aci || listed.contains(&uuid) || (!wanted.is_empty() && !wanted.contains(&uuid)) {
                continue;
            }
            let (system_name, stored_profile_name) = self.storage_names(uuid);
            let profile_name = self.profile_name(uuid).await.or(stored_profile_name);
            let mut entry = json!({
                "number": self.dir.borrow().get(&uuid).number,
                "uuid": uuid.to_string(),
                "name": system_name.unwrap_or_default(),
                "profileName": profile_name,
                "messageExpirationTime": 0,
            });
            if let (Value::Object(entry), Value::Object(flags)) = (&mut entry, self.contact_flags(uuid)) {
                entry.extend(flags);
            }
            out.push(entry);
        }
        Ok(Value::Array(out))
    }

    /// Fetches the profiles of group members not known yet, with the profile keys the groups
    /// carry, so their names are there (in the background: one request per person).
    pub async fn prefetch_profiles(&self) {
        let Ok(groups) = self.groups().await else { return };
        let mut seen = std::collections::HashSet::new();
        for (_, group) in groups {
            for member in group.members {
                let uuid = Uuid::from(member.aci);
                if uuid == self.self_aci || !seen.insert(uuid) || self.profile_name(uuid).await.is_some() {
                    continue;
                }
                let mut store = self.store().clone();
                let _ = store.upsert_profile_key(&uuid, member.profile_key).await;
                if let Err(e) = self.manager.clone().retrieve_profile_by_uuid(uuid, member.profile_key).await {
                    debug!(%e, "cannot fetch a group member's profile");
                }
            }
        }
        self.reload_directory().await;
    }

    async fn list_groups(&self, p: &Params) -> RpcResult {
        let filter = p.strings("groupId");
        let mut out = Vec::new();
        for (key, group) in self.groups().await? {
            let Some(id) = envelope::group_id(&key) else { continue };
            if !filter.is_empty() && !filter.contains(&id) {
                continue;
            }
            let extras = self.group_extras(&key, &group).await;
            let pending = group.pending_members.iter().map(|m| self.pending_json(m)).collect();
            let mut entry = groups::to_json(&self.dir.borrow(), &id, key, &group, self.self_aci, &extras, pending);
            entry["isBlocked"] = json!(self.is_group_blocked(&key));
            out.push(entry);
        }
        Ok(Value::Array(out))
    }

    pub(crate) async fn upload(&self, paths: &[String], voice_note: bool) -> Result<Vec<AttachmentPointer>, RpcError> {
        let mut specs = Vec::new();
        for path in paths {
            let data = tokio::fs::read(path).await.map_err(|e| RpcError::failed(format!("{path}: {e}")))?;
            let file_name = Path::new(path).file_name().map(|n| n.to_string_lossy().into_owned());
            let spec = AttachmentSpec {
                content_type: mime_guess::from_path(path).first_or_octet_stream().to_string(),
                length: data.len(),
                file_name,
                preview: None,
                voice_note: voice_note.then_some(true),
                borderless: None,
                width: None,
                height: None,
                caption: None,
                blur_hash: None,
            };
            specs.push((spec, data));
        }
        let mut pointers = Vec::new();
        for result in self.manager.upload_attachments(specs).await? {
            pointers.push(result.map_err(|e| RpcError::failed(format!("attachment upload failed: {e}")))?);
        }
        Ok(pointers)
    }

    async fn send(&self, p: &Params) -> RpcResult {
        let targets = self.targets(p).await?;
        let timestamp = now_ms();
        let mut dm = DataMessage {
            body: p.str("message").filter(|m| !m.is_empty()).map(str::to_owned),
            timestamp: Some(timestamp),
            body_ranges: textstyle::from_strings(p.strings("textStyle").iter().map(String::as_str)).map_err(RpcError::invalid)?,
            is_view_once: p.bool("viewOnce").then_some(true),
            ..Default::default()
        };
        dm.attachments = self.upload(&p.strings("attachments"), p.bool("voiceNote")).await?;
        if let Some(id) = p.u64("quoteTimestamp") {
            let author = self.resolve_aci(p.required_str("quoteAuthor")?).await?;
            dm.quote = Some(Quote {
                id: Some(id),
                author_aci: Some(author.to_string()),
                author_aci_binary: Some(author.as_bytes().to_vec()),
                text: p.str("quoteMessage").map(str::to_owned),
                r#type: Some(quote::Type::Normal as i32),
                ..Default::default()
            });
        }
        if let Some(url) = p.str("previewUrl") {
            let image = match p.str("previewImage") {
                Some(path) => self.upload(&[path.to_owned()], false).await?.into_iter().next(),
                None => None,
            };
            dm.preview = vec![Preview {
                url: Some(url.to_owned()),
                title: p.str("previewTitle").map(str::to_owned),
                description: p.str("previewDescription").map(str::to_owned),
                image,
                date: None,
            }];
        }
        if let Some(sticker) = p.str("sticker") {
            dm.sticker = Some(self.sticker_for_send(sticker).await?);
        }
        if dm.body.is_none() && dm.attachments.is_empty() && dm.sticker.is_none() {
            return Err(RpcError::invalid("no message text or attachments"));
        }
        let body = match p.u64("editTimestamp") {
            Some(target) => ContentBody::EditMessage(EditMessage { target_sent_timestamp: Some(target), data_message: Some(dm) }),
            None => ContentBody::DataMessage(dm),
        };
        self.send_to(&targets, body, timestamp).await
    }

    async fn send_reaction(&self, p: &Params) -> RpcResult {
        let targets = self.targets(p).await?;
        let author = self.resolve_aci(p.required_str("targetAuthor")?).await?;
        let timestamp = now_ms();
        let dm = DataMessage {
            reaction: Some(Reaction {
                emoji: Some(p.required_str("emoji")?.to_owned()),
                remove: Some(p.bool("remove")),
                target_author_aci: Some(author.to_string()),
                target_author_aci_binary: Some(author.as_bytes().to_vec()),
                target_sent_timestamp: Some(p.required_u64("targetTimestamp")?),
            }),
            timestamp: Some(timestamp),
            ..Default::default()
        };
        self.send_to(&targets, ContentBody::DataMessage(dm), timestamp).await
    }

    async fn remote_delete(&self, p: &Params) -> RpcResult {
        let targets = self.targets(p).await?;
        let timestamp = now_ms();
        let dm = DataMessage {
            delete: Some(Delete { target_sent_timestamp: Some(p.required_u64("targetTimestamp")?) }),
            timestamp: Some(timestamp),
            ..Default::default()
        };
        self.send_to(&targets, ContentBody::DataMessage(dm), timestamp).await
    }

    async fn send_receipt(&self, p: &Params) -> RpcResult {
        let recipient = self.resolve(p.required_str("recipient")?).await?;
        let kind = match p.str("type").unwrap_or("read") {
            "read" => ReceiptType::Read,
            "viewed" => ReceiptType::Viewed,
            "delivery" => ReceiptType::Delivery,
            other => return Err(RpcError::invalid(format!("unknown receipt type '{other}'"))),
        };
        let receipt = ReceiptMessage { r#type: Some(kind as i32), timestamp: p.u64s("targetTimestamps") };
        self.send_to(&[Target::Contact(recipient)], ContentBody::ReceiptMessage(receipt), now_ms()).await
    }

    async fn send_typing(&self, p: &Params) -> RpcResult {
        let timestamp = now_ms();
        let action = if p.bool("stop") { Action::Stopped } else { Action::Started };
        let _guard = self.send_lock.lock().await;
        for target in self.targets(p).await? {
            let mut m = self.manager.clone();
            match target {
                Target::Contact(sid) => {
                    let typing = TypingMessage { timestamp: Some(timestamp), action: Some(action as i32), group_id: None };
                    m.send_message(sid, ContentBody::TypingMessage(typing), timestamp).await?;
                }
                Target::Group(key) => {
                    let id = envelope::group_id(&key).and_then(|id| BASE64_STANDARD.decode(id).ok());
                    let typing = TypingMessage { timestamp: Some(timestamp), action: Some(action as i32), group_id: id };
                    m.send_message_to_group(&key, ContentBody::TypingMessage(typing), timestamp).await?;
                }
            }
        }
        Ok(json!({"timestamp": timestamp, "results": []}))
    }

    async fn get_attachment(&self, p: &Params) -> RpcResult {
        let id = p.required_str("id")?;
        if id.is_empty() || !id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_') {
            return Err(RpcError::invalid("invalid attachment id"));
        }
        let data = tokio::fs::read(self.attachments.join(id)).await.map_err(|_| RpcError::failed("Could not find attachment"))?;
        Ok(json!({"data": BASE64_STANDARD.encode(data)}))
    }

    async fn get_avatar(&self, p: &Params) -> RpcResult {
        let mut m = self.manager.clone();
        let avatar = if let Some(id) = p.str("groupId") {
            let ctx = self.group_context(self.group_key(id).await?).await;
            m.retrieve_group_avatar(ctx).await?
        } else if let Some(who) = p.str("contact") {
            let sid = self.resolve(who).await?;
            self.store().contact_by_id(&sid).await?.and_then(|c| c.avatar).map(|a| a.reader.to_vec())
        } else if let Some(who) = p.str("profile") {
            let uuid = self.resolve(who).await?.raw_uuid();
            let key: Option<ProfileKey> = if uuid == self.self_aci {
                Some(self.manager.registration_data().profile_key())
            } else {
                self.store().profile_key(&ServiceId::Aci(uuid.into())).await?
            };
            match key {
                Some(key) => m.retrieve_profile_avatar_by_uuid(uuid, key).await?,
                None => None,
            }
        } else {
            return Err(RpcError::invalid("one of groupId, contact or profile is required"));
        };
        match avatar {
            Some(data) if !data.is_empty() => Ok(json!({"data": BASE64_STANDARD.encode(data)})),
            _ => Err(RpcError::failed("No avatar found")),
        }
    }

    async fn update_contact(&self, p: &Params) -> RpcResult {
        let recipient = self.resolve(p.required_str("recipient")?).await?;
        let Some(expiration) = p.u64("expiration") else {
            // Local contact names are kept by corvus itself; nothing else to do.
            return Ok(json!({}));
        };
        let timestamp = now_ms();
        let dm = DataMessage {
            flags: Some(Flags::ExpirationTimerUpdate as u32),
            expire_timer: Some(u32::try_from(expiration).map_err(|_| RpcError::invalid("expiration out of range"))?),
            timestamp: Some(timestamp),
            ..Default::default()
        };
        self.send_to(&[Target::Contact(recipient)], ContentBody::DataMessage(dm), timestamp).await?;
        Ok(json!({}))
    }
}

/// The image of a received sticker, kept so getSticker works for packs that are not installed.
fn sticker_pointers(body: &ContentBody) -> Vec<(AttachmentPointer, String)> {
    let dm = match body {
        ContentBody::DataMessage(dm) => Some(dm),
        ContentBody::SynchronizeMessage(sync) => match &sync.content {
            Some(SyncContent::Sent(sent)) => sent.message.as_ref(),
            _ => None,
        },
        _ => None,
    };
    dm.and_then(|dm| dm.sticker.as_ref())
        .and_then(|s| Some((s.data.clone()?, crate::stickers::received_sticker_file(s.pack_id.as_deref()?, s.sticker_id?))))
        .into_iter()
        .collect()
}

fn collect_pointers(dm: &DataMessage, out: &mut Vec<AttachmentPointer>) {
    out.extend(dm.attachments.iter().cloned());
    out.extend(dm.preview.iter().filter_map(|p| p.image.clone()));
}

async fn write_private(path: &Path, data: &[u8]) -> std::io::Result<()> {
    let tmp = path.with_extension("part");
    tokio::fs::write(&tmp, data).await?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        tokio::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o600)).await?;
    }
    tokio::fs::rename(tmp, path).await
}

// signal-cli's group commands (updateGroup, quitGroup, joinGroup) on presage's group changes, and
// the parts of a group listGroups reports that presage's stored Group leaves out.

use std::collections::HashMap;

use presage::libsignal_service::groups_v2::{decrypt_group, AccessRequired, GroupInviteLink};
use presage::libsignal_service::prelude::Uuid;
use presage::libsignal_service::protocol::{Aci, ServiceId};
use presage::manager::{GroupEdit, GroupJoin};
use presage::model::groups::Group;
use presage::model::ServiceIdType;
use presage::store::ContentsStore;
use serde_json::{json, Value};

use crate::daemon::Daemon;
use crate::envelope;
use crate::rpc::{Params, RpcError, RpcResult};

/// What presage's Group doesn't keep: read from the server, again when the revision changed.
#[derive(Debug, Clone, Default)]
pub struct Extras {
    pub revision: u32,
    pub announcements_only: bool,
    pub banned: Vec<ServiceId>,
}

pub type ExtrasCache = HashMap<[u8; 32], Extras>;

fn access(value: &str) -> Result<AccessRequired, RpcError> {
    match value {
        "every-member" | "EVERY_MEMBER" => Ok(AccessRequired::Member),
        "only-admins" | "ONLY_ADMINS" => Ok(AccessRequired::Administrator),
        other => Err(RpcError::invalid(format!("invalid permission '{other}'"))),
    }
}

fn link_access(value: &str) -> Result<AccessRequired, RpcError> {
    match value {
        "enabled" => Ok(AccessRequired::Any),
        "enabled-with-approval" => Ok(AccessRequired::Administrator),
        "disabled" => Ok(AccessRequired::Unsatisfiable),
        other => Err(RpcError::invalid(format!("invalid link state '{other}'"))),
    }
}

fn pending_id(m: &presage::model::groups::PendingMember) -> ServiceId {
    match m.service_id_type {
        ServiceIdType::AccountIdentity => ServiceId::Aci(m.uuid.into()),
        ServiceIdType::PhoneNumberIdentity => ServiceId::Pni(m.uuid.into()),
    }
}

impl Daemon {
    fn self_pni(&self) -> Uuid {
        Uuid::from(self.manager.registration_data().service_ids.pni())
    }

    fn invited(&self, group: &Group) -> bool {
        let pni = self.self_pni();
        group.pending_members.iter().any(|m| m.uuid == self.self_aci || m.uuid == pni)
    }

    async fn read_avatar(path: &str) -> Result<Vec<u8>, RpcError> {
        tokio::fs::read(path).await.map_err(|e| RpcError::failed(format!("{path}: {e}")))
    }

    /// The edits one `updateGroup` call asks for, in the order signal-cli applies them.
    async fn group_edits(&self, p: &Params, group: &Group) -> Result<Vec<GroupEdit>, RpcError> {
        let mut edits = Vec::new();
        if let Some(name) = p.str("name") {
            edits.push(GroupEdit::Title(name.to_owned()));
        }
        if let Some(description) = p.str("description") {
            edits.push(GroupEdit::Description(description.to_owned()));
        }
        if let Some(path) = p.str("avatar") {
            edits.push(GroupEdit::Avatar(Self::read_avatar(path).await?));
        }
        if p.bool("removeAvatar") {
            edits.push(GroupEdit::Avatar(Vec::new()));
        }
        if let Some(seconds) = p.u64("expiration") {
            edits.push(GroupEdit::Timer(u32::try_from(seconds).map_err(|_| RpcError::invalid("expiration out of range"))?));
        }
        if let Some(v) = p.str("setPermissionAddMember") {
            edits.push(GroupEdit::MembersAccess(access(v)?));
        }
        if let Some(v) = p.str("setPermissionEditDetails") {
            edits.push(GroupEdit::AttributesAccess(access(v)?));
        }
        if let Some(v) = p.str("setPermissionSendMessages") {
            edits.push(GroupEdit::AnnouncementsOnly(access(v)? == AccessRequired::Administrator));
        }
        if let Some(v) = p.str("link") {
            edits.push(GroupEdit::InviteLinkAccess(link_access(v)?));
        }
        if p.bool("resetLink") {
            edits.push(GroupEdit::ResetInviteLink);
        }

        // "member" adds people, and lets in those who asked to join.
        let mut add = Vec::new();
        for who in p.strings("member") {
            let sid = self.resolve(&who).await?;
            match sid {
                ServiceId::Aci(aci) if group.requesting_members.iter().any(|m| m.aci == aci) => {
                    edits.push(GroupEdit::ApproveRequest(aci))
                }
                _ if group.members.iter().any(|m| ServiceId::Aci(m.aci) == sid) => {}
                _ => add.push(sid),
            }
        }
        if !add.is_empty() {
            edits.push(GroupEdit::AddMembers(add));
        }
        // "removeMember" removes members, withdraws invitations and turns requests down.
        for who in p.strings("removeMember") {
            let sid = self.resolve(&who).await?;
            if let ServiceId::Aci(aci) = sid {
                if group.members.iter().any(|m| m.aci == aci) {
                    edits.push(GroupEdit::RemoveMember(aci));
                    continue;
                }
                if group.requesting_members.iter().any(|m| m.aci == aci) {
                    edits.push(GroupEdit::DenyRequest(aci));
                    continue;
                }
            }
            if group.pending_members.iter().any(|m| pending_id(m) == sid) {
                edits.push(GroupEdit::WithdrawInvitation(sid));
            } else {
                return Err(RpcError::failed(format!("{who} is not a member of the group")));
            }
        }
        for who in p.strings("admin") {
            edits.push(GroupEdit::SetAdmin(Aci::from(self.resolve_aci(&who).await?), true));
        }
        for who in p.strings("removeAdmin") {
            edits.push(GroupEdit::SetAdmin(Aci::from(self.resolve_aci(&who).await?), false));
        }
        for who in p.strings("ban") {
            edits.push(GroupEdit::Ban(self.resolve(&who).await?));
        }
        for who in p.strings("unban") {
            edits.push(GroupEdit::Unban(self.resolve(&who).await?));
        }
        Ok(edits)
    }

    async fn apply(&self, key: &[u8; 32], edits: Vec<GroupEdit>) -> Result<u64, RpcError> {
        let _guard = self.send_lock.lock().await;
        let mut timestamp = crate::daemon::now_ms();
        for edit in edits {
            let mut m = self.manager.clone();
            timestamp = m.change_group(key, edit).await?;
        }
        Ok(timestamp)
    }

    pub(crate) async fn update_group(&self, p: &Params) -> RpcResult {
        let Some(id) = p.str("groupId") else {
            return self.create_group(p).await;
        };
        let key = self.group_key(id).await?;
        let group = self.store().group(key).await?.ok_or_else(|| RpcError::failed("Unknown group"))?;
        let mut edits = self.group_edits(p, &group).await?;
        if edits.is_empty() && self.invited(&group) {
            // signal-cli: an updateGroup without changes accepts an invitation.
            edits.push(GroupEdit::AcceptInvitation);
        }
        let timestamp = self.apply(&key, edits).await?;
        self.reload_groups().await;
        Ok(json!({"timestamp": timestamp, "results": [], "groupId": id}))
    }

    async fn create_group(&self, p: &Params) -> RpcResult {
        let name = p.required_str("name")?;
        let mut members = Vec::new();
        for who in p.strings("member") {
            members.push(Aci::from(self.resolve_aci(&who).await?));
        }
        let (key, mut timestamp) = {
            let _guard = self.send_lock.lock().await;
            self.manager.clone().create_group(name, &members).await?
        };
        let mut edits = Vec::new();
        if let Some(description) = p.str("description").filter(|d| !d.is_empty()) {
            edits.push(GroupEdit::Description(description.to_owned()));
        }
        if let Some(path) = p.str("avatar") {
            edits.push(GroupEdit::Avatar(Self::read_avatar(path).await?));
        }
        if !edits.is_empty() {
            timestamp = self.apply(&key, edits).await?;
        }
        self.reload_groups().await;
        let id = envelope::group_id(&key).ok_or_else(|| RpcError::failed("invalid group"))?;
        Ok(json!({"timestamp": timestamp, "results": [], "groupId": id}))
    }

    pub(crate) async fn quit_group(&self, p: &Params) -> RpcResult {
        let id = p.required_str("groupId")?;
        let key = self.group_key(id).await?;
        let group = self.store().group(key).await?.ok_or_else(|| RpcError::failed("Unknown group"))?;
        let own = Aci::from(self.self_aci);
        let mut edits = Vec::new();
        if self.invited(&group) {
            edits.push(GroupEdit::DeclineInvitation);
        } else if group.requesting_members.iter().any(|m| m.aci == own) {
            let _guard = self.send_lock.lock().await;
            self.manager.clone().cancel_join_request(&key).await?;
            return Ok(json!({"timestamp": crate::daemon::now_ms(), "results": []}));
        } else if group.is_member(&own) {
            let admin = |m: &presage::model::groups::Member| m.role == presage::libsignal_service::groups_v2::Role::Administrator;
            let successors = p.strings("admin");
            let only_admin = group.members.iter().any(|m| m.aci == own && admin(m))
                && !group.members.iter().any(|m| m.aci != own && admin(m))
                && group.members.len() > 1;
            if only_admin && successors.is_empty() {
                return Err(RpcError::failed("You need to specify a new admin with --admin: only admin"));
            }
            for who in successors {
                edits.push(GroupEdit::SetAdmin(Aci::from(self.resolve_aci(&who).await?), true));
            }
            edits.push(GroupEdit::Leave);
        } else {
            return Err(RpcError::failed("Group not found or not a member"));
        }
        let timestamp = self.apply(&key, edits).await?;
        self.reload_groups().await;
        Ok(json!({"timestamp": timestamp, "results": []}))
    }

    pub(crate) async fn join_group(&self, p: &Params) -> RpcResult {
        let uri = p.required_str("uri")?;
        let link = GroupInviteLink::parse(uri).ok_or_else(|| RpcError::failed("Group link is invalid: not a group link"))?;
        let joined = {
            let _guard = self.send_lock.lock().await;
            self.manager.clone().join_group(&link).await?
        };
        self.reload_groups().await;
        let id = envelope::group_id(&link.master_key).ok_or_else(|| RpcError::failed("invalid group"))?;
        Ok(match joined {
            GroupJoin::Joined { timestamp } => json!({"timestamp": timestamp, "results": [], "groupId": id}),
            GroupJoin::Requested => {
                json!({"timestamp": crate::daemon::now_ms(), "results": [], "groupId": id, "onlyRequested": true})
            }
        })
    }

    /// Announcements-only and the banned list, from the server when the stored revision moved on.
    pub(crate) async fn group_extras(&self, key: &[u8; 32], group: &Group) -> Extras {
        if let Some(extras) = self.group_extras.borrow().get(key) {
            if extras.revision == group.revision {
                return extras.clone();
            }
        }
        let fetched = async {
            let mut gm = self.manager.groups_manager().await.ok()?;
            let encrypted = gm.fetch_encrypted_group(&mut rand::rng(), key).await.ok()?;
            decrypt_group(key, encrypted).ok()
        }
        .await;
        let extras = match fetched {
            Some(full) => Extras {
                revision: group.revision,
                announcements_only: full.announcements_only,
                banned: full.members_banned.iter().map(|b| b.user_id).collect(),
            },
            // Not reachable now: report what is known, and try again next time.
            None => return Extras { revision: u32::MAX, ..Default::default() },
        };
        self.group_extras.borrow_mut().insert(*key, extras.clone());
        extras
    }

    pub(crate) fn pending_json(&self, m: &presage::model::groups::PendingMember) -> Value {
        let pni = self.self_pni();
        // Invited by phone number: corvus recognises its invitation by its own number.
        if m.uuid == pni || m.uuid == self.self_aci {
            return json!({"uuid": self.self_aci.to_string(), "number": self.account});
        }
        match m.service_id_type {
            ServiceIdType::PhoneNumberIdentity => json!({"uuid": format!("PNI:{}", m.uuid), "number": null}),
            ServiceIdType::AccountIdentity => {
                json!({"uuid": m.uuid.to_string(), "number": self.dir.borrow().get(&m.uuid).number})
            }
        }
    }
}

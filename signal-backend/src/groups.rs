// presage Group -> signal-cli listGroups entry (read by corvus in src/GroupInfo.cpp parseOne).

use presage::libsignal_service::groups_v2::{AccessRequired, Role};
use presage::libsignal_service::prelude::Uuid;
use presage::libsignal_service::protocol::ServiceId;
use presage::model::groups::Group;
use serde_json::{json, Value};

use crate::envelope::Directory;
use crate::groupops::Extras;

fn permission(access: Option<&AccessRequired>) -> &'static str {
    match access {
        Some(AccessRequired::Administrator) => "ONLY_ADMINS",
        _ => "EVERY_MEMBER",
    }
}

fn address(dir: &Directory, uuid: Uuid) -> Value {
    json!({"uuid": uuid.to_string(), "number": dir.get(&uuid).number})
}

/// `pending` is formatted by the caller, which knows our own PNI.
pub fn to_json(dir: &Directory, id: &str, key: [u8; 32], group: &Group, self_aci: Uuid, extras: &Extras, pending: Vec<Value>) -> Value {
    let members: Vec<Value> = group
        .members
        .iter()
        .map(|m| {
            let uuid = Uuid::from(m.aci);
            json!({
                "uuid": uuid.to_string(),
                "number": dir.get(&uuid).number,
                "isAdmin": m.role == Role::Administrator,
            })
        })
        .collect();
    let admins: Vec<Value> = group
        .members
        .iter()
        .filter(|m| m.role == Role::Administrator)
        .map(|m| address(dir, m.aci.into()))
        .collect();
    let banned: Vec<Value> = extras
        .banned
        .iter()
        .map(|sid| match sid {
            ServiceId::Aci(_) => address(dir, sid.raw_uuid()),
            ServiceId::Pni(_) => json!({"uuid": format!("PNI:{}", sid.raw_uuid()), "number": null}),
        })
        .collect();
    let requesting: Vec<Value> = group.requesting_members.iter().map(|m| address(dir, m.aci.into())).collect();
    let access = group.access_control.as_ref();
    json!({
        "id": id,
        "name": group.title,
        "description": group.description,
        "isMember": group.members.iter().any(|m| Uuid::from(m.aci) == self_aci),
        "isBlocked": false,
        "messageExpirationTime": group.disappearing_messages_timer.as_ref().map(|t| t.duration).unwrap_or(0),
        "revision": group.revision,
        "members": members,
        "pendingMembers": pending,
        "requestingMembers": requesting,
        "admins": admins,
        "banned": banned,
        "permissionAddMember": permission(access.map(|a| &a.members)),
        "permissionEditDetails": permission(access.map(|a| &a.attributes)),
        "permissionSendMessage": if extras.announcements_only { "ONLY_ADMINS" } else { "EVERY_MEMBER" },
        "groupInviteLink": group.invite_link(key).map(|link| link.to_url()),
    })
}

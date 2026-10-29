// The JSON corvus-signal emits, written to tests/data/corvus-signal-contract.json, where
// tests/backend_contract_test.cpp feeds it through corvus' own parsers. Regenerate after a change
// with CORVUS_UPDATE_CONTRACT=1 cargo test.

#![cfg(test)]

use presage::libsignal_service::content::{Content, ContentBody, Metadata};
use presage::libsignal_service::groups_v2::{AccessRequired, Role};
use presage::libsignal_service::prelude::{ProfileKey, Uuid};
use presage::libsignal_service::protocol::ServiceId;
use presage::model::groups::{AccessControl, Group, Member, RequestingMember};
use presage::proto::attachment_pointer::AttachmentIdentifier;
use presage::proto::body_range::{AssociatedValue, Style};
use presage::proto::data_message::{quote, Delete, Flags, Quote, Reaction, Sticker};
use presage::proto::receipt_message::Type as ReceiptType;
use presage::proto::sync_message::{Content as SyncContent, Sent};
use presage::proto::typing_message::Action;
use presage::proto::{
    AttachmentPointer, BodyRange, DataMessage, EditMessage, GroupContextV2, Preview, ReceiptMessage, SyncMessage,
    TypingMessage,
};
use serde_json::{json, Value};

use crate::envelope::{self, Directory, Person};
use crate::groupops::Extras;
use crate::{groups, rpc};

const ALICE: Uuid = Uuid::from_u128(0x1111_1111_1111_4111_8111_1111_1111_1111);
const BOB: Uuid = Uuid::from_u128(0x2222_2222_2222_4222_8222_2222_2222_2222);
const ME: Uuid = Uuid::from_u128(0x3333_3333_3333_4333_8333_3333_3333_3333);
const MASTER_KEY: [u8; 32] = [7; 32];
const TS: u64 = 1_790_000_000_000;

fn dir() -> Directory {
    let mut d = Directory::default();
    d.people.insert(ALICE, Person { number: Some("+49111".into()), name: Some("Alice".into()) });
    d.people.insert(BOB, Person { number: None, name: Some("Bob".into()) });
    d.people.insert(ME, Person { number: Some("+49999".into()), name: None });
    d
}

fn content(sender: Uuid, body: ContentBody) -> Content {
    Content {
        metadata: Metadata {
            sender: ServiceId::Aci(sender.into()),
            destination: ServiceId::Aci(ME.into()),
            sender_device: 1u8.try_into().unwrap(),
            pni_verified: None,
            client_timestamp: chrono::DateTime::from_timestamp_millis(TS as i64).unwrap(),
            server_timestamp: chrono::DateTime::from_timestamp_millis(TS as i64 + 50).unwrap(),
            needs_receipt: true,
            unidentified_sender: true,
            was_plaintext: false,
            server_guid: None,
        },
        body,
    }
}

fn group_ctx() -> GroupContextV2 {
    GroupContextV2 { master_key: Some(MASTER_KEY.to_vec()), revision: Some(3), group_change: None }
}

fn notification(sender: Uuid, body: ContentBody) -> Value {
    let env = envelope::to_envelope(&dir(), &content(sender, body)).unwrap();
    rpc::notification("receive", json!({"envelope": env, "account": "+49999"}))
}

fn bold(start: u32, length: u32) -> BodyRange {
    BodyRange { start: Some(start), length: Some(length), associated_value: Some(AssociatedValue::Style(Style::Bold as i32)) }
}

fn image(key: &str) -> AttachmentPointer {
    AttachmentPointer {
        attachment_identifier: Some(AttachmentIdentifier::CdnKey(key.into())),
        content_type: Some("image/png".into()),
        file_name: Some("cat.png".into()),
        size: Some(1234),
        width: Some(640),
        height: Some(480),
        ..Default::default()
    }
}

fn cases() -> Value {
    let text = DataMessage {
        body: Some("Hello world https://example.org".into()),
        timestamp: Some(TS),
        body_ranges: vec![bold(0, 5)],
        attachments: vec![image("att1")],
        quote: Some(Quote {
            id: Some(TS - 1000),
            author_aci: Some(BOB.to_string()),
            text: Some("earlier".into()),
            r#type: Some(quote::Type::Normal as i32),
            ..Default::default()
        }),
        preview: vec![Preview {
            url: Some("https://example.org".into()),
            title: Some("Example".into()),
            description: Some("An example".into()),
            image: Some(image("prev1")),
            date: None,
        }],
        expire_timer: Some(3600),
        ..Default::default()
    };
    let group_text = DataMessage {
        body: Some("in the group".into()),
        timestamp: Some(TS),
        group_v2: Some(group_ctx()),
        ..Default::default()
    };
    let reaction = DataMessage {
        reaction: Some(Reaction {
            emoji: Some("👍".into()),
            remove: Some(false),
            target_author_aci_binary: Some(ME.as_bytes().to_vec()),
            target_sent_timestamp: Some(TS - 5000),
            ..Default::default()
        }),
        timestamp: Some(TS),
        group_v2: Some(group_ctx()),
        ..Default::default()
    };
    let delete = DataMessage {
        delete: Some(Delete { target_sent_timestamp: Some(TS - 6000) }),
        timestamp: Some(TS),
        ..Default::default()
    };
    let sticker = DataMessage {
        sticker: Some(Sticker {
            pack_id: Some(vec![0xab, 0xcd]),
            pack_key: Some(vec![1; 32]),
            sticker_id: Some(4),
            data: Some(image("stk1")),
            emoji: Some("🐢".into()),
        }),
        timestamp: Some(TS),
        ..Default::default()
    };
    let expiration = DataMessage {
        flags: Some(Flags::ExpirationTimerUpdate as u32),
        expire_timer: Some(86400),
        timestamp: Some(TS),
        ..Default::default()
    };
    let voice = DataMessage {
        attachments: vec![AttachmentPointer {
            attachment_identifier: Some(AttachmentIdentifier::CdnId(987654321)),
            content_type: Some("audio/aac".into()),
            flags: Some(1),
            size: Some(4321),
            ..Default::default()
        }],
        timestamp: Some(TS),
        ..Default::default()
    };
    let view_once = DataMessage {
        attachments: vec![image("vo1")],
        is_view_once: Some(true),
        timestamp: Some(TS),
        ..Default::default()
    };
    let edit = EditMessage {
        target_sent_timestamp: Some(TS - 7000),
        data_message: Some(DataMessage {
            body: Some("fixed typo".into()),
            body_ranges: vec![bold(0, 5)],
            timestamp: Some(TS),
            ..Default::default()
        }),
    };
    let sent = SyncMessage {
        content: Some(SyncContent::Sent(Sent {
            destination_service_id: Some(ALICE.to_string()),
            timestamp: Some(TS),
            message: Some(DataMessage { body: Some("from my phone".into()), timestamp: Some(TS), ..Default::default() }),
            ..Default::default()
        })),
        ..Default::default()
    };
    let sent_edit = SyncMessage {
        content: Some(SyncContent::Sent(Sent {
            destination_service_id_binary: Some(ALICE.as_bytes().to_vec()),
            timestamp: Some(TS),
            edit_message: Some(EditMessage {
                target_sent_timestamp: Some(TS - 8000),
                data_message: Some(DataMessage { body: Some("edited on phone".into()), timestamp: Some(TS), ..Default::default() }),
            }),
            ..Default::default()
        })),
        ..Default::default()
    };
    let read = ReceiptMessage { r#type: Some(ReceiptType::Read as i32), timestamp: vec![TS - 100, TS - 200] };
    let delivery = ReceiptMessage { r#type: Some(ReceiptType::Delivery as i32), timestamp: vec![TS - 300] };
    let group_id = envelope::group_id(&MASTER_KEY).unwrap();
    let typing = TypingMessage {
        timestamp: Some(TS),
        action: Some(Action::Started as i32),
        group_id: Some(base64::Engine::decode(&base64::prelude::BASE64_STANDARD, &group_id).unwrap()),
    };

    let group = Group {
        title: "Team".into(),
        avatar: String::new(),
        disappearing_messages_timer: Some(presage::libsignal_service::prelude::Timer { duration: 604800 }),
        access_control: Some(AccessControl {
            attributes: AccessRequired::Administrator,
            members: AccessRequired::Member,
            add_from_invite_link: AccessRequired::Administrator,
            member_label: AccessRequired::Member,
        }),
        revision: 3,
        members: vec![
            Member { aci: ME.into(), role: Role::Administrator, profile_key: ProfileKey::create([0; 32]), joined_at_revision: 0, label: None, label_emoji: None },
            Member { aci: ALICE.into(), role: Role::Default, profile_key: ProfileKey::create([0; 32]), joined_at_revision: 1, label: None, label_emoji: None },
        ],
        pending_members: vec![],
        requesting_members: vec![RequestingMember { aci: BOB.into(), profile_key: ProfileKey::create([0; 32]), timestamp: TS }],
        invite_link_password: vec![9; 16],
        description: Some("Our team".into()),
    };
    let extras = Extras { revision: 3, announcements_only: true, banned: vec![] };
    let pending = vec![json!({"uuid": ME.to_string(), "number": "+49999"})];
    let listed = groups::to_json(&dir(), &group_id, MASTER_KEY, &group, ME, &extras, pending);

    json!({
        "groupId": group_id,
        "text": notification(ALICE, ContentBody::DataMessage(text)),
        "groupText": notification(BOB, ContentBody::DataMessage(group_text)),
        "reaction": notification(ALICE, ContentBody::DataMessage(reaction)),
        "delete": notification(ALICE, ContentBody::DataMessage(delete)),
        "sticker": notification(ALICE, ContentBody::DataMessage(sticker)),
        "expiration": notification(ALICE, ContentBody::DataMessage(expiration)),
        "voiceNote": notification(ALICE, ContentBody::DataMessage(voice)),
        "viewOnce": notification(ALICE, ContentBody::DataMessage(view_once)),
        "edit": notification(ALICE, ContentBody::EditMessage(edit)),
        "syncSent": notification(ME, ContentBody::SynchronizeMessage(sent)),
        "syncSentEdit": notification(ME, ContentBody::SynchronizeMessage(sent_edit)),
        "readReceipt": notification(ALICE, ContentBody::ReceiptMessage(read)),
        "deliveryReceipt": notification(ALICE, ContentBody::ReceiptMessage(delivery)),
        "groupTyping": notification(ALICE, ContentBody::TypingMessage(typing)),
        "listGroups": [listed],
    })
}

#[test]
fn contract_fixture_is_current() {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tests/data/corvus-signal-contract.json");
    let current = serde_json::to_string_pretty(&cases()).unwrap() + "\n";
    if std::env::var_os("CORVUS_UPDATE_CONTRACT").is_some() {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, &current).unwrap();
        return;
    }
    let saved = std::fs::read_to_string(&path).unwrap_or_default();
    assert!(saved == current, "{} is out of date: run CORVUS_UPDATE_CONTRACT=1 cargo test", path.display());
}

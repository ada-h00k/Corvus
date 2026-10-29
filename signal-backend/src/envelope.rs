// presage Content -> signal-cli "receive" envelope JSON, in the shape corvus parses
// (src/Envelope.cpp parseEnvelope, src/SignalCli.cpp envelopeFromNotification).

use std::collections::HashMap;

use base64::prelude::*;
use presage::libsignal_service::content::{Content, ContentBody, Metadata};
use presage::libsignal_service::prelude::{GroupMasterKey, GroupSecretParams, Uuid};
use presage::proto::attachment_pointer::AttachmentIdentifier;
use presage::proto::data_message::{Flags, Quote, Reaction};
use presage::proto::receipt_message::Type as ReceiptType;
use presage::proto::sync_message::{Content as SyncContent, Sent};
use presage::proto::typing_message::Action;
use presage::proto::{AttachmentPointer, DataMessage, EditMessage, GroupContextV2};
use serde_json::{json, Map, Value};

use crate::textstyle;

/// What corvus knows a person by: phone number (preferred key) and display name.
#[derive(Debug, Clone, Default)]
pub struct Person {
    pub number: Option<String>,
    pub name: Option<String>,
}

/// uuid -> Person, refreshed from the presage store when contacts or profiles change.
#[derive(Debug, Default)]
pub struct Directory {
    pub people: HashMap<Uuid, Person>,
}

impl Directory {
    pub fn get(&self, uuid: &Uuid) -> Person {
        self.people.get(uuid).cloned().unwrap_or_default()
    }

    /// Fills "<prefix>", "<prefix>Number" and "<prefix>Uuid" like signal-cli does for addresses.
    fn address(&self, out: &mut Map<String, Value>, prefix: &str, uuid: Uuid) {
        let person = self.get(&uuid);
        let uuid = uuid.to_string();
        out.insert(prefix.into(), json!(person.number.clone().unwrap_or_else(|| uuid.clone())));
        out.insert(format!("{prefix}Number"), json!(person.number));
        out.insert(format!("{prefix}Uuid"), json!(uuid));
    }
}

/// signal-cli's group id: base64 of the group identifier derived from the master key.
pub fn group_id(master_key: &[u8]) -> Option<String> {
    let bytes: [u8; 32] = master_key.try_into().ok()?;
    let params = GroupSecretParams::derive_from_master_key(GroupMasterKey::new(bytes));
    Some(BASE64_STANDARD.encode(params.get_group_identifier()))
}

/// Stable id for a received attachment: its CDN key/id, restricted to file-name-safe characters.
pub fn attachment_id(pointer: &AttachmentPointer) -> Option<String> {
    let raw = match pointer.attachment_identifier.as_ref()? {
        AttachmentIdentifier::CdnKey(key) => key.clone(),
        AttachmentIdentifier::CdnId(id) => id.to_string(),
    };
    let id: String = raw
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
        .collect();
    (!id.is_empty()).then_some(id)
}

/// Parses the 16-byte form, else the string form of an ACI (newer clients may only send one of them).
fn aci(binary: Option<&[u8]>, text: Option<&str>) -> Option<Uuid> {
    binary
        .and_then(|b| Uuid::from_slice(b).ok())
        .or_else(|| text.and_then(|t| Uuid::parse_str(t).ok()))
}

fn attachment_json(pointer: &AttachmentPointer) -> Value {
    json!({
        "id": attachment_id(pointer),
        "contentType": pointer.content_type.as_deref().unwrap_or("application/octet-stream"),
        "filename": pointer.file_name,
        "size": pointer.size,
        "width": pointer.width,
        "height": pointer.height,
        "caption": pointer.caption,
        "voiceNote": pointer.flags.unwrap_or(0) & 1 != 0,
    })
}

fn group_info(ctx: Option<&GroupContextV2>) -> Option<Value> {
    let id = group_id(ctx?.master_key.as_deref()?)?;
    // signal-cli: a message carrying a group change is an UPDATE.
    let kind = if ctx?.group_change.as_ref().is_some_and(|c| !c.is_empty()) { "UPDATE" } else { "DELIVER" };
    Some(json!({"groupId": id, "type": kind, "revision": ctx?.revision}))
}

fn quote_json(dir: &Directory, quote: &Quote) -> Value {
    let mut out = Map::new();
    out.insert("id".into(), json!(quote.id));
    if let Some(author) = aci(quote.author_aci_binary.as_deref(), quote.author_aci.as_deref()) {
        dir.address(&mut out, "author", author);
    }
    out.insert("text".into(), json!(quote.text));
    out.insert(
        "attachments".into(),
        quote
            .attachments
            .iter()
            .map(|a| json!({"contentType": a.content_type, "filename": a.file_name}))
            .collect(),
    );
    Value::Object(out)
}

fn reaction_json(dir: &Directory, reaction: &Reaction) -> Value {
    let mut out = Map::new();
    out.insert("emoji".into(), json!(reaction.emoji));
    if let Some(author) = aci(reaction.target_author_aci_binary.as_deref(), reaction.target_author_aci.as_deref()) {
        dir.address(&mut out, "targetAuthor", author);
    }
    out.insert("targetSentTimestamp".into(), json!(reaction.target_sent_timestamp));
    out.insert("isRemove".into(), json!(reaction.remove.unwrap_or(false)));
    Value::Object(out)
}

/// The members of a signal-cli dataMessage, also used inside syncMessage.sentMessage.
fn data_message_fields(dir: &Directory, dm: &DataMessage, out: &mut Map<String, Value>) {
    out.insert("timestamp".into(), json!(dm.timestamp));
    out.insert("message".into(), json!(dm.body));
    out.insert("expiresInSeconds".into(), json!(dm.expire_timer.unwrap_or(0)));
    out.insert("viewOnce".into(), json!(dm.is_view_once.unwrap_or(false)));
    if dm.flags.unwrap_or(0) & Flags::ExpirationTimerUpdate as u32 != 0 {
        out.insert("isExpirationUpdate".into(), json!(true));
    }
    if let Some(info) = group_info(dm.group_v2.as_ref()) {
        out.insert("groupInfo".into(), info);
    }
    if !dm.attachments.is_empty() {
        out.insert("attachments".into(), dm.attachments.iter().map(attachment_json).collect());
    }
    let styles = textstyle::to_strings(&dm.body_ranges);
    if !styles.is_empty() {
        out.insert("textStyle".into(), json!(styles));
    }
    if let Some(quote) = &dm.quote {
        out.insert("quote".into(), quote_json(dir, quote));
    }
    if let Some(reaction) = &dm.reaction {
        out.insert("reaction".into(), reaction_json(dir, reaction));
    }
    if let Some(ts) = dm.delete.as_ref().and_then(|d| d.target_sent_timestamp) {
        out.insert("remoteDelete".into(), json!({"timestamp": ts}));
    }
    if let Some(sticker) = &dm.sticker {
        out.insert(
            "sticker".into(),
            json!({
                "packId": sticker.pack_id.as_deref().map(hex),
                "stickerId": sticker.sticker_id,
                "emoji": sticker.emoji,
            }),
        );
    }
    if !dm.preview.is_empty() {
        let previews: Vec<Value> = dm
            .preview
            .iter()
            .map(|p| {
                json!({
                    "url": p.url,
                    "title": p.title,
                    "description": p.description,
                    "image": p.image.as_ref().map(attachment_json),
                })
            })
            .collect();
        out.insert("previews".into(), json!(previews));
    }
}

fn data_message_json(dir: &Directory, dm: &DataMessage) -> Value {
    let mut out = Map::new();
    data_message_fields(dir, dm, &mut out);
    Value::Object(out)
}

fn edit_json(dir: &Directory, edit: &EditMessage) -> Value {
    json!({
        "targetSentTimestamp": edit.target_sent_timestamp,
        "dataMessage": edit.data_message.as_ref().map(|dm| data_message_json(dir, dm)),
    })
}

fn sent_json(dir: &Directory, sent: &Sent) -> Value {
    let mut out = Map::new();
    if let Some(dest) = aci(sent.destination_service_id_binary.as_deref(), sent.destination_service_id.as_deref()) {
        dir.address(&mut out, "destination", dest);
    } else if let Some(e164) = &sent.destination_e164 {
        out.insert("destination".into(), json!(e164));
        out.insert("destinationNumber".into(), json!(e164));
    }
    if let Some(edit) = &sent.edit_message {
        out.insert("editMessage".into(), edit_json(dir, edit));
        out.insert("timestamp".into(), json!(sent.timestamp));
    } else if let Some(dm) = &sent.message {
        data_message_fields(dir, dm, &mut out);
    }
    Value::Object(out)
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn header(dir: &Directory, meta: &Metadata) -> Map<String, Value> {
    let mut env = Map::new();
    let uuid = meta.sender.raw_uuid();
    dir.address(&mut env, "source", uuid);
    env.insert("sourceName".into(), json!(dir.get(&uuid).name));
    env.insert("sourceDevice".into(), json!(u32::from(meta.sender_device)));
    env.insert("timestamp".into(), json!(meta.client_timestamp.timestamp_millis()));
    env.insert("serverReceivedTimestamp".into(), json!(meta.server_timestamp.timestamp_millis()));
    env
}

/// The envelope for one received message, or None for content corvus has no use for
/// (null messages, calls, stories, decryption error reports, unhandled sync messages).
pub fn to_envelope(dir: &Directory, content: &Content) -> Option<Value> {
    let mut env = header(dir, &content.metadata);
    match &content.body {
        ContentBody::DataMessage(dm) => {
            env.insert("dataMessage".into(), data_message_json(dir, dm));
        }
        ContentBody::EditMessage(edit) => {
            env.insert("editMessage".into(), edit_json(dir, edit));
        }
        ContentBody::ReceiptMessage(receipt) => {
            let kind = receipt.r#type.and_then(|t| ReceiptType::try_from(t).ok()).unwrap_or(ReceiptType::Delivery);
            env.insert(
                "receiptMessage".into(),
                json!({
                    "when": content.metadata.client_timestamp.timestamp_millis(),
                    "isDelivery": kind == ReceiptType::Delivery,
                    "isRead": kind == ReceiptType::Read,
                    "isViewed": kind == ReceiptType::Viewed,
                    "timestamps": receipt.timestamp,
                }),
            );
        }
        ContentBody::TypingMessage(typing) => {
            let started = typing.action.and_then(|a| Action::try_from(a).ok()) != Some(Action::Stopped);
            // TypingMessage carries the group identifier itself, not the master key.
            let group = typing.group_id.as_deref().map(|g| BASE64_STANDARD.encode(g));
            env.insert(
                "typingMessage".into(),
                json!({
                    "action": if started { "STARTED" } else { "STOPPED" },
                    "timestamp": typing.timestamp,
                    "groupId": group,
                }),
            );
        }
        ContentBody::SynchronizeMessage(sync) => {
            let Some(SyncContent::Sent(sent)) = &sync.content else { return None };
            env.insert("syncMessage".into(), json!({"sentMessage": sent_json(dir, sent)}));
        }
        _ => return None,
    }
    Some(Value::Object(env))
}

#[cfg(test)]
mod tests {
    use super::*;
    use presage::proto::body_range::{AssociatedValue, Style};
    use presage::libsignal_service::protocol::ServiceId;
    use presage::proto::{BodyRange, ReceiptMessage, SyncMessage, TypingMessage};

    const ALICE: &str = "11111111-1111-4111-8111-111111111111";

    fn dir() -> Directory {
        let mut d = Directory::default();
        d.people.insert(
            Uuid::parse_str(ALICE).unwrap(),
            Person { number: Some("+491111".into()), name: Some("Alice".into()) },
        );
        d
    }

    fn content(body: ContentBody) -> Content {
        let meta = Metadata {
            sender: ServiceId::Aci(Uuid::parse_str(ALICE).unwrap().into()),
            destination: ServiceId::Aci(Uuid::nil().into()),
            sender_device: 1u8.try_into().unwrap(),
            pni_verified: None,
            client_timestamp: chrono::DateTime::from_timestamp_millis(1_700_000_000_000).unwrap(),
            server_timestamp: chrono::DateTime::from_timestamp_millis(1_700_000_000_100).unwrap(),
            needs_receipt: false,
            unidentified_sender: true,
            was_plaintext: false,
            server_guid: None,
        };
        Content { metadata: meta, body }
    }

    #[test]
    fn text_message_with_style_and_attachment() {
        let dm = DataMessage {
            body: Some("hello world".into()),
            timestamp: Some(1_700_000_000_000),
            body_ranges: vec![BodyRange {
                start: Some(0),
                length: Some(5),
                associated_value: Some(AssociatedValue::Style(Style::Bold as i32)),
            }],
            attachments: vec![AttachmentPointer {
                attachment_identifier: Some(AttachmentIdentifier::CdnKey("ab/c+d".into())),
                content_type: Some("image/png".into()),
                file_name: Some("x.png".into()),
                size: Some(42),
                width: Some(10),
                height: Some(20),
                ..Default::default()
            }],
            ..Default::default()
        };
        let env = to_envelope(&dir(), &content(ContentBody::DataMessage(dm))).unwrap();
        assert_eq!(env["sourceNumber"], "+491111");
        assert_eq!(env["sourceUuid"], ALICE);
        assert_eq!(env["sourceName"], "Alice");
        assert_eq!(env["timestamp"], 1_700_000_000_000i64);
        let d = &env["dataMessage"];
        assert_eq!(d["message"], "hello world");
        assert_eq!(d["textStyle"], json!(["0:5:BOLD"]));
        assert_eq!(d["attachments"][0]["id"], "ab_c_d");
        assert_eq!(d["attachments"][0]["contentType"], "image/png");
        assert_eq!(d["attachments"][0]["size"], 42);
    }

    #[test]
    fn reaction_uses_binary_aci() {
        let dm = DataMessage {
            reaction: Some(Reaction {
                emoji: Some("👍".into()),
                remove: Some(false),
                target_author_aci_binary: Some(Uuid::parse_str(ALICE).unwrap().as_bytes().to_vec()),
                target_sent_timestamp: Some(5),
                ..Default::default()
            }),
            ..Default::default()
        };
        let env = to_envelope(&dir(), &content(ContentBody::DataMessage(dm))).unwrap();
        let r = &env["dataMessage"]["reaction"];
        assert_eq!(r["targetAuthorNumber"], "+491111");
        assert_eq!(r["targetAuthorUuid"], ALICE);
        assert_eq!(r["targetSentTimestamp"], 5);
        assert_eq!(r["isRemove"], false);
    }

    #[test]
    fn receipt_and_typing() {
        let receipt = ReceiptMessage { r#type: Some(ReceiptType::Read as i32), timestamp: vec![1, 2] };
        let env = to_envelope(&dir(), &content(ContentBody::ReceiptMessage(receipt))).unwrap();
        assert_eq!(env["receiptMessage"]["isRead"], true);
        assert_eq!(env["receiptMessage"]["timestamps"], json!([1, 2]));

        let typing = TypingMessage { timestamp: Some(1), action: Some(Action::Stopped as i32), group_id: Some(vec![1, 2, 3]) };
        let env = to_envelope(&dir(), &content(ContentBody::TypingMessage(typing))).unwrap();
        assert_eq!(env["typingMessage"]["action"], "STOPPED");
        assert_eq!(env["typingMessage"]["groupId"], "AQID");
    }

    #[test]
    fn sync_sent_edit() {
        let sent = Sent {
            destination_service_id: Some(ALICE.into()),
            timestamp: Some(9),
            edit_message: Some(EditMessage {
                target_sent_timestamp: Some(7),
                data_message: Some(DataMessage { body: Some("fixed".into()), timestamp: Some(9), ..Default::default() }),
            }),
            ..Default::default()
        };
        let sync = SyncMessage { content: Some(SyncContent::Sent(sent)), ..Default::default() };
        let env = to_envelope(&dir(), &content(ContentBody::SynchronizeMessage(sync))).unwrap();
        let s = &env["syncMessage"]["sentMessage"];
        assert_eq!(s["destinationNumber"], "+491111");
        assert_eq!(s["editMessage"]["targetSentTimestamp"], 7);
        assert_eq!(s["editMessage"]["dataMessage"]["message"], "fixed");
    }

    #[test]
    fn group_id_is_stable_base64() {
        let id = group_id(&[7u8; 32]).unwrap();
        assert_eq!(BASE64_STANDARD.decode(&id).unwrap().len(), 32);
        assert_eq!(group_id(&[7u8; 32]), Some(id));
        assert_eq!(group_id(&[1u8; 3]), None);
    }
}

// signal-cli's sticker commands: listStickerPacks, addStickerPack, getSticker, and the sticker
// option of `send`.

use base64::prelude::*;
use presage::libsignal_service::sender::AttachmentSpec;
use presage::proto::data_message::Sticker;
use presage::store::ContentsStore;
use serde_json::{json, Value};

use crate::daemon::Daemon;
use crate::rpc::{Params, RpcError, RpcResult};

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn unhex(text: &str) -> Option<Vec<u8>> {
    if text.len() % 2 != 0 {
        return None;
    }
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(text.get(i..i + 2)?, 16).ok()).collect()
}

/// "https://signal.art/addstickers/#pack_id=…&pack_key=…" -> (id, key)
pub fn parse_pack_uri(uri: &str) -> Option<(Vec<u8>, Vec<u8>)> {
    let fragment = uri.split_once('#')?.1;
    let (mut id, mut key) = (None, None);
    for pair in fragment.split('&') {
        match pair.split_once('=')? {
            ("pack_id", v) => id = unhex(v),
            ("pack_key", v) => key = unhex(v),
            _ => {}
        }
    }
    Some((id?, key?))
}

/// File name under which the image of a received sticker is kept.
pub fn received_sticker_file(pack_id: &[u8], sticker_id: u32) -> String {
    format!("sticker-{}-{}", hex(pack_id), sticker_id)
}

impl Daemon {
    pub(crate) async fn list_sticker_packs(&self) -> RpcResult {
        let mut out = Vec::new();
        for pack in self.store().sticker_packs().await?.flatten() {
            let stickers: Vec<Value> = pack
                .manifest
                .stickers
                .iter()
                .map(|s| json!({"id": s.id, "emoji": s.emoji, "contentType": s.content_type}))
                .collect();
            out.push(json!({
                "packId": hex(&pack.id),
                "url": format!("https://signal.art/addstickers/#pack_id={}&pack_key={}", hex(&pack.id), hex(&pack.key)),
                "installed": true,
                "title": pack.manifest.title,
                "author": pack.manifest.author,
                "stickers": stickers,
            }));
        }
        Ok(Value::Array(out))
    }

    pub(crate) async fn add_sticker_pack(&self, p: &Params) -> RpcResult {
        let uri = p.required_str("uri")?;
        let (id, key) = parse_pack_uri(uri).ok_or_else(|| RpcError::failed("Sticker pack link is invalid"))?;
        let _guard = self.send_lock.lock().await;
        self.manager.clone().install_sticker_pack(&id, &key).await?;
        Ok(json!({}))
    }

    /// From an installed pack, else the copy saved when the sticker was received.
    async fn sticker_bytes(&self, pack_id: &[u8], sticker_id: u32) -> Option<(Vec<u8>, Option<String>)> {
        if let Ok(Some(pack)) = self.store().sticker_pack(pack_id).await {
            if let Some(s) = pack.manifest.stickers.iter().find(|s| s.id == sticker_id) {
                if let Some(bytes) = &s.bytes {
                    return Some((bytes.clone(), s.content_type.clone()));
                }
            }
        }
        let path = self.attachments_path(&received_sticker_file(pack_id, sticker_id));
        tokio::fs::read(path).await.ok().map(|b| (b, None))
    }

    pub(crate) async fn get_sticker(&self, p: &Params) -> RpcResult {
        let pack_id = unhex(p.required_str("packId")?).ok_or_else(|| RpcError::invalid("invalid packId"))?;
        let sticker_id = u32::try_from(p.required_u64("stickerId")?).map_err(|_| RpcError::invalid("invalid stickerId"))?;
        let (bytes, _) = self
            .sticker_bytes(&pack_id, sticker_id)
            .await
            .ok_or_else(|| RpcError::failed("Could not find sticker pack"))?;
        Ok(json!({"data": BASE64_STANDARD.encode(bytes)}))
    }

    /// The sticker of a `send` ("packId:stickerId"), with its image uploaded as Signal's clients do.
    pub(crate) async fn sticker_for_send(&self, spec: &str) -> Result<Sticker, RpcError> {
        let (pack, id) = spec.split_once(':').ok_or_else(|| RpcError::invalid("sticker must be packId:stickerId"))?;
        let pack_id = unhex(pack).ok_or_else(|| RpcError::invalid("invalid sticker pack id"))?;
        let sticker_id: u32 = id.parse().map_err(|_| RpcError::invalid("invalid sticker id"))?;
        let pack = self
            .store()
            .sticker_pack(&pack_id)
            .await?
            .ok_or_else(|| RpcError::failed("Sticker pack not installed"))?;
        let sticker = pack
            .manifest
            .stickers
            .iter()
            .find(|s| s.id == sticker_id)
            .ok_or_else(|| RpcError::failed("Sticker not found in pack"))?;
        let bytes = sticker.bytes.clone().ok_or_else(|| RpcError::failed("Sticker image not downloaded"))?;
        let spec = AttachmentSpec {
            content_type: sticker.content_type.clone().unwrap_or_else(|| "image/webp".into()),
            length: bytes.len(),
            file_name: None,
            preview: None,
            voice_note: None,
            borderless: Some(true),
            width: None,
            height: None,
            caption: None,
            blur_hash: None,
        };
        let pointer = self
            .manager
            .upload_attachments(vec![(spec, bytes)])
            .await?
            .into_iter()
            .next()
            .ok_or_else(|| RpcError::failed("sticker upload failed"))?
            .map_err(|e| RpcError::failed(format!("sticker upload failed: {e}")))?;
        Ok(Sticker {
            pack_id: Some(pack.id.clone()),
            pack_key: Some(pack.key.clone()),
            sticker_id: Some(sticker_id),
            data: Some(pointer),
            emoji: sticker.emoji.clone(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pack_links() {
        let (id, key) = parse_pack_uri("https://signal.art/addstickers/#pack_id=0a1b&pack_key=ff00").unwrap();
        assert_eq!(id, [0x0a, 0x1b]);
        assert_eq!(key, [0xff, 0x00]);
        assert!(parse_pack_uri("https://signal.art/addstickers/#pack_id=0a1b").is_none());
        assert!(parse_pack_uri("https://example.org").is_none());
        assert!(parse_pack_uri("https://signal.art/addstickers/#pack_id=0g&pack_key=00").is_none());
        assert_eq!(received_sticker_file(&[1, 2], 3), "sticker-0102-3");
    }
}

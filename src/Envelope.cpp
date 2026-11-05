#include "Envelope.h"

#include <QJsonArray>
#include <algorithm>


static QList<TextStyleRange> parseTextStyles(const QJsonArray &arr)
{
    QList<TextStyleRange> styles;
    for (const QJsonValue &v : arr) {
        const QStringList p = v.toString().split(QLatin1Char(':'));
        if (p.size() != 3) continue;
        bool ok1 = false, ok2 = false;
        TextStyleRange r;
        r.start = p.at(0).toInt(&ok1);
        r.length = p.at(1).toInt(&ok2);
        r.style = p.at(2);
        if (ok1 && ok2 && r.start >= 0 && r.length > 0 && !r.style.isEmpty()) styles.append(r);
    }
    return styles;
}

static QList<Attachment> parseAttachments(const QJsonObject &dm)
{
    QList<Attachment> list;
    for (const QJsonValue &v : dm["attachments"].toArray()) {
        const QJsonObject o = v.toObject();
        Attachment a;
        a.id = o["id"].toString();
        a.contentType = o["contentType"].toString();
        a.filename = o["filename"].toString();
        a.size = o["size"].toVariant().toLongLong();
        a.width = o["width"].toInt();
        a.height = o["height"].toInt();
        list.append(a);
    }
    return list;
}

static LinkPreview parseLinkPreview(const QJsonObject &dm)
{
    const QJsonArray previews = dm["previews"].toArray();
    for (const QJsonValue &value : previews) {
        const QJsonObject o = value.toObject();
        LinkPreview lp;
        lp.url = o["url"].toString();
        lp.title = o["title"].toString();
        lp.description = o["description"].toString();
        const QJsonObject image = o["image"].toObject();
        lp.imageId = image["id"].toString();
        lp.imageContentType = image["contentType"].toString();
        lp.imageFilename = image["filename"].toString();
        lp.imageSize = image["size"].toVariant().toLongLong();
        if (lp.isValid()) return lp;
    }
    return {};
}

static Quote parseQuote(const QJsonObject &dm)
{
    Quote q;
    const QJsonObject o = dm["quote"].toObject();
    if (o.isEmpty()) return q;
    q.valid = true;
    q.id = o["id"].toVariant().toLongLong();
    q.authorKey = o["authorNumber"].toString();
    if (q.authorKey.isEmpty()) q.authorKey = o["authorUuid"].toString();
    if (q.authorKey.isEmpty()) q.authorKey = o["author"].toString();
    q.text = o["text"].toString();
    const QJsonArray atts = o["attachments"].toArray();
    if (!atts.isEmpty()) q.attachmentNote = atts.first().toObject()["contentType"].toString();
    return q;
}

static QList<MentionRange> parseMentions(const QJsonObject &dm, const QString &text)
{
    QList<MentionRange> mentions;
    QJsonArray ranges = dm.value(QStringLiteral("mentions")).toArray();
    if (ranges.isEmpty()) ranges = dm.value(QStringLiteral("bodyRanges")).toArray();
    for (const QJsonValue &value : ranges) {
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) continue;
        MentionRange range;
        range.start = object.value(QStringLiteral("start")).toInt(-1);
        range.length = object.value(QStringLiteral("length")).toInt(0);
        range.uuid = object.value(QStringLiteral("uuid")).toString();
        if (range.uuid.isEmpty()) range.uuid = object.value(QStringLiteral("recipient")).toString();
        if (range.uuid.isEmpty()) range.uuid = object.value(QStringLiteral("recipientUuid")).toString();
        if (range.uuid.isEmpty()) range.uuid = object.value(QStringLiteral("recipientNumber")).toString();
        if (range.uuid.isEmpty()) range.uuid = object.value(QStringLiteral("mentionUuid")).toString();
        if (range.uuid.isEmpty()) range.uuid = object.value(QStringLiteral("aci")).toString();
        if (range.uuid.isEmpty()) range.uuid = object.value(QStringLiteral("mentionAci")).toString();
        if (range.uuid.isEmpty()) range.uuid = object.value(QStringLiteral("number")).toString();
        const QJsonValue recipientValue = object.value(QStringLiteral("recipient"));
        const QJsonObject recipientObject = recipientValue.toObject();
        if (range.uuid.isEmpty()) range.uuid = recipientObject.value(QStringLiteral("uuid")).toString();
        if (range.uuid.isEmpty()) range.uuid = recipientObject.value(QStringLiteral("number")).toString();
        const QJsonObject nested = object.value(QStringLiteral("mention")).toObject();
        if (range.uuid.isEmpty()) range.uuid = nested.value(QStringLiteral("uuid")).toString();
        if (range.uuid.isEmpty()) range.uuid = nested.value(QStringLiteral("aci")).toString();
        if (range.uuid.isEmpty()) range.uuid = nested.value(QStringLiteral("number")).toString();
        if (range.start >= 0 && range.length > 0 && range.start < text.size()) {
            range.length = qMin(range.length, text.size() - range.start);
            mentions.append(range);
        }
    }

    // Some signal-cli builds expose the Signal Object Replacement Character but omit
    // mention metadata. Preserve the placeholder so the UI can render a safe generic label.
    for (int pos = 0; pos < text.size(); ++pos) {
        if (text.at(pos) != QChar(0xfffc)) continue;
        bool covered = false;
        for (const MentionRange &range : mentions) {
            if (pos >= range.start && pos < range.start + range.length) { covered = true; break; }
        }
        if (!covered) mentions.append({pos, 1, QString()});
    }
    std::sort(mentions.begin(), mentions.end(), [](const MentionRange &a, const MentionRange &b) {
        return a.start < b.start;
    });
    return mentions;
}

// Fills reaction or message data from a dataMessage-like object (also used for sync sentMessage).
static void parseBody(const QJsonObject &dm, ParsedEnvelope &out, qint64 envelopeTs)
{
    out.groupId = dm["groupInfo"].toObject()["groupId"].toString();
    out.expirationSeconds = dm["expiresInSeconds"].toInt();
    if (dm["isExpirationUpdate"].toBool()) {
        out.kind = ParsedEnvelope::ExpirationUpdate;
        return;
    }
    // signal-cli: {"remoteDelete": {"timestamp": <sent timestamp of the deleted message>}}
    const QJsonObject del = dm["remoteDelete"].toObject();
    if (!del.isEmpty()) {
        out.targetTs = del["timestamp"].toVariant().toLongLong();
        if (out.targetTs > 0) out.kind = ParsedEnvelope::Delete;
        return;
    }
    const QJsonObject r = dm["reaction"].toObject();
    if (!r.isEmpty()) {
        out.kind = ParsedEnvelope::Reaction;
        out.emoji = r["emoji"].toString();
        out.remove = r["isRemove"].toBool();
        out.targetNumber = r["targetAuthorNumber"].toString();
        if (out.targetNumber.isEmpty()) out.targetNumber = r["targetAuthor"].toString();
        out.targetUuid = r["targetAuthorUuid"].toString();
        out.targetTs = r["targetSentTimestamp"].toVariant().toLongLong();
        return;
    }

    out.msg.viewOnce = dm["viewOnce"].toBool();
    out.msg.text = dm["message"].toString();
    out.msg.textStyles = parseTextStyles(dm["textStyle"].toArray());
    out.msg.mentions = parseMentions(dm, out.msg.text);
    out.msg.attachments = parseAttachments(dm);
    out.msg.linkPreview = out.msg.viewOnce ? LinkPreview() : parseLinkPreview(dm);
    const QJsonObject sticker = dm["sticker"].toObject();
    if (!sticker.isEmpty()) {
        QString packId = sticker["packId"].toString();
        if (packId.isEmpty()) packId = sticker["packID"].toString();
        if (packId.isEmpty()) packId = sticker["pack_id"].toString();
        QJsonValue stickerIdValue = sticker["stickerId"];
        if (stickerIdValue.isUndefined()) stickerIdValue = sticker["sticker_id"];
        if (stickerIdValue.isUndefined()) stickerIdValue = sticker["id"];
        bool idOk = false;
        int stickerId = stickerIdValue.toInt(-1);
        if (stickerId < 0 && stickerIdValue.isString()) stickerId = stickerIdValue.toString().toInt(&idOk);
        else idOk = stickerId >= 0;
        if (idOk && stickerId >= 0 && !packId.isEmpty()) {
            Attachment a;
            a.contentType = QStringLiteral("image/webp");
            a.filename = QStringLiteral("signal-sticker:%1:%2.webp").arg(packId).arg(stickerId);
            out.msg.attachments.append(a);
        }
    }
    out.msg.quote = parseQuote(dm);
    out.msg.expiresInSeconds = out.expirationSeconds;
    if (out.msg.text.isEmpty() && out.msg.attachments.isEmpty()) return; // receipts, typing, sessions, ...

    qint64 ts = dm["timestamp"].toVariant().toLongLong();
    if (ts <= 0) ts = envelopeTs;
    out.msg.time = ts > 0 ? QDateTime::fromMSecsSinceEpoch(ts) : QDateTime::currentDateTime();
    out.kind = ParsedEnvelope::Message_;
}

// signal-cli: {"editMessage": {"targetSentTimestamp": ..., "dataMessage": {...}}}
// (top level for incoming edits, inside syncMessage.sentMessage for edits from our other devices).
// Only the text is taken over; attachments of an edit are not changed by the official clients either.
static bool parseEdit(const QJsonObject &edit, ParsedEnvelope &out, qint64 envelopeTs)
{
    const QJsonObject dm = edit["dataMessage"].toObject();
    const qint64 target = edit["targetSentTimestamp"].toVariant().toLongLong();
    const QString text = dm["message"].toString();
    if (dm.isEmpty() || target <= 0 || text.isEmpty()) return false;
    qint64 ts = dm["timestamp"].toVariant().toLongLong();
    if (ts <= 0) ts = envelopeTs;
    out.kind = ParsedEnvelope::Edit;
    out.targetTs = target;
    out.msg.text = text;
    out.msg.textStyles = parseTextStyles(dm["textStyle"].toArray());
    out.msg.mentions = parseMentions(dm, out.msg.text);
    out.msg.time = ts > 0 ? QDateTime::fromMSecsSinceEpoch(ts) : QDateTime::currentDateTime();
    out.groupId = dm["groupInfo"].toObject()["groupId"].toString();
    return true;
}

ParsedEnvelope parseEnvelope(const QJsonObject &env)
{
    ParsedEnvelope out;
    out.src = env["sourceNumber"].toString();
    if (out.src.isEmpty()) out.src = env["sourceUuid"].toString();
    if (out.src.isEmpty()) out.src = env["source"].toString();
    out.srcName = env["sourceName"].toString();
    const qint64 envTs = env["timestamp"].toVariant().toLongLong();

    // signal-cli emits receipt and typing events as distinct envelope members. They
    // are not data messages and must be handled before the empty-message filter below.
    const QJsonObject receipt = env.value(QStringLiteral("receiptMessage")).toObject();
    if (!receipt.isEmpty()) {
        out.kind = ParsedEnvelope::Receipt;
        out.receiptTimeMs = envTs;
        out.readReceipt = receipt.value(QStringLiteral("isRead")).toBool() ||
                          receipt.value(QStringLiteral("isViewed")).toBool();
        out.deliveryReceipt = receipt.value(QStringLiteral("isDelivery")).toBool();
        for (const QJsonValue &value : receipt.value(QStringLiteral("timestamps")).toArray()) {
            bool ok = false;
            qint64 timestamp = value.toVariant().toLongLong(&ok);
            if (!ok && value.isString()) timestamp = value.toString().toLongLong(&ok);
            if (ok && timestamp > 0) out.receiptTimestamps.append(timestamp);
        }
        return out;
    }

    const QJsonObject typing = env.value(QStringLiteral("typingMessage")).toObject();
    if (!typing.isEmpty()) {
        out.kind = ParsedEnvelope::Typing;
        QJsonValue action = typing.value(QStringLiteral("action"));
        if (action.isUndefined()) action = typing.value(QStringLiteral("type"));
        const QString actionText = action.toString().toUpper();
        out.typingStarted = actionText == QLatin1String("STARTED") || actionText == QLatin1String("START");
        out.groupId = typing.value(QStringLiteral("groupId")).toString();
        if (out.groupId.isEmpty())
            out.groupId = env.value(QStringLiteral("groupInfo")).toObject().value(QStringLiteral("groupId")).toString();
        if (out.groupId.isEmpty()) out.groupId = env.value(QStringLiteral("groupId")).toString();
        return out;
    }

    const QJsonObject dm = env["dataMessage"].toObject();
    if (!dm.isEmpty()) {
        parseBody(dm, out, envTs);
        return out;
    }

    const QJsonObject edit = env["editMessage"].toObject();
    if (!edit.isEmpty()) {
        parseEdit(edit, out, envTs);
        return out;
    }

    const QJsonObject sent = env["syncMessage"].toObject()["sentMessage"].toObject();
    if (!sent.isEmpty()) {
        out.outgoing = true;
        out.destination = sent["destinationNumber"].toString();
        if (out.destination.isEmpty()) out.destination = sent["destination"].toString();
        if (out.destination.isEmpty()) out.destination = sent["destinationUuid"].toString();
        const QJsonObject sentEdit = sent["editMessage"].toObject();
        if (!sentEdit.isEmpty())
            parseEdit(sentEdit, out, envTs);
        else
            parseBody(sent, out, envTs);
        out.msg.outgoing = true;
    }
    return out;
}

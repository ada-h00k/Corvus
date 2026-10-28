#include "Envelope.h"

#include <QJsonArray>


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
    out.msg.attachments = parseAttachments(dm);
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

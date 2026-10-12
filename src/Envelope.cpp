#include "Envelope.h"

#include <QJsonArray>

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

    out.msg.text = dm["message"].toString();
    out.msg.attachments = parseAttachments(dm);
    out.msg.quote = parseQuote(dm);
    if (out.msg.text.isEmpty() && out.msg.attachments.isEmpty()) return; // receipts, typing, sessions, ...

    qint64 ts = dm["timestamp"].toVariant().toLongLong();
    if (ts <= 0) ts = envelopeTs;
    out.msg.time = ts > 0 ? QDateTime::fromMSecsSinceEpoch(ts) : QDateTime::currentDateTime();
    out.groupId = dm["groupInfo"].toObject()["groupId"].toString();
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

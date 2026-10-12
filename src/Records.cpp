#include "Records.h"

#include <QJsonArray>
#include <QJsonValue>

namespace Records {

static QJsonObject attachmentToJson(const Attachment &a)
{
    QJsonObject o;
    if (!a.id.isEmpty()) o["id"] = a.id;
    if (!a.contentType.isEmpty()) o["ct"] = a.contentType;
    if (!a.filename.isEmpty()) o["fn"] = a.filename;
    if (!a.path.isEmpty()) o["p"] = a.path;
    if (a.size > 0) o["sz"] = QJsonValue(static_cast<qint64>(a.size));
    if (a.width > 0) o["w"] = a.width;
    if (a.height > 0) o["h"] = a.height;
    if (!a.thumb.isEmpty()) o["th"] = QString::fromLatin1(a.thumb.toBase64());
    return o;
}

static Attachment attachmentFromJson(const QJsonObject &o)
{
    Attachment a;
    a.id = o["id"].toString();
    a.contentType = o["ct"].toString();
    a.filename = o["fn"].toString();
    a.path = o["p"].toString();
    a.size = o["sz"].toVariant().toLongLong();
    a.width = o["w"].toInt();
    a.height = o["h"].toInt();
    a.thumb = QByteArray::fromBase64(o["th"].toString().toLatin1());
    return a;
}

static QJsonArray attachmentsToJson(const QList<Attachment> &list)
{
    QJsonArray arr;
    for (const Attachment &a : list) arr.append(attachmentToJson(a));
    return arr;
}

// Edit state: x = current text, et = timestamp of the latest revision, ec = number of edits.
static QJsonObject editToJson(const Message &m)
{
    return {{"x", m.text}, {"et", QJsonValue(static_cast<qint64>(m.editTs))}, {"ec", m.editCount}};
}

QJsonObject message(const QString &conv, const Message &m)
{
    QJsonObject d;
    if (m.expiresInSeconds > 0) {
        d["ex"] = m.expiresInSeconds;
        d["es"] = QJsonValue(static_cast<qint64>(m.expirationStartMs));
    }
    if (m.quote.valid) {
        d["q"] = QJsonObject{{"id", QJsonValue(static_cast<qint64>(m.quote.id))},
                             {"a", m.quote.authorKey},
                             {"t", m.quote.text},
                             {"n", m.quote.attachmentNote}};
    }
    if (!m.attachments.isEmpty()) d["at"] = attachmentsToJson(m.attachments);

    QJsonObject rec{{"t", "m"},
                    {"c", conv},
                    {"ts", QJsonValue(static_cast<qint64>(m.ts()))},
                    {"s", m.sender},
                    {"x", m.text},
                    {"o", m.outgoing},
                    {"au", m.authorKey}};
    if (m.editCount > 0) d["ed"] = editToJson(m);
    if (!d.isEmpty()) rec["d"] = d;
    return rec;
}

QJsonObject edit(const QString &conv, const Message &m)
{
    // Stored as an "update" record (merged into the extras of the message): the history file
    // format needs no new record type, and older versions simply ignore the "ed" extra.
    return {{"t", "u"},
            {"c", conv},
            {"ts", QJsonValue(static_cast<qint64>(m.ts()))},
            {"au", m.authorKey},
            {"d", QJsonObject{{"ed", editToJson(m)}}}};
}

QJsonObject expirationUpdate(const QString &conv, const Message &m)
{
    return {{"t", "u"},
            {"c", conv},
            {"ts", QJsonValue(static_cast<qint64>(m.ts()))},
            {"au", m.authorKey},
            {"d", QJsonObject{{"ex", m.expiresInSeconds},
                               {"es", QJsonValue(static_cast<qint64>(m.expirationStartMs))}}}};
}

void applyExtras(Message &m, const QJsonObject &d)
{
    if (d.contains("q")) {
        const QJsonObject q = d["q"].toObject();
        m.quote.valid = true;
        m.quote.id = q["id"].toVariant().toLongLong();
        m.quote.authorKey = q["a"].toString();
        m.quote.text = q["t"].toString();
        m.quote.attachmentNote = q["n"].toString();
    }
    if (d.contains("ed")) {
        const QJsonObject e = d["ed"].toObject();
        if (e.contains("x")) m.text = e["x"].toString();
        m.editTs = e["et"].toVariant().toLongLong();
        m.editCount = e["ec"].toInt();
    }
    if (d.contains("ex")) {
        m.expiresInSeconds = d["ex"].toInt();
        m.expirationStartMs = d["es"].toVariant().toLongLong();
    }
    if (d.contains("at")) {
        m.attachments.clear();
        for (const QJsonValue &v : d["at"].toArray()) m.attachments.append(attachmentFromJson(v.toObject()));
    }
}

Message messageFromRecord(const QJsonObject &rec)
{
    Message m;
    m.time = QDateTime::fromMSecsSinceEpoch(rec["ts"].toVariant().toLongLong());
    m.sender = rec["s"].toString();
    m.text = rec["x"].toString();
    m.outgoing = rec["o"].toBool();
    m.authorKey = rec["au"].toString();
    applyExtras(m, rec["d"].toObject());
    return m;
}

QJsonObject attachmentsUpdate(const QString &conv, const Message &m)
{
    return {{"t", "u"},
            {"c", conv},
            {"ts", QJsonValue(static_cast<qint64>(m.ts()))},
            {"au", m.authorKey},
            {"d", QJsonObject{{"at", attachmentsToJson(m.attachments)}}}};
}

QJsonObject reaction(const QString &conv, qint64 ts, const QString &author, const QString &by, const QString &emoji)
{
    return {{"t", "r"}, {"c", conv}, {"ts", QJsonValue(static_cast<qint64>(ts))}, {"au", author}, {"by", by}, {"e", emoji}};
}

QJsonObject archive(const QString &conv, bool archived, bool sigArchived)
{
    return {{"t", "a"}, {"c", conv}, {"v", archived}, {"s", sigArchived}};
}

QJsonObject mute(const QString &conv, bool muted)
{
    return {{"t", "k"}, {"c", conv}, {"v", muted}};
}

QJsonObject expiration(const QString &conv, int seconds)
{
    return {{"t", "e"}, {"c", conv}, {"v", seconds}};
}

QJsonObject name(const QString &conv, const QString &n)
{
    return {{"t", "n"}, {"c", conv}, {"n", n}};
}

} // namespace Records

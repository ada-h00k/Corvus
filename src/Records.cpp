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


static QJsonArray textStylesToJson(const QList<TextStyleRange> &styles)
{
    QJsonArray arr;
    for (const TextStyleRange &r : styles) {
        if (r.start >= 0 && r.length > 0 && !r.style.isEmpty())
            arr.append(QStringLiteral("%1:%2:%3").arg(r.start).arg(r.length).arg(r.style));
    }
    return arr;
}

static QList<TextStyleRange> textStylesFromJson(const QJsonArray &arr)
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

static QJsonArray attachmentsToJson(const QList<Attachment> &list)
{
    QJsonArray arr;
    for (const Attachment &a : list) arr.append(attachmentToJson(a));
    return arr;
}

// Edit state: x = current text, et = timestamp of the latest revision, ec = number of edits.
static QJsonObject editToJson(const Message &m)
{
    QJsonObject e{{"x", m.text}, {"et", QJsonValue(static_cast<qint64>(m.editTs))}, {"ec", m.editCount}};
    if (!m.textStyles.isEmpty()) e["st"] = textStylesToJson(m.textStyles);
    return e;
}

QJsonObject message(const QString &conv, const Message &m)
{
    QJsonObject d;
    if (m.orderTs > 0) d["ot"] = QJsonValue(static_cast<qint64>(m.orderTs));
    if (m.deleted) d["dl"] = true; // placeholder only: no quote, attachments or edit state
    if (m.expiresInSeconds > 0) {
        d["ex"] = m.expiresInSeconds;
        d["es"] = QJsonValue(static_cast<qint64>(m.expirationStartMs));
    }
    if (m.viewOnce) {
        d["vo"] = true;
        if (m.viewOnceViewed) d["vv"] = true;
        if (m.viewOnceExpired) d["ve"] = true;
    }
    if (m.quote.valid && !m.deleted) {
        d["q"] = QJsonObject{{"id", QJsonValue(static_cast<qint64>(m.quote.id))},
                             {"a", m.quote.authorKey},
                             {"t", m.quote.text},
                             {"n", m.quote.attachmentNote}};
    }
    // Never retain sent or consumed View Once media in local history. For an incoming
    // unopened item only the opaque signal-cli attachment reference is persisted.
    const bool keepViewOnceRef = !m.viewOnce || (!m.outgoing && !m.viewOnceViewed && !m.viewOnceExpired);
    if (!m.attachments.isEmpty() && !m.deleted && keepViewOnceRef) {
        QList<Attachment> safeAttachments = m.attachments;
        if (m.viewOnce) for (Attachment &a : safeAttachments) { a.thumb.clear(); a.path.clear(); }
        d["at"] = attachmentsToJson(safeAttachments);
    }
    if (!m.textStyles.isEmpty() && !m.deleted) d["st"] = textStylesToJson(m.textStyles);

    QJsonObject rec{{"t", "m"},
                    {"c", conv},
                    {"ts", QJsonValue(static_cast<qint64>(m.ts()))},
                    {"s", m.sender},
                    {"x", m.deleted ? QString() : m.text},
                    {"o", m.outgoing},
                    {"au", m.authorKey}};
    if (m.editCount > 0 && !m.deleted) d["ed"] = editToJson(m);
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

QJsonObject deletion(const QString &conv, const Message &m)
{
    return {{"t", "u"},
            {"c", conv},
            {"ts", QJsonValue(static_cast<qint64>(m.ts()))},
            {"au", m.authorKey},
            {"d", QJsonObject{{"dl", true}}}};
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
    if (d.contains("ot")) m.orderTs = d["ot"].toVariant().toLongLong();
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
        if (e.contains("st")) m.textStyles = textStylesFromJson(e["st"].toArray());
        m.editTs = e["et"].toVariant().toLongLong();
        m.editCount = e["ec"].toInt();
    }
    if (d.contains("ex")) {
        m.expiresInSeconds = d["ex"].toInt();
        m.expirationStartMs = d["es"].toVariant().toLongLong();
    }
    if (d.contains("vo")) m.viewOnce = d["vo"].toBool();
    if (d.contains("vv")) m.viewOnceViewed = d["vv"].toBool();
    if (d.contains("ve")) m.viewOnceExpired = d["ve"].toBool();
    if (d.contains("st")) m.textStyles = textStylesFromJson(d["st"].toArray());
    if (d.contains("at")) {
        m.attachments.clear();
        for (const QJsonValue &v : d["at"].toArray()) m.attachments.append(attachmentFromJson(v.toObject()));
    }
    if (m.viewOnce && (m.viewOnceViewed || m.viewOnceExpired)) m.attachments.clear();
    if (d["dl"].toBool()) { // last: wins over anything else in the same record
        m.deleted = true;
        m.text.clear();
        m.textStyles.clear();
        m.attachments.clear();
        m.quote = Quote();
        m.reactions.clear();
        m.viewOnce = false;
        m.viewOnceViewed = false;
        m.viewOnceExpired = false;
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

QJsonObject viewOnceUpdate(const QString &conv, const Message &m)
{
    QJsonObject d{{"vv", m.viewOnceViewed}, {"ve", m.viewOnceExpired}, {"at", QJsonArray{}}};
    return {{"t", "u"},
            {"c", conv},
            {"ts", QJsonValue(static_cast<qint64>(m.ts()))},
            {"au", m.authorKey},
            {"d", d}};
}

QJsonObject reaction(const QString &conv, qint64 ts, const QString &author, const QString &by, const QString &emoji)
{
    return {{"t", "r"}, {"c", conv}, {"ts", QJsonValue(static_cast<qint64>(ts))}, {"au", author}, {"by", by}, {"e", emoji}};
}

QJsonObject archive(const QString &conv, bool archived, bool sigArchived)
{
    return {{"t", "a"}, {"c", conv}, {"v", archived}, {"s", sigArchived}};
}

QJsonObject blocked(const QString &conv, bool blocked)
{
    return {{"t", "b"}, {"c", conv}, {"v", blocked}};
}

QJsonObject requestDeclined(const QString &conv)
{
    return {{"t", "j"}, {"c", conv}};
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

QJsonObject nickname(const QString &conv, const QString &n)
{
    return {{"t", "p"}, {"c", conv}, {"n", n}};
}

} // namespace Records

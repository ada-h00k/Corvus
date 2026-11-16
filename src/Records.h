#pragma once

#include <QJsonObject>

#include "Model.h"

// Conversion between the in-memory model and the JSON records of the
// encrypted history store (see history-store/include/history_store.h).
namespace Records {

QJsonObject message(const QString &conv, const Message &m);
Message messageFromRecord(const QJsonObject &rec);
// Extras ("d": quote, attachments, edit state) as stored in message / update records.
void applyExtras(Message &m, const QJsonObject &d);

// Text edit of a message (an "update" record with the extra "ed"); ts = original timestamp.
QJsonObject edit(const QString &conv, const Message &m);
// Marks a message as deleted for everyone (an "update" record with the extra "dl").
QJsonObject deletion(const QString &conv, const Message &m);
QJsonObject expirationUpdate(const QString &conv, const Message &m);
QJsonObject attachmentsUpdate(const QString &conv, const Message &m);
QJsonObject linkPreviewUpdate(const QString &conv, const Message &m);
// Consumes a View Once media item (or expires it) and removes its attachment reference.
QJsonObject viewOnceUpdate(const QString &conv, const Message &m);
QJsonObject receiptStatusUpdate(const QString &conv, const Message &m);
// Ends a poll early (an "update" record with the extra "pt").
QJsonObject pollTerminate(const QString &conv, const Message &m);
// One poll vote (a "pv" record, like a reaction): ts/au identify the poll message,
// by = voter, oi = the chosen option indexes (empty = vote taken back),
// vc = the per-voter voteCount, vt = timestamp of the vote message.
QJsonObject pollVote(const QString &conv, qint64 ts, const QString &author, const QString &by,
                     const QList<int> &optionIndexes, qint64 voteCount, qint64 voteTs);
QJsonObject reaction(const QString &conv, qint64 ts, const QString &author, const QString &by, const QString &emoji);
// Pin ("rm" = false) or unpin ("rm" = true) of a message: ts/au identify the pinned message,
// by = who pinned it, at = local pin time, d = duration in seconds (-1 = forever).
QJsonObject pin(const QString &conv, qint64 ts, const QString &author, const QString &by,
                qint64 pinnedAtMs, qint64 durationSeconds, bool removed);
QJsonObject archive(const QString &conv, bool archived, bool sigArchived);
QJsonObject blocked(const QString &conv, bool blocked);
// A declined message request (the hidden contact stays hidden and is not offered again).
QJsonObject requestDeclined(const QString &conv);
QJsonObject name(const QString &conv, const QString &name);
QJsonObject nickname(const QString &conv, const QString &nickname);
QJsonObject mute(const QString &conv, bool muted);
QJsonObject expiration(const QString &conv, int seconds);

} // namespace Records

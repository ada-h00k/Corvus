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
QJsonObject reaction(const QString &conv, qint64 ts, const QString &author, const QString &by, const QString &emoji);
QJsonObject archive(const QString &conv, bool archived, bool sigArchived);
QJsonObject blocked(const QString &conv, bool blocked);
// A declined message request (the hidden contact stays hidden and is not offered again).
QJsonObject requestDeclined(const QString &conv);
QJsonObject name(const QString &conv, const QString &name);
QJsonObject nickname(const QString &conv, const QString &nickname);
QJsonObject mute(const QString &conv, bool muted);
QJsonObject expiration(const QString &conv, int seconds);

} // namespace Records

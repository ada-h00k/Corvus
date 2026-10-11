#pragma once

#include <QJsonObject>

#include "Model.h"

// Conversion between the in-memory model and the JSON records of the
// encrypted history store (see history-store/include/history_store.h).
namespace Records {

QJsonObject message(const QString &conv, const Message &m);
Message messageFromRecord(const QJsonObject &rec);
// Extras ("d": quote + attachments) as stored in message / update records.
void applyExtras(Message &m, const QJsonObject &d);

QJsonObject attachmentsUpdate(const QString &conv, const Message &m);
QJsonObject reaction(const QString &conv, qint64 ts, const QString &author, const QString &by, const QString &emoji);
QJsonObject archive(const QString &conv, bool archived, bool sigArchived);
QJsonObject name(const QString &conv, const QString &name);
QJsonObject mute(const QString &conv, bool muted);

} // namespace Records

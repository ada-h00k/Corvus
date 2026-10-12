#pragma once

#include <QString>

namespace HistoryKeychain
{
bool isAvailable();
bool load(QString &secret, QString &error);
bool save(const QString &secret, QString &error);
bool clear(QString &error);
QString generateSecret();
}
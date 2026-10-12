#pragma once

#include <QString>

namespace GiphyApiKeyStore
{
bool load(QString &apiKey, QString &error);
bool save(const QString &apiKey, QString &error);
bool clear(QString &error);
}

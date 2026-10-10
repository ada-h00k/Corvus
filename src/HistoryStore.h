#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "history_store.h"

// Thin C++ wrapper around the Rust encrypted history store (history-store/).
class HistoryStore
{
public:
    HistoryStore() = default;
    ~HistoryStore() { close(); }
    HistoryStore(const HistoryStore &) = delete;
    HistoryStore &operator=(const HistoryStore &) = delete;

    static bool exists(const QString &path);
    static QString errorText(int code);

    // All return SC_OK (0) or an SC_ERR_* code.
    int create(const QString &path, const QString &passphrase);
    int open(const QString &path, const QString &passphrase, QJsonArray *records);
    int append(const QJsonObject &record);
    int rewrite(const QJsonArray &records);
    int changePassphrase(const QString &oldPass, const QString &newPass);
    void close();
    bool isOpen() const { return m_store != nullptr; }

private:
    ScStore *m_store = nullptr;
};

#include "HistoryStore.h"

#include <QCoreApplication>
#include <QJsonDocument>

namespace {
// Passphrase bytes are wiped after use (best effort).
struct Secret {
    QByteArray bytes;
    explicit Secret(const QString &s) : bytes(s.toUtf8()) {}
    ~Secret() { bytes.fill('\0'); }
    const char *c() const { return bytes.constData(); }
};
} // namespace

bool HistoryStore::exists(const QString &path)
{
    return sc_store_exists(path.toUtf8().constData()) == 1;
}

QString HistoryStore::errorText(int code)
{
    switch (code) {
    case SC_OK: return QString();
    case SC_ERR_WRONG_PASSPHRASE: return QCoreApplication::translate("HistoryStore", "Wrong passphrase.");
    case SC_ERR_CORRUPT: return QCoreApplication::translate("HistoryStore", "The history file is damaged or has been modified.");
    case SC_ERR_EXISTS: return QCoreApplication::translate("HistoryStore", "A history file already exists.");
    case SC_ERR_IO: return QCoreApplication::translate("HistoryStore", "The history file could not be read or written.");
    default: return QCoreApplication::translate("HistoryStore", "Encryption error (code %1).").arg(code);
    }
}

int HistoryStore::create(const QString &path, const QString &passphrase)
{
    close();
    Secret pw(passphrase);
    int err = SC_OK;
    m_store = sc_store_create(path.toUtf8().constData(), pw.c(), &err);
    return m_store ? SC_OK : err;
}

int HistoryStore::open(const QString &path, const QString &passphrase, QJsonArray *records)
{
    close();
    Secret pw(passphrase);
    int err = SC_OK;
    char *json = nullptr;
    m_store = sc_store_open(path.toUtf8().constData(), pw.c(), &json, &err);
    if (!m_store) return err;
    if (records && json) *records = QJsonDocument::fromJson(QByteArray(json)).array();
    sc_string_free(json);
    return SC_OK;
}

int HistoryStore::append(const QJsonObject &record)
{
    if (!m_store) return SC_ERR_ARG;
    const QByteArray j = QJsonDocument(record).toJson(QJsonDocument::Compact);
    return sc_store_append(m_store, j.constData());
}

int HistoryStore::rewrite(const QJsonArray &records)
{
    if (!m_store) return SC_ERR_ARG;
    const QByteArray j = QJsonDocument(records).toJson(QJsonDocument::Compact);
    return sc_store_rewrite(m_store, j.constData());
}

int HistoryStore::changePassphrase(const QString &oldPass, const QString &newPass)
{
    if (!m_store) return SC_ERR_ARG;
    Secret o(oldPass), n(newPass);
    return sc_store_change_passphrase(m_store, o.c(), n.c());
}

void HistoryStore::close()
{
    sc_store_close(m_store);
    m_store = nullptr;
}

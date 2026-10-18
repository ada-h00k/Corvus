#include "GiphyApiKeyStore.h"
#include "LegacyMigration.h"

#include <qt6keychain/keychain.h>

#include <QEventLoop>
#include <QSettings>

namespace
{
const QString kService = QStringLiteral("wren");
const QString kKey = QStringLiteral("giphy-api-key");
const QString kLegacySetting = QStringLiteral("giphyApiKey");

void waitForJob(QKeychain::Job &job)
{
    job.setAutoDelete(false);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
}

bool removeLegacySetting(QString &error)
{
    QSettings settings;
    settings.remove(kLegacySetting);
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    error = QObject::tr("Could not remove the old plaintext GIPHY API key from settings.");
    return false;
}

// Entries written before the rename to Wren live under the old service name.
QString legacyService()
{
    return QString::fromLatin1(LegacyMigration::kLegacyGiphyService);
}

bool readLegacyKeychain(QString &apiKey)
{
    QKeychain::ReadPasswordJob job(legacyService());
    job.setKey(kKey);
    job.setInsecureFallback(false);
    waitForJob(job);
    if (job.error() != QKeychain::NoError) return false;
    apiKey = job.textData();
    return true;
}

void deleteLegacyKeychain()
{
    QKeychain::DeletePasswordJob job(legacyService());
    job.setKey(kKey);
    job.setInsecureFallback(false);
    waitForJob(job);
}

bool writeKeychain(const QString &apiKey, QString &error)
{
    QKeychain::WritePasswordJob job(kService);
    job.setKey(kKey);
    job.setTextData(apiKey);
    job.setInsecureFallback(false);
    waitForJob(job);
    if (job.error() == QKeychain::NoError) return true;
    error = job.errorString();
    return false;
}
}

bool GiphyApiKeyStore::load(QString &apiKey, QString &error)
{
    apiKey.clear();
    error.clear();
    if (!QKeychain::isAvailable()) {
        error = QObject::tr("No secure credential store is available. Configure a system keyring and try again.");
        return false;
    }

    QKeychain::ReadPasswordJob job(kService);
    job.setKey(kKey);
    job.setInsecureFallback(false);
    waitForJob(job);

    if (job.error() == QKeychain::NoError) {
        apiKey = job.textData().trimmed();
        QSettings settings;
        if (settings.contains(kLegacySetting) && !removeLegacySetting(error)) {
            apiKey.clear();
            return false;
        }
        return true;
    }
    if (job.error() != QKeychain::EntryNotFound) {
        error = job.errorString();
        return false;
    }

    QString oldEntry;
    if (readLegacyKeychain(oldEntry)) {
        apiKey = oldEntry.trimmed();
        QString ignored;
        if (writeKeychain(apiKey, ignored)) deleteLegacyKeychain(); // otherwise retried on the next start
        if (QSettings().contains(kLegacySetting) && !removeLegacySetting(error)) {
            apiKey.clear();
            return false;
        }
        return true;
    }

    const QString legacyKey = QSettings().value(kLegacySetting).toString().trimmed();
    if (legacyKey.isEmpty()) return true;
    if (!writeKeychain(legacyKey, error)) return false;
    if (!removeLegacySetting(error)) return false;

    apiKey = legacyKey;
    return true;
}

bool GiphyApiKeyStore::save(const QString &apiKey, QString &error)
{
    error.clear();
    if (!QKeychain::isAvailable()) {
        error = QObject::tr("No secure credential store is available. Configure a system keyring and try again.");
        return false;
    }
    if (!writeKeychain(apiKey.trimmed(), error)) return false;
    return removeLegacySetting(error);
}


bool GiphyApiKeyStore::clear(QString &error)
{
    error.clear();
    if (!QKeychain::isAvailable()) {
        error = QObject::tr("No secure credential store is available. Configure a system keyring and try again.");
        return false;
    }

    QKeychain::DeletePasswordJob job(kService);
    job.setKey(kKey);
    job.setInsecureFallback(false);
    waitForJob(job);
    if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
        error = job.errorString();
        return false;
    }
    deleteLegacyKeychain();
    return removeLegacySetting(error);
}
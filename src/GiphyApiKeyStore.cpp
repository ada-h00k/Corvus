#include "GiphyApiKeyStore.h"

#include <qt6keychain/keychain.h>

#include <QEventLoop>
#include <QSettings>

namespace
{
const QString kService = QStringLiteral("signal-cli-qt");
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
    return removeLegacySetting(error);
}
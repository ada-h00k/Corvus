#include "HistoryKeychain.h"
#include "LegacyMigration.h"

#include <qt6keychain/keychain.h>

#include <QEventLoop>
#include <QRandomGenerator>

namespace
{
const QString kService = QStringLiteral("corvus-history");
const QString kKey = QStringLiteral("history-encryption-key");

void waitForJob(QKeychain::Job &job)
{
    job.setAutoDelete(false);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
}

// Entries written under a former name (wren-history, signal-cli-qt-history) are moved over on first read.
void deleteLegacy()
{
    const QStringList services = LegacyMigration::legacyHistoryServices();
    for (const QString &service : services) {
        QKeychain::DeletePasswordJob job(service);
        job.setKey(kKey);
        job.setInsecureFallback(false);
        waitForJob(job);
    }
}

// Copies the secret from the newest old service name that has one to the new one and removes the
// old entries. If the new entry cannot be written, the old ones are kept and tried again on the
// next start.
bool migrateLegacy(QString &secret)
{
    const QStringList services = LegacyMigration::legacyHistoryServices();
    for (const QString &service : services) {
        QKeychain::ReadPasswordJob legacy(service);
        legacy.setKey(kKey);
        legacy.setInsecureFallback(false);
        waitForJob(legacy);
        if (legacy.error() != QKeychain::NoError) continue;
        secret = legacy.textData();

        QKeychain::WritePasswordJob write(kService);
        write.setKey(kKey);
        write.setTextData(secret);
        write.setInsecureFallback(false);
        waitForJob(write);
        if (write.error() == QKeychain::NoError) deleteLegacy();
        return true;
    }
    return false;
}
}

bool HistoryKeychain::isAvailable()
{
    return QKeychain::isAvailable();
}

bool HistoryKeychain::load(QString &secret, QString &error)
{
    secret.clear();
    error.clear();
    QKeychain::ReadPasswordJob job(kService);
    job.setKey(kKey);
    job.setInsecureFallback(false);
    waitForJob(job);
    if (job.error() == QKeychain::EntryNotFound && migrateLegacy(secret)) return true;
    if (job.error() != QKeychain::NoError) {
        error = job.errorString();
        return false;
    }
    secret = job.textData();
    return true;
}

bool HistoryKeychain::save(const QString &secret, QString &error)
{
    error.clear();
    QKeychain::WritePasswordJob job(kService);
    job.setKey(kKey);
    job.setTextData(secret);
    job.setInsecureFallback(false);
    waitForJob(job);
    if (job.error() == QKeychain::NoError) return true;
    error = job.errorString();
    return false;
}

bool HistoryKeychain::clear(QString &error)
{
    error.clear();
    QKeychain::DeletePasswordJob job(kService);
    job.setKey(kKey);
    job.setInsecureFallback(false);
    waitForJob(job);
    deleteLegacy(); // an old entry must not resurrect a secret that was just removed
    if (job.error() == QKeychain::NoError || job.error() == QKeychain::EntryNotFound) return true;
    error = job.errorString();
    return false;
}

QString HistoryKeychain::generateSecret()
{
    QString secret;
    secret.reserve(64);
    QRandomGenerator *random = QRandomGenerator::system();
    for (int i = 0; i < 8; ++i)
        secret += QStringLiteral("%1").arg(random->generate(), 8, 16, QLatin1Char('0'));
    return secret;
}
#include "HistoryKeychain.h"

#include <qt6keychain/keychain.h>

#include <QEventLoop>
#include <QRandomGenerator>

namespace
{
const QString kService = QStringLiteral("signal-cli-qt-history");
const QString kKey = QStringLiteral("history-encryption-key");

void waitForJob(QKeychain::Job &job)
{
    job.setAutoDelete(false);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
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
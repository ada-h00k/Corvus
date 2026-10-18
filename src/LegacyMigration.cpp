#include "LegacyMigration.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>

namespace LegacyMigration
{
const char *const kLegacyHistoryService = "signal-cli-qt-history";
const char *const kLegacyGiphyService = "signal-cli-qt";

namespace
{
const QString kOldName = QStringLiteral("signal-cli-qt");
const QString kOldHistory = QStringLiteral("history.scq");
const QString kNewHistory = QStringLiteral("history.wren");

QString newFileName(const QString &oldName)
{
    // history.scq and its temporary siblings (e.g. history.scq.tmp) get the new base name.
    if (oldName.startsWith(kOldHistory)) return kNewHistory + oldName.mid(kOldHistory.size());
    return oldName;
}

bool moveFile(const QString &from, const QString &to)
{
    if (QFile::rename(from, to)) return true;
    // Different file system: copy (keeps the permissions, e.g. 0600) and remove the original.
    if (!QFile::copy(from, to)) {
        QFile::remove(to);
        return false;
    }
    return QFile::remove(from);
}

bool migrateData(QString &error)
{
    const QString oldDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/')
        + kOldName + QLatin1Char('/') + kOldName;
    const QString newDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    if (!QFileInfo::exists(oldDir + QLatin1Char('/') + kOldHistory)) return true; // nothing to migrate
    if (QFileInfo::exists(newDir + QLatin1Char('/') + kNewHistory)) return true;  // never overwrite

    // A still running previous version appends to the old file; moving it would corrupt the history.
    QLockFile oldLock(oldDir + QStringLiteral("/history.lock"));
    oldLock.setStaleLockTime(0);
    if (!oldLock.tryLock(0)) {
        error = QObject::tr("An older version of this program (signal-cli Qt) is still running and uses the chat history.\n\n"
                            "Close it first, then start Wren again. Your history is migrated automatically.");
        return false;
    }

    if (!QDir().mkpath(newDir)) {
        error = QObject::tr("Cannot create the data folder:\n%1").arg(newDir);
        return false;
    }

    const QDir dir(oldDir);
    const QStringList names = dir.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        if (name == QStringLiteral("history.lock")) continue;
        const QString to = newDir + QLatin1Char('/') + newFileName(name);
        if (QFileInfo::exists(to)) continue;
        if (!moveFile(dir.filePath(name), to)) {
            error = QObject::tr("Could not move the chat history from\n%1\nto\n%2").arg(oldDir, newDir);
            return false;
        }
    }
    oldLock.unlock();
    // Remove the old folders only if they are empty (rmdir never deletes content).
    QDir().rmdir(oldDir);
    QDir().rmdir(QFileInfo(oldDir).absolutePath());
    return true;
}

void migrateSettings()
{
    QSettings oldSettings(QSettings::NativeFormat, QSettings::UserScope, kOldName, kOldName);
    if (oldSettings.allKeys().isEmpty()) return;
    QSettings settings;
    if (!settings.allKeys().isEmpty()) return; // new settings already exist
    const QStringList keys = oldSettings.allKeys();
    for (const QString &key : keys) settings.setValue(key, oldSettings.value(key));
    settings.sync();
    if (settings.status() != QSettings::NoError) return;
    oldSettings.clear();
    oldSettings.sync();
}
}

bool migrateDataAndSettings(QString &error)
{
    error.clear();
    if (!migrateData(error)) return false;
    migrateSettings();
    return true;
}
}

#include "LegacyMigration.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>

namespace LegacyMigration
{
QStringList legacyHistoryServices()
{
    return {QStringLiteral("wren-history"), QStringLiteral("signal-cli-qt-history")};
}

QStringList legacyGiphyServices()
{
    return {QStringLiteral("wren"), QStringLiteral("signal-cli-qt")};
}

namespace
{
const QString kNewHistory = QStringLiteral("history.corvus");

// A former name of the program under which data may still exist.
struct Source
{
    QString displayName; // shown to the user if the old version is still running
    QString name;        // organisation and application name (the same value for both)
    QString historyFile; // e.g. history.wren
    QString dir;         // folder that holds the old files
};

// Newest name first, so that its data wins if several generations are present.
QList<Source> sources(const QString &newDir)
{
    // AppDataLocation is <base>/<organisation>/<application>; the former data folders sit
    // next to it in the same base folder.
    const QString base = QFileInfo(QFileInfo(newDir).absolutePath()).absolutePath();
    const QString generic = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return {
        {QStringLiteral("Wren"), QStringLiteral("wren"), QStringLiteral("history.wren"),
         base + QStringLiteral("/wren/wren")},
        {QStringLiteral("signal-cli Qt"), QStringLiteral("signal-cli-qt"), QStringLiteral("history.scq"),
         generic + QStringLiteral("/signal-cli-qt/signal-cli-qt")},
    };
}

QString newFileName(const Source &src, const QString &oldName)
{
    // The old history and its temporary siblings (e.g. history.wren.tmp) get the new base name.
    if (oldName.startsWith(src.historyFile)) return kNewHistory + oldName.mid(src.historyFile.size());
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

bool migrateData(const Source &src, const QString &newDir, QString &error)
{
    if (!QFileInfo::exists(src.dir + QLatin1Char('/') + src.historyFile)) return true; // nothing to migrate
    if (QFileInfo::exists(newDir + QLatin1Char('/') + kNewHistory)) return true;       // never overwrite

    // A still running previous version appends to the old file; moving it would corrupt the history.
    QLockFile oldLock(src.dir + QStringLiteral("/history.lock"));
    oldLock.setStaleLockTime(0);
    if (!oldLock.tryLock(0)) {
        error = QObject::tr("An older version of this program (%1) is still running and uses the chat history.\n\n"
                            "Close it first, then start Corvus again. Your history is migrated automatically.")
                    .arg(src.displayName);
        return false;
    }

    if (!QDir().mkpath(newDir)) {
        error = QObject::tr("Cannot create the data folder:\n%1").arg(newDir);
        return false;
    }

    const QDir dir(src.dir);
    const QStringList names = dir.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        if (name == QStringLiteral("history.lock")) continue;
        const QString to = newDir + QLatin1Char('/') + newFileName(src, name);
        if (QFileInfo::exists(to)) continue;
        if (!moveFile(dir.filePath(name), to)) {
            error = QObject::tr("Could not move the chat history from\n%1\nto\n%2").arg(src.dir, newDir);
            return false;
        }
    }
    oldLock.unlock();
    // Remove the old folders only if they are empty (rmdir never deletes content).
    QDir().rmdir(src.dir);
    QDir().rmdir(QFileInfo(src.dir).absolutePath());
    return true;
}

void migrateSettings(const Source &src)
{
    QSettings oldSettings(QSettings::NativeFormat, QSettings::UserScope, src.name, src.name);
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
    const QString newDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QList<Source> all = sources(newDir);
    for (const Source &src : all)
        if (!migrateData(src, newDir, error)) return false;
    for (const Source &src : all) migrateSettings(src);
    return true;
}
}

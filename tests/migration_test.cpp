// Checks the one-time migration from the former "wren" and "signal-cli-qt" locations to Corvus.
#include "LegacyMigration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(data);
}

QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
}

class MigrationTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString oldDir() const { return m_tmp.path() + "/data/signal-cli-qt/signal-cli-qt"; }
    QString wrenDir() const { return m_tmp.path() + "/data/wren/wren"; }
    QString newDir() const { return m_tmp.path() + "/data/corvus/corvus"; }

    void resetFolders()
    {
        QDir(m_tmp.path() + "/data").removeRecursively();
        QDir(m_tmp.path() + "/config").removeRecursively();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_DATA_HOME", (m_tmp.path() + "/data").toUtf8());
        qputenv("XDG_CONFIG_HOME", (m_tmp.path() + "/config").toUtf8());
        QCoreApplication::setOrganizationName("corvus");
        QCoreApplication::setApplicationName("corvus");
        QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation), newDir());
    }

    void movesHistoryAndSettings()
    {
        resetFolders();
        write(oldDir() + "/history.scq", "encrypted-bytes");
        write(oldDir() + "/history.scq.tmp", "leftover");
        {
            QSettings old(QSettings::NativeFormat, QSettings::UserScope, "signal-cli-qt", "signal-cli-qt");
            old.setValue("account", "+49123");
            old.setValue("appearance/radius", 20);
            old.sync();
        }

        QString error;
        QVERIFY2(LegacyMigration::migrateDataAndSettings(error), qPrintable(error));

        QCOMPARE(read(newDir() + "/history.corvus"), QByteArray("encrypted-bytes"));
        QCOMPARE(read(newDir() + "/history.corvus.tmp"), QByteArray("leftover"));
        QVERIFY(!QFileInfo::exists(oldDir() + "/history.scq")); // no stale copy of the history is left behind
        QVERIFY(!QFileInfo::exists(oldDir()));

        QSettings now;
        QCOMPARE(now.value("account").toString(), QString("+49123"));
        QCOMPARE(now.value("appearance/radius").toInt(), 20);
        QSettings old(QSettings::NativeFormat, QSettings::UserScope, "signal-cli-qt", "signal-cli-qt");
        QVERIFY(old.allKeys().isEmpty());
    }

    void doesNothingWithoutOldData()
    {
        resetFolders();
        QString error;
        QVERIFY(LegacyMigration::migrateDataAndSettings(error));
        QVERIFY(!QFileInfo::exists(newDir() + "/history.corvus"));
    }

    void neverOverwritesExistingHistory()
    {
        resetFolders();
        write(oldDir() + "/history.scq", "old");
        write(newDir() + "/history.corvus", "new");
        QString error;
        QVERIFY(LegacyMigration::migrateDataAndSettings(error));
        QCOMPARE(read(newDir() + "/history.corvus"), QByteArray("new"));
        QCOMPARE(read(oldDir() + "/history.scq"), QByteArray("old"));
    }

    void refusesWhileOlderVersionRuns()
    {
        resetFolders();
        write(oldDir() + "/history.scq", "old");
        QLockFile running(oldDir() + "/history.lock");
        running.setStaleLockTime(0);
        QVERIFY(running.tryLock(0));

        QString error;
        QVERIFY(!LegacyMigration::migrateDataAndSettings(error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(read(oldDir() + "/history.scq"), QByteArray("old"));
        QVERIFY(!QFileInfo::exists(newDir() + "/history.corvus"));
    }

    void movesWrenHistoryAndSettings()
    {
        resetFolders();
        write(wrenDir() + "/history.wren", "wren-bytes");
        write(wrenDir() + "/history.wren.tmp", "wren-leftover");
        {
            QSettings old(QSettings::NativeFormat, QSettings::UserScope, "wren", "wren");
            old.setValue("account", "+49456");
            old.setValue("historyProtectionMode", "keychain");
            old.sync();
        }

        QString error;
        QVERIFY2(LegacyMigration::migrateDataAndSettings(error), qPrintable(error));

        QCOMPARE(read(newDir() + "/history.corvus"), QByteArray("wren-bytes"));
        QCOMPARE(read(newDir() + "/history.corvus.tmp"), QByteArray("wren-leftover"));
        QVERIFY(!QFileInfo::exists(wrenDir() + "/history.wren")); // no stale copy of the history is left behind
        QVERIFY(!QFileInfo::exists(wrenDir()));

        QSettings now;
        QCOMPARE(now.value("account").toString(), QString("+49456"));
        QCOMPARE(now.value("historyProtectionMode").toString(), QString("keychain"));
        QSettings old(QSettings::NativeFormat, QSettings::UserScope, "wren", "wren");
        QVERIFY(old.allKeys().isEmpty());
    }

    void wrenDataWinsOverSignalCliQt()
    {
        resetFolders();
        write(oldDir() + "/history.scq", "scq-bytes");
        write(wrenDir() + "/history.wren", "wren-bytes");
        {
            QSettings scq(QSettings::NativeFormat, QSettings::UserScope, "signal-cli-qt", "signal-cli-qt");
            scq.setValue("account", "+49111");
            scq.sync();
            QSettings wren(QSettings::NativeFormat, QSettings::UserScope, "wren", "wren");
            wren.setValue("account", "+49222");
            wren.sync();
        }

        QString error;
        QVERIFY2(LegacyMigration::migrateDataAndSettings(error), qPrintable(error));

        QCOMPARE(read(newDir() + "/history.corvus"), QByteArray("wren-bytes"));
        QVERIFY(!QFileInfo::exists(wrenDir() + "/history.wren"));
        // The older generation is left alone; nothing of it may overwrite the newer data.
        QCOMPARE(read(oldDir() + "/history.scq"), QByteArray("scq-bytes"));
        QCOMPARE(QSettings().value("account").toString(), QString("+49222"));
    }

    void neverOverwritesExistingHistoryWithWrenData()
    {
        resetFolders();
        write(wrenDir() + "/history.wren", "old");
        write(newDir() + "/history.corvus", "new");
        QString error;
        QVERIFY(LegacyMigration::migrateDataAndSettings(error));
        QCOMPARE(read(newDir() + "/history.corvus"), QByteArray("new"));
        QCOMPARE(read(wrenDir() + "/history.wren"), QByteArray("old"));
    }

    void refusesWhileWrenRuns()
    {
        resetFolders();
        write(wrenDir() + "/history.wren", "old");
        QLockFile running(wrenDir() + "/history.lock");
        running.setStaleLockTime(0);
        QVERIFY(running.tryLock(0));

        QString error;
        QVERIFY(!LegacyMigration::migrateDataAndSettings(error));
        QVERIFY(error.contains("Wren"));
        QCOMPARE(read(wrenDir() + "/history.wren"), QByteArray("old"));
        QVERIFY(!QFileInfo::exists(newDir() + "/history.corvus"));
    }

    void legacyKeychainServicesAreNewestFirst()
    {
        QCOMPARE(LegacyMigration::legacyHistoryServices(),
                 QStringList({"wren-history", "signal-cli-qt-history"}));
        QCOMPARE(LegacyMigration::legacyGiphyServices(), QStringList({"wren", "signal-cli-qt"}));
    }
};

QTEST_GUILESS_MAIN(MigrationTest)
#include "migration_test.moc"

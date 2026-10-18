// Checks the one-time migration from the former "signal-cli-qt" locations to Wren.
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
    QString newDir() const { return m_tmp.path() + "/data/wren/wren"; }

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
        QCoreApplication::setOrganizationName("wren");
        QCoreApplication::setApplicationName("wren");
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

        QCOMPARE(read(newDir() + "/history.wren"), QByteArray("encrypted-bytes"));
        QCOMPARE(read(newDir() + "/history.wren.tmp"), QByteArray("leftover"));
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
        QVERIFY(!QFileInfo::exists(newDir() + "/history.wren"));
    }

    void neverOverwritesExistingHistory()
    {
        resetFolders();
        write(oldDir() + "/history.scq", "old");
        write(newDir() + "/history.wren", "new");
        QString error;
        QVERIFY(LegacyMigration::migrateDataAndSettings(error));
        QCOMPARE(read(newDir() + "/history.wren"), QByteArray("new"));
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
        QVERIFY(!QFileInfo::exists(newDir() + "/history.wren"));
    }
};

QTEST_GUILESS_MAIN(MigrationTest)
#include "migration_test.moc"

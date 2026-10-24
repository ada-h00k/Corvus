// How notifications reach the user without a notification service (Windows, macOS),
// and the sound resources behind the built-in "Pop" sound.
#include "NotificationSettings.h"
#include "Notifier.h"
#include "SoundPlayer.h"

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTest>

// These tests are about the fallback without a notification service. A desktop that
// has one (a developer machine) must not change the outcome, so the session bus is
// made unreachable before anything can connect to it.
static const bool kNoSessionBus = (qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/corvus-test-bus"), true);

class NotifierTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(kNoSessionBus);
        QStandardPaths::setTestModeEnabled(true);
    }

    void nothingToShowWithoutATray()
    {
        Notifier n;
        n.setTrayCapabilitiesForTesting(true, true);
        QCOMPARE(n.notify("k", "Alice", "hi"), Notifier::Delivery::None);
    }

    void visibleTrayIconShowsTheMessage()
    {
        QPixmap pm(16, 16);
        pm.fill(Qt::darkCyan);
        QSystemTrayIcon tray{QIcon(pm)};
        tray.show();
        Notifier n;
        n.setTrayCapabilitiesForTesting(true, true);
        n.setTrayIcon(&tray);
        QCOMPARE(n.notify("k", "Alice", "hi"), Notifier::Delivery::Tray);
        QCOMPARE(n.notify("k", "Alice", "hi", pm), Notifier::Delivery::Tray); // with an avatar
    }

    // The tray icon is switched off in the settings: notifications must still appear.
    void hiddenTrayIconUsesATemporaryOne()
    {
        QPixmap pm(16, 16);
        pm.fill(Qt::darkCyan);
        QSystemTrayIcon tray{QIcon(pm)};
        tray.hide();
        Notifier n;
        n.setTrayCapabilitiesForTesting(true, true);
        n.setTrayIcon(&tray);
        QCOMPARE(n.notify("k", "Alice", "hi"), Notifier::Delivery::TemporaryTray);
        QVERIFY(!tray.isVisible()); // the real tray icon stays off
        // a second message reuses the temporary icon
        QCOMPARE(n.notify("k", "Alice", "again"), Notifier::Delivery::TemporaryTray);
    }

    void noSystemTrayNoNotification()
    {
        QPixmap pm(16, 16);
        QSystemTrayIcon tray{QIcon(pm)};
        tray.hide();
        Notifier n;
        n.setTrayIcon(&tray);
        n.setTrayCapabilitiesForTesting(false, true); // no tray at all: cannot even use a temporary icon
        QCOMPARE(n.notify("k", "Alice", "hi"), Notifier::Delivery::None);
        n.setTrayCapabilitiesForTesting(true, false); // tray without message support
        QCOMPARE(n.notify("k", "Alice", "hi"), Notifier::Delivery::None);
    }

    void deliveryIsDescribed()
    {
        const QString none = Notifier::describe(Notifier::Delivery::None);
        const QString tray = Notifier::describe(Notifier::Delivery::Tray);
        QVERIFY(!none.isEmpty() && !tray.isEmpty());
        QVERIFY(none != tray);
        QVERIFY(tray.contains(QStringLiteral("Focus assist"))); // points at the Windows setting
        QVERIFY(!Notifier::describe(Notifier::Delivery::DBus).isEmpty());
        QVERIFY(!Notifier::describe(Notifier::Delivery::TemporaryTray).isEmpty());
    }

    // --- built-in Pop sound ------------------------------------------------

    // Windows plays sounds through PowerShell's Media.SoundPlayer, which only
    // understands plain PCM WAV: the OGG made "Pop (OGG)" fail there.
    void popWavIsPlainPcm()
    {
        QFile f(SoundPlayer::popResource(true));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray w = f.readAll();
        const auto le16 = [&](int o) { return quint16(quint8(w[o]) | (quint8(w[o + 1]) << 8)); };
        const auto le32 = [&](int o) { return quint32(le16(o)) | (quint32(le16(o + 2)) << 16); };
        QVERIFY(w.size() > 44);
        QCOMPARE(w.left(4), QByteArray("RIFF"));
        QCOMPARE(w.mid(8, 4), QByteArray("WAVE"));
        QCOMPARE(w.mid(12, 4), QByteArray("fmt "));
        QCOMPARE(le16(20), quint16(1)); // PCM
        QVERIFY(le16(22) >= 1 && le16(22) <= 2);
        QCOMPARE(le16(34), quint16(16));
        QCOMPARE(w.mid(36, 4), QByteArray("data"));
        QCOMPARE(int(le32(40)), int(w.size()) - 44); // the header matches the data
    }

    void popOggIsOgg()
    {
        QFile f(SoundPlayer::popResource(false));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().startsWith("OggS"));
    }

    void onlyWavGoesToPowerShell()
    {
        QVERIFY(SoundPlayer::powerShellCanPlay("C:/Users/x/pop.wav"));
        QVERIFY(SoundPlayer::powerShellCanPlay("POP.WAV"));
        QVERIFY(!SoundPlayer::powerShellCanPlay("pop-ogg.ogg"));
        QVERIFY(!SoundPlayer::powerShellCanPlay("a.flac"));
        QVERIFY(!SoundPlayer::powerShellCanPlay("a.mp3"));
        QVERIFY(!SoundPlayer::powerShellCanPlay("wav"));
    }

    void builtInPopResolvesToAPlayableFile()
    {
        SoundPlayer p;
        NotificationSettings n;
        n.soundId = QStringLiteral("pop-ogg");
        const QString file = p.resolveFile(n);
        QVERIFY(QFile::exists(file));
#ifdef Q_OS_WIN
        QVERIFY(file.endsWith(QLatin1String("pop-ogg.wav")));
        QVERIFY(SoundPlayer::powerShellCanPlay(file)); // what the Windows player can handle
#else
        QVERIFY(file.endsWith(QLatin1String("pop-ogg.ogg")));
#endif
    }

    // The in-process player (Windows: custom .ogg/.flac/.mp3 files) must not crash,
    // also when a sound replaces one that is still playing, and on bad files.
    void inProcessPlaybackIsRobust()
    {
        SoundPlayer p;
        NotificationSettings n;
        n.soundId = QStringLiteral("pop-ogg");
        const QString file = p.resolveFile(n);
        QVERIFY(p.playInProcess(file));
        QVERIFY(p.playInProcess(file));
        QVERIFY(p.playInProcess(QStringLiteral("/nonexistent/sound.ogg"))); // reported via failed(), not a crash
        QTest::qWait(300);
        QVERIFY(!p.playInProcess(QString())); // nothing to play
    }
};

QTEST_MAIN(NotifierTest)
#include "notifier_test.moc"

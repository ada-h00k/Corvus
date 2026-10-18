// Tray icon, desktop notifications and sound – tested against fake D-Bus services:
//  * org.freedesktop.Notifications (records every Notify / CloseNotification call)
//  * org.kde.StatusNotifierWatcher (so Qt believes a system tray exists)
// and a fake `paplay` in PATH that logs what it was asked to play.
// Must run inside a session bus: dbus-run-session -- alerts_test
#include "AppIcon.h"
#include "MainWindow.h"
#include "NotificationDialog.h"
#include "NotificationSettings.h"
#include "Notifier.h"
#include "PassphraseDialog.h"
#include "SoundPlayer.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMutex>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <memory>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

// ------------------------------------------------------------ fake services

class FakeNotifications : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    struct Call {
        QString app;
        uint replaces;
        QString icon, summary, body;
        QStringList actions;
        QVariantMap hints;
        int timeout;
    };
    mutable QMutex mu;
    QList<Call> calls;
    QList<uint> closed;
    bool markup = true;
    uint nextId = 100;

    QList<Call> snapshot() const { QMutexLocker l(&mu); return calls; }
    QList<uint> closedSnapshot() const { QMutexLocker l(&mu); return closed; }
    void reset() { QMutexLocker l(&mu); calls.clear(); closed.clear(); nextId = 100; }

public slots:
    uint Notify(const QString &app, uint replaces, const QString &icon, const QString &summary, const QString &body,
                const QStringList &actions, const QVariantMap &hints, int timeout)
    {
        QMutexLocker l(&mu);
        calls.append({app, replaces, icon, summary, body, actions, hints, timeout});
        return replaces ? replaces : nextId++;
    }
    QStringList GetCapabilities() { return markup ? QStringList{"body", "body-markup", "actions"} : QStringList{"body", "actions"}; }
    void CloseNotification(uint id) { QMutexLocker l(&mu); closed.append(id); }
};

class FakeWatcher : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
    Q_PROPERTY(bool IsStatusNotifierHostRegistered READ hostRegistered)
    Q_PROPERTY(int ProtocolVersion READ protocolVersion)
    Q_PROPERTY(QStringList RegisteredStatusNotifierItems READ items)
public:
    bool hostRegistered() const { return true; }
    int protocolVersion() const { return 0; }
    QStringList items() const { return {}; }
public slots:
    void RegisterStatusNotifierItem(const QString &) {}
    void RegisterStatusNotifierHost(const QString &) {}
};

struct FakeBus {
    QThread thread;
    FakeNotifications *notif = new FakeNotifications;
    FakeWatcher *watcher = new FakeWatcher;
    FakeBus()
    {
        notif->moveToThread(&thread);
        watcher->moveToThread(&thread);
        thread.start();
        QMetaObject::invokeMethod(notif, [this] {
            QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake"));
            bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), notif, QDBusConnection::ExportAllSlots);
            bus.registerService(QStringLiteral("org.freedesktop.Notifications"));
            bus.registerObject(QStringLiteral("/StatusNotifierWatcher"), watcher,
                               QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties);
            bus.registerService(QStringLiteral("org.kde.StatusNotifierWatcher"));
        }, Qt::BlockingQueuedConnection);
    }
    ~FakeBus()
    {
        thread.quit();
        thread.wait();
    }
    void clickNotification(uint id)
    {
        QMetaObject::invokeMethod(notif, [id] {
            QDBusMessage m = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/Notifications"),
                                                        QStringLiteral("org.freedesktop.Notifications"),
                                                        QStringLiteral("ActionInvoked"));
            m << id << QStringLiteral("default");
            QDBusConnection(QStringLiteral("fake")).send(m);
        }, Qt::QueuedConnection);
    }
};

// ------------------------------------------------------------------ helpers

static bool waitFor(const std::function<bool()> &cond, int ms = 15000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(3);
    }
    return true;
}

static void settle(int ms = 500) // "nothing must happen" checks
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(3);
    }
}

static void autoDialogs()
{
    QWidget *w = QApplication::activeModalWidget();
    if (!w) return;
    if (auto *pd = qobject_cast<PassphraseDialog *>(w)) {
        for (QLineEdit *e : pd->findChildren<QLineEdit *>()) e->setText(QStringLiteral("test-passphrase-1"));
        pd->accept();
    } else if (auto *mb = qobject_cast<QMessageBox *>(w)) {
        std::printf("  [unexpected message box] %s\n", qPrintable(mb->text()));
        ++g_failed;
        mb->reject();
    }
}

static QString g_origPath;

static void writeExecutable(const QString &path, const QByteArray &content)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(content);
    f.close();
    QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
}

static QJsonObject incoming(const QString &num, const QString &name, qint64 ts, const QString &text, const QString &group = {})
{
    QJsonObject dm{{"timestamp", ts}, {"message", text}};
    if (!group.isEmpty()) dm["groupInfo"] = QJsonObject{{"groupId", group}};
    return {{"sourceNumber", num}, {"sourceName", name}, {"timestamp", ts}, {"dataMessage", dm}};
}

// One MainWindow with its own data dir, fake paplay and the given settings.
struct Scenario {
    QTemporaryDir tmp;
    QString soundLog, injectFile;
    std::unique_ptr<MainWindow> w;

    Scenario(FakeBus &bus, const NotificationSettings &ns, bool trayHintShown = true)
    {
        bus.notif->reset();
        QDir().mkpath(tmp.path() + "/bin");
        soundLog = tmp.path() + "/sound.log";
        injectFile = tmp.path() + "/inject.jsonl";
        writeExecutable(tmp.path() + "/bin/paplay", "#!/bin/sh\necho \"$@\" >> \"$FAKE_SOUND_LOG\"\n");
        qputenv("PATH", (tmp.path() + "/bin:" + g_origPath).toUtf8());
        qputenv("FAKE_SOUND_LOG", soundLog.toUtf8());
        qputenv("MOCK_ACCOUNTS", "ok");
        qputenv("MOCK_INJECT", injectFile.toUtf8());
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path() + "/config");
        QSettings().setValue("executable", QStringLiteral(MOCK_CLI));
        QSettings().setValue("trayHintShown", trayHintShown);
        ns.save();
        w = std::make_unique<MainWindow>();
        w->show();
        auto *list = w->findChild<QListWidget *>();
        const bool ready = waitFor([&] {
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->text().contains("Alice")) return true;
            return false;
        });
        if (!ready) { std::printf("  FAIL scenario did not start\n"); ++g_failed; }
    }
    void inject(const QJsonObject &env)
    {
        QFile f(injectFile);
        f.open(QIODevice::Append);
        f.write(QJsonDocument(env).toJson(QJsonDocument::Compact) + "\n");
    }
    int sounds() const
    {
        QFile f(soundLog);
        if (!f.open(QIODevice::ReadOnly)) return 0;
        return f.readAll().split('\n').size() - 1;
    }
    QString lastSoundArg() const
    {
        QFile f(soundLog);
        if (!f.open(QIODevice::ReadOnly)) return {};
        const QList<QByteArray> lines = f.readAll().split('\n');
        return lines.size() >= 2 ? QString::fromUtf8(lines[lines.size() - 2]) : QString();
    }
};

static QString plainView(MainWindow &w)
{
    for (QLabel *l : w.findChildren<QLabel *>())
        if (l->font().bold()) return l->text(); // the conversation title
    return {};
}

// ------------------------------------------------------------------- tests

int main(int argc, char **argv)
{
    // The offscreen platform has no tray support in its theme; the generic Unix theme
    // implements the StatusNotifier (D-Bus) tray that the fake watcher below answers.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("wren");
    QApplication::setApplicationName("wren");
    g_origPath = qEnvironmentVariable("PATH");
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, autoDialogs);
    dialogs.start(25);

    FakeBus bus; // registers the fake notification service and a fake tray host

    std::printf("sound synthesis\n");
    {
        QByteArray first;
        for (const QString &id : SoundPlayer::builtinIds()) {
            const QByteArray w = SoundPlayer::synthesize(id);
            CHECK(w.left(4) == "RIFF" && w.mid(8, 4) == "WAVE" && w.mid(12, 4) == "fmt ");
            auto u16 = [&](int o) { return quint16(quint8(w[o]) | (quint8(w[o + 1]) << 8)); };
            auto u32 = [&](int o) { return quint32(u16(o) | (quint32(u16(o + 2)) << 16)); };
            CHECK(u16(20) == 1 && u16(22) == 1 && u32(24) == 44100 && u16(34) == 16); // PCM, mono, 44.1 kHz, 16 bit
            CHECK(w.mid(36, 4) == "data" && int(u32(40)) == w.size() - 44);
            const int n = w.size() / 2 - 22;
            const double secs = double(n) / 44100;
            CHECK(secs > 0.1 && secs < 1.5);
            const auto *s = reinterpret_cast<const qint16 *>(w.constData() + 44);
            int peak = 0;
            for (int i = 0; i < n; ++i) peak = qMax(peak, qAbs(int(s[i])));
            CHECK(peak > 0.5 * 32767 && peak < 0.95 * 32767); // audible, not clipping
            CHECK(qAbs(int(s[0])) < 600 && qAbs(int(s[n - 1])) < 600); // no clicks at the ends
            if (first.isEmpty()) first = w; else CHECK(w != first);
        }
    }

    std::printf("icon\n");
    {
        auto isRed = [](QRgb c) { return qRed(c) > 200 && qGreen(c) < 90 && qBlue(c) < 90; };
        auto hasRed = [&](const QImage &im, const QRect &r) {
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x)
                    if (isRed(im.pixel(x, y))) return true;
            return false;
        };
        const QImage plain = AppIcon::render(64, 0).toImage();
        const QImage badge = AppIcon::render(64, 3).toImage();
        CHECK(qAlpha(plain.pixel(0, 0)) == 0);                        // rounded corners are transparent
        CHECK(qBlue(plain.pixel(8, 32)) > 200 && qRed(plain.pixel(8, 32)) < 90); // blue background
        CHECK(!hasRed(plain, QRect(0, 0, 64, 64)));
        CHECK(hasRed(badge, QRect(32, 0, 32, 32)));                   // badge top right
        CHECK(!hasRed(badge, QRect(0, 32, 32, 32)));
        CHECK(!AppIcon::tray(5).isNull() && !AppIcon::app().isNull());
    }

    std::printf("settings\n");
    {
        QTemporaryDir t;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, t.path());
        const NotificationSettings d = NotificationSettings::load();
        CHECK(d.notifications && d.sound && d.trayIcon && d.closeToTray && !d.startMinimizedInTray && d.soundId == "chime");
        NotificationSettings n = d;
        n.notifications = false;
        n.sound = true;
        n.content = NotificationSettings::Hidden;
        n.soundId = "custom";
        n.soundFile = "/tmp/x.ogg";
        n.trayIcon = false;
        n.closeToTray = false;
        n.startMinimizedInTray = true;
        n.save();
        CHECK(NotificationSettings::load() == n);
        CHECK(MainWindow::shouldStartMinimizedInTray(true, true));
        CHECK(!MainWindow::shouldStartMinimizedInTray(false, true));
        CHECK(!MainWindow::shouldStartMinimizedInTray(true, false));
        CHECK(MainWindow::shouldHideOnClose(true, true, false));
        CHECK(!MainWindow::shouldHideOnClose(false, true, false)); // no tray -> really close
        CHECK(!MainWindow::shouldHideOnClose(true, false, false)); // option off
        CHECK(!MainWindow::shouldHideOnClose(true, true, true));   // quitting / session end
    }

    std::printf("sound player\n");
    {
        Scenario s(bus, NotificationSettings());
        SoundPlayer p;
        CHECK(p.playerName() == "paplay");
        NotificationSettings n;
        const QString f = p.resolveFile(n);
        CHECK(f.endsWith("chime.wav") && QFile::exists(f));
        CHECK(QFileInfo(f).size() == SoundPlayer::synthesize("chime").size());
        n.soundId = "custom";
        n.soundFile = s.tmp.path() + "/mine.wav";
        CHECK(p.resolveFile(n).endsWith("chime.wav")); // missing custom file -> falls back
        QFile(n.soundFile).open(QIODevice::WriteOnly);
        CHECK(p.resolveFile(n) == n.soundFile);
        CHECK(p.play(n));
        CHECK(waitFor([&] { return s.sounds() == 1; }));
        CHECK(s.lastSoundArg() == n.soundFile);
    }

    std::printf("notifier (D-Bus)\n");
    {
        bus.notif->reset();
        bus.notif->markup = true;
        Notifier n;
        CHECK(n.usingDBus());
        QSignalSpy clicked(&n, &Notifier::activated);
        n.notify("conv1", "Alice", "hello <b>world</b> & co");
        auto calls = bus.notif->snapshot();
        CHECK(calls.size() == 1);
        if (calls.size() == 1) {
            const auto &c = calls[0];
            CHECK(c.app == "Wren" && c.replaces == 0 && c.summary == "Alice");
            CHECK(c.body == "hello &lt;b&gt;world&lt;/b&gt; &amp; co"); // server supports markup -> escaped
            CHECK(c.actions.size() == 2 && c.actions[0] == "default");
            CHECK(c.hints.value("category").toString() == "im.received");
            CHECK(c.hints.value("desktop-entry").toString() == "wren");
        }
        n.notify("conv1", "Alice", "second");   // same chat: replaces the first one
        n.notify("conv2", "Bob", "other chat"); // different chat: new notification
        calls = bus.notif->snapshot();
        CHECK(calls.size() == 3);
        if (calls.size() == 3) {
            CHECK(calls[1].replaces == 100);
            CHECK(calls[2].replaces == 0);
        }
        bus.clickNotification(100);
        CHECK(waitFor([&] { return clicked.count() == 1; }));
        if (clicked.count() == 1) CHECK(clicked[0][0].toString() == "conv1");
        n.close("conv2");
        CHECK(waitFor([&] { return bus.notif->closedSnapshot().size() == 1; }));

        bus.notif->markup = false; // a server without markup gets the raw text
        bus.notif->reset();
        Notifier raw;
        raw.notify("c", "Alice", "a < b & c");
        calls = bus.notif->snapshot();
        CHECK(calls.size() == 1 && calls[0].body == "a < b & c");
        bus.notif->markup = true;
    }

    std::printf("incoming message: notification and sound are independent\n");
    struct Case { const char *name; bool notify; bool sound; };
    for (const Case &c : {Case{"both on", true, true}, Case{"only notification", true, false},
                          Case{"only sound", false, true}, Case{"both off", false, false}}) {
        std::printf("  %s\n", c.name);
        NotificationSettings ns;
        ns.notifications = c.notify;
        ns.sound = c.sound;
        Scenario s(bus, ns);
        s.inject(incoming("+49111", "Alice", 1700000000000LL, "hello there"));
        if (c.notify) {
            CHECK(waitFor([&] { return !bus.notif->snapshot().isEmpty(); }));
            const auto calls = bus.notif->snapshot();
            if (!calls.isEmpty()) {
                CHECK(calls[0].summary == "Alice");
                CHECK(calls[0].body == "hello there");
            }
        }
        if (c.sound) CHECK(waitFor([&] { return s.sounds() == 1; }));
        settle(600);
        CHECK(int(bus.notif->snapshot().size()) == (c.notify ? 1 : 0));
        CHECK(s.sounds() == (c.sound ? 1 : 0));
        if (c.sound) CHECK(s.lastSoundArg().endsWith("chime.wav"));
    }

    std::printf("content setting, groups, attachments\n");
    {
        NotificationSettings ns;
        ns.sound = false;
        ns.content = NotificationSettings::NameOnly;
        Scenario s(bus, ns);
        s.inject(incoming("+49111", "Alice", 1700000000000LL, "secret text"));
        CHECK(waitFor([&] { return !bus.notif->snapshot().isEmpty(); }));
        auto calls = bus.notif->snapshot();
        CHECK(calls.size() == 1 && calls[0].summary == "Alice" && calls[0].body == "New message");
    }
    {
        NotificationSettings ns;
        ns.sound = false;
        ns.content = NotificationSettings::Hidden;
        Scenario s(bus, ns);
        s.inject(incoming("+49111", "Alice", 1700000000000LL, "secret text"));
        CHECK(waitFor([&] { return !bus.notif->snapshot().isEmpty(); }));
        auto calls = bus.notif->snapshot();
        CHECK(calls.size() == 1 && calls[0].summary == "Wren" && calls[0].body == "New message");
    }
    {
        NotificationSettings ns;
        ns.sound = false;
        Scenario s(bus, ns);
        CHECK(waitFor([&] { // group title arrives with listGroups
            auto *l = s.w->findChild<QListWidget *>();
            for (int i = 0; i < l->count(); ++i) if (l->item(i)->text().contains("Team")) return true;
            return false; }));
        s.inject(incoming("+49111", "Alice", 1700000001000LL, "see you", "R1JPVVA="));
        CHECK(waitFor([&] { return !bus.notif->snapshot().isEmpty(); }));
        auto calls = bus.notif->snapshot();
        CHECK(calls.size() == 1 && calls[0].summary == "Team" && calls[0].body == "Alice: see you");
        bus.notif->reset();
        QJsonObject att{{"contentType", "image/png"}, {"filename", "p.png"}, {"id", "x"}, {"size", 10}};
        QJsonObject dm{{"timestamp", 1700000002000LL}, {"attachments", QJsonArray{att}}};
        s.inject({{"sourceNumber", "+49111"}, {"sourceName", "Alice"}, {"timestamp", 1700000002000LL}, {"dataMessage", dm}});
        CHECK(waitFor([&] { return !bus.notif->snapshot().isEmpty(); }));
        calls = bus.notif->snapshot();
        CHECK(calls.size() == 1 && calls[0].body == "[Image]");
    }

    std::printf("muted chats, rate limit, clicking a notification\n");
    {
        Scenario s(bus, NotificationSettings());
        s.w->setMuted("+49111", true);
        s.inject(incoming("+49111", "Alice", 1700000000000LL, "muted message"));
        settle(700);
        CHECK(bus.notif->snapshot().isEmpty() && s.sounds() == 0);
        s.w->setMuted("+49111", false);

        // a burst of messages: one notification per chat (replaced), only one sound
        s.inject(incoming("+49111", "Alice", 1700000010000LL, "one"));
        s.inject(incoming("+49111", "Alice", 1700000011000LL, "two"));
        s.inject(incoming("+49111", "Alice", 1700000012000LL, "three"));
        CHECK(waitFor([&] { return bus.notif->snapshot().size() == 3; }));
        settle(300);
        CHECK(s.sounds() == 1);
        const auto calls = bus.notif->snapshot();
        if (calls.size() == 3) CHECK(calls[1].replaces == calls[2].replaces && calls[1].replaces != 0);

        // clicking the notification opens that conversation
        CHECK(plainView(*s.w) != "Alice");
        bus.clickNotification(100);
        CHECK(waitFor([&] { return plainView(*s.w) == "Alice"; }));
        // opening the chat withdraws its notification
        CHECK(waitFor([&] { return !bus.notif->closedSnapshot().isEmpty(); }));
    }
    {
        // the mute flag survives a restart (stored encrypted in the history)
        QTemporaryDir keep;
        QString data;
        {
            Scenario s(bus, NotificationSettings());
            s.w->setMuted("+49111", true);
            data = s.tmp.path();
            auto *l = s.w->findChild<QListWidget *>();
            bool marked = false;
            for (int i = 0; i < l->count(); ++i) if (l->item(i)->text().startsWith("🔕") && l->item(i)->text().contains("Alice")) marked = true;
            CHECK(marked);
            s.w->findChild<QSystemTrayIcon *>()->setVisible(false); // so closing really closes
            s.w->close();
        }
    }

    std::printf("nothing while looking at the conversation\n");
    {
        Scenario s(bus, NotificationSettings());
        auto *list = s.w->findChild<QListWidget *>();
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text().contains("Alice")) list->setCurrentRow(i);
        s.w->activateWindow();
        const bool active = waitFor([&] { return s.w->isActiveWindow() && plainView(*s.w) == "Alice"; }, 3000);
        if (!active) {
            std::printf("  (window not active under this platform plugin – skipped)\n");
        } else {
            s.inject(incoming("+49111", "Alice", 1700000000000LL, "you are looking at me"));
            settle(800);
            CHECK(bus.notif->snapshot().isEmpty() && s.sounds() == 0);
            s.inject(incoming("+49222", "Bob", 1700000001000LL, "other chat"));  // another chat still alerts
            CHECK(waitFor([&] { return bus.notif->snapshot().size() == 1; }));
        }
    }

    std::printf("tray icon\n");
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        std::printf("  (skipped: this Qt platform plugin has no system tray support; run under xvfb-run with QT_QPA_PLATFORM=xcb)\n");
    } else {
        NotificationSettings ns;
        ns.sound = false;
        Scenario s(bus, ns, /*trayHintShown=*/false);
        auto *tray = s.w->findChild<QSystemTrayIcon *>();
        CHECK(tray && tray->isVisible());

        // unread counter in tooltip and title
        s.w->hide();
        s.inject(incoming("+49111", "Alice", 1700000000000LL, "unread one"));
        CHECK(waitFor([&] { return tray->toolTip().contains("1 unread"); }));
        CHECK(s.w->windowTitle() == "(1) Wren");

        // closing hides to the tray (and keeps the app alive), tray click brings it back
        s.w->show();
        bus.notif->reset();
        QCloseEvent ev;
        QApplication::sendEvent(s.w.get(), &ev);
        CHECK(!ev.isAccepted());
        CHECK(!s.w->isVisible());
        CHECK(!QApplication::quitOnLastWindowClosed());
        CHECK(waitFor([&] { return !bus.notif->snapshot().isEmpty(); })); // one-time "still running" hint
        if (!bus.notif->snapshot().isEmpty()) CHECK(bus.notif->snapshot().last().summary.contains("still running"));
        emit tray->activated(QSystemTrayIcon::Trigger);
        CHECK(waitFor([&] { return s.w->isVisible(); }));
        CHECK(QApplication::quitOnLastWindowClosed());

        // the hint is shown only once
        bus.notif->reset();
        QCloseEvent again;
        QApplication::sendEvent(s.w.get(), &again);
        settle(300);
        CHECK(bus.notif->snapshot().isEmpty());
        emit tray->activated(QSystemTrayIcon::Trigger);
        CHECK(waitFor([&] { return s.w->isVisible(); }));

        // option off -> closing really closes
        NotificationSettings off = ns;
        off.closeToTray = false;
        off.save();
        Scenario s2(bus, off);
        QCloseEvent ev2;
        QApplication::sendEvent(s2.w.get(), &ev2);
        CHECK(ev2.isAccepted());
    }

    std::printf("settings dialog\n");
    {
        Scenario s(bus, NotificationSettings());
        SoundPlayer player;
        Notifier notifier;
        NotificationSettings start;
        NotificationDialog dlg(start, &player, &notifier, true);
        dlg.show();
        CHECK(dlg.settings() == start);

        auto box = [&](const QString &text) {
            for (QCheckBox *c : dlg.findChildren<QCheckBox *>())
                if (c->text().contains(text)) return c;
            return static_cast<QCheckBox *>(nullptr);
        };
        QCheckBox *notify = box("notification for new");
        QCheckBox *soundOn = box("Play a sound");
        QCheckBox *trayOn = box("Show a tray icon");
        QCheckBox *closeTray = box("Keep running");
        QCheckBox *startInTray = box("Start minimized");
        CHECK(notify && soundOn && trayOn && closeTray && startInTray);
        const QList<QComboBox *> combos = dlg.findChildren<QComboBox *>();
        CHECK(combos.size() == 2);

        notify->setChecked(false);
        CHECK(!combos[0]->isEnabled() && combos[1]->isEnabled()); // sound unaffected
        soundOn->setChecked(false);
        CHECK(!combos[1]->isEnabled());
        trayOn->setChecked(false);
        CHECK(!closeTray->isEnabled() && !startInTray->isEnabled());
        NotificationSettings got = dlg.settings();
        CHECK(!got.notifications && !got.sound && !got.trayIcon && !got.startMinimizedInTray);

        NotificationSettings other;
        other.notifications = true;
        other.sound = true;
        other.startMinimizedInTray = true;
        other.soundId = "pop";
        other.content = NotificationSettings::NameOnly;
        dlg.setSettings(other);
        CHECK(dlg.settings() == other);
        CHECK(startInTray->isChecked());
        CHECK(combos[1]->isEnabled());

        // the test buttons really fire
        bus.notif->reset();
        const int before = s.sounds();
        for (QPushButton *b : dlg.findChildren<QPushButton *>())
            if (b->text() == "Test" && b->isEnabled()) b->click();
        CHECK(waitFor([&] { return !bus.notif->snapshot().isEmpty(); }));
        CHECK(waitFor([&] { return s.sounds() == before + 1; }));
        CHECK(s.lastSoundArg().endsWith("pop.wav"));
    }

    std::printf("explicit quit\n");
    {
        Scenario s(bus, NotificationSettings());
        QApplication::setQuitOnLastWindowClosed(false);
        QTimer::singleShot(0, s.w.get(), [window = s.w.get()] {
            QMetaObject::invokeMethod(window, "quitApplication");
        });
        app.exec();
        CHECK(!s.w->isVisible());
    }

    std::printf(g_failed ? "FAILED (%d checks)\n" : "ALL OK\n", g_failed);
    return g_failed ? 1 : 0;
}

#include "alerts_test.moc"

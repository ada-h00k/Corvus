// Start-up with a backlog: the newest messages show up first, everything ends up in the right
// order, reactions / deletes that refer to older messages are applied afterwards, and the event
// loop stays responsive while the backlog is worked off.
//
// A MainWindow runs against the mock backend, whose inject file is filled before the start, so
// all envelopes arrive as a backlog right after connecting.
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtCore>
#include <QtWidgets>
#include <cstdio>

#include "PassphraseDialog.h"
#define private public
#include "MainWindow.h"
#undef private

static int g_failed = 0;

#define CHECK(cond, what)                                                          \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, what);           \
            ++g_failed;                                                            \
        } else {                                                                   \
            std::printf("  ok   %s\n", what);                                      \
        }                                                                          \
    } while (0)

static void autoDialogs()
{
    QWidget *w = QApplication::activeModalWidget();
    if (!w) return;
    if (auto *pd = qobject_cast<PassphraseDialog *>(w)) {
        for (QLineEdit *e : pd->findChildren<QLineEdit *>()) e->setText(QStringLiteral("test-passphrase-1"));
        pd->accept();
    } else if (auto *mb = qobject_cast<QMessageBox *>(w)) {
        if (mb->windowTitle() == QStringLiteral("Protect chat history")) {
            for (QAbstractButton *b : mb->buttons())
                if (b->text() == QStringLiteral("Use a passphrase")) {
                    b->click();
                    return;
                }
        }
        mb->reject();
    }
}

template <class F>
static bool waitFor(F pred, int timeoutMs = 20000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        autoDialogs();
        if (pred()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return pred();
}

static const Conversation *aliceChat(MainWindow &w)
{
    for (auto it = w.m_convs.constBegin(); it != w.m_convs.constEnd(); ++it)
        if (!it->isGroup && !it->msgs.isEmpty() && it->title.contains(QStringLiteral("Alice"))) return &it.value();
    return nullptr;
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QApplication app(argc, argv);
    // Dialogs of the start-up (history protection) run their own event loop: answer them from a timer.
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, autoDialogs);
    dialogs.start(50);
    QCoreApplication::setOrganizationName(QStringLiteral("corvus-test"));
    QCoreApplication::setApplicationName(QStringLiteral("catchup-order-test"));

    const int kMessages = qEnvironmentVariableIntValue("CATCHUP_MESSAGES") > 0 ? qEnvironmentVariableIntValue("CATCHUP_MESSAGES") : 400;
    constexpr qint64 kBase = 1700000000000LL;
    auto tsOf = [&](int i) { return kBase + qint64(i) * 1000; };

    QTemporaryDir tmp;
    const QString injectFile = tmp.path() + QStringLiteral("/inject.jsonl");
    {
        QFile f(injectFile);
        f.open(QIODevice::WriteOnly);
        auto put = [&](const QJsonObject &env) { f.write(QJsonDocument(env).toJson(QJsonDocument::Compact) + "\n"); };
        for (int i = 0; i < kMessages; ++i) {
            QJsonObject dm{{"timestamp", tsOf(i)}, {"message", QStringLiteral("m%1").arg(i)}};
            put({{"sourceNumber", "+49111"}, {"sourceName", "Alice"}, {"timestamp", tsOf(i)}, {"dataMessage", dm}});
            // A reaction to message 5 and a delete of message 7 are sent right after the messages
            // they refer to, so they sit in the middle of the backlog.
            if (i == 8) {
                QJsonObject reaction{{"emoji", "👍"}, {"targetAuthorNumber", "+49111"}, {"targetSentTimestamp", tsOf(5)}, {"isRemove", false}};
                put({{"sourceNumber", "+49111"}, {"timestamp", tsOf(i) + 1}, {"dataMessage", QJsonObject{{"timestamp", tsOf(i) + 1}, {"reaction", reaction}}}});
                put({{"sourceNumber", "+49111"}, {"timestamp", tsOf(i) + 2},
                     {"dataMessage", QJsonObject{{"timestamp", tsOf(i) + 2}, {"remoteDelete", QJsonObject{{"timestamp", tsOf(7)}}}}}});
            }
        }
    }

    QDir().mkpath(tmp.path() + "/data");
    qputenv("MOCK_ACCOUNTS", "ok");
    qputenv("MOCK_INJECT", injectFile.toUtf8());
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path() + "/config");
    QSettings().setValue("executable", QStringLiteral(MOCK_CLI));
    QSettings().setValue("trayHintShown", true);

    std::printf("backlog of %d messages\n", kMessages);
    QElapsedTimer total;
    total.start();
    auto w = std::make_unique<MainWindow>();
    w->show();

    // Event loop gaps while the backlog is processed.
    QElapsedTimer sinceTick;
    qint64 maxGapMs = 0;
    QTimer ticker;
    ticker.setInterval(5);
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        if (sinceTick.isValid()) maxGapMs = std::max(maxGapMs, sinceTick.elapsed());
        sinceTick.restart();
    });

    // 1. The first message that shows up is the newest one.
    int firstSize = 0;
    qint64 firstLastTs = 0;
    bool seen = waitFor([&] {
        if (const Conversation *c = aliceChat(*w)) {
            firstSize = int(c->msgs.size());
            firstLastTs = c->msgs.last().ts();
            return true;
        }
        return false;
    });
    CHECK(seen, "messages arrive");
    ticker.start();
    CHECK(firstLastTs == tsOf(kMessages - 1), "the newest message is processed first");
    CHECK(firstSize < kMessages, "... while the backlog is still being worked off");

    // 2. Everything arrives; the history has the right order and the dependent envelopes are applied.
    qint64 messagesMs = -1;
    bool all = waitFor([&] {
        const Conversation *c = aliceChat(*w);
        if (c && c->msgs.size() == kMessages && messagesMs < 0) messagesMs = total.elapsed();
        return c && c->msgs.size() == kMessages && w->stagedCount() == 0 && w->m_cli.pendingBacklog() == 0;
    }, 120000);
    const qint64 allMs = total.elapsed();
    ticker.stop();
    CHECK(all, "all messages are there");
    if (const Conversation *c = aliceChat(*w)) {
        bool ordered = c->msgs.size() == kMessages;
        for (int i = 0; ordered && i < c->msgs.size(); ++i)
            ordered = c->msgs[i].ts() == tsOf(i);
        CHECK(ordered, "chat order is the order of arrival (oldest first)");
        if (c->msgs.size() > 7) {
            CHECK(!c->msgs[5].reactions.isEmpty(), "a reaction that arrived later is applied to an older message");
            CHECK(c->msgs[7].deleted, "a delete that arrived later is applied to an older message");
            CHECK(!c->msgs[6].deleted && c->msgs[6].reactions.isEmpty(), "other messages are untouched");
        }
    }
    std::printf("  info: %d messages in %lld ms (all messages shown after %lld ms, the rest = reactions/deletes), longest event loop gap %lld ms\n",
                kMessages, (long long)allMs, (long long)messagesMs, (long long)maxGapMs);
    CHECK(maxGapMs < 200, "the event loop stayed responsive");

    w.reset();
    std::printf(g_failed ? "FAILED (%d checks)\n" : "PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}

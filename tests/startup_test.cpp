// Start-up behaviour against a mock signal-cli that fails, is empty, slow, or exits non-zero.
#include "MainWindow.h"
#include "PassphraseDialog.h"
#include "SignalCli.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static QString g_box; // text of the last message box
static bool g_boxSeen = false;

static void autoDialogs()
{
    QWidget *w = QApplication::activeModalWidget();
    if (!w) return;
    if (auto *pd = qobject_cast<PassphraseDialog *>(w)) {
        for (QLineEdit *e : pd->findChildren<QLineEdit *>()) e->setText(QStringLiteral("test-passphrase-1"));
        pd->accept();
    } else if (auto *mb = qobject_cast<QMessageBox *>(w)) {
        g_box = mb->text() + QLatin1Char('\n') + mb->informativeText();
        g_boxSeen = true;
        mb->reject();
    }
}

static bool waitFor(const std::function<bool()> &cond, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

static QString status(MainWindow &w)
{
    for (QLabel *l : w.findChildren<QLabel *>()) {
        const QString t = l->text();
        if (t.contains("Connect") || t.contains("Disconnected") || t.contains("Not connected")) return t;
    }
    return {};
}

// Runs a MainWindow with a fresh data directory and the given mock behaviour.
struct Run {
    QTemporaryDir tmp;
    std::unique_ptr<MainWindow> w;
    explicit Run(const QByteArray &accounts, const QByteArray &sleep = "0")
    {
        g_box.clear();
        g_boxSeen = false;
        qputenv("MOCK_ACCOUNTS", accounts);
        qputenv("MOCK_SLEEP", sleep);
        qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path() + "/config");
        QSettings().setValue("executable", QStringLiteral(MOCK_CLI));
        w = std::make_unique<MainWindow>();
        w->show();
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("signal-cli-qt");
    QApplication::setApplicationName("signal-cli-qt");
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, autoDialogs);
    dialogs.start(25);

    std::printf("pure functions\n");
    {
        CHECK(SignalCli::parseAccounts(R"([{"number":"+49123456789"}])") == QStringList{"+49123456789"});
        CHECK(SignalCli::parseAccounts("[]").isEmpty());
        CHECK(SignalCli::parseAccounts(R"([{"number":"+49111111"},{"number":"+49222222"}])").size() == 2);
        CHECK(SignalCli::parseAccounts("Number: +4915112345678\n") == QStringList{"+4915112345678"});
        CHECK(SignalCli::parseAccounts("WARNING: something\n[{\"number\":\"+49123456\"}]") == QStringList{"+49123456"});
        CHECK(SignalCli::parseAccounts("garbage without numbers").isEmpty());

        QProcessEnvironment base;
        base.insert("APPDIR", "/tmp/.mount_x");
        base.insert("LD_LIBRARY_PATH", "/tmp/.mount_x/usr/lib:/usr/lib");
        base.insert("PATH", "/tmp/.mount_x/usr/bin:/usr/bin:/bin");
        base.insert("QT_PLUGIN_PATH", "/tmp/.mount_x/usr/plugins");
        const QProcessEnvironment clean = SignalCli::childEnvironment(base);
        CHECK(clean.value("LD_LIBRARY_PATH") == "/usr/lib");
        CHECK(clean.value("PATH") == "/usr/bin:/bin");
        CHECK(!clean.contains("QT_PLUGIN_PATH"));
        QProcessEnvironment plain;
        plain.insert("LD_LIBRARY_PATH", "/opt/x/lib");
        CHECK(SignalCli::childEnvironment(plain).value("LD_LIBRARY_PATH") == "/opt/x/lib"); // untouched outside an AppImage

        qunsetenv("SIGNAL_CLI_QT_DEBUG");
        CHECK(!SignalCli::maskNumbers("account +4915112345678 ok").contains("15112345"));
    }

    std::printf("signal-cli fails -> the real error is shown, NOT \"no account\"\n");
    {
        Run r("fail");
        CHECK(waitFor([] { return g_boxSeen; }));
        CHECK(g_box.contains("boom: cannot read config"));
        CHECK(g_box.contains("exited with code 3"));
        CHECK(g_box.contains("Command:"));
        CHECK(!g_box.contains("No account"));
        CHECK(!g_box.contains("no account is set up"));
        CHECK(!status(*r.w).contains("Connected ·"));
    }

    std::printf("empty account list -> offer to link\n");
    {
        Run r("empty");
        CHECK(waitFor([] { return g_boxSeen; }));
        CHECK(g_box.contains("no account is set up"));
        CHECK(status(*r.w).contains("no account linked"));
    }

    std::printf("valid output but non-zero exit code -> still connects\n");
    {
        Run r("okexit1");
        CHECK(waitFor([&] { return status(*r.w).contains("Connected ·"); }));
        CHECK(!g_boxSeen);
    }

    std::printf("slow signal-cli -> the window stays responsive\n");
    {
        Run r("ok", "1.0"); // --version and listAccounts take one second each
        QElapsedTimer tick;
        tick.start();
        qint64 maxGap = 0;
        bool sawConnecting = false;
        QTimer ticker;
        QObject::connect(&ticker, &QTimer::timeout, [&] {
            const bool connecting = status(*r.w).contains("Connecting");
            if (connecting) {
                sawConnecting = true;
                maxGap = qMax(maxGap, tick.elapsed());
            }
            tick.restart();
        });
        ticker.start(10);
        CHECK(waitFor([&] { return status(*r.w).contains("Connected ·"); }));
        ticker.stop();
        CHECK(sawConnecting);
        std::printf("  longest event-loop gap while connecting: %lld ms\n", static_cast<long long>(maxGap));
        CHECK(maxGap < 400);
        CHECK(!g_boxSeen);
    }

    std::printf(g_failed ? "FAILED (%d checks)\n" : "ALL OK\n", g_failed);
    return g_failed ? 1 : 0;
}

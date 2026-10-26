// End-to-end: the "safety number changed" notice in the real MainWindow, against the mock signal-cli.
#include "MainWindow.h"
#include "MessageInput.h"
#include "PassphraseDialog.h"
#include "SafetyNumber.h"
#include "SafetyNumberDialog.h"

#include <QApplication>
#include <QDateTime>
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
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <cstdio>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static bool waitFor(const std::function<bool()> &cond, int ms = 15000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

static void autoDialogs()
{
    QWidget *w = QApplication::activeModalWidget();
    if (!w) return;
    if (auto *pd = qobject_cast<PassphraseDialog *>(w)) {
        for (QLineEdit *e : pd->findChildren<QLineEdit *>()) e->setText(QStringLiteral("test-passphrase-1"));
        pd->accept();
    } else if (auto *mb = qobject_cast<QMessageBox *>(w)) {
        if (mb->windowTitle() == QStringLiteral("Protect chat history")) {
            for (QAbstractButton *button : mb->buttons())
                if (button->text() == QStringLiteral("Use a passphrase")) button->click();
            return;
        }
        std::printf("  [unexpected message box] %s\n", qPrintable(mb->text()));
        mb->reject();
    }
}

static bool listHas(QListWidget *l, const QString &needle)
{
    for (int i = 0; i < l->count(); ++i)
        if (l->item(i)->text().contains(needle)) return true;
    return false;
}

static void selectItem(QListWidget *l, const QString &needle)
{
    for (int i = 0; i < l->count(); ++i)
        if (l->item(i)->text().contains(needle)) {
            l->setCurrentRow(i);
            return;
        }
}

static void inject(const QString &file, const QString &number, const QString &uuid, const QString &name,
                   qint64 ts, const QString &text)
{
    QFile f(file);
    f.open(QIODevice::Append);
    const QJsonObject env{{"sourceNumber", number}, {"sourceUuid", uuid}, {"sourceName", name}, {"timestamp", ts},
                          {"dataMessage", QJsonObject{{"timestamp", ts}, {"message", text}}}};
    f.write(QJsonDocument(env).toJson(QJsonDocument::Compact) + "\n");
}

static QJsonObject identity(const QString &number, const QString &uuid, const QString &safety, const QString &trust)
{
    return {{"number", number}, {"uuid", uuid}, {"fingerprint", "05ab"}, {"safetyNumber", safety},
            {"scannableSafetyNumber", "AA=="}, {"trustLevel", trust}, {"addedTimestamp", 1700000000000.0}};
}

static void writeIdentities(const QString &file, const QJsonArray &list)
{
    QFile f(file);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(QJsonDocument(list).toJson(QJsonDocument::Compact));
}

static QString digitsFor(int seed)
{
    QString d;
    while (d.size() < 60) d += QString::number(10000 + seed * 7919 + d.size() * 13).right(5);
    return d.left(60);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("corvus"));
    QApplication::setApplicationName(QStringLiteral("corvus"));

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path() + "/config");
    QSettings().setValue("executable", QStringLiteral(MOCK_CLI));
    QSettings().setValue("historyProtectionMode", QStringLiteral("passphrase"));

    const QString injectFile = tmp.path() + "/inject.jsonl";
    const QString identityFile = tmp.path() + "/identities.json";
    const QString logFile = tmp.path() + "/rpc.log";
    qputenv("MOCK_INJECT", injectFile.toUtf8());
    qputenv("MOCK_IDENTITY_FILE", identityFile.toUtf8());
    qputenv("MOCK_LOG", logFile.toUtf8());

    const QString aliceOld = digitsFor(1), aliceNew = digitsFor(2), bobOld = digitsFor(3), bobNew = digitsFor(4);
    writeIdentities(identityFile, {identity("+49111", "u-alice", aliceOld, "TRUSTED_UNVERIFIED"),
                                   identity("+49222", "u-bob", bobOld, "TRUSTED_UNVERIFIED")});

    QTimer dialogTimer;
    QObject::connect(&dialogTimer, &QTimer::timeout, autoDialogs);
    dialogTimer.start(30);

    MainWindow w;
    w.show();
    auto *list = w.findChild<QListWidget *>();
    auto *input = w.findChild<MessageInput *>();
    QWidget *bar = w.findChild<QWidget *>(QStringLiteral("safetyChangedBar"));
    CHECK(list && input && bar);
    if (!list || !input || !bar) return 1;
    auto barText = [&] {
        QString t;
        for (QLabel *l : bar->findChildren<QLabel *>()) t += l->text();
        return t;
    };
    auto knownStored = [](const QString &who) {
        QSettings s;
        s.sync();
        for (const QString &k : s.allKeys())
            if (k.startsWith(QStringLiteral("knownSafetyNumbers/")) && k.endsWith(who)) return s.value(k).toString();
        return QString();
    };

    CHECK(waitFor([&] { return listHas(list, "Team"); }));

    std::printf("1: first message from Alice -> her number is remembered, no warning\n");
    inject(injectFile, "+49111", "u-alice", "Alice", QDateTime::currentMSecsSinceEpoch(), "hello");
    CHECK(waitFor([&] { return listHas(list, "Alice (1)"); }));
    CHECK(waitFor([&] { return knownStored("+49111") == aliceOld; }));
    selectItem(list, "Alice");
    CHECK(!bar->isVisible());
    CHECK(!listHas(list, "⚠"));

    std::printf("2: Alice's key changes and is untrusted -> a send fails and the warning appears\n");
    writeIdentities(identityFile, {identity("+49111", "u-alice", aliceNew, "UNTRUSTED"),
                                   identity("+49222", "u-bob", bobOld, "TRUSTED_UNVERIFIED")});
    input->setText("are you there?");
    emit input->returnPressed();
    CHECK(waitFor([&] { return bar->isVisible(); }));
    CHECK(barText().contains("has changed"));
    CHECK(barText().contains("cannot be sent"));
    CHECK(listHas(list, "⚠"));
    CHECK(input->text() == "are you there?"); // the unsent message is not lost

    std::printf("3: verifying in the dialog clears the warning and sending works again\n");
    QPushButton *verifyBtn = bar->findChild<QPushButton *>(QStringLiteral("safetyVerifyButton"));
    CHECK(verifyBtn);
    if (verifyBtn) verifyBtn->click();
    SafetyNumberDialog *dialog = nullptr;
    CHECK(waitFor([&] { return (dialog = w.findChild<SafetyNumberDialog *>()) != nullptr; }));
    if (dialog) {
        auto dialogShows = [&](const QString &text) {
            for (QLabel *l : dialog->findChildren<QLabel *>())
                if (l->text().contains(text)) return true;
            return false;
        };
        CHECK(waitFor([&] { return dialogShows(SafetyNumber::format(aliceNew).left(23)); }));
        CHECK(dialogShows("Not trusted"));
        for (QLineEdit *e : dialog->findChildren<QLineEdit *>()) e->setText(SafetyNumber::format(aliceNew));
        QPushButton *mark = nullptr;
        for (QPushButton *b : dialog->findChildren<QPushButton *>())
            if (b->text() == "Mark as verified") mark = b;
        CHECK(mark && waitFor([&] { return mark->isEnabled(); }));
        if (mark) mark->click();
        CHECK(waitFor([&] { return !bar->isVisible(); }));
        CHECK(waitFor([&] { return !listHas(list, "⚠"); }));
        CHECK(knownStored("+49111") == aliceNew);
        dialog->close();
    }
    emit input->returnPressed();
    CHECK(waitFor([&] { return input->text().isEmpty(); }));
    CHECK(!bar->isVisible());

    std::printf("4: Bob writes with a key signal-cli does not trust -> flagged without any send\n");
    writeIdentities(identityFile, {identity("+49111", "u-alice", aliceNew, "TRUSTED_VERIFIED"),
                                   identity("+49222", "u-bob", bobNew, "UNTRUSTED")});
    inject(injectFile, "+49222", "u-bob", "Bob", QDateTime::currentMSecsSinceEpoch(), "new phone");
    CHECK(waitFor([&] { return listHas(list, "⚠ "); }));
    selectItem(list, "Bob");
    CHECK(waitFor([&] { return bar->isVisible(); }));
    CHECK(barText().contains("Bob"));
    CHECK(barText().contains("cannot be sent")); // an untrusted key: signal-cli refuses to send to it

    std::printf("5: dismissing accepts the new number\n");
    auto *dismiss = bar->findChild<QToolButton *>(QStringLiteral("safetyDismissButton"));
    CHECK(dismiss);
    if (dismiss) dismiss->click();
    CHECK(!bar->isVisible());
    CHECK(!listHas(list, "⚠"));
    CHECK(knownStored("+49222") == bobNew);

    w.close();
    std::printf(g_failed ? "FAILED (%d)\n" : "OK\n", g_failed);
    return g_failed ? 1 : 0;
}

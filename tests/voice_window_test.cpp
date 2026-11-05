// End-to-end: recording a voice message in the real MainWindow (mic button, recording bar, sending as a
// voice note), with a fake recorder instead of a microphone and the mock signal-cli.
#include "MainWindow.h"
#include "MessageInput.h"
#include "PassphraseDialog.h"
#include "VoiceRecorder.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
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
#include <QStatusBar>
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

static QString g_lastBox;

// Stands in for the microphone: the test says how long the recording was.
class FakeRecorder : public VoiceRecorder
{
public:
    explicit FakeRecorder(QObject *parent) : VoiceRecorder(parent) {}
    QString reason;          // non-empty: no microphone
    qint64 elapsed = 3000;   // duration reported when the recording ends
    QString lastPath;
    int started_ = 0, cancelled_ = 0;

    QString unavailableReason() const override { return reason; }
    void start(const QString &directory, const QString &baseName) override
    {
        QDir().mkpath(directory);
        lastPath = QDir(directory).filePath(baseName + ".wav");
        QFile f(lastPath);
        f.open(QIODevice::WriteOnly);
        f.write("RIFF....WAVEfmt ");
        f.close();
        m_recording = true;
        ++started_;
        emit started();
    }
    void tick(qint64 ms) { emit durationChanged(ms); }
    void stop() override
    {
        if (!m_recording) return;
        m_recording = false;
        const QString path = lastPath;
        const qint64 ms = elapsed;
        QTimer::singleShot(0, this, [this, path, ms] { emit finished(path, ms); });
    }
    void cancel() override
    {
        if (!m_recording) return;
        m_recording = false;
        ++cancelled_;
        QFile::remove(lastPath);
    }
    bool isRecording() const override { return m_recording; }

private:
    bool m_recording = false;
};

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
        g_lastBox = mb->text();
        mb->accept();
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

static void inject(const QString &file, const QString &number, const QString &uuid, const QString &name, qint64 ts,
                   const QString &text)
{
    QFile f(file);
    f.open(QIODevice::Append);
    const QJsonObject env{{"sourceNumber", number}, {"sourceUuid", uuid}, {"sourceName", name}, {"timestamp", ts},
                          {"dataMessage", QJsonObject{{"timestamp", ts}, {"message", text}}}};
    f.write(QJsonDocument(env).toJson(QJsonDocument::Compact) + "\n");
}

static QList<QJsonObject> calls(const QString &logFile, const QString &method)
{
    QList<QJsonObject> out;
    QFile f(logFile);
    if (!f.open(QIODevice::ReadOnly)) return out;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o["method"].toString() == method) out << o;
    }
    return out;
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
    const QString logFile = tmp.path() + "/rpc.log";
    qputenv("MOCK_INJECT", injectFile.toUtf8());
    qputenv("MOCK_LOG", logFile.toUtf8());

    FakeRecorder *fake = nullptr;
    VoiceRecording::setFactoryForTesting([&fake](QObject *parent) {
        fake = new FakeRecorder(parent);
        return static_cast<VoiceRecorder *>(fake);
    });

    QTimer dialogTimer;
    QObject::connect(&dialogTimer, &QTimer::timeout, autoDialogs);
    dialogTimer.start(30);

    MainWindow w;
    w.show();
    CHECK(fake);
    if (!fake) return 1;
    auto *list = w.findChild<QListWidget *>("conversationList");
    auto *input = w.findChild<MessageInput *>();
    auto *mic = w.findChild<QToolButton *>("voiceButton");
    QWidget *bar = w.findChild<QWidget *>("recordBar");
    CHECK(list && input && mic && bar);
    if (!list || !input || !mic || !bar) return 1;
    auto button = [&](const char *name) { return bar->findChild<QPushButton *>(QLatin1String(name)); };
    CHECK(waitFor([&] { return listHas(list, "Team"); }));
    inject(injectFile, "+49111", "u-alice", "Alice", QDateTime::currentMSecsSinceEpoch(), "hello");
    inject(injectFile, "+49222", "u-bob", "Bob", QDateTime::currentMSecsSinceEpoch() + 1, "hi");
    CHECK(waitFor([&] { return listHas(list, "Alice") && listHas(list, "Bob"); }));
    selectItem(list, "Alice");
    CHECK(waitFor([&] { return mic->isEnabled(); }));
    CHECK(!bar->isVisible());

    std::printf("1: no microphone -> explained, nothing starts\n");
    fake->reason = "No microphone was found.";
    g_lastBox.clear();
    mic->click();
    CHECK(waitFor([&] { return g_lastBox.contains("No microphone"); }));
    CHECK(!bar->isVisible() && fake->started_ == 0);
    fake->reason.clear();

    std::printf("2: record and send straight away -> a voice note\n");
    mic->click();
    CHECK(bar->isVisible());
    CHECK(!input->isEnabled() && !mic->isEnabled()); // the composer is not usable while recording
    fake->tick(7400);
    CHECK(bar->findChild<QLabel *>("recordTime")->text() == "0:07");
    fake->elapsed = 7400;
    const QString firstFile = fake->lastPath;
    button("recordSendButton")->click();
    CHECK(waitFor([&] { return !calls(logFile, "send").isEmpty(); }));
    {
        const QJsonObject p = calls(logFile, "send").last()["params"].toObject();
        CHECK(p["voiceNote"].toBool());
        CHECK(p["attachments"].toArray().size() == 1);
        CHECK(p["attachments"].toArray().first().toString().endsWith(".wav"));
        CHECK(!p.contains("message")); // no text
    }
    CHECK(!bar->isVisible() && input->isEnabled() && mic->isEnabled());

    std::printf("3: discard -> no file, nothing queued\n");
    mic->click();
    CHECK(bar->isVisible());
    const QString discarded = fake->lastPath;
    button("recordCancelButton")->click();
    CHECK(!bar->isVisible() && input->isEnabled());
    CHECK(fake->cancelled_ == 1 && !QFileInfo::exists(discarded));
    const int sendsBefore = calls(logFile, "send").size();

    std::printf("4: too short -> dropped\n");
    mic->click();
    fake->elapsed = 200;
    button("recordStopButton")->click();
    CHECK(waitFor([&] { return !bar->isVisible(); }));
    CHECK(w.statusBar()->currentMessage().contains("too short"));
    CHECK(!QFileInfo::exists(fake->lastPath));

    std::printf("5: stop -> attached, add text, send with the voice note\n");
    mic->click();
    fake->elapsed = 4000;
    button("recordStopButton")->click();
    CHECK(waitFor([&] { return !bar->isVisible(); }));
    CHECK(calls(logFile, "send").size() == sendsBefore); // stop does not send
    QLabel *attachLabel = nullptr;
    CHECK(waitFor([&] {
        for (QLabel *l : w.findChildren<QLabel *>())
            if (l->isVisible() && l->text().contains("Voice message (0:04)")) attachLabel = l;
        return attachLabel != nullptr;
    }));
    input->setText("listen to this");
    emit input->returnPressed();
    CHECK(waitFor([&] { return calls(logFile, "send").size() == sendsBefore + 1; }));
    {
        const QJsonObject p = calls(logFile, "send").last()["params"].toObject();
        CHECK(p["voiceNote"].toBool());
        CHECK(p["message"].toString() == "listen to this");
    }

    std::printf("6: a recording stays with its chat when another one is opened\n");
    waitFor([] { return false; }, 700); // let the answer of the previous send arrive (it clears the status line)
    mic->click();
    fake->elapsed = 5000;
    selectItem(list, "Bob");
    CHECK(waitFor([&] { return !bar->isVisible(); }));
    bool sawDraftNote = false;
    CHECK(waitFor([&] {
        if (w.statusBar()->currentMessage().contains("draft")) sawDraftNote = true;
        return sawDraftNote;
    }, 3000));
    std::printf("   status now: [%s]\n", qPrintable(w.statusBar()->currentMessage()));
    CHECK(calls(logFile, "send").size() == sendsBefore + 1);
    selectItem(list, "Alice");
    CHECK(waitFor([&] {
        for (QLabel *l : w.findChildren<QLabel *>())
            if (l->isVisible() && l->text().contains("Voice message (0:05)")) return true;
        return false;
    }));
    // discarding the queued recording deletes the file
    const QString queued = fake->lastPath;
    CHECK(QFileInfo::exists(queued));
    for (QToolButton *b : w.findChildren<QToolButton *>())
        if (b->isVisible() && b->text() == QStringLiteral("✕") && b->parentWidget() &&
            b->parentWidget()->findChild<QLabel *>() &&
            b->parentWidget()->findChild<QLabel *>()->text().contains("Voice message"))
            b->click();
    CHECK(waitFor([&] { return !QFileInfo::exists(queued); }));

    std::printf("7: a normal file is not sent as a voice note\n");
    const QString plain = tmp.path() + "/music.mp3";
    QFile pf(plain);
    pf.open(QIODevice::WriteOnly);
    pf.write("ID3");
    pf.close();
    w.addPendingFiles({plain});
    input->setText("song");
    emit input->returnPressed();
    CHECK(waitFor([&] { return calls(logFile, "send").size() == sendsBefore + 2; }));
    CHECK(!calls(logFile, "send").last()["params"].toObject().contains("voiceNote"));

    w.close();
    std::printf(g_failed ? "FAILED (%d)\n" : "OK\n", g_failed);
    return g_failed ? 1 : 0;
}

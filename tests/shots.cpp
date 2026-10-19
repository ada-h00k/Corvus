// Screenshot harness: real MainWindow + mock signal-cli, Breeze colour scheme.
#include "ChatView.h"
#include "MainWindow.h"
#include "PassphraseDialog.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QIcon>
#include <cstdio>

static bool waitFor(const std::function<bool()> &c, int ms = 15000) {
    QElapsedTimer t; t.start();
    while (!c()) { if (t.elapsed() > ms) return false; QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(5); }
    return true;
}
static void pump(int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); QThread::msleep(5);} }

static QColor rgb(QSettings &s, const QString &g, const QString &k) {
    const QStringList p = s.value(g + "/" + k).toStringList();
    return p.size() >= 3 ? QColor(p[0].toInt(), p[1].toInt(), p[2].toInt()) : QColor();
}
static QPalette breezePalette(const QString &file) {
    QSettings s(file, QSettings::IniFormat);
    QPalette p;
    auto set = [&](QPalette::ColorRole r, const QString &g, const QString &k) { QColor c = rgb(s, g, k); if (c.isValid()) p.setColor(r, c); };
    set(QPalette::Window, "Colors:Window", "BackgroundNormal");
    set(QPalette::WindowText, "Colors:Window", "ForegroundNormal");
    set(QPalette::Base, "Colors:View", "BackgroundNormal");
    set(QPalette::AlternateBase, "Colors:View", "BackgroundAlternate");
    set(QPalette::Text, "Colors:View", "ForegroundNormal");
    set(QPalette::Button, "Colors:Button", "BackgroundNormal");
    set(QPalette::ButtonText, "Colors:Button", "ForegroundNormal");
    set(QPalette::Highlight, "Colors:Selection", "BackgroundNormal");
    set(QPalette::HighlightedText, "Colors:Selection", "ForegroundNormal");
    set(QPalette::ToolTipBase, "Colors:Tooltip", "BackgroundNormal");
    set(QPalette::ToolTipText, "Colors:Tooltip", "ForegroundNormal");
    set(QPalette::Link, "Colors:View", "ForegroundLink");
    set(QPalette::LinkVisited, "Colors:View", "ForegroundVisited");
    set(QPalette::PlaceholderText, "Colors:View", "ForegroundInactive");
    QColor dis = rgb(s, "Colors:View", "ForegroundInactive");
    if (dis.isValid()) for (auto r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) p.setColor(QPalette::Disabled, r, dis);
    QColor mid = rgb(s, "Colors:Window", "BackgroundAlternate");
    if (mid.isValid()) { p.setColor(QPalette::Mid, mid); p.setColor(QPalette::Midlight, mid); p.setColor(QPalette::Dark, mid.darker(150)); p.setColor(QPalette::Light, mid.lighter(130)); }
    return p;
}
static void inject(const QString &file, const QJsonObject &env) {
    QFile f(file); f.open(QIODevice::Append);
    f.write(QJsonDocument(env).toJson(QJsonDocument::Compact) + "\n");
}
static void autoDialogs() {
    QWidget *w = QApplication::activeModalWidget();
    if (!w) return;
    if (auto *pd = qobject_cast<PassphraseDialog *>(w)) {
        for (QLineEdit *e : pd->findChildren<QLineEdit *>()) e->setText("screenshot-passphrase");
        pd->accept();
    } else if (auto *mb = qobject_cast<QMessageBox *>(w)) {
        for (QAbstractButton *b : mb->buttons()) if (b->text() == "Use a passphrase") { b->click(); return; }
        mb->reject();
    }
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    const QString scheme = argv[1], out = argv[2], mock = argv[3];
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Corvus");
    QApplication::setApplicationName("Corvus");
    if (scheme != "native") {  // "native": use the real desktop style (e.g. Breeze under Plasma)
        QApplication::setStyle(QStyleFactory::create("Fusion"));
        QApplication::setPalette(breezePalette(scheme));
        QIcon::setThemeName("breeze");
        QFont f("Noto Sans", 10); QApplication::setFont(f);
    }

    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path() + "/config");
    QSettings().setValue("executable", mock);
    QSettings().setValue("historyProtectionMode", "passphrase");
    const QString injectFile = tmp.path() + "/inject.jsonl";
    qputenv("MOCK_INJECT", injectFile.toUtf8());

    QTimer dt; QObject::connect(&dt, &QTimer::timeout, autoDialogs); dt.start(30);

    MainWindow w;
    w.resize(1000, 640);
    w.show();
    auto *list = w.findChild<QListWidget *>();
    auto *view = w.findChild<ChatView *>();
    auto has = [&](const QString &n) { for (int i = 0; i < list->count(); ++i) if (list->item(i)->text().contains(n)) return true; return false; };
    if (!waitFor([&] { return has("Team"); })) { std::printf("no list\n"); return 1; }

    const qint64 base = QDateTime::currentMSecsSinceEpoch() - 3600 * 1000;
    auto msg = [&](qint64 off, const QString &text, QJsonObject extra = {}) {
        QJsonObject dm{{"timestamp", base + off}, {"message", text}};
        for (auto it = extra.begin(); it != extra.end(); ++it) dm[it.key()] = it.value();
        inject(injectFile, QJsonObject{{"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"sourceName", "Alice"}, {"timestamp", base + off}, {"dataMessage", dm}});
    };
    auto mine = [&](qint64 off, const QString &text, QJsonObject extra = {}) {
        QJsonObject sm{{"destinationNumber", "+49111"}, {"timestamp", base + off}, {"message", text}};
        for (auto it = extra.begin(); it != extra.end(); ++it) sm[it.key()] = it.value();
        inject(injectFile, QJsonObject{{"sourceNumber", "+491234567"}, {"timestamp", base + off}, {"syncMessage", QJsonObject{{"sentMessage", sm}}}});
    };
    msg(0, "Hey! Have you tried the new version of Corvus yet?");
    mine(60000, "Yes, it works great. The history is even encrypted 🔒");
    msg(120000, "Cool! Check out my screenshot 📷", {{"attachments", QJsonArray{QJsonObject{{"contentType", "image/png"}, {"filename", "screenshot.png"}, {"id", "att1"}, {"size", 6321}}}}});
    mine(180000, "Looks good!", {{"quote", QJsonObject{{"id", base + 120000}, {"authorNumber", "+49111"}, {"text", "Cool! Check out my screenshot 📷"}}}});
    msg(240000, "Shall we meet later? More info: https://github.com/ada-h00k/Corvus");
    inject(injectFile, QJsonObject{{"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"timestamp", base + 250000},
        {"dataMessage", QJsonObject{{"timestamp", base + 250000}, {"reaction", QJsonObject{{"emoji", "👍"}, {"targetAuthorNumber", "+491234567"}, {"targetSentTimestamp", base + 180000}, {"isRemove", false}}}}}});
    inject(injectFile, QJsonObject{{"sourceNumber", "+49222"}, {"sourceUuid", "u-bob"}, {"sourceName", "Bob"}, {"timestamp", base + 300000},
        {"dataMessage", QJsonObject{{"timestamp", base + 300000}, {"message", "Are you around tonight?"}}}});
    waitFor([&] { return has("Alice"); });
    waitFor([&] { return has("Bob"); });
    for (int i = 0; i < list->count(); ++i) if (list->item(i)->text().contains("Alice")) { list->setCurrentRow(i); break; }
    waitFor([&] { return view->messageCount() >= 5; });
    waitFor([&] { return view->plainText().contains("[image: screenshot.png]"); });
    pump(1500);
    for (int i = 0; i < list->count(); ++i) if (list->item(i)->text().contains("Alice")) { list->setCurrentRow(i); break; }
    pump(800);
    const bool ok = w.grab().save(out);
    std::printf("saved %s: %d (messages=%d)\n", qPrintable(out), ok, view->messageCount());
    return ok ? 0 : 1;
}

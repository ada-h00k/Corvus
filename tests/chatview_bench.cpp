// Layout benchmark for the bubble view (not part of ctest).
//   QT_QPA_PLATFORM=offscreen ./chatview_bench [messages]
// Prints timings and a checksum over every bubble/text rectangle: the checksum must be identical
// before and after a performance change (same layout, only faster).
#include "Appearance.h"
#include "ChatView.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <ctime>

static QList<Message> makeMessages(int n)
{
    static const char *words[] = {"hello", "world", "meeting", "tomorrow", "ok", "thanks", "lunch", "see", "you",
                                  "later", "https://example.org/some/long/path?a=1&b=2", "😀", "weekend", "plan"};
    QList<Message> out;
    unsigned seed = 12345;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < n; ++i) {
        Message m;
        m.time = QDateTime::fromMSecsSinceEpoch(1700000000000LL + qint64(i) * 60000);
        m.outgoing = rnd() % 2;
        m.sender = m.outgoing ? "You" : "Alice";
        m.authorKey = m.outgoing ? "+491234567" : "+49111";
        QString text;
        const int len = 1 + int(rnd() % (i % 7 == 0 ? 60 : 12));
        for (int w = 0; w < len; ++w) text += QString::fromUtf8(words[rnd() % 14]) + (rnd() % 9 == 0 ? "\n" : " ");
        m.text = text.trimmed();
        if (i % 11 == 0 && m.text.size() > 5) m.textStyles << TextStyleRange{0, 5, "BOLD"};
        if (i % 97 == 0) { m.deleted = true; m.text.clear(); }
        out << m;
    }
    return out;
}

static quint64 checksum(const ChatView &v)
{
    quint64 h = 1469598103934665603ULL;
    auto mix = [&h](int x) { h = (h ^ quint64(quint32(x))) * 1099511628211ULL; };
    for (int i = 0; i < v.messageCount(); ++i) {
        const QRect b = v.bubbleRect(i), t = v.textRect(i);
        mix(b.x()); mix(b.y()); mix(b.width()); mix(b.height());
        mix(t.x()); mix(t.y()); mix(t.width()); mix(t.height());
    }
    return h;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("corvus-bench");
    QApplication::setApplicationName("chatview-bench");
    QTemporaryDir tmp;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path());
    const int n = argc > 1 ? QString(argv[1]).toInt() : 20000;

    Appearance ap;
    ap.background = QColor("#ffffff"); ap.outBg = QColor("#2c6bed"); ap.outText = QColor("#ffffff");
    ap.inBg = QColor("#e9e9eb"); ap.inText = QColor("#1b1b1b"); ap.radius = 16;

    ChatView view;
    view.setSelfKey("+491234567");
    view.setNameResolver([](const QString &k) { return k; });
    view.setAppearance(ap);
    view.resize(900, 700);
    view.show();
    QCoreApplication::processEvents();

    const QList<Message> msgs = makeMessages(n);
    QElapsedTimer t;
    auto step = [&](const char *name, auto &&fn) {
        t.start();
        fn();
        QCoreApplication::processEvents();
        std::printf("%-34s %8.1f ms   checksum %016llx\n", name, t.nsecsElapsed() / 1e6,
                    static_cast<unsigned long long>(checksum(view)));
    };
    std::printf("messages: %d\n", n);
    step("open chat (cold)", [&] { view.showMessages(msgs); });
    step("switch away and back (warm)", [&] { view.showMessages(msgs); });
    // The relayout after a resize is debounced by a timer, so measure CPU time while it settles.
    auto cpuMs = [] { timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; };
    auto resizeStep = [&](const char *name, int w) {
        const double c0 = cpuMs();
        view.resize(w, 700);
        for (int i = 0; i < 30; ++i) { QCoreApplication::processEvents(); QThread::msleep(10); }
        std::printf("%-34s %8.1f ms CPU  checksum %016llx\n", name, cpuMs() - c0,
                    static_cast<unsigned long long>(checksum(view)));
    };
    resizeStep("resize 900 -> 1000 (same wrap)", 1000);
    resizeStep("resize 1000 -> 500 (new wrap)", 500);
    resizeStep("resize 500 -> 900 (back)", 900);
    resizeStep("resize 900 -> 1000 (again)", 1000);
    return 0;
}

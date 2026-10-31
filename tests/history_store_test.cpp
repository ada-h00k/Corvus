// Tests for HistoryStore's background writer: ordering, batching, async rewrite, flush on close.
#include "HistoryStore.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <random>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static QJsonObject msg(qint64 ts)
{
    return QJsonObject{{"t", "m"}, {"c", "+491"}, {"ts", ts}, {"s", "Alice"},
                       {"x", QString("text %1").arg(ts)}, {"o", ts % 2 == 0}};
}

static QList<qint64> tsOf(const QJsonArray &a)
{
    QList<qint64> out;
    for (const QJsonValue &v : a) out << v.toObject().value("ts").toVariant().toLongLong();
    return out;
}

static QList<qint64> range(qint64 from, qint64 to) // [from, to)
{
    QList<qint64> out;
    for (qint64 i = from; i < to; ++i) out << i;
    return out;
}

static QJsonArray snapshot(const QList<qint64> &ts)
{
    QJsonArray a;
    for (qint64 t : ts) a << msg(t);
    return a;
}

static bool pumpUntil(const std::function<bool()> &pred, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    int n = 0;
    auto newPath = [&] { return tmp.path() + QString("/h%1.corvus").arg(n++); };

    std::printf("appends keep their order and survive close/reopen\n");
    {
        const QString p = newPath();
        HistoryStore s;
        CHECK(s.create(p, "pw") == SC_OK);
        for (qint64 i = 0; i < 3000; ++i) CHECK(s.append(msg(i)) == SC_OK);
        s.close();
        QJsonArray recs;
        CHECK(s.open(p, "pw", &recs) == SC_OK);
        CHECK(tsOf(recs) == range(0, 3000));
    }

    std::printf("append on a closed store is rejected\n");
    {
        HistoryStore s;
        CHECK(s.append(msg(1)) == SC_ERR_ARG);
        bool called = false;
        s.rewriteAsync(QJsonArray(), qApp, [&](int rc) { called = rc == SC_ERR_ARG; });
        CHECK(pumpUntil([&] { return called; }));
    }

    std::printf("rewriteAsync is ordered between appends and reports on this thread\n");
    {
        const QString p = newPath();
        HistoryStore s;
        CHECK(s.create(p, "pw") == SC_OK);
        for (qint64 i = 0; i < 50; ++i) s.append(msg(i));
        QList<qint64> kept;
        for (qint64 i = 0; i < 50; i += 2) kept << i;
        int rc = -1;
        QThread *cbThread = nullptr;
        s.rewriteAsync(snapshot(kept), qApp, [&](int r) { rc = r; cbThread = QThread::currentThread(); });
        for (qint64 i = 100; i < 120; ++i) s.append(msg(i)); // after the rewrite
        CHECK(pumpUntil([&] { return rc != -1; }));
        CHECK(rc == SC_OK);
        CHECK(cbThread == QThread::currentThread());
        s.close();
        QJsonArray recs;
        CHECK(s.open(p, "pw", &recs) == SC_OK);
        CHECK(tsOf(recs) == kept + range(100, 120));
    }

    std::printf("sync rewrite waits for queued appends and replaces the file\n");
    {
        const QString p = newPath();
        HistoryStore s;
        CHECK(s.create(p, "pw") == SC_OK);
        for (qint64 i = 0; i < 500; ++i) s.append(msg(i));
        CHECK(s.rewrite(snapshot(range(0, 10))) == SC_OK);
        s.append(msg(999));
        s.close();
        QJsonArray recs;
        CHECK(s.open(p, "pw", &recs) == SC_OK);
        CHECK(tsOf(recs) == range(0, 10) + QList<qint64>{999});
    }

    std::printf("changePassphrase keeps queued records\n");
    {
        const QString p = newPath();
        HistoryStore s;
        CHECK(s.create(p, "old") == SC_OK);
        for (qint64 i = 0; i < 200; ++i) s.append(msg(i));
        CHECK(s.changePassphrase("old", "new") == SC_OK);
        s.append(msg(200));
        s.close();
        QJsonArray recs;
        CHECK(s.open(p, "old", &recs) == SC_ERR_WRONG_PASSPHRASE);
        CHECK(s.open(p, "new", &recs) == SC_OK);
        CHECK(tsOf(recs) == range(0, 201));
    }

    std::printf("a malformed record does not take its neighbours down\n");
    {
        const QString p = newPath();
        HistoryStore s;
        int errors = 0;
        s.setAppendErrorHandler(qApp, [&](int rc) { if (rc == SC_ERR_ARG) ++errors; });
        CHECK(s.create(p, "pw") == SC_OK);
        s.append(msg(1));
        s.append(QJsonObject{{"t", "no-such-kind"}});
        s.append(msg(2));
        s.close();
        pumpUntil([&] { return errors > 0; }, 2000);
        CHECK(errors >= 1);
        QJsonArray recs;
        CHECK(s.open(p, "pw", &recs) == SC_OK);
        CHECK(tsOf(recs) == (QList<qint64>{1, 2}));
    }

    std::printf("stress: random appends and rewrites equal an in-memory model\n");
    {
        const QString p = newPath();
        HistoryStore s;
        CHECK(s.create(p, "pw") == SC_OK);
        std::mt19937 rng(42);
        QList<qint64> model;
        qint64 next = 0;
        int pendingDone = 0, okDone = 0;
        for (int step = 0; step < 6000; ++step) {
            const unsigned r = rng() % 100;
            if (r < 90) {
                model << next;
                s.append(msg(next++));
            } else if (r < 97) {
                for (int i = model.size() - 1; i >= 0; --i)
                    if (rng() % 3 == 0) model.removeAt(i);
                ++pendingDone;
                s.rewriteAsync(snapshot(model), qApp, [&](int rc) { if (rc == SC_OK) ++okDone; });
            } else if (r < 99) {
                for (int i = model.size() - 1; i >= 0; --i)
                    if (rng() % 5 == 0) model.removeAt(i);
                CHECK(s.rewrite(snapshot(model)) == SC_OK);
            } else {
                s.flush();
            }
            if (step % 200 == 0) QCoreApplication::processEvents();
        }
        s.close();
        CHECK(pumpUntil([&] { return okDone == pendingDone; }));
        QJsonArray recs;
        CHECK(s.open(p, "pw", &recs) == SC_OK);
        CHECK(tsOf(recs) == model);
    }

    std::printf(g_failed ? "FAILED (%d checks)\n" : "ALL OK\n", g_failed);
    return g_failed ? 1 : 0;
}

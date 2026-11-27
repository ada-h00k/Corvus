// The backend's stdout is handled in short slices (SignalCli::drain), so a big backlog of
// envelopes and very long lines (base64 attachments) cannot freeze the GUI thread.
//
// The fake backend is a small script that prints a backlog and then waits.
#include "SignalCli.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {

// Burns CPU on the GUI thread, like the real per-message work (history, list, notifications).
void burnMicroseconds(int us)
{
    QElapsedTimer t;
    t.start();
    while (t.nsecsElapsed() < qint64(us) * 1000) {
    }
}

} // namespace

class SignalCliDrainTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString writeBackend(int envelopes, int bigLineBytes)
    {
        const QString path = m_dir.path() + QStringLiteral("/fake-backend-%1-%2.py").arg(envelopes).arg(bigLineBytes);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        f.write(QByteArray(R"(#!/usr/bin/env python3
import sys, json
N = %1
BIG = %2
out = sys.stdout
for i in range(N):
    out.write('{"jsonrpc":"2.0","method":"receive","params":{"envelope":{"sourceNumber":"+49111","timestamp":%d,"dataMessage":{"message":"m"}}}}\n' % (1000 + i))
    if BIG and i == N // 2:
        out.write('{"jsonrpc":"2.0","method":"attachmentDownloaded","params":{"id":"big","pad":"' + 'A' * BIG + '"}}\n')
out.flush()
for req in sys.stdin:  # answer calls, like the real backend
    try:
        rid = json.loads(req).get('id')
    except Exception:
        continue
    out.write('{"jsonrpc":"2.0","id":%d,"result":"pong"}\n' % rid)
    out.flush()
)").replace("%1", QByteArray::number(envelopes)).replace("%2", QByteArray::number(bigLineBytes)));
        f.close();
        f.setPermissions(f.permissions() | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
        return path;
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // Every line arrives, in order, including one that is many megabytes long.
    void deliversEverythingInOrder()
    {
        const int kEnvelopes = 3000;
        SignalCli cli;
        cli.setExecutable(writeBackend(kEnvelopes, 6 * 1024 * 1024));
        QList<qint64> stamps;
        QStringList downloaded;
        connect(&cli, &SignalCli::envelopeReceived, this, [&](const QJsonObject &env) {
            stamps << env.value(QStringLiteral("timestamp")).toVariant().toLongLong();
        });
        connect(&cli, &SignalCli::attachmentDownloaded, this, [&](const QString &id) { downloaded << id; });

        cli.start(QStringLiteral("+49000"));
        QTRY_COMPARE_WITH_TIMEOUT(stamps.size(), kEnvelopes, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(downloaded.size(), 1, 5000);
        QCOMPARE(downloaded.first(), QStringLiteral("big"));
        for (int i = 0; i < stamps.size(); ++i) QCOMPARE(stamps.at(i), qint64(1000 + i));
        cli.stop();
    }

    // While a backlog is processed the event loop keeps turning: a 5 ms timer must not starve.
    void keepsTheEventLoopResponsive()
    {
        const int kEnvelopes = 4000;
        SignalCli cli;
        cli.setExecutable(writeBackend(kEnvelopes, 0));
        int received = 0;
        connect(&cli, &SignalCli::envelopeReceived, this, [&](const QJsonObject &) {
            burnMicroseconds(150); // 4000 x 150 us = 0.6 s of GUI work in total
            ++received;
        });

        QElapsedTimer sinceTick;
        qint64 maxGapMs = 0;
        QTimer ticker;
        ticker.setInterval(5);
        connect(&ticker, &QTimer::timeout, this, [&] {
            if (sinceTick.isValid()) maxGapMs = std::max(maxGapMs, sinceTick.elapsed());
            sinceTick.restart();
        });
        ticker.start();

        cli.start(QStringLiteral("+49000"));
        QTRY_COMPARE_WITH_TIMEOUT(received, kEnvelopes, 30000);
        ticker.stop();
        cli.stop();
        // One slice is ~12 ms plus the line being handled; allow generous headroom for slow CI machines.
        QVERIFY2(maxGapMs < 120, qPrintable(QStringLiteral("event loop was blocked for %1 ms").arg(maxGapMs)));
    }

    // A callback may stop the backend in the middle of a slice: nothing is delivered afterwards.
    void stopInsideACallbackDropsTheRest()
    {
        const int kEnvelopes = 2000;
        SignalCli cli;
        cli.setExecutable(writeBackend(kEnvelopes, 0));
        int received = 0;
        connect(&cli, &SignalCli::envelopeReceived, this, [&](const QJsonObject &) {
            ++received;
            if (received == 50) cli.stop();
        });
        cli.start(QStringLiteral("+49000"));
        QTRY_VERIFY_WITH_TIMEOUT(received >= 50, 10000);
        QTest::qWait(300);
        QCOMPARE(received, 50);
        QVERIFY(!cli.isRunning());
    }
    // The answer to a call sits behind the whole backlog on the pipe, but is handled at once:
    // the queued messages are worked off in the background afterwards.
    void answersDoNotWaitForTheBacklog()
    {
        const int kEnvelopes = 4000;
        SignalCli cli;
        cli.setExecutable(writeBackend(kEnvelopes, 0));
        int received = 0;
        int receivedWhenAnswered = -1;
        connect(&cli, &SignalCli::envelopeReceived, this, [&](const QJsonObject &) {
            burnMicroseconds(150);
            ++received;
        });
        cli.start(QStringLiteral("+49000"));
        cli.call(QStringLiteral("ping"), {}, [&](const QJsonValue &res, const QString &err) {
            QVERIFY(err.isEmpty());
            QCOMPARE(res.toString(), QStringLiteral("pong"));
            receivedWhenAnswered = received;
        });
        QTRY_VERIFY_WITH_TIMEOUT(receivedWhenAnswered >= 0, 20000);
        QVERIFY2(receivedWhenAnswered < kEnvelopes / 2,
                 qPrintable(QStringLiteral("answer came after %1 of %2 messages").arg(receivedWhenAnswered).arg(kEnvelopes)));
        QTRY_COMPARE_WITH_TIMEOUT(received, kEnvelopes, 30000); // and nothing is lost
        QTRY_COMPARE_WITH_TIMEOUT(cli.pendingBacklog(), 0, 5000);
        cli.stop();
    }
};

QTEST_GUILESS_MAIN(SignalCliDrainTest)
#include "signalcli_drain_test.moc"

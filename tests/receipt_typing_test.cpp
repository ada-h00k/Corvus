#include "Envelope.h"
#include "SignalCli.h"
#include "Records.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

class ReceiptTypingTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesReadReceipt()
    {
        constexpr qint64 first = 1'790'000'000'123LL;
        constexpr qint64 second = 1'790'000'000'456LL;
        const QJsonObject envelope{
            {"sourceNumber", "+49111"},
            {"timestamp", 1'790'000'000'999LL},
            {"receiptMessage", QJsonObject{
                {"isRead", true},
                {"isDelivery", false},
                {"timestamps", QJsonArray{QJsonValue(first), QJsonValue(second)}}
            }}
        };

        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QCOMPARE(parsed.kind, ParsedEnvelope::Receipt);
        QCOMPARE(parsed.src, QStringLiteral("+49111"));
        QVERIFY(parsed.readReceipt);
        QVERIFY(!parsed.deliveryReceipt);
        QCOMPARE(parsed.receiptTimeMs, qint64(1'790'000'000'999LL));
        QCOMPARE(parsed.receiptTimestamps, QList<qint64>({first, second}));
    }

    void parsesDeliveryReceipt()
    {
        constexpr qint64 timestamp = 1'790'000'000'789LL;
        const ParsedEnvelope parsed = parseEnvelope(QJsonObject{
            {"sourceUuid", "remote-device-uuid"},
            {"receiptMessage", QJsonObject{
                {"isRead", false},
                {"isDelivery", true},
                {"timestamps", QJsonArray{QString::number(timestamp)}}
            }}
        });

        QCOMPARE(parsed.kind, ParsedEnvelope::Receipt);
        QVERIFY(!parsed.readReceipt);
        QVERIFY(parsed.deliveryReceipt);
        QCOMPARE(parsed.receiptTimestamps, QList<qint64>({timestamp}));
    }

    void parsesTypingStartAndStopForDirectAndGroupChats()
    {
        const ParsedEnvelope directStart = parseEnvelope(QJsonObject{
            {"sourceNumber", "+49111"},
            {"typingMessage", QJsonObject{{"action", "STARTED"}}}
        });
        QCOMPARE(directStart.kind, ParsedEnvelope::Typing);
        QVERIFY(directStart.typingStarted);
        QVERIFY(directStart.groupId.isEmpty());

        const ParsedEnvelope groupStop = parseEnvelope(QJsonObject{
            {"sourceUuid", "member-uuid"},
            {"typingMessage", QJsonObject{{"action", "STOPPED"}, {"groupId", "opaque-group-id"}}}
        });
        QCOMPARE(groupStop.kind, ParsedEnvelope::Typing);
        QVERIFY(!groupStop.typingStarted);
        QCOMPARE(groupStop.groupId, QStringLiteral("opaque-group-id"));
        QCOMPARE(groupStop.src, QStringLiteral("member-uuid"));
    }

    void normalizesSeparateTypingNotification()
    {
        const QJsonObject notification{
            {"method", "receiveTyping"},
            {"params", QJsonObject{
                {"sourceNumber", "+49111"},
                {"sourceUuid", "peer-uuid"},
                {"sourceName", "Alice"},
                {"timestamp", 1'790'000'001'234LL},
                {"action", "STARTED"},
                {"groupId", "opaque-group-id"}
            }}
        };

        const QJsonObject envelope = SignalCli::envelopeFromNotification(notification);
        QVERIFY(!envelope.isEmpty());
        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QCOMPARE(parsed.kind, ParsedEnvelope::Typing);
        QCOMPARE(parsed.src, QStringLiteral("+49111"));
        QCOMPARE(parsed.srcName, QStringLiteral("Alice"));
        QCOMPARE(parsed.groupId, QStringLiteral("opaque-group-id"));
        QVERIFY(parsed.typingStarted);
    }

    void preservesStandardReceiveTypingEnvelope()
    {
        const QJsonObject envelope{
            {"sourceNumber", "+49222"},
            {"timestamp", 1'790'000'001'500LL},
            {"typingMessage", QJsonObject{{"action", "STOPPED"}}}
        };
        const QJsonObject notification{
            {"method", "receive"},
            {"params", QJsonObject{{"envelope", envelope}}}
        };

        const QJsonObject normalized = SignalCli::envelopeFromNotification(notification);
        QCOMPARE(normalized, envelope);
        const ParsedEnvelope parsed = parseEnvelope(normalized);
        QCOMPARE(parsed.kind, ParsedEnvelope::Typing);
        QVERIFY(!parsed.typingStarted);
    }

    void persistsOutgoingReceiptState()
    {
        Message sent;
        sent.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'100LL);
        sent.outgoing = true;
        sent.authorKey = QStringLiteral("self");
        sent.text = QStringLiteral("hello");
        sent.receiptStatus = 2;
        sent.recipientReceipts.insert(QStringLiteral("+49111"), RecipientReceipt{2, 1'790'000'000'500LL, 1'790'000'000'800LL});
        sent.recipientReceipts.insert(QStringLiteral("+49222"), RecipientReceipt{1, 1'790'000'000'600LL, 0});

        const QJsonObject record = Records::message(QStringLiteral("g:group"), sent);
        QCOMPARE(record.value(QStringLiteral("d")).toObject().value(QStringLiteral("rs")).toInt(), 2);
        Message restored = Records::messageFromRecord(record);
        QCOMPARE(restored.receiptStatus, 2);
        QCOMPARE(restored.recipientReceipts.size(), 2);
        QCOMPARE(restored.recipientReceipts.value(QStringLiteral("+49111")).status, 2);
        QCOMPARE(restored.recipientReceipts.value(QStringLiteral("+49111")).deliveredAtMs, qint64(1'790'000'000'500LL));
        QCOMPARE(restored.recipientReceipts.value(QStringLiteral("+49111")).readAtMs, qint64(1'790'000'000'800LL));
        QCOMPARE(restored.recipientReceipts.value(QStringLiteral("+49222")).status, 1);

        restored.receiptStatus = 1;
        const QJsonObject update = Records::receiptStatusUpdate(QStringLiteral("g:group"), restored);
        QCOMPARE(update.value(QStringLiteral("t")).toString(), QStringLiteral("u"));
        Records::applyExtras(restored, update.value(QStringLiteral("d")).toObject());
        QCOMPARE(restored.receiptStatus, 1);
        QCOMPARE(restored.recipientReceipts.value(QStringLiteral("+49111")).readAtMs, qint64(1'790'000'000'800LL));
    }

    void receiptStateIsIgnoredForIncomingMessages()
    {
        Message incoming;
        incoming.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'200LL);
        incoming.outgoing = false;
        incoming.receiptStatus = 0;
        Records::applyExtras(incoming, QJsonObject{{"rs", 2}});
        QCOMPARE(incoming.receiptStatus, 0);
    }
};

QTEST_GUILESS_MAIN(ReceiptTypingTest)
#include "receipt_typing_test.moc"

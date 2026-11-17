// Headless test of Signal polls: envelope parsing (pollCreate / pollVote / pollTerminate,
// also inside syncMessage.sentMessage), history records and the ChatView poll bubble
// (rendering, vote counting, click-to-vote hit-testing).
#include "ChatView.h"
#include "Envelope.h"
#include "Records.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <cstdio>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static void pump()
{
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

static QJsonObject pollCreateEnvelope(qint64 timestamp, bool allowMultiple)
{
    return QJsonObject{
        {"sourceNumber", "+49111"},
        {"sourceName", "Alice"},
        {"timestamp", QJsonValue(timestamp)},
        {"dataMessage", QJsonObject{
            {"timestamp", QJsonValue(timestamp)},
            {"message", QJsonValue()},
            {"pollCreate", QJsonObject{
                {"question", QStringLiteral("Where to eat?")},
                {"allowMultiple", allowMultiple},
                {"options", QJsonArray{QStringLiteral("Pizza"), QStringLiteral("Sushi"), QStringLiteral("Ramen")}}
            }}
        }}
    };
}

class PollsTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesIncomingPollCreate()
    {
        const ParsedEnvelope parsed = parseEnvelope(pollCreateEnvelope(1'790'000'000'000LL, true));
        CHECK(parsed.kind == ParsedEnvelope::Message_);
        CHECK(!parsed.outgoing);
        CHECK(parsed.msg.hasPoll);
        CHECK(parsed.msg.poll.question == QStringLiteral("Where to eat?"));
        CHECK(parsed.msg.poll.allowMultiple);
        CHECK(parsed.msg.poll.options.size() == 3);
        CHECK(parsed.msg.poll.options.first() == QStringLiteral("Pizza"));
        CHECK(parsed.msg.text.isEmpty());
        CHECK(parsed.msg.attachments.isEmpty());
        // A poll message without a body must not be dropped as an empty data message.
        CHECK(parsed.kind == ParsedEnvelope::Message_);
    }

    void parsesIncomingPollVote()
    {
        const qint64 timestamp = 1'790'000'000'010LL;
        QJsonObject envelope{
            {"sourceNumber", "+49222"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"pollVote", QJsonObject{
                    {"authorNumber", "+49111"},
                    {"targetSentTimestamp", QJsonValue(1'790'000'000'000LL)},
                    {"optionIndexes", QJsonArray{0, 2}},
                    {"voteCount", 2}
                }}
            }}
        };
        const ParsedEnvelope parsed = parseEnvelope(envelope);
        CHECK(parsed.kind == ParsedEnvelope::PollVote);
        CHECK(parsed.targetNumber == QStringLiteral("+49111"));
        CHECK(parsed.targetTs == 1'790'000'000'000LL);
        CHECK((parsed.pollOptionIndexes == QList<int>{0, 2}));
        CHECK(parsed.pollVoteCount == 2);
        CHECK(parsed.msg.text.isEmpty());
    }

    void parsesIncomingPollTerminate()
    {
        const qint64 timestamp = 1'790'000'000'020LL;
        QJsonObject envelope{
            {"sourceNumber", "+49111"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"pollTerminate", QJsonObject{{"targetSentTimestamp", QJsonValue(1'790'000'000'000LL)}}}
            }}
        };
        const ParsedEnvelope parsed = parseEnvelope(envelope);
        CHECK(parsed.kind == ParsedEnvelope::PollTerminate);
        CHECK(parsed.targetTs == 1'790'000'000'000LL);
    }

    void parsesPollVoteAndPollFromSyncSent()
    {
        // The phone voted on a poll created in Corvus: the vote arrives as a sync message.
        const qint64 timestamp = 1'790'000'000'030LL;
        QJsonObject voteEnvelope{
            {"timestamp", QJsonValue(timestamp)},
            {"syncMessage", QJsonObject{
                {"sentMessage", QJsonObject{
                    {"destinationNumber", "+49111"},
                    {"timestamp", QJsonValue(timestamp)},
                    {"pollVote", QJsonObject{
                        {"authorNumber", "+49111"},
                        {"targetSentTimestamp", QJsonValue(1'790'000'000'000LL)},
                        {"optionIndexes", QJsonArray{1}},
                        {"voteCount", 1}
                    }}
                }}
            }}
        };
        const ParsedEnvelope vote = parseEnvelope(voteEnvelope);
        CHECK(vote.kind == ParsedEnvelope::PollVote);
        CHECK(vote.outgoing);
        CHECK(vote.pollOptionIndexes == QList<int>{1});

        // A poll created on the phone arrives in Corvus as a sync sent pollCreate.
        QJsonObject createEnvelope{
            {"timestamp", QJsonValue(timestamp + 1)},
            {"syncMessage", QJsonObject{
                {"sentMessage", QJsonObject{
                    {"destinationNumber", "+49111"},
                    {"timestamp", QJsonValue(timestamp + 1)},
                    {"pollCreate", QJsonObject{
                        {"question", QStringLiteral("Lunch?")},
                        {"allowMultiple", false},
                        {"options", QJsonArray{QStringLiteral("Salad"), QStringLiteral("Bowl")}}
                    }}
                }}
            }}
        };
        const ParsedEnvelope create = parseEnvelope(createEnvelope);
        CHECK(create.kind == ParsedEnvelope::Message_);
        CHECK(create.outgoing);
        CHECK(create.msg.hasPoll);
        CHECK(create.msg.poll.question == QStringLiteral("Lunch?"));
        CHECK(!create.msg.poll.allowMultiple);
    }

    void voteWithoutOptionIndexesRetracts()
    {
        const qint64 timestamp = 1'790'000'000'040LL;
        QJsonObject envelope{
            {"sourceNumber", "+49222"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"pollVote", QJsonObject{
                    {"authorNumber", "+49111"},
                    {"targetSentTimestamp", QJsonValue(1'790'000'000'000LL)},
                    {"optionIndexes", QJsonArray{}},
                    {"voteCount", 3}
                }}
            }}
        };
        const ParsedEnvelope parsed = parseEnvelope(envelope);
        CHECK(parsed.kind == ParsedEnvelope::PollVote);
        CHECK(parsed.pollOptionIndexes.isEmpty());
        CHECK(parsed.pollVoteCount == 3);
    }

    void pollRoundTripsThroughTheHistory()
    {
        const QDateTime base = QDateTime::fromMSecsSinceEpoch(1'790'000'000'000LL);
        Message poll;
        poll.time = base;
        poll.sender = QStringLiteral("Alice");
        poll.authorKey = QStringLiteral("+49111");
        poll.hasPoll = true;
        poll.poll.question = QStringLiteral("Where to eat?");
        poll.poll.options = {QStringLiteral("Pizza"), QStringLiteral("Sushi"), QStringLiteral("Ramen")};
        poll.poll.allowMultiple = true;

        // Two votes, persisted as their own records.
        PollVote first;
        first.optionIndexes = {0};
        first.voteCount = 1;
        first.voteTs = base.addSecs(2).toMSecsSinceEpoch();
        poll.poll.votes.insert(QStringLiteral("+49222"), first);
        PollVote second;
        second.optionIndexes = {1, 2};
        second.voteCount = 1;
        second.voteTs = base.addSecs(4).toMSecsSinceEpoch();
        poll.poll.votes.insert(QStringLiteral("+49333"), second);

        const QJsonObject record = Records::message(QStringLiteral("+49111"), poll);
        const QJsonObject extras = record.value(QStringLiteral("d")).toObject();
        const QJsonObject storedPoll = extras.value(QStringLiteral("pl")).toObject();
        CHECK(storedPoll.value(QStringLiteral("q")).toString() == QStringLiteral("Where to eat?"));
        CHECK(storedPoll.value(QStringLiteral("o")).toArray().size() == 3);
        CHECK(storedPoll.value(QStringLiteral("m")).toBool());

        const Message restored = Records::messageFromRecord(record);
        CHECK(restored.hasPoll);
        CHECK(restored.poll.question == QStringLiteral("Where to eat?"));
        CHECK(restored.poll.options.size() == 3);
        CHECK(restored.poll.allowMultiple);
        CHECK((restored.poll.optionCounts() == QList<int>{1, 1, 1}));
        CHECK(restored.poll.topCount() == 1);

        // The vote records are restored exactly like the live ones (they were written
        // while loading the history).
        const QJsonObject voteRecord = Records::pollVote(QStringLiteral("+49111"), poll.ts(),
                                                         QStringLiteral("+49111"), QStringLiteral("+49222"),
                                                         QList<int>{2}, 2, base.addSecs(9).toMSecsSinceEpoch());
        CHECK(voteRecord.value(QStringLiteral("t")).toString() == QStringLiteral("pv"));
        CHECK(voteRecord.value(QStringLiteral("by")).toString() == QStringLiteral("+49222"));

        // A vote retraction is stored as an empty option list.
        const QJsonObject retraction = Records::pollVote(QStringLiteral("+49111"), poll.ts(),
                                                         QStringLiteral("+49111"), QStringLiteral("+49333"),
                                                         QList<int>{}, 2, base.addSecs(12).toMSecsSinceEpoch());
        CHECK(retraction.value(QStringLiteral("oi")).toArray().isEmpty());

        // Ending the poll early is an update record merged via applyExtras.
        poll.poll.terminated = true;
        const QJsonObject terminate = Records::pollTerminate(QStringLiteral("+49111"), poll);
        CHECK(terminate.value(QStringLiteral("t")).toString() == QStringLiteral("u"));
        Message afterTerminate = restored;
        Records::applyExtras(afterTerminate, terminate.value(QStringLiteral("d")).toObject());
        CHECK(afterTerminate.hasPoll);
        CHECK(afterTerminate.poll.terminated);
        CHECK(afterTerminate.poll.question == QStringLiteral("Where to eat?"));
    }

    void deletedPollLosesItsData()
    {
        Message restored;
        restored.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'050LL);
        restored.hasPoll = true;
        restored.poll.question = QStringLiteral("Gone?");
        restored.poll.options = {QStringLiteral("A"), QStringLiteral("B")};
        PollVote vote;
        vote.optionIndexes = {0};
        vote.voteCount = 1;
        restored.poll.votes.insert(QStringLiteral("+49222"), vote);
        Records::applyExtras(restored, QJsonObject{{"dl", true}});
        CHECK(restored.deleted);
        CHECK(!restored.hasPoll);
        CHECK(restored.poll.options.isEmpty());
        CHECK(restored.poll.votes.isEmpty());
    }

    void chatViewShowsPollAndCountsVotes()
    {
        ChatView view;
        view.resize(560, 700);
        view.show();
        pump();

        QList<Message> msgs;
        Message poll;
        poll.time = QDateTime::currentDateTime().addSecs(-60);
        poll.sender = QStringLiteral("Alice");
        poll.authorKey = QStringLiteral("+49111");
        poll.hasPoll = true;
        poll.poll.question = QStringLiteral("Where to eat?");
        poll.poll.options = {QStringLiteral("Pizza"), QStringLiteral("Sushi"), QStringLiteral("Ramen")};
        poll.poll.allowMultiple = false;
        PollVote vote;
        vote.optionIndexes = {0};
        vote.voteCount = 1;
        vote.voteTs = poll.ts() + 1000;
        poll.poll.votes.insert(QStringLiteral("+49222"), vote);
        PollVote ownVote;
        ownVote.optionIndexes = {1};
        ownVote.voteCount = 1;
        ownVote.voteTs = poll.ts() + 2000;
        poll.poll.votes.insert(QStringLiteral("+49"), ownVote);
        poll.outgoing = false;
        msgs << poll;
        view.setNameResolver([](const QString &key) {
            return key == QStringLiteral("+49111") ? QStringLiteral("Alice") : key;
        });
        view.setSelfKey(QStringLiteral("+49"));
        view.showMessages(msgs);
        pump();

        // plainText describes the poll and the current tally (used by tests and search).
        const QString text = view.plainText();
        CHECK(text.contains(QStringLiteral("[Poll] Where to eat?")));
        CHECK(text.contains(QStringLiteral("  Pizza (1)")));
        CHECK(text.contains(QStringLiteral("  Sushi (1)")));
        CHECK(text.contains(QStringLiteral("  Ramen (0)")));

        // The three option rows exist and clicking each of them votes: probe the bubble's
        // middle column top-down; every probe inside an option row emits exactly one
        // pollOptionClicked with the message index and the option index.
        const QRect bubble = view.bubbleRect(0);
        QSignalSpy clicked(&view, &ChatView::pollOptionClicked);
        QSet<int> hitOptions;
        for (int dy = 10; dy < bubble.height() - 10; dy += 10) {
            const QPoint probe(bubble.left() + 30, bubble.top() + dy);
            const int before = clicked.count();
            QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, probe);
            pump();
            if (clicked.count() > before) hitOptions.insert(clicked.last().at(1).toInt());
        }
        CHECK((hitOptions == QSet<int>{0, 1, 2}));
        CHECK(clicked.last().at(1).toInt() == 2); // the lowest probe was in the last row

        // The pinned-message snippet quotes the poll question.
        CHECK(view.pinSnippet(0) == QStringLiteral("Poll: Where to eat?"));
    }
};

QTEST_MAIN(PollsTest)
#include "polls_test.moc"

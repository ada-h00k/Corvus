// What corvus-signal (signal-backend/) emits, as read by corvus' own parsers. The JSON comes from
// signal-backend/src/contract.rs (CORVUS_UPDATE_CONTRACT=1 cargo test writes it).
#include "Envelope.h"
#include "GroupInfo.h"
#include "SignalCli.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

class BackendContractTest : public QObject
{
    Q_OBJECT

    QJsonObject m_cases;

    ParsedEnvelope parsed(const char *name) const
    {
        const QJsonObject notification = m_cases.value(QLatin1String(name)).toObject();
        return parseEnvelope(SignalCli::envelopeFromNotification(notification));
    }

    QString groupId() const { return m_cases.value(QStringLiteral("groupId")).toString(); }

private slots:
    void initTestCase()
    {
        QFile f(QStringLiteral(CONTRACT_JSON));
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
        m_cases = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(!m_cases.isEmpty());
        QVERIFY(!groupId().isEmpty());
    }

    void textMessage()
    {
        const ParsedEnvelope pe = parsed("text");
        QCOMPARE(pe.kind, ParsedEnvelope::Message_);
        QVERIFY(!pe.outgoing);
        QCOMPARE(pe.src, QStringLiteral("+49111"));
        QCOMPARE(pe.srcName, QStringLiteral("Alice"));
        QVERIFY(pe.groupId.isEmpty());
        QCOMPARE(pe.msg.text, QStringLiteral("Hello world https://example.org"));
        QCOMPARE(pe.msg.time.toMSecsSinceEpoch(), qint64(1'790'000'000'000LL));
        QCOMPARE(pe.msg.expiresInSeconds, 3600);
        QCOMPARE(pe.msg.textStyles.size(), 1);
        QCOMPARE(pe.msg.textStyles[0].start, 0);
        QCOMPARE(pe.msg.textStyles[0].length, 5);
        QCOMPARE(pe.msg.textStyles[0].style, QStringLiteral("BOLD"));
        QCOMPARE(pe.msg.attachments.size(), 1);
        const Attachment &a = pe.msg.attachments[0];
        QCOMPARE(a.id, QStringLiteral("att1"));
        QCOMPARE(a.contentType, QStringLiteral("image/png"));
        QCOMPARE(a.filename, QStringLiteral("cat.png"));
        QCOMPARE(a.size, qint64(1234));
        QCOMPARE(a.width, 640);
        QCOMPARE(a.height, 480);
        QCOMPARE(pe.msg.linkPreview.url, QStringLiteral("https://example.org"));
        QCOMPARE(pe.msg.linkPreview.title, QStringLiteral("Example"));
        QCOMPARE(pe.msg.linkPreview.imageId, QStringLiteral("prev1"));
        QVERIFY(pe.msg.quote.valid);
        QCOMPARE(pe.msg.quote.id, qint64(1'789'999'999'000LL));
        QCOMPARE(pe.msg.quote.authorKey, QStringLiteral("22222222-2222-4222-8222-222222222222"));
        QCOMPARE(pe.msg.quote.text, QStringLiteral("earlier"));
    }

    void groupMessage()
    {
        const ParsedEnvelope pe = parsed("groupText");
        QCOMPARE(pe.kind, ParsedEnvelope::Message_);
        QCOMPARE(pe.groupId, groupId());
        // Without a known number the sender is keyed by UUID, as with signal-cli.
        QCOMPARE(pe.src, QStringLiteral("22222222-2222-4222-8222-222222222222"));
        QCOMPARE(pe.srcName, QStringLiteral("Bob"));
    }

    void reaction()
    {
        const ParsedEnvelope pe = parsed("reaction");
        QCOMPARE(pe.kind, ParsedEnvelope::Reaction);
        QCOMPARE(pe.emoji, QStringLiteral("👍"));
        QVERIFY(!pe.remove);
        QCOMPARE(pe.targetNumber, QStringLiteral("+49999"));
        QCOMPARE(pe.targetUuid, QStringLiteral("33333333-3333-4333-8333-333333333333"));
        QCOMPARE(pe.targetTs, qint64(1'789'999'995'000LL));
        QCOMPARE(pe.groupId, groupId());
    }

    void deleteForEveryone()
    {
        const ParsedEnvelope pe = parsed("delete");
        QCOMPARE(pe.kind, ParsedEnvelope::Delete);
        QCOMPARE(pe.targetTs, qint64(1'789'999'994'000LL));
    }

    void sticker()
    {
        const ParsedEnvelope pe = parsed("sticker");
        QCOMPARE(pe.kind, ParsedEnvelope::Message_);
        QCOMPARE(pe.msg.attachments.size(), 1);
        QVERIFY(pe.msg.attachments[0].isSticker());
        QCOMPARE(pe.msg.attachments[0].filename, QStringLiteral("signal-sticker:abcd:4.webp"));
        QCOMPARE(pe.msg.attachments[0].stickerPackId(), QStringLiteral("abcd"));
    }

    void expirationUpdate()
    {
        const ParsedEnvelope pe = parsed("expiration");
        QCOMPARE(pe.kind, ParsedEnvelope::ExpirationUpdate);
        QCOMPARE(pe.expirationSeconds, 86400);
    }

    void voiceNoteAndViewOnce()
    {
        const ParsedEnvelope voice = parsed("voiceNote");
        QCOMPARE(voice.kind, ParsedEnvelope::Message_);
        QCOMPARE(voice.msg.attachments.size(), 1);
        QCOMPARE(voice.msg.attachments[0].id, QStringLiteral("987654321"));
        QCOMPARE(voice.msg.attachments[0].contentType, QStringLiteral("audio/aac"));

        const ParsedEnvelope once = parsed("viewOnce");
        QCOMPARE(once.kind, ParsedEnvelope::Message_);
        QVERIFY(once.msg.viewOnce);
    }

    void edit()
    {
        const ParsedEnvelope pe = parsed("edit");
        QCOMPARE(pe.kind, ParsedEnvelope::Edit);
        QCOMPARE(pe.targetTs, qint64(1'789'999'993'000LL));
        QCOMPARE(pe.msg.text, QStringLiteral("fixed typo"));
        QCOMPARE(pe.msg.textStyles.size(), 1);
    }

    void sentFromOurPhone()
    {
        const ParsedEnvelope pe = parsed("syncSent");
        QVERIFY(pe.outgoing);
        QCOMPARE(pe.kind, ParsedEnvelope::Message_);
        QCOMPARE(pe.destination, QStringLiteral("+49111"));
        QCOMPARE(pe.msg.text, QStringLiteral("from my phone"));

        const ParsedEnvelope edit = parsed("syncSentEdit");
        QVERIFY(edit.outgoing);
        QCOMPARE(edit.kind, ParsedEnvelope::Edit);
        QCOMPARE(edit.destination, QStringLiteral("+49111"));
        QCOMPARE(edit.targetTs, qint64(1'789'999'992'000LL));
        QCOMPARE(edit.msg.text, QStringLiteral("edited on phone"));
    }

    void receipts()
    {
        const ParsedEnvelope read = parsed("readReceipt");
        QCOMPARE(read.kind, ParsedEnvelope::Receipt);
        QVERIFY(read.readReceipt);
        QVERIFY(!read.deliveryReceipt);
        QCOMPARE(read.receiptTimestamps, QList<qint64>({1'789'999'999'900LL, 1'789'999'999'800LL}));

        const ParsedEnvelope delivered = parsed("deliveryReceipt");
        QCOMPARE(delivered.kind, ParsedEnvelope::Receipt);
        QVERIFY(delivered.deliveryReceipt);
        QVERIFY(!delivered.readReceipt);
    }

    void groupTyping()
    {
        const ParsedEnvelope pe = parsed("groupTyping");
        QCOMPARE(pe.kind, ParsedEnvelope::Typing);
        QVERIFY(pe.typingStarted);
        QCOMPARE(pe.groupId, groupId());
    }

    void listGroups()
    {
        const QList<GroupInfo::Info> groups = GroupInfo::parse(m_cases.value(QStringLiteral("listGroups")));
        QCOMPARE(groups.size(), 1);
        const GroupInfo::Info &g = groups[0];
        QCOMPARE(g.id, groupId());
        QCOMPARE(g.name, QStringLiteral("Team"));
        QCOMPARE(g.description, QStringLiteral("Our team"));
        QVERIFY(g.isMember);
        QCOMPARE(g.expiration, 604800);
        QCOMPARE(g.members.size(), 2);
        QVERIFY(g.isAdmin(QStringLiteral("+49999")));
        QVERIFY(!g.isAdmin(QStringLiteral("+49111")));
        QCOMPARE(g.adminCount(), 1);
        QCOMPARE(g.requesting.size(), 1);
        QCOMPARE(g.requesting[0].uuid, QStringLiteral("22222222-2222-4222-8222-222222222222"));
        QCOMPARE(g.permAddMember, QStringLiteral("EVERY_MEMBER"));
        QCOMPARE(g.permEditDetails, QStringLiteral("ONLY_ADMINS"));
        QCOMPARE(g.permSendMessage, QStringLiteral("ONLY_ADMINS"));
        QVERIFY(g.inviteLink.startsWith(QStringLiteral("https://signal.group/#")));
        QVERIFY(g.canAddMembers(QStringLiteral("+49111")));
        QVERIFY(!g.canEditDetails(QStringLiteral("+49111")));
    }
};

QTEST_MAIN(BackendContractTest)
#include "backend_contract_test.moc"

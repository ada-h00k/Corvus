// Safety numbers: parsing / formatting of signal-cli's "listIdentities" and the dialog on top of it.
#include "SafetyNumber.h"
#include "SafetyNumberDialog.h"
#include "SignalCli.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

namespace {
const QString kNumber = QStringLiteral("12345" "67890" "11111" "22222" "33333" "44444"
                                       "55555" "66666" "77777" "88888" "99999" "00000");

QJsonObject identity(const QString &number, const QString &safety, const QString &trust, qint64 added = 0)
{
    return {{"number", number}, {"uuid", "u-" + number}, {"safetyNumber", safety},
            {"trustLevel", trust}, {"addedTimestamp", static_cast<double>(added)}};
}

QPushButton *button(QWidget *w, const QString &text)
{
    for (auto *b : w->findChildren<QPushButton *>())
        if (b->text() == text) return b;
    return nullptr;
}
} // namespace

class SafetyNumberTest : public QObject
{
    Q_OBJECT
private slots:
    void formatsInThreeLinesOfFourGroups()
    {
        const QString f = SafetyNumber::format(kNumber);
        QCOMPARE(f.count(QLatin1Char('\n')), 2);
        QCOMPARE(f.section(QLatin1Char('\n'), 0, 0), QStringLiteral("12345 67890 11111 22222"));
        QCOMPARE(f.section(QLatin1Char('\n'), 2, 2), QStringLiteral("77777 88888 99999 00000"));
        QCOMPARE(SafetyNumber::digitsOnly(f), kNumber);
        QCOMPARE(SafetyNumber::format(QStringLiteral("123")), QStringLiteral("123")); // not 60 digits: unchanged
    }

    void matchingIgnoresSeparators()
    {
        QVERIFY(SafetyNumber::matches(kNumber, SafetyNumber::format(kNumber)));
        QVERIFY(SafetyNumber::matches(kNumber, QStringLiteral("  ") + kNumber.left(30) + QStringLiteral("-\n") + kNumber.mid(30)));
        QVERIFY(!SafetyNumber::matches(kNumber, kNumber.left(59) + QLatin1Char('1')));
        QVERIFY(!SafetyNumber::matches(kNumber, kNumber.left(30)));
        QVERIFY(!SafetyNumber::matches(QString(), QString())); // empty is never a match
    }

    void decodesScannableBytes()
    {
        QCOMPARE(SafetyNumber::scannableBytes(QStringLiteral("CAEKAA3/")), QByteArray::fromHex("08010a000dff"));
        QVERIFY(SafetyNumber::scannableBytes(QString()).isEmpty());
        QVERIFY(SafetyNumber::scannableBytes(QStringLiteral("not base64 !")).isEmpty());
    }

    void detectsChanges()
    {
        using SafetyNumber::Change;
        auto id = [](const QString &safety, const QString &trust) {
            return SafetyNumber::parse(QJsonArray{identity("+49111", safety, trust)}).first();
        };
        const QString other = kNumber.left(59) + QLatin1Char('1');
        // nothing remembered yet: first sight is not a change, an untrusted key is
        QCOMPARE(SafetyNumber::evaluate(QString(), id(kNumber, "TRUSTED_UNVERIFIED")), Change::None);
        QCOMPARE(SafetyNumber::evaluate(QString(), id(kNumber, "UNTRUSTED")), Change::Changed);
        // remembered: same number is fine (also when grouped differently), another one is a change
        QCOMPARE(SafetyNumber::evaluate(kNumber, id(kNumber, "TRUSTED_VERIFIED")), Change::None);
        QCOMPARE(SafetyNumber::evaluate(SafetyNumber::format(kNumber), id(kNumber, "TRUSTED_UNVERIFIED")), Change::None);
        QCOMPARE(SafetyNumber::evaluate(kNumber, id(other, "TRUSTED_UNVERIFIED")), Change::Changed);
        QCOMPARE(SafetyNumber::evaluate(kNumber, id(other, "UNTRUSTED")), Change::Changed);
        // no usable safety number: nothing to compare
        QCOMPARE(SafetyNumber::evaluate(kNumber, id(QString(), "UNTRUSTED")), Change::None);
    }

    void recognisesUntrustedSendErrors()
    {
        QVERIFY(SafetyNumber::isUntrustedIdentityError(QStringLiteral("Failed to send message: Untrusted Identity for \"+49111\"")));
        QVERIFY(SafetyNumber::isUntrustedIdentityError(QStringLiteral("untrusted identity")));
        QVERIFY(!SafetyNumber::isUntrustedIdentityError(QStringLiteral("Network failure")));
        QVERIFY(!SafetyNumber::isUntrustedIdentityError(QString()));
    }

    void parsesIdentities()
    {
        const auto list = SafetyNumber::parse(QJsonArray{identity("+49111", kNumber, "TRUSTED_VERIFIED", 5), 42, QJsonObject()});
        QCOMPARE(list.size(), 1);
        QVERIFY(list.first().isVerified());
        QVERIFY(list.first().hasSafetyNumber());
        QCOMPARE(list.first().addedMs, 5);
        QVERIFY(SafetyNumber::parse(QJsonValue()).isEmpty());
        QVERIFY(SafetyNumber::parse(QJsonArray()).isEmpty());
    }

    void picksTheRightIdentity()
    {
        const auto list = SafetyNumber::parse(QJsonArray{
            identity("+49222", kNumber, "UNTRUSTED", 1),
            identity("+49111", QString(), "TRUSTED_UNVERIFIED", 9),   // no safety number
            identity("+49111", kNumber, "TRUSTED_UNVERIFIED", 2),
            identity("+49111", kNumber, "TRUSTED_VERIFIED", 3)});     // newest entry of that recipient
        QCOMPARE(SafetyNumber::pick(list, "+49111"), 3);
        QCOMPARE(SafetyNumber::pick(list, "u-+49222"), 0);
        QCOMPARE(SafetyNumber::pick(list, "+49999"), 3); // unknown recipient: newest usable entry
        QCOMPARE(SafetyNumber::pick({}, "+49111"), -1);
    }

    void dialogShowsAndVerifies()
    {
        SignalCli cli;
        cli.setExecutable(QStringLiteral(MOCK_CLI));
        QSignalSpy started(&cli, &SignalCli::started);
        cli.start(QStringLiteral("+49000"));
        QTRY_VERIFY_WITH_TIMEOUT(started.count() == 1, 10000);

        SafetyNumberDialog dialog(cli, QStringLiteral("+49111"), QStringLiteral("Alice"));
        dialog.show();

        auto shown = [&dialog](const QString &text) {
            for (auto *l : dialog.findChildren<QLabel *>())
                if (l->text().contains(text)) return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(shown(QStringLiteral("12345 67890 11111 22222")), 10000);
        QVERIFY(shown(QStringLiteral("Not verified")));

        // QR code (only if qrencode is installed on the machine running the test)
        if (!QStandardPaths::findExecutable(QStringLiteral("qrencode")).isEmpty()) {
            bool qr = false;
            for (auto *l : dialog.findChildren<QLabel *>())
                if (!l->pixmap(Qt::ReturnByValue).isNull() && l->isVisible()) qr = true;
            QVERIFY2(qr, "QR code not shown");
        }

        QPushButton *verify = button(&dialog, QStringLiteral("Mark as verified"));
        QVERIFY(verify);
        QTRY_VERIFY(verify->isEnabled());

        // a wrong number blocks verification
        auto *edit = dialog.findChild<QLineEdit *>();
        QVERIFY(edit);
        edit->setText(kNumber.left(59) + QLatin1Char('1'));
        QVERIFY(shown(QStringLiteral("do NOT match")));
        QVERIFY(!verify->isEnabled());

        // the right number (as the contact displays it) allows it
        edit->setText(SafetyNumber::format(kNumber));
        QVERIFY(shown(QStringLiteral("numbers match")));
        QVERIFY(verify->isEnabled());
        verify->click();

        QTRY_VERIFY_WITH_TIMEOUT(shown(QStringLiteral("Status: Verified")), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(verify->text(), QStringLiteral("Verified ✓"), 10000);
        QVERIFY(!verify->isEnabled());
    }

    void dialogReportsUnknownContact()
    {
        SignalCli cli;
        cli.setExecutable(QStringLiteral(MOCK_CLI));
        QSignalSpy started(&cli, &SignalCli::started);
        cli.start(QStringLiteral("+49000"));
        QTRY_VERIFY_WITH_TIMEOUT(started.count() == 1, 10000);

        SafetyNumberDialog dialog(cli, QStringLiteral("+49777"), QStringLiteral("Nobody"));
        dialog.show();
        auto shown = [&dialog](const QString &text) {
            for (auto *l : dialog.findChildren<QLabel *>())
                if (l->text().contains(text)) return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(shown(QStringLiteral("Unknown recipient")), 10000);
        QVERIFY(!button(&dialog, QStringLiteral("Mark as verified"))->isEnabled());
        QVERIFY(!button(&dialog, QStringLiteral("Copy number"))->isEnabled());
    }
};

QTEST_MAIN(SafetyNumberTest)
#include "safety_number_test.moc"

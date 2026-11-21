// Tests for the attachment open-path safety: filename normalization and the
// open allowlist (regression tests for the "executable disguised as an image"
// finding — a claimed image content type must never enable opening a file the
// system handler could execute).
#include "Attachments.h"

#include <QDateTime>
#include <QTest>

namespace {

Attachment attachment(const QString &type, const QString &name)
{
    Attachment a;
    a.id = name;
    a.contentType = type;
    a.filename = name;
    a.size = 1000;
    return a;
}

} // namespace

class AttachmentSafetyTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizationStripsTrailingDotsAndSpaces();
    void normalizationDropsNtfsStream();
    void normalizationStripsBidiControls();
    void allowlistBlocksExecutableDisguisedAsImage();
    void allowlistPassesAppRenderedTypes();
    void allowlistPassesPlainDocuments();
};

void AttachmentSafetyTest::normalizationStripsTrailingDotsAndSpaces()
{
    // Windows strips trailing dots/spaces when saving, so the suffix check must see
    // the real name, not the empty suffix of "evil.exe.".
    QCOMPARE(AttachmentUtil::normalizedFileName(QStringLiteral("evil.exe.")), QStringLiteral("evil.exe"));
    QCOMPARE(AttachmentUtil::normalizedFileName(QStringLiteral("evil.exe ")), QStringLiteral("evil.exe"));
    QCOMPARE(AttachmentUtil::normalizedFileName(QStringLiteral("photo.png. .")), QStringLiteral("photo.png"));
    QCOMPARE(AttachmentUtil::normalizedSuffix(QStringLiteral("evil.exe.")), QStringLiteral("exe"));
    QCOMPARE(AttachmentUtil::normalizedSuffix(QStringLiteral("invoice.hta ")), QStringLiteral("hta"));
}

void AttachmentSafetyTest::normalizationDropsNtfsStream()
{
    // "name.exe:stream" would carry an empty suffix; the stream part is not the name.
    QCOMPARE(AttachmentUtil::normalizedFileName(QStringLiteral("invoice.exe:hidden")),
             QStringLiteral("invoice.exe"));
    QCOMPARE(AttachmentUtil::normalizedSuffix(QStringLiteral("invoice.exe:hidden")), QStringLiteral("exe"));
}

void AttachmentSafetyTest::normalizationStripsBidiControls()
{
    // U+202E (RIGHT-TO-LEFT OVERRIDE) before "gpj" makes "invoicegpj.exe" display
    // as "invoiceexe.jpg" — an executable that reads like a JPEG.
    const QString bidi = QStringLiteral("invoice") + QChar(0x202E) + QStringLiteral("gpj.exe");
    QCOMPARE(AttachmentUtil::normalizedFileName(bidi), QStringLiteral("invoicegpj.exe"));
    QCOMPARE(AttachmentUtil::displayFileName(attachment(QStringLiteral("application/octet-stream"), bidi),
                                            QDateTime()),
             QStringLiteral("invoicegpj.exe"));
    // The suffix check must see the real suffix, not the bidi-reordered one.
    QCOMPARE(AttachmentUtil::normalizedSuffix(bidi), QStringLiteral("exe"));
}

void AttachmentSafetyTest::allowlistBlocksExecutableDisguisedAsImage()
{
    // The attack from the report: an executable file name with a claimed image type.
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("image/png"), QStringLiteral("invoice.hta"))));
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("image/png"), QStringLiteral("invoice.exe"))));
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("image/png"), QStringLiteral("x.jar"))));
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("image/png"), QStringLiteral("x.desktop"))));
    // Trailing dot and NTFS stream variants must not slip past either.
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("image/png"), QStringLiteral("evil.exe."))));
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("image/png"), QStringLiteral("evil.exe: hid"))));
    // Known dangerous types stay blocked without any claimed image type as well.
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("application/octet-stream"), QStringLiteral("x.exe"))));
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("text/html"), QStringLiteral("page.html"))));
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("image/svg+xml"), QStringLiteral("art.svg"))));
}

void AttachmentSafetyTest::allowlistPassesAppRenderedTypes()
{
    // Images, audio and video are decoded/played inside Corvus and never reach the
    // system "open" handler, so the claimed type may pass the gate.
    QVERIFY(AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("image/png"), QStringLiteral("a.png"))));
    QVERIFY(AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("video/mp4"), QStringLiteral("clip.mp4"))));
    QVERIFY(AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("audio/aac"), QStringLiteral("voice.aac"))));
}

void AttachmentSafetyTest::allowlistPassesPlainDocuments()
{
    QVERIFY(AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("application/pdf"), QStringLiteral("report.pdf"))));
    QVERIFY(AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("text/plain"), QStringLiteral("notes.txt"))));
    // Documents of other types are save-only.
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("application/zip"), QStringLiteral("a.zip"))));
    QVERIFY(!AttachmentUtil::isSafeToOpen(attachment(QStringLiteral("application/msword"), QStringLiteral("a.doc"))));
}

QTEST_MAIN(AttachmentSafetyTest)
#include "attachment_safety_test.moc"

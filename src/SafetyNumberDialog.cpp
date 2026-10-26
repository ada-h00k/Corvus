#include "SafetyNumberDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString findQrencode()
{
    // Same places as the link dialog: next to the application (Windows setup), PATH, Homebrew.
    QString exe = QStandardPaths::findExecutable(QStringLiteral("qrencode"), {QCoreApplication::applicationDirPath()});
    if (exe.isEmpty()) exe = QStandardPaths::findExecutable(QStringLiteral("qrencode"));
    if (exe.isEmpty())
        exe = QStandardPaths::findExecutable(QStringLiteral("qrencode"),
                                             {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
    return exe;
}

// QR code with the raw bytes of `data` (binary, like the official apps). Files are used for input
// and output: qrencode.exe on Windows does not put stdin/stdout into binary mode.
QPixmap renderQr(const QByteArray &data, QString *error)
{
    const QString qrencode = findQrencode();
    if (qrencode.isEmpty()) {
        *error = SafetyNumberDialog::tr("To display the QR code, please install “qrencode”\n"
                                        "(e.g. apt/brew install qrencode). You can still compare the number.");
        return {};
    }
    QTemporaryFile in(QDir::temp().filePath(QStringLiteral("corvus-sn-XXXXXX.bin")));
    QTemporaryFile out(QDir::temp().filePath(QStringLiteral("corvus-sn-XXXXXX.png")));
    if (!in.open() || !out.open()) {
        *error = SafetyNumberDialog::tr("Could not create a temporary file for the QR code.");
        return {};
    }
    in.write(data);
    in.close();
    out.close();

    QProcess q;
    q.start(qrencode, {QStringLiteral("-8"), QStringLiteral("-t"), QStringLiteral("PNG"), QStringLiteral("-s"),
                       QStringLiteral("5"), QStringLiteral("-m"), QStringLiteral("2"), QStringLiteral("-r"),
                       in.fileName(), QStringLiteral("-o"), out.fileName()});
    QPixmap pm;
    if (!q.waitForFinished(5000) || q.exitCode() != 0 || !pm.load(out.fileName(), "PNG")) {
        q.kill();
        *error = SafetyNumberDialog::tr("The QR code could not be created.");
        return {};
    }
    return pm;
}

} // namespace

SafetyNumberDialog::SafetyNumberDialog(SignalCli &cli, const QString &recipient, const QString &displayName,
                                       QWidget *parent)
    : QDialog(parent), m_cli(cli), m_recipient(recipient), m_name(displayName)
{
    setWindowTitle(tr("Safety number"));
    setMinimumWidth(460);
    auto *lay = new QVBoxLayout(this);

    auto *title = new QLabel(tr("Safety number with %1").arg(m_name.toHtmlEscaped()));
    title->setTextFormat(Qt::RichText);
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.15);
    title->setFont(tf);
    lay->addWidget(title);

    auto *intro = new QLabel(tr("To check that your messages with %1 are end-to-end encrypted, compare this number "
                                "with the one shown on their device (conversation → View safety number). Do this in "
                                "person or over a channel you trust. If the numbers match, mark the contact as verified.")
                                 .arg(m_name.toHtmlEscaped()));
    intro->setTextFormat(Qt::RichText);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    m_qr = new QLabel;
    m_qr->setAlignment(Qt::AlignCenter);
    m_qr->setWordWrap(true);
    m_qr->setVisible(false);
    lay->addWidget(m_qr);

    m_number = new QLabel;
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() * 1.3);
    m_number->setFont(mono);
    m_number->setAlignment(Qt::AlignCenter);
    m_number->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_number->setContentsMargins(0, 8, 0, 8);
    lay->addWidget(m_number);

    m_trust = new QLabel;
    m_trust->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_trust);

    lay->addSpacing(6);
    lay->addWidget(new QLabel(tr("Optional: paste the number from their device to compare it here:")));
    m_compare = new QLineEdit;
    m_compare->setPlaceholderText(tr("60 digits"));
    m_compare->setClearButtonEnabled(true);
    lay->addWidget(m_compare);
    m_compareResult = new QLabel;
    lay->addWidget(m_compareResult);

    m_message = new QLabel;
    m_message->setWordWrap(true);
    lay->addWidget(m_message);

    auto *row = new QHBoxLayout;
    m_copy = new QPushButton(tr("Copy number"));
    m_verify = new QPushButton(tr("Mark as verified"));
    m_verify->setDefault(false);
    row->addWidget(m_copy);
    row->addStretch(1);
    row->addWidget(m_verify);
    lay->addLayout(row);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    lay->addWidget(box);

    connect(m_copy, &QPushButton::clicked, this, [this] {
        if (m_hasIdentity) QApplication::clipboard()->setText(m_identity.digits());
    });
    connect(m_verify, &QPushButton::clicked, this, &SafetyNumberDialog::verify);
    connect(m_compare, &QLineEdit::textChanged, this, [this] { updateCompare(); });

    showIdentity();
    reload();
}

void SafetyNumberDialog::reload()
{
    m_busy = true;
    setStatus(tr("Loading …"), false);
    showIdentity();

    QPointer<SafetyNumberDialog> self(this);
    m_cli.call(QStringLiteral("listIdentities"), {{QStringLiteral("number"), m_recipient}},
               [self](const QJsonValue &result, const QString &error) {
        if (!self) return;
        self->m_busy = false;
        self->m_hasIdentity = false;
        if (!error.isEmpty()) {
            self->setStatus(tr("Could not load the safety number: %1").arg(error), true);
            self->showIdentity();
            return;
        }
        const auto identities = SafetyNumber::parse(result);
        const int index = SafetyNumber::pick(identities, self->m_recipient);
        if (index < 0) {
            self->setStatus(tr("No safety number is available for this contact yet. It appears after "
                               "you have exchanged at least one message."), false);
        } else {
            self->m_identity = identities.at(index);
            self->m_hasIdentity = true;
            self->setStatus(QString(), false);
        }
        self->showIdentity();
    });
}

void SafetyNumberDialog::showIdentity()
{
    if (!m_hasIdentity) {
        m_number->setText(m_busy ? QStringLiteral("…") : QStringLiteral("—"));
        m_trust->clear();
    } else {
        m_number->setText(SafetyNumber::format(m_identity.safetyNumber));
        m_trust->setText(tr("Status: %1").arg(SafetyNumber::trustLabel(m_identity.trustLevel)));
    }
    showQrCode();
    m_copy->setEnabled(m_hasIdentity);
    m_compare->setEnabled(m_hasIdentity);
    m_verify->setText(m_hasIdentity && m_identity.isVerified() ? tr("Verified ✓") : tr("Mark as verified"));
    updateCompare();
}

void SafetyNumberDialog::showQrCode()
{
    m_qr->clear();
    m_qr->setVisible(false);
    if (!m_hasIdentity) return;
    const QByteArray data = SafetyNumber::scannableBytes(m_identity.scannable);
    if (data.isEmpty()) return; // signal-cli too old to report it: the number alone is shown

    QString error;
    const QPixmap qr = renderQr(data, &error);
    if (!qr.isNull()) {
        m_qr->setPixmap(qr);
        m_qr->setToolTip(tr("Scan this code with Signal on %1's phone (conversation → View safety number → Scan).")
                             .arg(m_name));
    } else {
        m_qr->setText(error);
    }
    m_qr->setVisible(true);
    adjustSize();
}

void SafetyNumberDialog::updateCompare()
{
    const QString typed = SafetyNumber::digitsOnly(m_compare->text());
    bool canVerify = m_hasIdentity && !m_busy && !m_identity.isVerified();
    if (!m_hasIdentity || typed.isEmpty()) {
        m_compareResult->clear();
    } else if (SafetyNumber::matches(m_identity.safetyNumber, typed)) {
        m_compareResult->setText(tr("✓ The numbers match."));
        m_compareResult->setStyleSheet(QStringLiteral("color: #2e8b57;"));
    } else if (typed.size() < SafetyNumber::kDigits) {
        m_compareResult->setText(tr("%1 of %2 digits entered.").arg(typed.size()).arg(SafetyNumber::kDigits));
        m_compareResult->setStyleSheet(QString());
        canVerify = false; // incomplete comparison: finish it or clear the field
    } else {
        m_compareResult->setText(tr("✗ The numbers do NOT match. Do not verify this contact."));
        m_compareResult->setStyleSheet(QStringLiteral("color: #c0392b; font-weight: bold;"));
        canVerify = false;
    }
    m_verify->setEnabled(canVerify);
}

void SafetyNumberDialog::verify()
{
    if (!m_hasIdentity || m_busy || m_identity.isVerified()) return;

    // Without a pasted number the user compared by eye: ask once.
    if (SafetyNumber::digitsOnly(m_compare->text()).isEmpty()) {
        const auto answer = QMessageBox::question(
            this, tr("Mark as verified"),
            tr("Did you compare this safety number with the one on %1's device, and do both match?").arg(m_name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    m_busy = true;
    m_verify->setEnabled(false);
    setStatus(tr("Saving …"), false);

    const QString verifiedDigits = m_identity.digits();
    QPointer<SafetyNumberDialog> self(this);
    m_cli.call(QStringLiteral("trust"),
               {{QStringLiteral("recipient"), m_recipient},
                {QStringLiteral("verifiedSafetyNumber"), m_identity.digits()}},
               [self, verifiedDigits](const QJsonValue &, const QString &error) {
        if (!self) return;
        self->m_busy = false;
        if (!error.isEmpty()) {
            self->setStatus(tr("Could not mark the contact as verified: %1").arg(error), true);
            self->updateCompare();
            return;
        }
        emit self->verified(self->m_recipient, verifiedDigits);
        if (self) self->reload();
    });
}

void SafetyNumberDialog::setStatus(const QString &text, bool error)
{
    m_message->setText(text);
    m_message->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

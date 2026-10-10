#include "PassphraseDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

static const int kMinLength = 8;

PassphraseDialog::PassphraseDialog(Mode mode, QWidget *parent) : QDialog(parent), m_mode(mode)
{
    auto *lay = new QVBoxLayout(this);
    setMinimumWidth(420);

    auto field = [this, lay](const QString &label) {
        lay->addWidget(new QLabel(label));
        auto *e = new QLineEdit;
        e->setEchoMode(QLineEdit::Password);
        lay->addWidget(e);
        connect(e, &QLineEdit::textChanged, this, [this] { validate(); });
        return e;
    };

    QString intro;
    switch (mode) {
    case Create:
        setWindowTitle(tr("Protect chat history"));
        intro = tr("Your chat history is stored encrypted on this computer.\n"
                   "Choose a passphrase (at least %1 characters). It cannot be recovered – "
                   "if you forget it, the stored history is lost.").arg(kMinLength);
        break;
    case Unlock:
        setWindowTitle(tr("Unlock chat history"));
        intro = tr("Enter your passphrase to unlock the encrypted chat history.");
        break;
    case Change:
        setWindowTitle(tr("Change passphrase"));
        intro = tr("Choose a new passphrase for the chat history (at least %1 characters).").arg(kMinLength);
        break;
    }
    auto *introLabel = new QLabel(intro);
    introLabel->setWordWrap(true);
    lay->addWidget(introLabel);

    if (mode == Change) m_old = field(tr("Current passphrase:"));
    m_pass = field(mode == Unlock ? tr("Passphrase:") : (mode == Change ? tr("New passphrase:") : tr("Passphrase:")));
    if (mode != Unlock) m_confirm = field(tr("Repeat passphrase:"));

    m_error = new QLabel;
    m_error->setStyleSheet(QStringLiteral("color: #c0392b;"));
    m_error->setWordWrap(true);
    lay->addWidget(m_error);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_ok = box->button(QDialogButtonBox::Ok);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
    validate();
}

QString PassphraseDialog::passphrase() const { return m_pass->text(); }
QString PassphraseDialog::oldPassphrase() const { return m_old ? m_old->text() : QString(); }
void PassphraseDialog::setError(const QString &text) { m_error->setText(text); }

void PassphraseDialog::validate()
{
    bool ok = !m_pass->text().isEmpty();
    if (m_old && m_old->text().isEmpty()) ok = false;
    if (m_mode != Unlock) {
        if (m_pass->text().size() < kMinLength) ok = false;
        if (m_confirm && m_confirm->text() != m_pass->text()) {
            ok = false;
            m_error->setText(m_confirm->text().isEmpty() ? QString() : tr("The passphrases do not match."));
        } else if (m_error->text() == tr("The passphrases do not match.")) {
            m_error->clear();
        }
    }
    m_ok->setEnabled(ok);
}

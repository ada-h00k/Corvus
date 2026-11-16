#include "PollDialog.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kMaxOptions = 10;
constexpr int kMaxQuestionLength = 200;
constexpr int kMaxOptionLength = 100;
}

PollDialog::PollDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New poll"));
    setMinimumWidth(420);

    auto *lay = new QVBoxLayout(this);

    lay->addWidget(new QLabel(tr("Question")));
    auto *questionEdit = new QLineEdit;
    questionEdit->setPlaceholderText(tr("Ask a question"));
    questionEdit->setMaxLength(kMaxQuestionLength);
    connect(questionEdit, &QLineEdit::textChanged, this, [this] { updateRows(); });
    lay->addWidget(questionEdit);

    lay->addSpacing(8);
    lay->addWidget(new QLabel(tr("Options")));
    auto *optionsHost = new QWidget;
    m_optionsLayout = new QGridLayout(optionsHost);
    m_optionsLayout->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(optionsHost);

    m_addOption = new QPushButton(tr("Add option"));
    lay->addWidget(m_addOption);

    m_multiple = new QCheckBox(tr("Allow multiple votes"));
    m_multiple->setChecked(true);
    lay->addWidget(m_multiple);

    m_error = new QLabel;
    m_error->setStyleSheet(QStringLiteral("color: #c0392b;"));
    m_error->hide();
    lay->addWidget(m_error);

    auto *buttons = new QHBoxLayout;
    auto *cancel = new QPushButton(tr("Cancel"));
    m_send = new QPushButton(tr("Send"));
    m_send->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(m_send);
    lay->addLayout(buttons);

    addOptionRow();
    addOptionRow();
    updateRows();

    connect(m_addOption, &QPushButton::clicked, this, [this] { addOptionRow(); updateRows(); });
    connect(m_multiple, &QCheckBox::toggled, this, [this] { updateRows(); });
    connect(cancel, &QPushButton::clicked, this, &PollDialog::reject);
    connect(m_send, &QPushButton::clicked, this, &PollDialog::send);

    questionEdit->setFocus();
    m_questionEdit = questionEdit;
}

QString PollDialog::question() const
{
    return m_questionEdit ? m_questionEdit->text().trimmed() : QString();
}

QStringList PollDialog::options() const
{
    QStringList out;
    for (const QLineEdit *edit : m_optionEdits) {
        const QString text = edit->text().trimmed();
        if (!text.isEmpty()) out << text;
    }
    return out;
}

bool PollDialog::allowMultiple() const
{
    return m_multiple && m_multiple->isChecked();
}

void PollDialog::addOptionRow(const QString &text)
{
    if (m_optionEdits.size() >= kMaxOptions) return;
    auto *edit = new QLineEdit;
    edit->setPlaceholderText(tr("Option %1").arg(m_optionEdits.size() + 1));
    edit->setMaxLength(kMaxOptionLength);
    if (!text.isEmpty()) edit->setText(text);
    auto *remove = new QToolButton;
    remove->setText(QStringLiteral("✕"));
    remove->setToolTip(tr("Remove option"));
    remove->setAutoRaise(true);
    const int row = m_optionsLayout->rowCount();
    m_optionsLayout->addWidget(edit, row, 0);
    m_optionsLayout->addWidget(remove, row, 1);
    connect(edit, &QLineEdit::textChanged, this, [this] { updateRows(); });
    connect(remove, &QToolButton::clicked, this, [this, edit] {
        const int index = m_optionEdits.indexOf(edit);
        if (index < 0 || m_optionEdits.size() <= 2) return;
        delete m_optionEdits.takeAt(index);
        delete m_optionRemoves.takeAt(index);
        // rebuild the grid so no hole is left behind
        for (int i = 0; i < m_optionEdits.size(); ++i) {
            m_optionsLayout->addWidget(m_optionEdits.at(i), i, 0);
            m_optionsLayout->addWidget(m_optionRemoves.at(i), i, 1);
        }
        updateRows();
    });
    m_optionEdits << edit;
    m_optionRemoves << remove;
    edit->setFocus();
}

void PollDialog::updateRows()
{
    for (int i = 0; i < m_optionEdits.size(); ++i) {
        m_optionEdits.at(i)->setPlaceholderText(tr("Option %1").arg(i + 1));
        m_optionRemoves.at(i)->setVisible(m_optionEdits.size() > 2);
        m_optionRemoves.at(i)->setEnabled(m_optionEdits.size() > 2);
    }
    m_addOption->setEnabled(m_optionEdits.size() < kMaxOptions);
    const bool sendable = !question().isEmpty() && options().size() >= 2;
    m_send->setEnabled(sendable);
    if (sendable) m_error->hide();
}

void PollDialog::send()
{
    const QStringList chosen = options();
    if (question().isEmpty()) {
        m_error->setText(tr("Poll question is required"));
        m_error->show();
        return;
    }
    if (chosen.size() < 2) {
        m_error->setText(tr("Poll requires at least 2 options"));
        m_error->show();
        return;
    }
    accept();
}

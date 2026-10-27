#include "MessageRequestsDialog.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

MessageRequestsDialog::MessageRequestsDialog(SignalCli &cli, const RequestList &requests, QWidget *parent)
    : QDialog(parent)
    , m_cli(cli)
{
    setWindowTitle(tr("Message requests"));
    setMinimumSize(430, 260);

    auto *l = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("These people sent you a message but are not accepted yet. Accept to chat with "
                               "them, decline to hide the request, or block them."));
    hint->setWordWrap(true);
    l->addWidget(hint);

    m_list = new QListWidget(this);
    l->addWidget(m_list, 1);

    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color: #b00020;"));
    m_status->hide();
    l->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *close = new QPushButton(tr("Close"), this);
    close->setObjectName(QStringLiteral("requestsClose"));
    buttons->addWidget(close);
    l->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);

    for (const auto &request : requests) {
        const QString &key = request.first;
        const QString &title = request.second;

        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, key);
        item->setSizeHint(QSize(0, 36));

        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 2, 4, 2);
        auto *name = new QLabel(title, row);
        name->setToolTip(key);
        rowLayout->addWidget(name, 1);

        auto *acceptBtn = new QPushButton(tr("Accept"), row);
        acceptBtn->setObjectName(QStringLiteral("requestAccept"));
        acceptBtn->setToolTip(tr("Accept the message request and show the conversation"));
        rowLayout->addWidget(acceptBtn);
        auto *declineBtn = new QPushButton(tr("Decline"), row);
        declineBtn->setObjectName(QStringLiteral("requestDecline"));
        declineBtn->setToolTip(tr("Keep the conversation hidden and remove the request"));
        rowLayout->addWidget(declineBtn);
        auto *blockBtn = new QPushButton(tr("Block …"), row);
        blockBtn->setObjectName(QStringLiteral("requestBlock"));
        blockBtn->setToolTip(tr("Block this contact and remove the request"));
        rowLayout->addWidget(blockBtn);

        m_list->setItemWidget(item, row);
        connect(acceptBtn, &QPushButton::clicked, this, [this, key] { accept(key); });
        connect(declineBtn, &QPushButton::clicked, this, [this, key] { decline(key); });
        connect(blockBtn, &QPushButton::clicked, this, [this, key] { block(key); });
    }
}

// Accept = clear the hidden state (signal-cli "removeContact" without options). The messages
// and the session are kept; only the "not accepted yet" marker goes away.
void MessageRequestsDialog::accept(const QString &key)
{
    m_cli.call(QStringLiteral("removeContact"), QJsonObject{{QStringLiteral("recipient"), key}},
               [this, key](const QJsonValue &, const QString &err) {
        if (err.isEmpty()) {
            emit accepted(key);
            removeRow(key);
        } else if (m_status) {
            m_status->setText(tr("Could not accept the request: %1").arg(err));
            m_status->show();
        }
    });
}

// Decline is purely local: the contact stays hidden in signal-cli, Corvus just stops offering it.
void MessageRequestsDialog::decline(const QString &key)
{
    emit declined(key);
    removeRow(key);
}

void MessageRequestsDialog::block(const QString &key)
{
    m_cli.call(QStringLiteral("block"), QJsonObject{{QStringLiteral("recipient"), QJsonArray{key}}},
               [this, key](const QJsonValue &, const QString &err) {
        if (err.isEmpty()) {
            emit blockedContact(key);
            removeRow(key);
        } else if (m_status) {
            m_status->setText(tr("Could not block the contact: %1").arg(err));
            m_status->show();
        }
    });
}

void MessageRequestsDialog::removeRow(const QString &key)
{
    for (int i = m_list->count() - 1; i >= 0; --i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == key) delete m_list->item(i);
    }
    if (m_list->count() == 0) QDialog::accept(); // nothing left to do: close the dialog
}

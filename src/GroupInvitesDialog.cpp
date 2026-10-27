#include "GroupInvitesDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int kIdRole = Qt::UserRole;
constexpr int kNameRole = Qt::UserRole + 1;
}

GroupInvitesDialog::GroupInvitesDialog(SignalCli &cli, const QList<GroupInfo::Info> &invitations, QWidget *parent)
    : QDialog(parent), m_cli(cli)
{
    setWindowTitle(tr("Group invitations"));
    setMinimumWidth(440);
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("You have been invited to these groups. Accept to join, or decline the invitation.")));

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("invitationList"));
    for (const GroupInfo::Info &g : invitations) {
        const QString name = g.name.isEmpty() ? tr("Group") : g.name;
        auto *item = new QListWidgetItem(tr("%1  (%n member(s))", nullptr, int(g.members.size())).arg(name), m_list);
        item->setData(kIdRole, g.id);
        item->setData(kNameRole, name);
    }
    lay->addWidget(m_list, 1);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    auto *row = new QHBoxLayout;
    m_accept = new QPushButton(tr("Accept"));
    m_accept->setObjectName(QStringLiteral("acceptInviteButton"));
    m_decline = new QPushButton(tr("Decline"));
    m_decline->setObjectName(QStringLiteral("declineInviteButton"));
    auto *close = new QPushButton(tr("Close"));
    row->addWidget(m_accept);
    row->addWidget(m_decline);
    row->addStretch(1);
    row->addWidget(close);
    lay->addLayout(row);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_accept, &QPushButton::clicked, this, [this] { act(true); });
    connect(m_decline, &QPushButton::clicked, this, [this] { act(false); });
    connect(m_list, &QListWidget::currentItemChanged, this, [this] { updateButtons(); });
    if (m_list->count() > 0) m_list->setCurrentRow(0);
    updateButtons();
}

void GroupInvitesDialog::updateButtons()
{
    const bool can = !m_busy && m_list->currentItem();
    m_accept->setEnabled(can);
    m_decline->setEnabled(can);
}

void GroupInvitesDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_list->setEnabled(!busy);
    updateButtons();
}

void GroupInvitesDialog::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

void GroupInvitesDialog::act(bool accept)
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item || m_busy) return;
    const QString id = item->data(kIdRole).toString();
    const QString name = item->data(kNameRole).toString();
    if (!accept &&
        QMessageBox::question(this, tr("Decline invitation"), tr("Decline the invitation to “%1”?").arg(name)) !=
            QMessageBox::Yes)
        return;

    setBusy(true);
    setStatus(accept ? tr("Joining …") : tr("Declining …"), false);
    QPointer<GroupInvitesDialog> self(this);
    auto done = [self, id, name, accept](const QJsonValue &, const QString &err) {
        if (!self) return;
        self->setBusy(false);
        if (!err.isEmpty()) {
            self->setStatus((accept ? tr("Could not join the group: %1") : tr("Could not decline the invitation: %1")).arg(err), true);
            return;
        }
        for (int i = 0; i < self->m_list->count(); ++i)
            if (self->m_list->item(i)->data(kIdRole).toString() == id) {
                delete self->m_list->takeItem(i);
                break;
            }
        self->setStatus(accept ? tr("Joined “%1”.").arg(name) : tr("Declined the invitation to “%1”.").arg(name), false);
        self->updateButtons();
        if (accept) emit self->joined(id);
        else emit self->declined(id);
    };
    if (accept) m_cli.call(QStringLiteral("updateGroup"), {{QStringLiteral("groupId"), id}}, done);
    else m_cli.call(QStringLiteral("quitGroup"), {{QStringLiteral("groupId"), id}}, done);
}

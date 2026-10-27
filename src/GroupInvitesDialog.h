#pragma once

#include "GroupInfo.h"
#include "SignalCli.h"

#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;

// Open invitations to groups: accept (signal-cli "updateGroup" on a group we are invited to joins
// it) or decline ("quitGroup" on such a group declines the invitation).
class GroupInvitesDialog : public QDialog
{
    Q_OBJECT
public:
    GroupInvitesDialog(SignalCli &cli, const QList<GroupInfo::Info> &invitations, QWidget *parent = nullptr);

signals:
    void joined(const QString &groupId);
    void declined(const QString &groupId);

private:
    void act(bool accept);
    void updateButtons();
    void setBusy(bool busy);
    void setStatus(const QString &text, bool error);

    SignalCli &m_cli;
    bool m_busy = false;
    QListWidget *m_list;
    QPushButton *m_accept;
    QPushButton *m_decline;
    QLabel *m_status;
};

#pragma once

#include "SignalCli.h"

#include <QDialog>
#include <QList>
#include <QPair>

class QLabel;
class QListWidget;

// Message requests from hidden contacts (like the official Signal client): accept to chat with
// them (signal-cli "removeContact" clears the hidden state), decline to keep them hidden, or
// block them (signal-cli "block"). The caller owns the state changes via the signals.
class MessageRequestsDialog : public QDialog
{
    Q_OBJECT
public:
    // key -> title as shown in the row (number or UUID as fallback).
    using RequestList = QList<QPair<QString, QString>>;
    explicit MessageRequestsDialog(SignalCli &cli, const RequestList &requests, QWidget *parent = nullptr);

signals:
    void accepted(const QString &key);
    void declined(const QString &key);
    void blockedContact(const QString &key);

private:
    void accept(const QString &key);
    void decline(const QString &key);
    void block(const QString &key);
    void removeRow(const QString &key);

    SignalCli &m_cli;
    QListWidget *m_list;
    QLabel *m_status;
};

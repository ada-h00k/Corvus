#pragma once

#include "SafetyNumber.h"
#include "SignalCli.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

// Shows the safety number of a contact and lets the user mark the contact as verified,
// like "View safety number" in the official Signal clients.
class SafetyNumberDialog : public QDialog
{
    Q_OBJECT
public:
    // `recipient` is the phone number (or UUID) signal-cli knows the contact by.
    SafetyNumberDialog(SignalCli &cli, const QString &recipient, const QString &displayName,
                       QWidget *parent = nullptr);

    void reload();

signals:
    // The contact was marked as verified (recipient as passed to the dialog, the 60 digits).
    void verified(const QString &recipient, const QString &digits);

private:
    void showIdentity();
    void showQrCode();
    void updateCompare();
    void verify();
    void setStatus(const QString &text, bool error);

    SignalCli &m_cli;
    QString m_recipient;
    QString m_name;
    SafetyNumber::Identity m_identity;
    bool m_hasIdentity = false;
    bool m_busy = false;

    QLabel *m_qr;
    QLabel *m_number;
    QLabel *m_trust;
    QLabel *m_compareResult;
    QLabel *m_message;
    QLineEdit *m_compare;
    QPushButton *m_copy;
    QPushButton *m_verify;
};

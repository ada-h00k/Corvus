#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QProcess;
class QPushButton;
class QWidget;

// Links corvus-cli as an additional device to an existing Signal account
// ("corvus-cli link -n <name>") and shows the link URI / QR code.
class LinkDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LinkDialog(const QString &executable, QWidget *parent = nullptr);
    ~LinkDialog() override;

    // The unlock secret, passed to corvus-cli so the freshly linked account is encrypted
    // right away (see SignalCli::environmentWithSecret).
    void setSecret(const QString &secret) { m_secret = secret; }

private slots:
    void startLinking();
    void onStdout();
    void onFinished(int exitCode);
    void reject() override;

private:
    void showUri(const QString &uri);
    void offerHistoryTransfer();
    void answerTransfer(bool transfer);
    void killProcess();

    QString m_exe;
    QString m_secret;
    QProcess *m_proc = nullptr;
    QByteArray m_buf;
    QLineEdit *m_name;
    QPushButton *m_start;
    QLabel *m_qr;
    QLineEdit *m_uri;
    QLabel *m_status;
    // The transfer offer the phone made after scanning the QR code (synchronized start),
    // and the widget holding the answer buttons while it is open.
    QWidget *m_transferChoice = nullptr;
    bool m_transferAnswered = false;
};

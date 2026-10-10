#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QProcess;
class QPushButton;

// Links signal-cli as an additional device to an existing Signal account
// ("signal-cli link -n <name>") and shows the link URI / QR code.
class LinkDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LinkDialog(const QString &executable, QWidget *parent = nullptr);
    ~LinkDialog() override;

private slots:
    void startLinking();
    void onStdout();
    void onFinished(int exitCode);
    void reject() override;

private:
    void showUri(const QString &uri);
    void killProcess();

    QString m_exe;
    QProcess *m_proc = nullptr;
    QByteArray m_buf;
    QLineEdit *m_name;
    QPushButton *m_start;
    QLabel *m_qr;
    QLineEdit *m_uri;
    QLabel *m_status;
};

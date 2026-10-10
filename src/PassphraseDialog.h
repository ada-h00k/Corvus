#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

class PassphraseDialog : public QDialog
{
    Q_OBJECT
public:
    enum Mode { Create, Unlock, Change };
    explicit PassphraseDialog(Mode mode, QWidget *parent = nullptr);

    QString passphrase() const;    // the (new) passphrase
    QString oldPassphrase() const; // Change mode only
    void setError(const QString &text);

private:
    void validate();

    Mode m_mode;
    QLineEdit *m_old = nullptr;
    QLineEdit *m_pass;
    QLineEdit *m_confirm = nullptr;
    QLabel *m_error;
    QPushButton *m_ok;
};

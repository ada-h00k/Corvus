#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

// Settings → Profile settings …
// A window of its own (like Appearance) that collects all Signal profile options:
// username, display name and avatar. The dialog does not talk to signal-cli itself;
// it only emits signals that MainWindow turns into signal-cli calls.
class ProfileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProfileDialog(QWidget *parent = nullptr);

    // Shown next to "Current username:" (e.g. the username, "not set" or "unavailable").
    void setCurrentUsername(const QString &text);
    // Result of the last action (success or error), shown at the bottom of the window.
    void setStatus(const QString &text, bool error = false);

signals:
    void usernameChosen(const QString &username);
    void displayNameChosen(const QString &displayName);
    void avatarChosen(const QString &path);
    void avatarRemoveRequested();

private:
    void chooseAvatar();

    QLabel *m_currentUsername;
    QLineEdit *m_username;
    QPushButton *m_applyUsername;
    QLineEdit *m_displayName;
    QPushButton *m_applyDisplayName;
    QPushButton *m_chooseAvatar;
    QPushButton *m_removeAvatar;
    QLabel *m_status;
};

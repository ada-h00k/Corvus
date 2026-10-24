#pragma once

#include <QPixmap>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

// Settings → Profile: username, display name and avatar of the Signal account.
// Unlike the other pages every button here acts immediately (it talks to Signal, not to
// a local file), so the page is independent of the dialog's OK / Apply buttons. The page does
// not talk to signal-cli itself; it only emits signals that MainWindow turns into calls.
class ProfilePage : public QWidget
{
    Q_OBJECT
public:
    explicit ProfilePage(QWidget *parent = nullptr);

    // Without a signal-cli connection the page is greyed out and says why.
    void setConnected(bool connected);

    // Shown next to "Current username:" (e.g. the username, "not set" or "unavailable").
    void setCurrentUsername(const QString &text);
    // The account's current display name (put into the edit field unless the user is typing there)
    // and its current avatar (a placeholder is shown for a null pixmap).
    void setCurrentDisplayName(const QString &name);
    void setAvatar(const QPixmap &avatar);
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
    QLabel *m_avatarView;
    QString m_currentName;
    QLabel *m_offline;
    QWidget *m_content;
};

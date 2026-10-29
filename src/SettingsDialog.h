#pragma once

#include <QDialog>

#include "Appearance.h"
#include "NotificationSettings.h"

class AppearancePage;
class NotificationPage;
class ProfilePage;
class Notifier;
class SoundPlayer;
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;

// Settings → Settings … (Ctrl+,): everything in one window, grouped by topic.
//
//   General        window & tray, contact list, message field
//   Appearance     bubble colours, corner radius, app icon (with live preview)
//   Notifications  desktop notifications and sound
//   Profile        Signal username, display name, avatar
//   Privacy        protection of the local chat history
//   Advanced       signal-cli and the GIPHY API key
//
// General, Appearance and Notifications are applied with OK / Apply (and can be cancelled).
// Buttons on the Profile, Privacy and Advanced pages act immediately because they change
// something outside this window (Signal, the keychain, the signal-cli process).
// The dialog never touches the application itself: MainWindow reacts to the signals.

// --- General --------------------------------------------------------------------------------
class GeneralPage : public QWidget
{
    Q_OBJECT
public:
    GeneralPage(const Appearance &ap, const NotificationSettings &ns, bool trayAvailable, QWidget *parent = nullptr);
    void applyTo(Appearance &ap) const;
    void applyTo(NotificationSettings &ns) const;
    void load(const Appearance &ap, const NotificationSettings &ns);
signals:
    void changed();

private:
    void refreshEnabled();
    bool m_trayAvailable;
    QCheckBox *m_tray;
    QCheckBox *m_closeToTray;
    QCheckBox *m_startMinimizedInTray;
    QCheckBox *m_showAvatars;
    QCheckBox *m_sortAlphabetically;
    QCheckBox *m_alwaysShowSend;
    QCheckBox *m_generateLinkPreviews;
};

// --- Privacy --------------------------------------------------------------------------------
class PrivacyPage : public QWidget
{
    Q_OBJECT
public:
    explicit PrivacyPage(QWidget *parent = nullptr);
    void setHistory(const QString &path, const QString &protectionMode, bool ready);
signals:
    void changeProtectionRequested();

private:
    QLabel *m_path;
    QLabel *m_mode;
    QPushButton *m_change;
};

// --- Advanced -------------------------------------------------------------------------------
class AdvancedPage : public QWidget
{
    Q_OBJECT
public:
    explicit AdvancedPage(QWidget *parent = nullptr);
    void setSignalCli(const QString &path, const QString &version);
    void setGiphyKeyState(bool known, bool set);
signals:
    void chooseExecutableRequested();
    void checkForUpdateRequested();
    void reconnectRequested();
    void giphyKeyRequested();

private:
    QLabel *m_path;
    QLabel *m_version;
    QLabel *m_giphy;
};

// --- The dialog -----------------------------------------------------------------------------
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    enum Page { General, AppearanceSettings, Notifications, Profile, Privacy, Advanced };

    SettingsDialog(const Appearance &ap, const NotificationSettings &ns, SoundPlayer *sound, Notifier *notifier,
                   bool trayAvailable, QWidget *parent = nullptr);

    void showPage(Page page);

    // The edited values (valid at any time; what "Apply" would hand over).
    Appearance appearance() const;
    NotificationSettings notifications() const;

    ProfilePage *profilePage() const { return m_profile; }
    PrivacyPage *privacyPage() const { return m_privacy; }
    AdvancedPage *advancedPage() const { return m_advanced; }

signals:
    // The user switched to another page (int == Page).
    void pageChanged(int page);
    // Emitted by OK and Apply.
    void applied(const Appearance &ap, const NotificationSettings &ns);

private:
    void apply();
    void updateButtons();

    QListWidget *m_list;
    QStackedWidget *m_stack;
    GeneralPage *m_general;
    AppearancePage *m_appearance;
    NotificationPage *m_notify;
    ProfilePage *m_profile;
    PrivacyPage *m_privacy;
    AdvancedPage *m_advanced;
    QPushButton *m_applyButton;
    Appearance m_initialAppearance;
    NotificationSettings m_initialNotify;
};

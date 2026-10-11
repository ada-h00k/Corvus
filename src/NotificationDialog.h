#pragma once

#include <QDialog>

#include "NotificationSettings.h"

class Notifier;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class SoundPlayer;

// Settings → Notifications & sound …
class NotificationDialog : public QDialog
{
    Q_OBJECT
public:
    // `sound` and `notifier` are used by the test buttons (not owned).
    NotificationDialog(const NotificationSettings &current, SoundPlayer *sound, Notifier *notifier,
                       bool trayAvailable, QWidget *parent = nullptr);

    NotificationSettings settings() const;
    void setSettings(const NotificationSettings &s);

private:
    void refreshEnabled();
    void chooseFile();

    SoundPlayer *m_sound;
    Notifier *m_notifier;

    QCheckBox *m_tray;
    QCheckBox *m_closeToTray;
    QCheckBox *m_notify;
    QComboBox *m_content;
    QPushButton *m_testNotify;
    QCheckBox *m_soundOn;
    QComboBox *m_soundBox;
    QPushButton *m_pickFile;
    QLabel *m_fileLabel;
    QPushButton *m_testSound;
    QLabel *m_soundNote;
    QString m_customFile;
};

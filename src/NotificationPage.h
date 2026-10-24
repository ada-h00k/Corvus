#pragma once

#include <QWidget>

#include "NotificationSettings.h"

class Notifier;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class SoundPlayer;

// Settings → Notifications: desktop notifications and sound. (The tray icon lives on the General page.)
// settings() returns the values of the notification / sound controls; the other fields are passed
// through unchanged from setSettings().
class NotificationPage : public QWidget
{
    Q_OBJECT
public:
    // `sound` and `notifier` are used by the test buttons (not owned).
    NotificationPage(const NotificationSettings &current, SoundPlayer *sound, Notifier *notifier,
                     QWidget *parent = nullptr);

    NotificationSettings settings() const;
    void setSettings(const NotificationSettings &s);

private:
    void refreshEnabled();
    void chooseFile();

    SoundPlayer *m_sound;
    Notifier *m_notifier;

    QCheckBox *m_notify;
    QComboBox *m_content;
    QPushButton *m_testNotify;
    QLabel *m_notifyNote; // result of the last "Test"
    QCheckBox *m_soundOn;
    QComboBox *m_soundBox;
    QPushButton *m_pickFile;
    QLabel *m_fileLabel;
    QPushButton *m_testSound;
    QLabel *m_soundNote;
    QString m_customFile;
    NotificationSettings m_base; // fields this page does not edit

signals:
    void changed();
};

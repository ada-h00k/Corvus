#pragma once

#include <QString>

// Settings → Notifications & sound …  Every switch is independent of the others.
struct NotificationSettings {
    enum Content { NameAndText = 0, NameOnly = 1, Hidden = 2 };

    bool notifications = true;     // desktop notifications
    int content = NameAndText;     // what a notification reveals
    bool sound = true;             // play a sound for new messages
    QString soundId = QStringLiteral("chime"); // built-in id or "custom"
    QString soundFile;             // used when soundId == "custom"
    bool trayIcon = true;          // show the tray icon
    bool closeToTray = true;       // closing the window keeps the app running in the tray

    static NotificationSettings load();
    void save() const;
    bool operator==(const NotificationSettings &o) const;
    bool operator!=(const NotificationSettings &o) const { return !(*this == o); }
};

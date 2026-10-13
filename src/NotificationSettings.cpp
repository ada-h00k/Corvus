#include "NotificationSettings.h"

#include <QSettings>

static const char *kGroup = "notifications";

NotificationSettings NotificationSettings::load()
{
    NotificationSettings n;
    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    n.notifications = s.value(QStringLiteral("enabled"), n.notifications).toBool();
    n.content = qBound(0, s.value(QStringLiteral("content"), n.content).toInt(), 2);
    n.sound = s.value(QStringLiteral("sound"), n.sound).toBool();
    n.soundId = s.value(QStringLiteral("soundId"), n.soundId).toString();
    n.soundFile = s.value(QStringLiteral("soundFile")).toString();
    n.trayIcon = s.value(QStringLiteral("trayIcon"), n.trayIcon).toBool();
    n.closeToTray = s.value(QStringLiteral("closeToTray"), n.closeToTray).toBool();
    n.startMinimizedInTray = s.value(QStringLiteral("startMinimizedInTray"), n.startMinimizedInTray).toBool();
    return n;
}

void NotificationSettings::save() const
{
    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    s.setValue(QStringLiteral("enabled"), notifications);
    s.setValue(QStringLiteral("content"), content);
    s.setValue(QStringLiteral("sound"), sound);
    s.setValue(QStringLiteral("soundId"), soundId);
    s.setValue(QStringLiteral("soundFile"), soundFile);
    s.setValue(QStringLiteral("trayIcon"), trayIcon);
    s.setValue(QStringLiteral("closeToTray"), closeToTray);
    s.setValue(QStringLiteral("startMinimizedInTray"), startMinimizedInTray);
}

bool NotificationSettings::operator==(const NotificationSettings &o) const
{
    return notifications == o.notifications && content == o.content && sound == o.sound && soundId == o.soundId &&
           soundFile == o.soundFile && trayIcon == o.trayIcon && closeToTray == o.closeToTray &&
           startMinimizedInTray == o.startMinimizedInTray;
}

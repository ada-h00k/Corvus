#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSystemTrayIcon>

class QDBusInterface;

// Desktop notifications.
//
// On Linux the freedesktop.org Notifications service is used directly, so they work
// independently of the tray icon, are replaced per conversation instead of piling up,
// and clicking one reports the conversation to open. On other platforms (or without a
// notification service) the visible tray icon is used as a fallback.
class Notifier : public QObject
{
    Q_OBJECT
public:
    explicit Notifier(QObject *parent = nullptr);

    void setTrayIcon(QSystemTrayIcon *tray) { m_tray = tray; }

    // `key` identifies the conversation (a newer notification for the same key replaces the old one).
    // `icon` (optional) is shown instead of the generic application icon, e.g. the contact's avatar.
    void notify(const QString &key, const QString &title, const QString &body, const QPixmap &icon = QPixmap());
    void close(const QString &key); // e.g. when the conversation is opened

    bool usingDBus();

signals:
    void activated(const QString &key); // the user clicked a notification

private slots:
    void onActionInvoked(uint id, const QString &action);
    void onClosed(uint id, uint reason);

private:
    bool ensureDBus();

    QPointer<QSystemTrayIcon> m_tray;
    QDBusInterface *m_iface = nullptr;
    bool m_checked = false;
    bool m_ok = false;
    bool m_markup = false;
    QHash<QString, uint> m_idForKey;
    QHash<uint, QString> m_keyForId;
    QString m_lastTrayKey;
    bool m_trayConnected = false;
};

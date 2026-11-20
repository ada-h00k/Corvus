#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVariant>
#include <optional>

class QDBusInterface;

// Desktop notifications.
//
// On Linux they first go through the XDG desktop portal (org.freedesktop.portal.Notification),
// so they also work inside a Flatpak sandbox without any --talk-name permission: the portal is
// always reachable there, keeps the last notification per app-chosen id (so they are replaced
// per conversation instead of piling up), and clicking one reports the conversation to open.
// Where no portal runs, the freedesktop.org Notifications service is used directly as a
// fallback, so minimal setups without a portal keep daemon notifications. On other platforms
// (or without either service) the tray icon is used as a fallback: its balloon messages are
// what Windows and macOS show as notifications. If the tray icon is switched off in the
// settings, a temporary one is shown for the moment a notification is on screen.
class Notifier : public QObject
{
    Q_OBJECT
public:
    explicit Notifier(QObject *parent = nullptr);

    void setTrayIcon(QSystemTrayIcon *tray) { m_tray = tray; }

    // How a notification was handed over (the system may still suppress it, e.g. "Do not disturb").
    enum class Delivery {
        None,          // no way to show one: no portal, no notification service and no usable tray
        Portal,        // XDG desktop portal (works inside Flatpak without extra permissions)
        DBus,          // freedesktop.org Notifications service, used directly (no portal running)
        Tray,          // balloon message of the tray icon
        TemporaryTray, // same, with a tray icon that is only shown for the message
    };
    static QString describe(Delivery d);

    // For tests: pretend the system tray is (not) available / can (not) show messages.
    void setTrayCapabilitiesForTesting(bool available, bool supportsMessages);

    // `key` identifies the conversation (a newer notification for the same key replaces the old one).
    // `icon` (optional) is shown instead of the generic application icon, e.g. the contact's avatar.
    Delivery notify(const QString &key, const QString &title, const QString &body, const QPixmap &icon = QPixmap());
    void close(const QString &key); // e.g. when the conversation is opened

    bool usingPortal();
    bool usingDBus();

signals:
    void activated(const QString &key); // the user clicked a notification

private slots:
    void onActionInvoked(uint id, const QString &action);
    void onPortalActionInvoked(const QString &id);
    void onClosed(uint id, uint reason);

private:
    bool ensurePortal();
    bool ensureDBus();
    QVariant portalIcon(const QPixmap &icon);
    QSystemTrayIcon *trayForMessages(bool *temporary);
    void connectTray(QSystemTrayIcon *tray);

    QPointer<QSystemTrayIcon> m_tray;
    // The portal: reached whenever it runs (always inside a Flatpak), tried first.
    QDBusInterface *m_portal = nullptr;
    uint m_portalVersion = 0;
    bool m_portalChecked = false;
    bool m_portalOk = false;
    QSet<QString> m_portalSent; // keys sent through the portal, so close() withdraws only those
    // The direct freedesktop.org Notifications service: fallback when no portal runs.
    QDBusInterface *m_iface = nullptr;
    bool m_checked = false;
    bool m_ok = false;
    bool m_markup = false;
    QHash<QString, uint> m_idForKey;
    QHash<uint, QString> m_keyForId;
    QString m_lastTrayKey;
    bool m_trayConnected = false;
    QSystemTrayIcon *m_tempTray = nullptr; // only while a notification is shown
    bool m_tempTrayConnected = false;
    std::optional<bool> m_forcedTrayAvailable;
    std::optional<bool> m_forcedTrayMessages;
    QTimer m_tempTrayTimer;
};

#include "Notifier.h"

#include <QBuffer>
#include <QImage>
#include <QSystemTrayIcon>

#ifdef HAVE_QT_DBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusReply>
#endif

#ifdef HAVE_QT_DBUS
// "image-data" hint of the freedesktop.org Notifications spec: (iiibiiay)
// width, height, rowstride, has_alpha, bits_per_sample, channels, data
struct NotificationImage
{
    int width = 0;
    int height = 0;
    int rowStride = 0;
    bool hasAlpha = false;
    int bitsPerSample = 8;
    int channels = 0;
    QByteArray data;
};
Q_DECLARE_METATYPE(NotificationImage)

static QDBusArgument &operator<<(QDBusArgument &arg, const NotificationImage &img)
{
    arg.beginStructure();
    arg << img.width << img.height << img.rowStride << img.hasAlpha << img.bitsPerSample << img.channels << img.data;
    arg.endStructure();
    return arg;
}

static const QDBusArgument &operator>>(const QDBusArgument &arg, NotificationImage &img)
{
    arg.beginStructure();
    arg >> img.width >> img.height >> img.rowStride >> img.hasAlpha >> img.bitsPerSample >> img.channels >> img.data;
    arg.endStructure();
    return arg;
}

static QVariant imageHint(const QPixmap &pm)
{
    qDBusRegisterMetaType<NotificationImage>();
    const QImage img = pm.toImage().convertToFormat(QImage::Format_RGBA8888);
    NotificationImage out;
    out.width = img.width();
    out.height = img.height();
    out.rowStride = int(img.bytesPerLine());
    out.hasAlpha = true;
    out.bitsPerSample = 8;
    out.channels = 4;
    out.data = QByteArray(reinterpret_cast<const char *>(img.constBits()), qsizetype(img.sizeInBytes()));
    return QVariant::fromValue(out);
}
#endif

namespace {
// The XDG desktop portal's notification interface. It is always reachable from inside a
// Flatpak sandbox (no --talk-name permission needed) and is tried first wherever it runs.
const char *kPortalService = "org.freedesktop.portal.Desktop";
const char *kPortalPath = "/org/freedesktop/portal/desktop";
const char *kPortalInterface = "org.freedesktop.portal.Notification";
// Fallback when no portal runs: the freedesktop.org Notifications service, used directly.
const char *kService = "org.freedesktop.Notifications";
const char *kPath = "/org/freedesktop/Notifications";
}

// The icon name announced for notifications without an avatar. In a Flatpak the icon (like the
// .desktop file) is named after the Flatpak app id, elsewhere it is the plain "corvus".
static QString themedIconName()
{
    const QString flatpakId = qEnvironmentVariable("FLATPAK_ID");
    return flatpakId.isEmpty() ? QStringLiteral("corvus") : flatpakId;
}

Notifier::Notifier(QObject *parent) : QObject(parent)
{
    // The temporary tray icon stays long enough for the message to be read.
    m_tempTrayTimer.setSingleShot(true);
    m_tempTrayTimer.setInterval(12000);
    connect(&m_tempTrayTimer, &QTimer::timeout, this, [this] {
        if (m_tempTray) m_tempTray->hide();
    });
}

void Notifier::setTrayCapabilitiesForTesting(bool available, bool supportsMessages)
{
    m_forcedTrayAvailable = available;
    m_forcedTrayMessages = supportsMessages;
}

QString Notifier::describe(Delivery d)
{
    switch (d) {
    case Delivery::Portal:
        return tr("Sent to the desktop notification service through the portal.");
    case Delivery::DBus:
        return tr("Sent to the desktop notification service.");
    case Delivery::Tray:
        return tr("Sent as a tray icon message. If nothing appeared, check that notifications are allowed for "
                  "Corvus in your system settings (on Windows: Settings → System → Notifications, and turn off "
                  "Do not disturb / Focus assist).");
    case Delivery::TemporaryTray:
        return tr("Sent as a message of a tray icon that is shown only for this (the tray icon is switched off). "
                  "If nothing appeared, check that notifications are allowed for Corvus in your system settings "
                  "(on Windows: Settings → System → Notifications, and turn off Do not disturb / Focus assist).");
    case Delivery::None:
        break;
    }
    return tr("Notifications cannot be shown here: no notification service and no system tray were found.");
}

// The tray icon whose balloon messages serve as notifications; null if there is none.
// With the tray icon switched off a temporary one takes over, so that notifications do
// not silently disappear just because the icon is not wanted permanently.
QSystemTrayIcon *Notifier::trayForMessages(bool *temporary)
{
    *temporary = false;
    const bool messages = m_forcedTrayMessages.value_or(QSystemTrayIcon::supportsMessages());
    const bool available = m_forcedTrayAvailable.value_or(QSystemTrayIcon::isSystemTrayAvailable());
    if (!m_tray || !messages) return nullptr;
    if (m_tray->isVisible()) return m_tray;
    if (!available) return nullptr;
    if (!m_tempTray) m_tempTray = new QSystemTrayIcon(this);
    m_tempTray->setIcon(m_tray->icon());
    m_tempTray->show();
    m_tempTrayTimer.start();
    *temporary = true;
    return m_tempTray;
}

void Notifier::connectTray(QSystemTrayIcon *tray)
{
    bool &connected = tray == m_tempTray ? m_tempTrayConnected : m_trayConnected;
    if (connected) return;
    connected = true;
    connect(tray, &QSystemTrayIcon::messageClicked, this, [this] {
        if (!m_lastTrayKey.isEmpty()) emit activated(m_lastTrayKey);
    });
}

bool Notifier::ensurePortal()
{
#ifdef HAVE_QT_DBUS
    if (m_portalChecked) return m_portalOk;
    m_portalChecked = true;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;
    m_portal = new QDBusInterface(QLatin1String(kPortalService), QLatin1String(kPortalPath),
                                  QLatin1String(kPortalInterface), bus, this);
    m_portal->setTimeout(3000);
    if (!m_portal->isValid()) {
        m_portal->deleteLater();
        m_portal = nullptr;
        return false;
    }
    const QVariant version = m_portal->property("version"); // advertised by every portal
    m_portalVersion = version.isValid() ? version.toUInt() : 1;
    // The portal reports clicks by sending ActionInvoked back to the caller. The signal grew
    // arguments over versions (id, action, [platform data …]); a slot with only the first one
    // (the notification id, i.e. our conversation key) matches every variant of it.
    bus.connect(QLatin1String(kPortalService), QLatin1String(kPortalPath), QLatin1String(kPortalInterface),
                QStringLiteral("ActionInvoked"), this, SLOT(onPortalActionInvoked(QString)));
    m_portalOk = true;
    return true;
#else
    return false;
#endif
}

bool Notifier::usingPortal()
{
    return ensurePortal();
}

bool Notifier::ensureDBus()
{
#ifdef HAVE_QT_DBUS
    if (m_checked) return m_ok;
    m_checked = true;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;
    m_iface = new QDBusInterface(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService), bus, this);
    m_iface->setTimeout(3000);
    if (!m_iface->isValid()) {
        m_iface->deleteLater();
        m_iface = nullptr;
        return false;
    }
    const QDBusReply<QStringList> caps = m_iface->call(QStringLiteral("GetCapabilities"));
    m_markup = caps.isValid() && caps.value().contains(QStringLiteral("body-markup"));
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService), QStringLiteral("ActionInvoked"),
                this, SLOT(onActionInvoked(uint, QString)));
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService), QStringLiteral("NotificationClosed"),
                this, SLOT(onClosed(uint, uint)));
    m_ok = true;
    return true;
#else
    return false;
#endif
}

bool Notifier::usingDBus()
{
    return ensureDBus();
}

// The "icon" field of the portal notification: a serialized GIcon. With an avatar that is a
// ("bytes", PNG data) tuple, the form every portal accepts; the raw-pixel form of the direct
// service is not part of the portal API. Without an avatar the themed application icon
// shorthand (a plain icon name) is used.
QVariant Notifier::portalIcon(const QPixmap &icon)
{
#ifdef HAVE_QT_DBUS
    if (!icon.isNull()) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        icon.save(&buffer, "PNG");
        QDBusArgument serialized;
        serialized.beginStructure();
        serialized << QStringLiteral("bytes") << png;
        serialized.endStructure();
        return QVariant::fromValue(serialized);
    }
    return themedIconName();
#else
    Q_UNUSED(icon)
    return {};
#endif
}

Notifier::Delivery Notifier::notify(const QString &key, const QString &title, const QString &body, const QPixmap &icon)
{
#ifdef HAVE_QT_DBUS
    if (ensurePortal()) {
        QVariantMap notification;
        notification.insert(QStringLiteral("title"), title);
        notification.insert(QStringLiteral("body"), body); // plain text: the portal escapes it itself
        notification.insert(QStringLiteral("category"), QStringLiteral("im.received"));
        notification.insert(QStringLiteral("default-action"), QStringLiteral("open"));
        notification.insert(QStringLiteral("icon"), portalIcon(icon));
        if (m_portalVersion >= 2)
            notification.insert(QStringLiteral("sound"), QStringLiteral("silent")); // Corvus plays its own sound
        // Re-adding the same id replaces the previous notification, so per-conversation
        // replacement needs no id bookkeeping here (unlike the direct service below).
        const QDBusMessage reply =
            m_portal->call(QStringLiteral("AddNotification"), key, QVariant::fromValue(notification));
        if (reply.type() != QDBusMessage::ErrorMessage) {
            m_portalSent.insert(key);
            return Delivery::Portal;
        }
        // The portal stopped answering: fall through to the direct service or the tray.
        m_portalOk = false;
        m_portalChecked = false;
        m_portal->deleteLater();
        m_portal = nullptr;
    }
    if (ensureDBus()) {
        QVariantMap hints;
        hints.insert(QStringLiteral("desktop-entry"), themedIconName());
        hints.insert(QStringLiteral("category"), QStringLiteral("im.received"));
        if (!icon.isNull()) hints.insert(QStringLiteral("image-data"), imageHint(icon)); // preferred over app_icon
        const QString text = m_markup ? body.toHtmlEscaped() : body; // servers with markup would eat "<"
        const QDBusMessage reply = m_iface->call(QStringLiteral("Notify"), QStringLiteral("Corvus"),
                                                 QVariant::fromValue<uint>(m_idForKey.value(key, 0)),
                                                 themedIconName(), title, text,
                                                 QStringList{QStringLiteral("default"), tr("Open")}, hints, -1);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            const uint id = reply.arguments().first().toUInt();
            const uint old = m_idForKey.value(key, 0);
            if (old && old != id) m_keyForId.remove(old);
            m_idForKey.insert(key, id);
            m_keyForId.insert(id, key);
            return Delivery::DBus;
        }
        // The service stopped answering: fall through to the tray fallback.
        m_ok = false;
        m_checked = false;
    }
#endif
    bool temporary = false;
    if (QSystemTrayIcon *tray = trayForMessages(&temporary)) {
        connectTray(tray);
        m_lastTrayKey = key;
        if (icon.isNull())
            tray->showMessage(title, body, QSystemTrayIcon::NoIcon, 8000);
        else
            tray->showMessage(title, body, QIcon(icon), 8000);
        return temporary ? Delivery::TemporaryTray : Delivery::Tray;
    }
    return Delivery::None;
}

void Notifier::close(const QString &key)
{
#ifdef HAVE_QT_DBUS
    if (m_portalSent.remove(key) && m_portalOk && m_portal)
        m_portal->call(QStringLiteral("RemoveNotification"), key);
    const uint id = m_idForKey.take(key);
    if (id) {
        m_keyForId.remove(id);
        if (m_ok && m_iface) m_iface->call(QStringLiteral("CloseNotification"), QVariant::fromValue<uint>(id));
    }
#else
    Q_UNUSED(key)
#endif
}

void Notifier::onActionInvoked(uint id, const QString &)
{
    const QString key = m_keyForId.value(id);
    if (!key.isEmpty()) emit activated(key);
}

void Notifier::onPortalActionInvoked(const QString &id)
{
    if (!id.isEmpty()) emit activated(id);
}

void Notifier::onClosed(uint id, uint)
{
    const QString key = m_keyForId.take(id);
    if (!key.isEmpty() && m_idForKey.value(key) == id) m_idForKey.remove(key);
}

#include "Notifier.h"

#include <QImage>
#include <QSystemTrayIcon>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusArgument>
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
const char *kService = "org.freedesktop.Notifications";
const char *kPath = "/org/freedesktop/Notifications";
}

Notifier::Notifier(QObject *parent) : QObject(parent) {}

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

void Notifier::notify(const QString &key, const QString &title, const QString &body, const QPixmap &icon)
{
#ifdef HAVE_QT_DBUS
    if (ensureDBus()) {
        QVariantMap hints;
        hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("signal-cli-qt"));
        hints.insert(QStringLiteral("category"), QStringLiteral("im.received"));
        if (!icon.isNull()) hints.insert(QStringLiteral("image-data"), imageHint(icon)); // preferred over app_icon
        const QString text = m_markup ? body.toHtmlEscaped() : body; // servers with markup would eat "<"
        const QDBusMessage reply = m_iface->call(QStringLiteral("Notify"), QStringLiteral("signal-cli Qt"),
                                                 QVariant::fromValue<uint>(m_idForKey.value(key, 0)),
                                                 QStringLiteral("signal-cli-qt"), title, text,
                                                 QStringList{QStringLiteral("default"), tr("Open")}, hints, -1);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            const uint id = reply.arguments().first().toUInt();
            const uint old = m_idForKey.value(key, 0);
            if (old && old != id) m_keyForId.remove(old);
            m_idForKey.insert(key, id);
            m_keyForId.insert(id, key);
            return;
        }
        // The service stopped answering: fall through to the tray fallback.
        m_ok = false;
        m_checked = false;
    }
#endif
    if (m_tray && m_tray->isVisible() && QSystemTrayIcon::supportsMessages()) {
        if (!m_trayConnected) {
            m_trayConnected = true;
            connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
                if (!m_lastTrayKey.isEmpty()) emit activated(m_lastTrayKey);
            });
        }
        m_lastTrayKey = key;
        if (icon.isNull())
            m_tray->showMessage(title, body, QSystemTrayIcon::NoIcon, 8000);
        else
            m_tray->showMessage(title, body, QIcon(icon), 8000);
    }
}

void Notifier::close(const QString &key)
{
#ifdef HAVE_QT_DBUS
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

void Notifier::onClosed(uint id, uint)
{
    const QString key = m_keyForId.take(id);
    if (!key.isEmpty() && m_idForKey.value(key) == id) m_idForKey.remove(key);
}

// SPDX-License-Identifier: AGPL-3.0-only
#include "portalsecret.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QEventLoop>
#include <QFile>
#include <QMessageAuthenticationCode>
#include <QObject>
#include <QRandomGenerator>
#include <QTimer>
#include <QVariantMap>

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

const char kService[] = "org.freedesktop.portal.Desktop";

class ResponseWaiter : public QObject
{
    Q_OBJECT
public:
    QEventLoop loop;
    uint response = 2; // 0 = Erfolg, 1 = abgebrochen, 2 = sonstiger Fehler
    bool done = false;

public slots:
    void onResponse(uint code, const QVariantMap &)
    {
        response = code;
        done = true;
        loop.quit();
    }
};

} // namespace

namespace PortalSecret {

bool runningInFlatpak()
{
    return QFile::exists(QStringLiteral("/.flatpak-info"));
}

std::optional<QByteArray> retrieve(QString *error, int timeoutMs)
{
    auto fail = [error](const QString &msg) -> std::optional<QByteArray> {
        if (error)
            *error = msg;
        return std::nullopt;
    };

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return fail(QStringLiteral("no D-Bus session bus"));

    // Das Portal schreibt das Secret in das eine Ende dieses Socketpairs.
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
        return fail(QStringLiteral("socketpair() failed"));
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    // Request-Objektpfad vorab berechnen, damit das Response-Signal nicht
    // verpasst wird (siehe Portal-Dokumentation zu handle_token).
    const QString token = QStringLiteral("corvus%1").arg(QRandomGenerator::global()->generate64(), 0, 16);
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString requestPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    ResponseWaiter waiter;
    bus.connect(QLatin1String(kService), requestPath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                &waiter, SLOT(onResponse(uint, QVariantMap)));

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Secret"), QStringLiteral("RetrieveSecret"));
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    msg << QVariant::fromValue(QDBusUnixFileDescriptor(fds[1])) << options;

    const QDBusMessage reply = bus.call(msg, QDBus::Block, 10000);
    ::close(fds[1]); // QDBusUnixFileDescriptor hat dupliziert; unser Ende schließen -> EOF möglich

    auto cleanup = [&]() {
        ::close(fds[0]);
        bus.disconnect(QLatin1String(kService), requestPath,
                       QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                       &waiter, SLOT(onResponse(uint, QVariantMap)));
    };

    if (reply.type() == QDBusMessage::ErrorMessage) {
        cleanup();
        return fail(QStringLiteral("RetrieveSecret failed: %1").arg(reply.errorMessage()));
    }

    if (!waiter.done) {
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &waiter.loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        waiter.loop.exec();
    }

    if (!waiter.done) {
        cleanup();
        return fail(QStringLiteral("timeout waiting for the Secret portal"));
    }
    if (waiter.response != 0) {
        cleanup();
        return fail(QStringLiteral("Secret portal request denied or cancelled (code %1)")
                        .arg(waiter.response));
    }

    // Das Portal hat vor dem Response-Signal geschrieben und sein Ende geschlossen.
    QByteArray secret;
    char buf[256];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof buf)) > 0)
        secret.append(buf, int(n));
    cleanup();

    if (secret.isEmpty())
        return fail(QStringLiteral("Secret portal returned an empty secret"));
    return secret;
}

std::optional<QByteArray> deriveKey(const QByteArray &purpose, QString *error)
{
    const auto secret = retrieve(error);
    if (!secret)
        return std::nullopt;
    return QMessageAuthenticationCode::hash(purpose, *secret, QCryptographicHash::Sha256);
}

} // namespace PortalSecret

#include "portalsecret.moc"

#include "AppIcon.h"
#include "LegacyMigration.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>
#include <cerrno>
#include <cstdio>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

// Brings the running window to the front (also out of the tray). A modal dialog that is open,
// e.g. the passphrase prompt, gets the focus instead of the blocked main window.
static void bringToFront(MainWindow &w)
{
    if (QWidget *modal = QApplication::activeModalWidget()) {
        modal->raise();
        modal->activateWindow();
        return;
    }
    w.showNormal();
    w.raise();
    w.activateWindow();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("corvus"));
    QApplication::setApplicationName(QStringLiteral("corvus"));

    // Data written under the former name "signal-cli-qt" (history, settings) is moved over once.
    QString migrationError;
    if (!LegacyMigration::migrateDataAndSettings(migrationError)) {
        QMessageBox::critical(nullptr, QObject::tr("Corvus"), migrationError);
        return 1;
    }

    // Registering an application ID with the desktop portal only works when a matching
    // .desktop file is installed. Without one (e.g. running from the build directory) it
    // just produces "Could not register app ID" warnings, so only announce it if it exists.
    // In a Flatpak the desktop file (and the Wayland app ID) is named after the Flatpak app ID.
    const QString desktopId = qEnvironmentVariableIsEmpty("FLATPAK_ID")
        ? QStringLiteral("corvus")
        : qEnvironmentVariable("FLATPAK_ID");
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopId + QStringLiteral(".desktop")).isEmpty())
        QGuiApplication::setDesktopFileName(desktopId);

    QApplication::setWindowIcon(AppIcon::app());

    // Only one instance may use the chat history: two processes appending to history.corvus (or
    // compacting it) at the same time would corrupt it. The lock lives next to the file, so
    // different data directories (XDG_DATA_HOME) can run side by side.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    const QString lockPath = dataDir + QStringLiteral("/history.lock");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0); // never "stale" by age (home on a network share); a dead owner is still detected
    const QString serverName = QStringLiteral("corvus-")
        + QString::fromLatin1(QCryptographicHash::hash(lockPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));

    // QLockFile detects a stale lock by checking whether a process with the recorded
    // PID still exists and runs this application. Inside a Flatpak that check is
    // fooled: every session runs in its own PID namespace whose numbering restarts
    // at low PIDs, so the PID left over in a lock file of a crashed session (usually
    // 2) exists again in the next session — often as Corvus itself, under the same
    // name. The leftover lock then never looks stale and Corvus never starts again.
    //
    // In a Flatpak an flock() therefore decides first whether an instance is really
    // alive: the kernel holds that lock until the owner dies (even on SIGKILL), and
    // the data directory is shared between the sessions, so the check is independent
    // of PID namespaces. With the kernel lock held, a contradicting QLockFile is
    // provably a leftover of this machine and gets removed. Outside Flatpak nothing
    // changes: PIDs are global there and QLockFile's own stale detection works.
#if defined(Q_OS_UNIX)
    const bool inFlatpak = !qEnvironmentVariableIsEmpty("FLATPAK_ID");
    bool kernelLockHeld = false;  // flock() is ours: no instance runs right now
    bool instanceRunning = false; // flock() was refused: an instance is alive
    if (inFlatpak) {
        const int fd = ::open(QFile::encodeName(lockPath + QStringLiteral(".flock")).constData(),
                              O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (fd >= 0) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                kernelLockHeld = true; // keep the fd open: closing would release it
            } else {
                instanceRunning = errno == EWOULDBLOCK;
                ::close(fd);
            }
        }
    }
#else
    constexpr bool kernelLockHeld = false;
    constexpr bool instanceRunning = false;
#endif

    if (!instanceRunning) {
        if (!lock.tryLock(0) && lock.error() == QLockFile::LockFailedError && kernelLockHeld) {
            // The kernel lock says no instance is alive, so the remaining QLockFile
            // is either unreadable or claims an owner on this machine — impossible
            // now, as explained above. Remove it and try once more. A lock that
            // names another machine (home on a network share) is left alone.
            qint64 pid = 0;
            QString host, appName;
            if (!lock.getLockInfo(&pid, &host, &appName)
                || host == QSysInfo::machineHostName()) {
                lock.removeStaleLockFile();
                lock.tryLock(0);
            }
        }
    }

    if (instanceRunning || !lock.isLocked()) {
        if (instanceRunning || lock.error() == QLockFile::LockFailedError) {
            // Another instance owns the history: ask it to show itself, then leave.
            QLocalSocket socket;
            socket.connectToServer(serverName);
            if (socket.waitForConnected(2000)) {
                socket.disconnectFromServer();
                std::fprintf(stderr, "Corvus is already running, bringing it to the front.\n");
                return 0;
            }
            qint64 pid = 0;
            QString host, appName;
            lock.getLockInfo(&pid, &host, &appName);
            QMessageBox::warning(nullptr, QObject::tr("Corvus"),
                                 QObject::tr("Corvus is already running (process %1), but it does not respond.\n\n"
                                             "Close the other instance first. Two instances would damage the chat history.")
                                     .arg(pid));
        } else {
            QMessageBox::critical(nullptr, QObject::tr("Corvus"),
                                  QObject::tr("Cannot lock the chat history (%1).\nCheck the permissions of the folder:\n%2")
                                      .arg(lockPath, dataDir));
        }
        return 1;
    }

    MainWindow w;

    // Later start attempts connect here; that is the signal to show the window.
    QLocalServer::removeServer(serverName); // leftover socket of a crashed run
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(&server, &QLocalServer::newConnection, &w, [&server, &w] {
        while (QLocalSocket *client = server.nextPendingConnection()) {
            client->disconnectFromServer();
            client->deleteLater();
        }
        bringToFront(w);
    });
    if (!server.listen(serverName))
        std::fprintf(stderr, "Corvus: cannot listen for further start attempts: %s\n",
                     server.errorString().toLocal8Bit().constData());

    w.showAtStartup();
    return app.exec();
}

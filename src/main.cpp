#include "AppIcon.h"
#include "LegacyMigration.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStringList>
#include <cstdio>

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
    QApplication::setOrganizationName(QStringLiteral("wren"));
    QApplication::setApplicationName(QStringLiteral("wren"));

    // Data written under the former name "signal-cli-qt" (history, settings) is moved over once.
    QString migrationError;
    if (!LegacyMigration::migrateDataAndSettings(migrationError)) {
        QMessageBox::critical(nullptr, QObject::tr("Wren"), migrationError);
        return 1;
    }

    // Registering an application ID with the desktop portal only works when a matching
    // .desktop file is installed. Without one (e.g. running from the build directory) it
    // just produces "Could not register app ID" warnings, so only announce it if it exists.
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QStringLiteral("wren.desktop")).isEmpty())
        QGuiApplication::setDesktopFileName(QStringLiteral("wren"));

    QApplication::setWindowIcon(AppIcon::app());

    // Only one instance may use the chat history: two processes appending to history.wren (or
    // compacting it) at the same time would corrupt it. The lock lives next to the file, so
    // different data directories (XDG_DATA_HOME) can run side by side.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    const QString lockPath = dataDir + QStringLiteral("/history.lock");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0); // never "stale" by age (home on a network share); a dead owner is still detected
    const QString serverName = QStringLiteral("wren-")
        + QString::fromLatin1(QCryptographicHash::hash(lockPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));

    if (!lock.tryLock(0)) {
        if (lock.error() == QLockFile::LockFailedError) {
            // Another instance owns the history: ask it to show itself, then leave.
            QLocalSocket socket;
            socket.connectToServer(serverName);
            if (socket.waitForConnected(2000)) {
                socket.disconnectFromServer();
                std::fprintf(stderr, "Wren is already running, bringing it to the front.\n");
                return 0;
            }
            qint64 pid = 0;
            QString host, appName;
            lock.getLockInfo(&pid, &host, &appName);
            QMessageBox::warning(nullptr, QObject::tr("Wren"),
                                 QObject::tr("Wren is already running (process %1), but it does not respond.\n\n"
                                             "Close the other instance first. Two instances would damage the chat history.")
                                     .arg(pid));
        } else {
            QMessageBox::critical(nullptr, QObject::tr("Wren"),
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
        std::fprintf(stderr, "Wren: cannot listen for further start attempts: %s\n",
                     server.errorString().toLocal8Bit().constData());

    w.showAtStartup();
    return app.exec();
}

#include "AppIcon.h"
#include "MainWindow.h"

#include <QApplication>
#include <QStandardPaths>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("signal-cli-qt"));
    QApplication::setApplicationName(QStringLiteral("signal-cli-qt"));

    // Registering an application ID with the desktop portal only works when a matching
    // .desktop file is installed. Without one (e.g. running from the build directory) it
    // just produces "Could not register app ID" warnings, so only announce it if it exists.
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QStringLiteral("signal-cli-qt.desktop")).isEmpty())
        QGuiApplication::setDesktopFileName(QStringLiteral("signal-cli-qt"));

    QApplication::setWindowIcon(AppIcon::app());

    MainWindow w;
    w.showAtStartup();
    return app.exec();
}

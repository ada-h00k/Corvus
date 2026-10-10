#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("signal-cli-qt"));
    QApplication::setApplicationName(QStringLiteral("signal-cli-qt"));
    QGuiApplication::setDesktopFileName(QStringLiteral("signal-cli-qt"));

    MainWindow w;
    w.show();
    return app.exec();
}

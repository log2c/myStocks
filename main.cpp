#include <QApplication>
#include <QDebug>

#include "app_controller.h"
#include "app_logging.h"
#include "config_manager.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("myStocks");
    QApplication::setApplicationName("myStocks");
    app.setQuitOnLastWindowClosed(false);

    const AppConfig cfg = ConfigManager::loadConfig();
    app_logging::initFileLogger(cfg);
    qInfo() << "Application startup.";

    AppController controller;
    const int exitCode = app.exec();
    qInfo() << "Application exit with code" << exitCode;
    return exitCode;
}

#include <QApplication>
#include <QDebug>
#include <QSharedMemory>

#include "app_controller.h"
#include "app_logging.h"
#include "config_manager.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("myStocks");
    QApplication::setApplicationName("myStocks");
    app.setQuitOnLastWindowClosed(false);

    const QString instanceKey = QString("%1.%2.singleton")
        .arg(QApplication::organizationName())
        .arg(QApplication::applicationName());
    QSharedMemory singleInstanceGuard(instanceKey);

    bool lockAcquired = singleInstanceGuard.create(1);
    if (!lockAcquired && singleInstanceGuard.error() == QSharedMemory::AlreadyExists) {
        // Try to recover once from stale shared-memory segments.
        if (singleInstanceGuard.attach()) {
            singleInstanceGuard.detach();
        }
        lockAcquired = singleInstanceGuard.create(1);
    }

    if (!lockAcquired) {
        if (singleInstanceGuard.error() == QSharedMemory::AlreadyExists) {
            qInfo() << "Another MyStocks instance is already running. Exit.";
            return 0;
        }

        qWarning() << "Failed to acquire single-instance lock:" << singleInstanceGuard.errorString();
        return 1;
    }

    const AppConfig cfg = ConfigManager::loadConfig();
    app_logging::initFileLogger(cfg);
    qInfo() << "Application startup.";

    AppController controller;
    const int exitCode = app.exec();
    qInfo() << "Application exit with code" << exitCode;
    return exitCode;
}

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QSharedMemory>

#include "app_constants.h"
#include "app_controller.h"
#include "app_logging.h"
#include "config_manager.h"
#include "crash_logging.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const QIcon appIcon(QStringLiteral(":/icon.png"));
    if (!appIcon.isNull()) {
        app.setWindowIcon(appIcon);
    }

    QApplication::setOrganizationName(app_constants::kOrganizationName);
    QApplication::setApplicationName(app_constants::kApplicationName);
    app.setQuitOnLastWindowClosed(false);
    crash_logging::installCrashHandlers();

    const QString instanceKey = QString("%1.%2.singleton")
        .arg(QApplication::organizationName())
        .arg(QApplication::applicationName());
    QSharedMemory singleInstanceGuard(instanceKey);

    qInfo() << "Process bootstrap"
            << "pid=" << QCoreApplication::applicationPid()
            << "time=" << QDateTime::currentDateTime().toString(Qt::ISODate)
            << "cwd=" << QDir::currentPath()
            << "args=" << QCoreApplication::arguments();

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

    qInfo() << "Single-instance lock acquired with key" << instanceKey;

    const AppConfig cfg = ConfigManager::loadConfig();
    app_logging::initFileLogger(cfg);
    qInfo() << "Application startup"
            << "apiSource=" << cfg.apiSource
            << "pollMs=" << cfg.pollMs
            << "language=" << cfg.language
            << "logEnabled=" << cfg.logEnabled
            << "logLevel=" << cfg.logLevel;

    AppController controller;
    const int exitCode = app.exec();
    qInfo() << "Application exit with code" << exitCode;
    return exitCode;
}

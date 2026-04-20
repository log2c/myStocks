#include <QApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>

#include "app_controller.h"

#ifndef DEBUG_MODE

namespace {

static QFile   g_logFile;
static QMutex  g_logMutex;

static void fileMessageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    const char* level = "INFO";
    switch (type) {
    case QtDebugMsg:    level = "DEBUG"; break;
    case QtInfoMsg:     level = "INFO";  break;
    case QtWarningMsg:  level = "WARN";  break;
    case QtCriticalMsg: level = "ERROR"; break;
    case QtFatalMsg:    level = "FATAL"; break;
    }

    const QByteArray line = QString("[%1][%2] %3\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(QLatin1String(level))
        .arg(msg)
        .toUtf8();

    QMutexLocker locker(&g_logMutex);
    if (g_logFile.isOpen()) {
        g_logFile.write(line);
        g_logFile.flush();
    }

    if (type == QtFatalMsg) {
        abort();
    }
}

void initFileLogger() {
#if defined(Q_OS_WIN)
    const QString logDir = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("myStocks/logs");
#elif defined(Q_OS_MACOS)
    const QString logDir = QDir::homePath() + "/Library/Logs/myStocks";
#else
    const QString logDir = QDir::homePath() + "/.local/share/myStocks/logs";
#endif

    if (!QDir().mkpath(logDir)) {
        return;
    }

    const QString logPath = QDir(logDir).filePath(
        "myStocks_" + QDate::currentDate().toString("yyyyMMdd") + ".log"
    );

    g_logFile.setFileName(logPath);
    if (g_logFile.open(QIODevice::Append | QIODevice::Text)) {
        qInstallMessageHandler(fileMessageHandler);
    }
}

} // namespace

#endif // !DEBUG_MODE

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("myStocks");
    QApplication::setApplicationName("myStocks");
    app.setQuitOnLastWindowClosed(false);

#ifndef DEBUG_MODE
    initFileLogger();
#endif

    AppController controller;
    return app.exec();
}

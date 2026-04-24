#include "app_logging.h"
#include "crash_logging.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include <atomic>
#include <cstdlib>

namespace {

QFile g_logFile;
QMutex g_logMutex;
std::atomic<int> g_minSeverity {1};
std::atomic<bool> g_logEnabled {true};
std::atomic<bool> g_handlerInstalled {false};

int severityForType(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return 0;
    case QtInfoMsg:
        return 1;
    case QtWarningMsg:
        return 2;
    case QtCriticalMsg:
        return 3;
    case QtFatalMsg:
        return 4;
    }
    return 1;
}

const char* levelText(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO";
    case QtWarningMsg:
        return "WARN";
    case QtCriticalMsg:
        return "ERROR";
    case QtFatalMsg:
        return "FATAL";
    }
    return "INFO";
}

int minSeverityForLevel(const QString& level) {
    if (level == "debug") {
        return 0;
    }
    if (level == "warn") {
        return 2;
    }
    if (level == "error") {
        return 3;
    }
    return 1;
}

bool shouldWrite(QtMsgType type) {
    if (type == QtFatalMsg) {
        return true;
    }

    if (!g_logEnabled.load(std::memory_order_relaxed)) {
        return false;
    }

    const int minSeverity = g_minSeverity.load(std::memory_order_relaxed);
    return severityForType(type) >= minSeverity;
}

QString resolvedLogDirectoryPath() {
#if defined(Q_OS_WIN)
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty()) {
        return QDir(localAppData).filePath("myStocks/logs");
    }
#endif

#if defined(Q_OS_MACOS)
    return QDir::homePath() + "/Library/Logs/myStocks";
#else
    return QDir::homePath() + "/.local/share/myStocks/logs";
#endif
}

void fileMessageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    if (!shouldWrite(type)) {
        if (type == QtFatalMsg) {
            crash_logging::appendCurrentStackTrace(QStringLiteral("Qt Fatal Message"), msg);
            std::abort();
        }
        return;
    }

    const QByteArray line = QString("[%1][%2] %3\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(QLatin1String(levelText(type)))
        .arg(msg)
        .toUtf8();

    {
        QMutexLocker locker(&g_logMutex);
        if (g_logFile.isOpen()) {
            g_logFile.write(line);
            g_logFile.flush();
        }
    }

    // Always mirror to stderr so terminal output works during development.
    std::fputs(line.constData(), stderr);

    if (type == QtFatalMsg) {
        crash_logging::appendCurrentStackTrace(QStringLiteral("Qt Fatal Message"), msg);
        std::abort();
    }
}

} // namespace

namespace app_logging {

QString logDirectoryPath() {
    return resolvedLogDirectoryPath();
}

QString normalizeLogLevel(const QString& rawLevel) {
    QString level = rawLevel.trimmed().toLower();

    if (level == "warning") {
        level = "warn";
    }

    if (level == "debug" || level == "info" || level == "warn" || level == "error") {
        return level;
    }

    return "info";
}

void setLogConfig(bool enabled, const QString& level) {
    g_logEnabled.store(enabled, std::memory_order_relaxed);
    g_minSeverity.store(minSeverityForLevel(normalizeLogLevel(level)), std::memory_order_relaxed);
}

void initFileLogger(const AppConfig& cfg) {
    setLogConfig(cfg.logEnabled, cfg.logLevel);

    if (g_handlerInstalled.load(std::memory_order_acquire)) {
        return;
    }

    const QString logDir = logDirectoryPath();
    if (!QDir().mkpath(logDir)) {
        return;
    }

    const QString logPath = QDir(logDir).filePath(
        "myStocks_" + QDate::currentDate().toString("yyyyMMdd") + ".log"
    );

    g_logFile.setFileName(logPath);
    if (!g_logFile.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }

    qInstallMessageHandler(fileMessageHandler);
    g_handlerInstalled.store(true, std::memory_order_release);
}

} // namespace app_logging

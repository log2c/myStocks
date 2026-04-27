#include "crash_logging.h"

#include "app_logging.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

#if defined(Q_OS_WIN)
#include <Windows.h>
#include <DbgHelp.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_installed {false};
std::atomic_flag g_crashInProgress = ATOMIC_FLAG_INIT;
QString g_crashLogPath;
QString g_sessionSummary;
QByteArray g_crashLogPathBytes;
QByteArray g_sessionSummaryBytes;
std::array<char, 8192> g_pendingCrashContext {};
std::atomic<size_t> g_pendingCrashContextLen {0};
std::atomic<bool> g_pendingCrashContextPersisted {false};

QString buildCrashLogPath() {
    const QString logDir = app_logging::logDirectoryPath();
    if (!QDir().mkpath(logDir)) {
        return QString();
    }

    return QDir(logDir).filePath(
        QStringLiteral("myStocks_crash_%1_pid%2.log")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")))
            .arg(QCoreApplication::applicationPid())
    );
}

QString buildSessionSummary() {
    QStringList lines;
    lines << QStringLiteral("session-start=%1")
                 .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    lines << QStringLiteral("app=%1").arg(QCoreApplication::applicationName());
    lines << QStringLiteral("pid=%1").arg(QCoreApplication::applicationPid());
    lines << QStringLiteral("cwd=%1").arg(QDir::currentPath());
#ifdef APP_VERSION_STRING
    lines << QStringLiteral("version=%1").arg(QStringLiteral(APP_VERSION_STRING));
#endif
    return lines.join(QStringLiteral("\n"));
}

void appendTextBlock(const QByteArray& block) {
    if (g_crashLogPath.isEmpty()) {
        return;
    }

    QFile file(g_crashLogPath);
    const bool existed = QFileInfo::exists(g_crashLogPath);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }

    if (!existed) {
        file.write("========== Crash Session ==========\n");
        file.write(g_sessionSummary.toUtf8());
        file.write("\n");
    }

    file.write(block);
    if (!block.endsWith('\n')) {
        file.write("\n");
    }
    file.flush();
}

void appendStructuredEvent(const QString& title, const QStringList& lines) {
    QByteArray block;
    block += "---------- ";
    block += title.toUtf8();
    block += " ----------\n";
    for (const QString& line : lines) {
        block += line.toUtf8();
        block += '\n';
    }
    appendTextBlock(block);
}

QByteArray buildStackTraceBlock(const QStringList& lines) {
    QByteArray block("stacktrace:\n");
    if (lines.isEmpty()) {
        block += "  <unavailable>\n";
        return block;
    }

    for (const QString& line : lines) {
        block += line.toUtf8();
        block += '\n';
    }
    return block;
}

void storePendingCrashContext(const QByteArray& block) {
    const size_t copyLen = (std::min)(
        static_cast<size_t>(block.size()),
        g_pendingCrashContext.size() - 1
    );
    if (copyLen > 0) {
        std::memcpy(g_pendingCrashContext.data(), block.constData(), copyLen);
    }
    g_pendingCrashContext[copyLen] = '\0';
    g_pendingCrashContextLen.store(copyLen, std::memory_order_release);
    g_pendingCrashContextPersisted.store(false, std::memory_order_release);
}

QByteArray buildContextBlock(const QString& title, const QString& details) {
    QByteArray block;
    block += "---------- ";
    block += title.toUtf8();
    block += " ----------\n";
    block += QStringLiteral("timestamp=%1\n")
                 .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
                 .toUtf8();
    if (!details.trimmed().isEmpty()) {
        block += details.toUtf8();
        block += '\n';
    }
    return block;
}

const char* posixSignalName(int signalNumber) {
    switch (signalNumber) {
    case SIGABRT:
        return "SIGABRT";
    case SIGSEGV:
        return "SIGSEGV";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
#if defined(SIGBUS)
    case SIGBUS:
        return "SIGBUS";
#endif
    default:
        return "UNKNOWN";
    }
}

#if defined(Q_OS_WIN)
QString formatExceptionCode(unsigned long code) {
    return QStringLiteral("0x%1").arg(code, 8, 16, QLatin1Char('0'));
}

QStringList captureWindowsStackTrace(unsigned int framesToSkip) {
    QStringList lines;
    void* frames[62] = {};
    const USHORT frameCount = CaptureStackBackTrace(framesToSkip, 62, frames, nullptr);
    if (frameCount == 0) {
        return lines;
    }

    HANDLE process = GetCurrentProcess();
    if (!SymInitialize(process, nullptr, TRUE)) {
        return lines;
    }

    QByteArray symbolBuffer(sizeof(SYMBOL_INFO) + MAX_SYM_NAME, '\0');
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    for (USHORT i = 0; i < frameCount; ++i) {
        DWORD64 displacement = 0;
        QString line = QStringLiteral("  #%1 0x%2")
                           .arg(i)
                           .arg(reinterpret_cast<quintptr>(frames[i]), 0, 16);
        if (SymFromAddr(process, reinterpret_cast<DWORD64>(frames[i]), &displacement, symbol)) {
            line += QStringLiteral(" %1+0x%2")
                        .arg(QString::fromLocal8Bit(symbol->Name))
                        .arg(displacement, 0, 16);
        }
        lines.push_back(line);
    }

    SymCleanup(process);
    return lines;
}

void appendWindowsStackTrace(HANDLE process, void* contextPtr) {
    Q_UNUSED(process);
    Q_UNUSED(contextPtr);
    appendTextBlock(buildStackTraceBlock(captureWindowsStackTrace(0)));
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
    if (g_crashInProgress.test_and_set(std::memory_order_acq_rel)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    QStringList lines;
    lines << QStringLiteral("timestamp=%1")
                 .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    lines << QStringLiteral("thread-id=%1").arg(GetCurrentThreadId());
    if (exceptionInfo && exceptionInfo->ExceptionRecord) {
        const auto* record = exceptionInfo->ExceptionRecord;
        lines << QStringLiteral("exception-code=%1")
                     .arg(formatExceptionCode(record->ExceptionCode));
        lines << QStringLiteral("exception-address=0x%1")
                     .arg(reinterpret_cast<quintptr>(record->ExceptionAddress), 0, 16);
    }

    appendStructuredEvent(QStringLiteral("Unhandled Exception"), lines);
    appendWindowsStackTrace(
        GetCurrentProcess(),
        exceptionInfo ? exceptionInfo->ContextRecord : nullptr
    );
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
QStringList capturePosixStackTrace(int framesToSkip) {
    QStringList lines;
    void* frames[64] = {};
    const int frameCount = ::backtrace(frames, 64);
    if (frameCount <= framesToSkip) {
        return lines;
    }

    char** symbols = ::backtrace_symbols(frames, frameCount);
    if (symbols == nullptr) {
        return lines;
    }

    for (int i = framesToSkip; i < frameCount; ++i) {
        lines.push_back(QStringLiteral("  #%1 %2").arg(i - framesToSkip).arg(symbols[i]));
    }
    std::free(symbols);
    return lines;
}

bool writeAllToFd(int fd, const char* data, size_t len) {
    while (len > 0) {
        const ssize_t written = ::write(fd, data, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += written;
        len -= static_cast<size_t>(written);
    }
    return true;
}

bool ensureSessionHeaderWritten(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return false;
    }

    if (st.st_size != 0) {
        return true;
    }

    static const char kHeader[] = "========== Crash Session ==========\n";
    return writeAllToFd(fd, kHeader, sizeof(kHeader) - 1)
        && writeAllToFd(
            fd,
            g_sessionSummaryBytes.constData(),
            static_cast<size_t>(g_sessionSummaryBytes.size())
        )
        && writeAllToFd(fd, "\n", 1);
}

bool appendPosixBlock(const QByteArray& block) {
    if (g_crashLogPathBytes.isEmpty()) {
        return false;
    }

    const int fd = ::open(g_crashLogPathBytes.constData(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        return false;
    }

    const bool ok = ensureSessionHeaderWritten(fd)
        && writeAllToFd(fd, block.constData(), static_cast<size_t>(block.size()))
        && (block.endsWith('\n') || writeAllToFd(fd, "\n", 1));
    ::close(fd);
    return ok;
}

bool appendCurrentPosixStackTrace(const QString& title, const QString& details, int framesToSkip) {
    if (g_crashLogPathBytes.isEmpty()) {
        return false;
    }

    const int fd = ::open(g_crashLogPathBytes.constData(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        return false;
    }

    if (!ensureSessionHeaderWritten(fd)) {
        ::close(fd);
        return false;
    }

    const QByteArray titleUtf8 = title.toUtf8();
    const QByteArray detailsUtf8 = details.toUtf8();
    const long long epochSeconds = static_cast<long long>(std::time(nullptr));
    char header[512] = {};
    const int headerSize = std::snprintf(
        header,
        sizeof(header),
        "---------- %s ----------\n"
        "unix-time=%lld\n",
        titleUtf8.constData(),
        epochSeconds
    );
    if (headerSize <= 0
        || !writeAllToFd(fd, header, static_cast<size_t>(headerSize))) {
        ::close(fd);
        return false;
    }

    if (!detailsUtf8.isEmpty()) {
        if (!writeAllToFd(fd, detailsUtf8.constData(), static_cast<size_t>(detailsUtf8.size()))
            || !writeAllToFd(fd, "\n", 1)) {
            ::close(fd);
            return false;
        }
    }

    if (!writeAllToFd(fd, "stacktrace:\n", sizeof("stacktrace:\n") - 1)) {
        ::close(fd);
        return false;
    }

    void* frames[64] = {};
    const int frameCount = ::backtrace(frames, 64);
    if (frameCount > framesToSkip) {
        ::backtrace_symbols_fd(frames + framesToSkip, frameCount - framesToSkip, fd);
    } else {
        if (!writeAllToFd(fd, "  <unavailable>\n", sizeof("  <unavailable>\n") - 1)) {
            ::close(fd);
            return false;
        }
    }

    if (!writeAllToFd(fd, "\n", 1)) {
        ::close(fd);
        return false;
    }

    ::close(fd);
    return true;
}

void writeToFd(int fd, const char* data) {
    if (fd < 0 || data == nullptr) {
        return;
    }

    const size_t len = std::strlen(data);
    if (len == 0) {
        return;
    }

    (void)::write(fd, data, len);
}

void posixSignalHandler(int signalNumber, siginfo_t* info, void*) {
    if (g_crashInProgress.test_and_set(std::memory_order_acq_rel)) {
        std::_Exit(128 + signalNumber);
    }

    const int fd = ::open(g_crashLogPathBytes.constData(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
        (void)ensureSessionHeaderWritten(fd);

        const size_t pendingLen = g_pendingCrashContextLen.load(std::memory_order_acquire);
        if (pendingLen > 0
            && !g_pendingCrashContextPersisted.load(std::memory_order_acquire)) {
            (void)writeAllToFd(fd, g_pendingCrashContext.data(), pendingLen);
            if (g_pendingCrashContext[pendingLen - 1] != '\n') {
                (void)writeAllToFd(fd, "\n", 1);
            }
        }

        const long long epochSeconds = static_cast<long long>(std::time(nullptr));
        char buffer[512] = {};
        const int size = std::snprintf(
            buffer,
            sizeof(buffer),
            "---------- Fatal Signal ----------\n"
            "unix-time=%lld\n"
            "signal-name=%s\n"
            "signal=%d\n"
            "code=%d\n"
            "address=%p\n",
            epochSeconds,
            posixSignalName(signalNumber),
            signalNumber,
            info ? info->si_code : 0,
            info ? info->si_addr : nullptr
        );
        if (size > 0) {
            (void)::write(fd, buffer, static_cast<size_t>(size));
        }

        writeToFd(fd, "stacktrace:\n");
        void* frames[64] = {};
        const int frameCount = ::backtrace(frames, 64);
        if (frameCount > 0) {
            ::backtrace_symbols_fd(frames, frameCount, fd);
        } else {
            writeToFd(fd, "  <unavailable>\n");
        }
        writeToFd(fd, "\n");
        ::close(fd);
    }

    std::_Exit(128 + signalNumber);
}

void installPosixSignalHandler(int signalNumber) {
    struct sigaction action {};
    action.sa_sigaction = posixSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    ::sigaction(signalNumber, &action, nullptr);
}
#endif

void terminateHandler() {
    if (g_crashInProgress.test_and_set(std::memory_order_acq_rel)) {
        std::_Exit(EXIT_FAILURE);
    }

    QString reason = QStringLiteral("terminate called without an active exception");
    if (const std::exception_ptr ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            reason = QStringLiteral("std::exception: %1").arg(QString::fromLocal8Bit(e.what()));
        } catch (...) {
            reason = QStringLiteral("non-std exception");
        }
    }

    appendStructuredEvent(
        QStringLiteral("Unhandled Terminate"),
        {
            QStringLiteral("timestamp=%1")
                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)),
            QStringLiteral("reason=%1").arg(reason)
        }
    );
    std::_Exit(EXIT_FAILURE);
}

} // namespace

namespace crash_logging {

void installCrashHandlers() {
    if (g_installed.load(std::memory_order_acquire)) {
        return;
    }

    const QString crashLogPath = buildCrashLogPath();
    if (crashLogPath.isEmpty()) {
        return;
    }

    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    g_crashLogPath = crashLogPath;
    g_sessionSummary = buildSessionSummary();
    g_crashLogPathBytes = g_crashLogPath.toLocal8Bit();
    g_sessionSummaryBytes = g_sessionSummary.toUtf8();

    std::set_terminate(terminateHandler);

#if defined(Q_OS_WIN)
    ::SetUnhandledExceptionFilter(unhandledExceptionFilter);
#else
    installPosixSignalHandler(SIGSEGV);
    installPosixSignalHandler(SIGABRT);
    installPosixSignalHandler(SIGILL);
    installPosixSignalHandler(SIGFPE);
#if defined(SIGBUS)
    installPosixSignalHandler(SIGBUS);
#endif
#endif

    qInfo() << "Crash handlers installed. Crash log path:" << g_crashLogPath;
}

void appendCrashContext(const QString& title, const QString& details) {
    if (g_crashLogPath.isEmpty()) {
        return;
    }

    const QByteArray block = buildContextBlock(title, details);
#if defined(Q_OS_WIN)
    appendTextBlock(block);
#else
    if (!appendPosixBlock(block)) {
        appendTextBlock(block);
    }
#endif
}

void appendCurrentStackTrace(const QString& title, const QString& details) {
    if (g_crashLogPath.isEmpty()) {
        return;
    }

#if !defined(Q_OS_WIN)
    if (appendCurrentPosixStackTrace(title, details, 2)) {
        g_pendingCrashContextLen.store(0, std::memory_order_release);
        g_pendingCrashContextPersisted.store(true, std::memory_order_release);
        return;
    }
#endif

    const QByteArray block =
        buildContextBlock(title, details)
        + buildStackTraceBlock(
#if defined(Q_OS_WIN)
              captureWindowsStackTrace(1)
#else
              capturePosixStackTrace(1)
#endif
          );
    storePendingCrashContext(block);
#if defined(Q_OS_WIN)
    appendTextBlock(block);
    g_pendingCrashContextPersisted.store(true, std::memory_order_release);
#else
    const bool persisted = appendPosixBlock(block);
    g_pendingCrashContextPersisted.store(persisted, std::memory_order_release);
    if (!persisted) {
        appendTextBlock(block);
    }
#endif
}

} // namespace crash_logging

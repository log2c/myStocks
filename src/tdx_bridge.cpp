#include "tdx_bridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFutureWatcher>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QtConcurrent>

namespace tdx_bridge {
namespace {
struct Result { QJsonObject object; QString error; };
}

QJsonObject requestBlocking(const QJsonObject& payload, QString* error) {
    // Loading and calls are serialized: gotdx clients share connection state.
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    static QLibrary library;
    using RequestFn = char* (*)(const char*);
    using FreeFn = void (*)(char*);
    static RequestFn invoke = nullptr;
    static FreeFn release = nullptr;
    if (error) error->clear();
    if (!invoke) {
        const QDir appDir(QCoreApplication::applicationDirPath());
#if defined(Q_OS_MACOS)
        const QString path = appDir.absoluteFilePath(QStringLiteral("../Frameworks/libmystocks_gotdx.dylib"));
#elif defined(Q_OS_WIN)
        const QString path = appDir.absoluteFilePath(QStringLiteral("mystocks_gotdx.dll"));
#else
        const QString path = appDir.absoluteFilePath(QStringLiteral("libmystocks_gotdx.so"));
#endif
        library.setFileName(path);
        // A Go runtime cannot safely be unloaded while its goroutines exist.
        library.setLoadHints(QLibrary::PreventUnloadHint);
        if (!library.load()) {
            if (error) *error = QStringLiteral("Cannot load gotdx: %1").arg(library.errorString());
            return {};
        }
        invoke = reinterpret_cast<RequestFn>(library.resolve("MyStocksTdxRequest"));
        release = reinterpret_cast<FreeFn>(library.resolve("MyStocksTdxFree"));
        if (!invoke || !release) {
            invoke = nullptr;
            if (error) *error = QStringLiteral("Incompatible gotdx bridge ABI");
            return {};
        }
    }
    QJsonObject request = payload;
    request.insert(QStringLiteral("calendarPath"), QFileInfo(library.fileName()).dir().filePath(QStringLiteral("gotdx-futures-calendar.db")));
    const QByteArray input = QJsonDocument(request).toJson(QJsonDocument::Compact);
    char* response = invoke(input.constData());
    if (!response) {
        if (error) *error = QStringLiteral("gotdx returned a null response");
        return {};
    }
    const QByteArray bytes(response);
    release(response);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Invalid gotdx response: %1").arg(parseError.errorString());
        return {};
    }
    const QJsonObject object = document.object();
    if (error) *error = object.value(QStringLiteral("error")).toString();
    return object;
}

void request(QObject* context, const AppConfig& config, QJsonObject payload, Callback callback) {
    payload.insert(QStringLiteral("proxy"), QJsonObject{
        {QStringLiteral("type"), config.proxyType},
        {QStringLiteral("host"), config.proxyHost.trimmed()},
        {QStringLiteral("port"), config.proxyPort},
        {QStringLiteral("user"), config.proxyUser},
        {QStringLiteral("password"), config.proxyPassword}
    });
    payload.insert(QStringLiteral("user"), QJsonObject{
        {QStringLiteral("userId"), config.tdxUserId},
        {QStringLiteral("token"), config.tdxToken}
    });
    auto* watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, context,
        [watcher, callback = std::move(callback)]() {
            const Result result = watcher->result();
            watcher->deleteLater();
            callback(result.object, result.error);
        });
    watcher->setFuture(QtConcurrent::run([payload = std::move(payload)]() {
        Result result;
        result.object = requestBlocking(payload, &result.error);
        return result;
    }));
}
}

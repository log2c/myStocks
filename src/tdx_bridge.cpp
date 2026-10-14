#include "tdx_bridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFutureWatcher>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QtConcurrent>

namespace tdx_bridge {
namespace {
struct Result { QJsonObject object; QString error; };
using RequestFn = char* (*)(const char*);
using FreeFn = void (*)(char*);

struct Bridge {
    QLibrary library;
    RequestFn request = nullptr;
    RequestFn networkProxy = nullptr;
    FreeFn release = nullptr;
    QString error;

    Bridge() {
        const QDir appDir(QCoreApplication::applicationDirPath());
#if defined(Q_OS_MACOS)
        const QString path = appDir.absoluteFilePath(QStringLiteral("../Frameworks/libmystocks_gotdx.dylib"));
#elif defined(Q_OS_WIN)
        const QString path = appDir.absoluteFilePath(QStringLiteral("mystocks_gotdx.dll"));
#else
        const QString path = appDir.absoluteFilePath(QStringLiteral("libmystocks_gotdx.so"));
#endif
        library.setFileName(path);
        library.setLoadHints(QLibrary::PreventUnloadHint);
        if (!library.load()) {
            error = QStringLiteral("Cannot load gotdx: %1").arg(library.errorString());
            return;
        }
        request = reinterpret_cast<RequestFn>(library.resolve("MyStocksTdxRequest"));
        networkProxy = reinterpret_cast<RequestFn>(library.resolve("MyStocksNetworkProxy"));
        release = reinterpret_cast<FreeFn>(library.resolve("MyStocksTdxFree"));
        if (!request || !networkProxy || !release) error = QStringLiteral("Incompatible gotdx bridge ABI");
    }
};

Bridge& bridge() {
    // C++ static initialization is thread-safe. Loading is shared; the network
    // relay API has an independent Go lock and never waits for TDX quotation I/O.
    static Bridge instance;
    return instance;
}

QJsonObject invokeJson(RequestFn invoke, const QJsonObject& payload, QString* error) {
    Bridge& runtime = bridge();
    if (error) error->clear();
    if (!runtime.error.isEmpty()) {
        if (error) *error = runtime.error;
        return {};
    }
    const QByteArray input = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    char* response = invoke(input.constData());
    if (!response) {
        if (error) *error = QStringLiteral("gotdx returned a null response");
        return {};
    }
    const QByteArray bytes(response);
    runtime.release(response);
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
} // namespace

QJsonObject proxySettings(const AppConfig& config) {
    return {{QStringLiteral("type"), config.proxyType},
        {QStringLiteral("host"), config.proxyHost.trimmed()},
        {QStringLiteral("port"), config.proxyPort},
        {QStringLiteral("user"), config.proxyUser},
        {QStringLiteral("password"), config.proxyPassword},
        {QStringLiteral("interface"), config.networkInterface.trimmed()}};
}

QJsonObject networkProxyBlocking(const AppConfig& config, QString* error) {
    return invokeJson(bridge().networkProxy, proxySettings(config), error);
}

QJsonObject requestBlocking(const QJsonObject& payload, QString* error) {
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    QJsonObject request = payload;
    request.insert(QStringLiteral("calendarPath"), QFileInfo(bridge().library.fileName()).dir()
        .filePath(QStringLiteral("gotdx-futures-calendar.db")));
    return invokeJson(bridge().request, request, error);
}

void request(QObject* context, const AppConfig& config, QJsonObject payload, Callback callback) {
    payload.insert(QStringLiteral("proxy"), proxySettings(config));
    payload.insert(QStringLiteral("user"), QJsonObject{
        {QStringLiteral("userId"), config.tdxUserId},
        {QStringLiteral("token"), config.tdxToken}
    });
    payload.insert(QStringLiteral("servers"), QJsonObject{
        {QStringLiteral("main"), QJsonObject{
            {QStringLiteral("mode"), config.tdxMainAutoSelect ? QStringLiteral("auto") : QStringLiteral("manual")},
            {QStringLiteral("address"), config.tdxMainServer.trimmed()},
            {QStringLiteral("addresses"), QJsonArray::fromStringList(config.tdxMainServerPool)}
        }},
        {QStringLiteral("ex"), QJsonObject{
            {QStringLiteral("mode"), config.tdxExAutoSelect ? QStringLiteral("auto") : QStringLiteral("manual")},
            {QStringLiteral("address"), config.tdxExServer.trimmed()},
            {QStringLiteral("addresses"), QJsonArray::fromStringList(config.tdxExServerPool)}
        }}
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

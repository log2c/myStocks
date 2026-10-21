#include "updater.h"

#include <QCoreApplication>
#include <QJsonObject>

namespace {
void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
}

// network_utils.cpp references the bridge; this test never creates a relay.
namespace tdx_bridge {
QJsonObject networkProxyBlocking(const AppConfig&, QString* error) {
    if (error) error->clear();
    return {};
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    check(Updater::accelerationPrefix() == "https://gh-proxy.com/",
        "The domestic mirror must be gh-proxy.com");

    const QString release =
        "https://github.com/log2c/myStocks/releases/download/v1.2.3/myStocks-1.2.3-macos.dmg";
    check(Updater::acceleratedUrl(release) == "https://gh-proxy.com/" + release,
        "Accelerated downloads must prefix the origin URL verbatim");
    check(Updater::acceleratedUrl(QString()) == QString(),
        "An empty URL must stay empty instead of becoming a bare mirror request");
    check(Updater::acceleratedUrl("  ") == QString(),
        "A blank URL must not produce a mirror request");
    const QString already = "https://gh-proxy.com/" + release;
    check(Updater::acceleratedUrl(already) == already,
        "An already accelerated URL must not be prefixed twice");
    check(Updater::acceleratedUrl(already.toUpper()).toLower() == already.toLower(),
        "An accelerated URL must be recognised regardless of prefix case");
    return 0;
}

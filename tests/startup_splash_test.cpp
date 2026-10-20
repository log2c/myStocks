#include "startup_splash.h"
#include <QApplication>
#include <QPushButton>
#include <QScreen>
#include <QTimer>

namespace {
void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QRect screen(-1920, 30, 1920, 1000);
    const QSize size(qRound(380 * 2.0 / 3.0), qRound(260 * 2.0 / 3.0));
    const QRect corner = StartupSplash::placement(size, screen);
    check(screen.contains(corner), "Splash must stay inside the selected screen, including negative origins");
    check(corner.bottomRight() == screen.bottomRight() - QPoint(16, 16), "Splash must use the bottom right corner");
    check(QRect(0, 0, 240, 180).contains(StartupSplash::placement(size, QRect(0, 0, 240, 180))),
        "Small-screen placement must clamp oversized splash geometry");

    StartupSplash splash;
    splash.setLanguage("zh_CN");
    splash.setStatus("正在重新加载", "正在检测线路延迟并验证通达信行情响应…");
    splash.setRouteStatus(false, "主行情 · 127.0.0.1:7709 · 18.4 毫秒");
    splash.setRouteStatus(true, "扩展行情 · 正在检测线路…");
    splash.setWarning("主行情：手动线路不可用，已自动切换到可用节点。");
    splash.showFor(nullptr);
    check(splash.isVisible(), "Splash must be visible during route probing");
    check(splash.width() == qRound(380 * 2.0 / 3.0), "Splash width must shrink by one third");
    const auto checkCorner = [&]() {
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        check(splash.geometry() == StartupSplash::placement(splash.size(), available),
            "Splash must remain anchored at the primary screen bottom right");
    };
    checkCorner();
    QWidget floating;
    floating.setGeometry(100, 100, 440, 350);
    floating.show();
    splash.showFor(&floating);
    checkCorner();
    floating.move(280, 180);
    floating.resize(550, 400);
    QApplication::processEvents();
    checkCorner();
    floating.hide();
    QApplication::processEvents();
    checkCorner();
    check(splash.windowFlags().testFlag(Qt::WindowStaysOnTopHint), "Splash must overlay an always-on-top floating window");
    bool retried = false, offline = false;
    QObject::connect(&splash, &StartupSplash::retryRequested, [&]() { retried = true; });
    QObject::connect(&splash, &StartupSplash::offlineRequested, [&]() { offline = true; });
    splash.setFailed("没有可用的通达信线路");
    checkCorner();
    const auto buttons = splash.findChildren<QPushButton*>();
    check(buttons.size() == 2, "Failure must offer retry and offline loading");
    for (auto* button : buttons) {
        check(button->isVisible(), "Failure actions must be visible");
        button->click();
    }
    check(retried && offline, "Failure actions must dispatch both recovery paths");
    splash.setRouteStatus(true, "扩展行情 · 127.0.0.1:7727 · 24.6 毫秒");
    splash.setStatus("加载完成", "配置已就绪，开始加载行情。", 100);
    checkCorner();
    for (auto* button : buttons) check(!button->isVisible(), "Failure actions must disappear after retry success");
    if (app.arguments().contains("--capture")) {
        QTimer::singleShot(100, [&]() {
            splash.grab().save("/tmp/mystocks-splash.png");
            app.quit();
        });
        return app.exec();
    }
    splash.hide();
    return 0;
}

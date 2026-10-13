#include "timeline_popup.h"

#include "i18n.h"
#include "network_logger.h"
#include "network_utils.h"
#include "watchlist_utils.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

constexpr int kPopupWidth = 560;
constexpr int kPopupHeight = 360;
constexpr int kScreenMargin = 12;

void enforcePopupWindowLevel(QWidget* widget) {
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    if (hwnd) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    }
#elif defined(Q_OS_MACOS)
    void* nsView = reinterpret_cast<void*>(widget->winId());
    if (nsView) {
        const auto sendObject = reinterpret_cast<void* (*)(void*, SEL)>(objc_msgSend);
        void* nsWindow = sendObject(nsView, sel_registerName("window"));
        if (nsWindow) {
            const auto sendInteger = reinterpret_cast<void (*)(void*, SEL, long)>(objc_msgSend);
            sendInteger(nsWindow, sel_registerName("setLevel:"),
                static_cast<long>(CGWindowLevelForKey(kCGPopUpMenuWindowLevelKey)));
        }
    }
#endif
    widget->raise();
}

QString signedPercentage(double value) {
    return (value > 0.0 ? QStringLiteral("+") : QString())
        + QString::number(value, 'f', 2) + QLatin1Char('%');
}

class TimelineImageWidget : public QWidget {
public:
    explicit TimelineImageWidget(QWidget* parent) : QWidget(parent) {
        setMinimumSize(280, 180);
    }

    void applyConfig(const AppConfig& cfg) {
        m_cfg = cfg;
        update();
    }

    void setStock(const QString& code, const QString& name, double cost, double price) {
        if (m_code.compare(code, Qt::CaseInsensitive) != 0) {
            m_image = {};
            m_status.clear();
        }
        m_code = code;
        m_name = name;
        setQuote(cost, price);
    }

    void setQuote(double cost, double price) {
        m_cost = cost;
        m_price = price;
        update();
    }

    void setImage(const QPixmap& image) {
        m_image = image;
        m_status.clear();
        update();
    }

    void setStatus(const QString& status) {
        // Keep the last successfully downloaded image during a refresh or a
        // transient network failure. A different stock clears it in setStock.
        if (m_image.isNull()) {
            m_status = status;
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), m_cfg.timelineChartBgColor);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        const QString language = i18n::resolveLanguage(m_cfg.language);
        const bool hasCost = std::isfinite(m_cost) && m_cost > 0.0;
        const bool hasPrice = std::isfinite(m_price) && m_price > 0.0;
        const bool belowCost = hasCost && hasPrice && m_price < m_cost;
        const int lineHeight = qMax(22, painter.fontMetrics().height() + 4);
        const int headerHeight = lineHeight * (belowCost ? 2 : 1) + 8;
        const QRect header(8, 4, width() - 16, lineHeight);
        const QRect imageRect(0, headerHeight, width(), qMax(0, height() - headerHeight));

        if (!m_image.isNull()) {
            // Use the whole body without cropping the chart's axes or prices.
            painter.drawPixmap(imageRect, m_image, m_image.rect());
        } else {
            painter.setPen(m_cfg.timelineChartTextColor);
            painter.drawText(imageRect.adjusted(8, 8, -8, -8),
                Qt::AlignCenter | Qt::TextWordWrap, m_status);
        }

        int titleWidth = header.width();
        if (hasCost) {
            const double profit = hasPrice ? (m_price - m_cost) / m_cost * 100.0 : qQNaN();
            const QString profitText = i18n::t("popup.timeline.profit", language)
                + QStringLiteral(": ")
                + (hasPrice ? signedPercentage(profit) : QStringLiteral("--"));
            QFont metricFont = painter.font();
            metricFont.setBold(true);
            painter.setFont(metricFont);
            titleWidth = qMax(0, header.width() - QFontMetrics(metricFont).horizontalAdvance(profitText) - 12);
            painter.setPen(hasPrice && profit != 0.0
                ? (profit > 0.0 ? m_cfg.timelineChartUpColor : m_cfg.timelineChartDownColor)
                : m_cfg.timelineChartTextColor);
            painter.drawText(header, Qt::AlignRight | Qt::AlignVCenter, profitText);
            if (belowCost) {
                const QString breakevenText = i18n::t("popup.timeline.breakevenGain", language)
                    + QStringLiteral(": ") + signedPercentage((m_cost / m_price - 1.0) * 100.0);
                painter.setPen(m_cfg.timelineChartDownColor);
                painter.drawText(header.translated(0, lineHeight),
                    Qt::AlignRight | Qt::AlignVCenter, breakevenText);
            }
            metricFont.setBold(false);
            painter.setFont(metricFont);
        }
        painter.setPen(m_cfg.timelineChartTextColor);
        const QString title = m_name.isEmpty() ? m_code : QStringLiteral("%1  %2").arg(m_name, m_code);
        painter.drawText(QRect(header.topLeft(), QSize(titleWidth, header.height())),
            Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(title, Qt::ElideRight, titleWidth));
    }

private:
    AppConfig m_cfg;
    QString m_code;
    QString m_name;
    QString m_status;
    QPixmap m_image;
    double m_cost = qQNaN();
    double m_price = qQNaN();
};

} // namespace

QUrl timelineImageUrlForCode(const QString& rawCode) {
    QString code = rawCode.trimmed().toLower();
    if (code.endsWith(QStringLiteral(".hk"))) {
        code = QStringLiteral("hk") + code.left(code.size() - 3);
    }
    if (code.startsWith(QStringLiteral("bj"))) {
        const QString digits = code.mid(2);
        return digits.size() == 6 && watchlist_utils::isDigitsOnly(digits)
            ? QUrl(QStringLiteral("https://image.sinajs.cn/newchart/min/n/%1.gif").arg(digits))
            : QUrl();
    }
    const QString normalized = watchlist_utils::normalizeApiWatchCode(code);
    const QStringList parts = normalized.split(QLatin1Char('.'));
    if (parts.size() != 2 || !watchlist_utils::isDigitsOnly(parts.at(1))) {
        return {};
    }
    const QString market = parts.at(0);
    const QString digits = parts.at(1);
    if ((market == QLatin1String("116") || market == QLatin1String("128")) && digits.size() == 5) {
        return QUrl(QStringLiteral("https://image.sinajs.cn/newchart/png/min/hk/%1.png").arg(digits));
    }
    if ((market != QLatin1String("0") && market != QLatin1String("1")) || digits.size() != 6) {
        return {};
    }
    const bool beijing = market == QLatin1String("0")
        && (digits.startsWith(QLatin1Char('8')) || digits.startsWith(QLatin1Char('4'))
            || digits.startsWith(QStringLiteral("920")));
    const QString symbol = beijing ? digits
        : (market == QLatin1String("1") ? QStringLiteral("sh") : QStringLiteral("sz")) + digits;
    return QUrl(QStringLiteral("https://image.sinajs.cn/newchart/min/n/%1.gif").arg(symbol));
}

bool isTimelinePopupSupportedCode(const QString& rawCode) {
    return !timelineImageUrlForCode(rawCode).isEmpty();
}

class SharedTimelineChartPopupPrivate : public QObject {
public:
    explicit SharedTimelineChartPopupPrivate(SharedTimelineChartPopup* popup)
        : QObject(popup), m_popup(popup) {
        auto* layout = new QVBoxLayout(popup);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        m_imageWidget = new TimelineImageWidget(popup);
        layout->addWidget(m_imageWidget);
        m_refreshTimer = new QTimer(this);
        connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
            if (!m_popup->isVisible() || !isTradingTime()) {
                m_refreshTimer->stop();
                return;
            }
            fetchImage();
        });
    }

    ~SharedTimelineChartPopupPrivate() override {
        cancelPendingReply();
    }

    void applyConfig(const AppConfig& cfg) {
        m_cfg = cfg;
        m_nam.setProxy(network_utils::proxyFromConfig(cfg));
        m_imageWidget->applyConfig(cfg);
        updateRefreshTimer();
    }

    void showForStock(const QString& code, const QString& name,
        const QRect& anchorRect, double cost, double price) {
        if (!isTimelinePopupSupportedCode(code)) {
            hidePopup();
            return;
        }
        const bool changed = m_code.compare(code.trimmed(), Qt::CaseInsensitive) != 0;
        if (changed) {
            cancelPendingReply();
        }
        m_code = code.trimmed();
        m_imageWidget->setStock(m_code, name.trimmed(), cost, price);
        m_popup->resize(kPopupWidth, kPopupHeight);
        QRect target(anchorRect.topRight() + QPoint(12, 0), m_popup->size());
        QScreen* screen = QGuiApplication::screenAt(anchorRect.center());
        if (!screen) {
            screen = QGuiApplication::primaryScreen();
        }
        if (screen) {
            const QRect available = screen->availableGeometry().adjusted(
                kScreenMargin, kScreenMargin, -kScreenMargin, -kScreenMargin);
            if (target.right() > available.right()) {
                target.moveLeft(anchorRect.left() - kPopupWidth - 12);
            }
            target.moveLeft(qMax(available.left(), target.left()));
            target.moveTop(qMax(available.top(),
                qMin(target.top(), available.bottom() - target.height() + 1)));
        }
        m_popup->setGeometry(target);
        m_popup->show();
        enforcePopupWindowLevel(m_popup);
        if (changed) {
            fetchImage();
        }
        updateRefreshTimer();
    }

    void updateQuote(const QString& code, double cost, double price) {
        if (m_popup->isVisible() && m_code.compare(code, Qt::CaseInsensitive) == 0) {
            m_imageWidget->setQuote(cost, price);
        }
    }

    void hidePopup() {
        m_refreshTimer->stop();
        cancelPendingReply();
        m_code.clear();
        m_imageWidget->setStock({}, {}, qQNaN(), qQNaN());
        m_popup->hide();
    }

private:
    bool isTradingTime() const {
        if (m_cfg.debugIgnoreTradingTime) {
            return true;
        }
        const auto now = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Shanghai"));
        if (now.date().dayOfWeek() > 5) {
            return false;
        }
        const bool hongKong = watchlist_utils::isHongKongCode(m_code)
            || m_code.endsWith(QStringLiteral(".hk"), Qt::CaseInsensitive);
        const QTime morningEnd = hongKong ? QTime(12, 0) : QTime(11, 30);
        const QTime afternoonEnd = hongKong ? QTime(16, 0) : QTime(15, 0);
        return (now.time() >= QTime(9, 30) && now.time() <= morningEnd)
            || (now.time() >= QTime(13, 0) && now.time() <= afternoonEnd);
    }

    void updateRefreshTimer() {
        if (m_popup->isVisible() && !m_code.isEmpty() && isTradingTime()) {
            const int interval = qBound(10, m_cfg.timelineChartRefreshSecs, 3600) * 1000;
            if (!m_refreshTimer->isActive() || m_refreshTimer->interval() != interval) {
                m_refreshTimer->start(interval);
            }
        } else {
            m_refreshTimer->stop();
        }
    }

    void cancelPendingReply() {
        if (!m_reply) {
            return;
        }
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }

    void fetchImage() {
        if (m_reply) {
            return;
        }
        const QUrl url = timelineImageUrlForCode(m_code);
        if (url.isEmpty()) {
            return;
        }
        m_imageWidget->setStatus(QStringLiteral("Loading..."));
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", network_utils::effectiveUserAgent(m_cfg).toUtf8());
        request.setRawHeader("Referer", "https://finance.sina.com.cn/");
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork);
        request.setTransferTimeout(20000);
        const auto trace = network_logger::logRequestStart(
            QStringLiteral("sina-timeline-image"), QStringLiteral("GET"), request, m_nam.proxy());
        QNetworkReply* reply = m_nam.get(request);
        m_reply = reply;
        connect(reply, &QNetworkReply::finished, this, [this, reply, trace]() {
            if (m_reply != reply) {
                reply->deleteLater();
                return;
            }
            m_reply = nullptr;
            const QByteArray body = reply->readAll();
            network_logger::logRequestFinish(trace, reply, body.size(), {});
            const QString error = reply->error() == QNetworkReply::NoError
                ? QString() : reply->errorString();
            reply->deleteLater();
            if (!error.isEmpty()) {
                m_imageWidget->setStatus(QStringLiteral("Request failed: %1").arg(error));
                return;
            }
            QPixmap image;
            if (body.isEmpty() || !image.loadFromData(body) || image.isNull()) {
                qWarning() << "Timeline image decode failed" << trace.url
                    << "supported formats=" << QImageReader::supportedImageFormats();
                m_imageWidget->setStatus(QStringLiteral("Unable to load timeline image"));
                return;
            }
            m_imageWidget->setImage(image);
        });
    }

    SharedTimelineChartPopup* m_popup = nullptr;
    TimelineImageWidget* m_imageWidget = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QNetworkAccessManager m_nam;
    QNetworkReply* m_reply = nullptr;
    AppConfig m_cfg;
    QString m_code;
};

SharedTimelineChartPopup::SharedTimelineChartPopup(QWidget* parent)
    : QWidget(nullptr), d(new SharedTimelineChartPopupPrivate(this)) {
    Q_UNUSED(parent);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
}

SharedTimelineChartPopup::~SharedTimelineChartPopup() {
    delete d;
}

void SharedTimelineChartPopup::applyConfig(const AppConfig& cfg) {
    d->applyConfig(cfg);
}

void SharedTimelineChartPopup::showForStock(const QString& code, const QString& name,
    const QRect& anchorRect, int baseWidth, double cost, double price) {
    Q_UNUSED(baseWidth);
    d->showForStock(code, name, anchorRect, cost, price);
}

void SharedTimelineChartPopup::updateQuoteForStock(const QString& code, double cost, double price) {
    d->updateQuote(code, cost, price);
}

void SharedTimelineChartPopup::hidePopup() {
    d->hidePopup();
}

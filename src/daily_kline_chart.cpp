#include "daily_kline_chart.h"
#include "tdx_bridge.h"
#include "i18n.h"
#include "daily_kline_indicators.h"
#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
// Qt swaps Command and Control on macOS unless explicitly disabled. This
// interaction uses the physical Ctrl key on every platform.
Qt::KeyboardModifier controlModifier() {
#if defined(Q_OS_MACOS)
    if (!QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta)) return Qt::MetaModifier;
#endif
    return Qt::ControlModifier;
}
int controlKey() {
    return controlModifier() == Qt::MetaModifier ? Qt::Key_Meta : Qt::Key_Control;
}

QString periodName(KlinePeriod period, const QString& language) {
    switch (period) {
    case KlinePeriod::Weekly: return i18n::t("popup.kline.weekly", language);
    case KlinePeriod::Monthly: return i18n::t("popup.kline.monthly", language);
    case KlinePeriod::Daily:
    case KlinePeriod::None:
    default: return i18n::t("popup.kline.daily", language);
    }
}
} // namespace

DailyKlineChart::DailyKlineChart(QWidget* timeline)
    : QWidget(timeline), m_timeline(timeline) {
    setObjectName(QStringLiteral("dailyKlineChart"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    hide();
    timeline->installEventFilter(this);
    qApp->installEventFilter(this);
    m_modifierTimer.setInterval(50);
    setGeometry(timeline->rect());
    connect(&m_modifierTimer, &QTimer::timeout, this, [this]() {
        setControlHeld(QGuiApplication::queryKeyboardModifiers().testFlag(controlModifier()));
        if (m_controlHeld) fetch();
    });
    if (timeline->isVisible()) m_modifierTimer.start();
}

void DailyKlineChart::setContext(const AppConfig& cfg, const QString& code, const QString& name) {
    const bool changed = m_code != code || m_cfg.dailyKlineDays != qBound(1, cfg.dailyKlineDays, 800)
        || m_cfg.dailyKlineMAPeriods != normalizeDailyKlineMAPeriods(cfg.dailyKlineMAPeriods)
        || m_cfg.networkInterface != cfg.networkInterface || m_cfg.proxyType != cfg.proxyType
        || m_cfg.proxyHost != cfg.proxyHost || m_cfg.proxyPort != cfg.proxyPort
        || m_cfg.proxyUser != cfg.proxyUser || m_cfg.proxyPassword != cfg.proxyPassword
        || m_cfg.tdxUserId != cfg.tdxUserId || m_cfg.tdxToken != cfg.tdxToken
        || m_cfg.tdxMainAutoSelect != cfg.tdxMainAutoSelect || m_cfg.tdxExAutoSelect != cfg.tdxExAutoSelect
        || m_cfg.tdxMainServer != cfg.tdxMainServer || m_cfg.tdxExServer != cfg.tdxExServer
        || m_cfg.tdxMainServerPool != cfg.tdxMainServerPool || m_cfg.tdxExServerPool != cfg.tdxExServerPool;
    m_cfg = cfg;
    m_cfg.dailyKlineDays = qBound(1, cfg.dailyKlineDays, 800);
    m_cfg.dailyKlineMAPeriods = normalizeDailyKlineMAPeriods(cfg.dailyKlineMAPeriods);
    m_code = code;
    m_name = name;
    if (!changed) { update(); return; }
    resetSeries();
    if (m_controlHeld && m_timeline->isVisible()) fetch();
    update();
}

bool DailyKlineChart::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_timeline) {
        if (event->type() == QEvent::Show) {
            setGeometry(m_timeline->rect());
            m_modifierTimer.start();
            setControlHeld(QGuiApplication::queryKeyboardModifiers().testFlag(controlModifier()));
        } else if (event->type() == QEvent::Hide) {
            m_modifierTimer.stop();
            setControlHeld(false);
        } else if (event->type() == QEvent::Resize) {
            setGeometry(m_timeline->rect());
        } else if (event->type() == QEvent::MouseMove && isVisible()) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint pos = mouse->position().toPoint();
            int index = -1;
            if (m_priceRect.contains(pos) && m_slot > 0.0 && m_visibleCount > 0) {
                const int offset = static_cast<int>((pos.x() - m_priceRect.left()) / m_slot);
                index = m_visibleStart + qBound(0, offset, m_visibleCount - 1);
            }
            if (index != m_hoverIndex || pos != m_hoverPos) {
                m_hoverIndex = index;
                m_hoverPos = pos;
                setProperty("hoverIndex", index);
                update();
            }
        } else if (event->type() == QEvent::Leave && m_hoverIndex >= 0) {
            m_hoverIndex = -1;
            setProperty("hoverIndex", -1);
            setProperty("hoverOpen", QVariant());
            setProperty("hoverHigh", QVariant());
            setProperty("hoverLow", QVariant());
            setProperty("hoverClose", QVariant());
            update();
        }
    }
    if (m_timeline->isVisible()
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (!key->isAutoRepeat()) {
            setControlHeld(key->key() == controlKey()
                ? event->type() == QEvent::KeyPress
                : key->modifiers().testFlag(controlModifier()));
        }
    }
    return false;
}

void DailyKlineChart::setPinnedDaily(bool pinned) {
    setPinnedPeriod(pinned ? KlinePeriod::Daily : KlinePeriod::None);
}

void DailyKlineChart::setPinnedPeriod(KlinePeriod period) {
    const int previousPeriod = activePeriod();
    m_pinnedPeriod = period;
    if (previousPeriod != activePeriod()) {
        resetSeries();
    }
    setControlHeld(QGuiApplication::queryKeyboardModifiers().testFlag(controlModifier()));
}

int DailyKlineChart::activePeriod() const {
    return m_pinnedPeriod == KlinePeriod::None
        ? static_cast<int>(KlinePeriod::Daily)
        : static_cast<int>(m_pinnedPeriod);
}

void DailyKlineChart::resetSeries() {
    ++m_generation;
    setProperty("barCount", 0);
    m_bars.clear();
    m_status.clear();
    m_pending = false;
    m_nextFetchMs = 0;
    m_priceRect = QRect();
    m_visibleStart = 0;
    m_visibleCount = 0;
    m_slot = 0.0;
    m_hoverIndex = -1;
    setProperty("hoverIndex", -1);
    setProperty("hoverOpen", QVariant());
    setProperty("hoverHigh", QVariant());
    setProperty("hoverLow", QVariant());
    setProperty("hoverClose", QVariant());
}

void DailyKlineChart::setControlHeld(bool held) {
    held = (held || m_pinnedPeriod != KlinePeriod::None) && m_timeline->isVisible() && !m_code.isEmpty();
    if (m_controlHeld != held) {
        m_timeline->setProperty("dailyKlineVisible", held);
        m_timeline->update();
    }
    m_controlHeld = held;
    setVisible(held);
    if (held) {
        raise();
        fetch();
    } else {
        m_hoverIndex = -1;
        setProperty("hoverIndex", -1);
        setProperty("hoverOpen", QVariant());
        setProperty("hoverHigh", QVariant());
        setProperty("hoverLow", QVariant());
        setProperty("hoverClose", QVariant());
    }
}

void DailyKlineChart::fetch() {
    if (!m_controlHeld || m_pending || m_code.isEmpty()
        || QDateTime::currentMSecsSinceEpoch() < m_nextFetchMs) return;
    m_pending = true;
    const int period = activePeriod();
    const QString language = i18n::resolveLanguage(m_cfg.language);
    const QString periodLabel = periodName(static_cast<KlinePeriod>(period), language);
    m_status = m_bars.isEmpty()
        ? i18n::t("popup.kline.loading", language).arg(periodLabel)
        : QString();
    update();
    const int generation = m_generation;
    tdx_bridge::request(this, m_cfg, {{"operation", "klines"}, {"code", m_code},
        {"name", m_name}, {"period", period}, {"count", daily_kline::historyCount(m_cfg.dailyKlineDays, m_cfg.dailyKlineMAPeriods)}},
        [this, generation, period, language, periodLabel](const QJsonObject& result, const QString& error) {
            if (generation != m_generation || period != activePeriod()) return;
            m_pending = false;
            m_nextFetchMs = QDateTime::currentMSecsSinceEpoch() + 30000;
            if (!error.isEmpty()) {
                m_status = i18n::t("popup.kline.error", language).arg(periodLabel, error);
                update();
                return;
            }
            QVector<Bar> bars;
            for (const auto& value : result.value("bars").toArray()) {
                const auto row = value.toObject();
                Bar bar{row.value("time").toString().left(10),
                    row.value("open").toDouble(qQNaN()), row.value("high").toDouble(qQNaN()),
                    row.value("low").toDouble(qQNaN()), row.value("close").toDouble(qQNaN()),
                    row.value("volume").toDouble(qQNaN())};
                if (!QDate::fromString(bar.date, Qt::ISODate).isValid()
                    || !std::isfinite(bar.open) || !std::isfinite(bar.high)
                    || !std::isfinite(bar.low) || !std::isfinite(bar.close)
                    || bar.low <= 0 || bar.high < std::max(bar.open, bar.close)
                    || bar.low > std::min(bar.open, bar.close)) continue;
                bars.append(bar);
            }
            std::sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) { return a.date < b.date; });
            bars.erase(std::unique(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) {
                return a.date == b.date;
            }), bars.end());
            const int required = daily_kline::historyCount(m_cfg.dailyKlineDays, m_cfg.dailyKlineMAPeriods);
            if (bars.size() > required) bars = bars.mid(bars.size() - required);
            m_bars = bars;
            m_status = bars.isEmpty() ? i18n::t("popup.kline.empty", language).arg(periodLabel) : QString();
            setProperty("barCount", qMin(m_cfg.dailyKlineDays, int(m_bars.size())));
            update();
        });
}

void DailyKlineChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // The timeline parent paints the shared background once. This transparent
    // overlay paints chart content only, preserving the same configured alpha.
    const QRect bounds = rect().adjusted(12, 12, -12, -12);
    m_priceRect = QRect();
    p.setPen(m_cfg.timelineChartTextColor);
    const QString language = i18n::resolveLanguage(m_cfg.language);
    const int visibleCount = qMin(m_cfg.dailyKlineDays, int(m_bars.size()));
    const int visibleStart = m_bars.size() - visibleCount;
    const int titleCount = visibleCount > 0 ? visibleCount : m_cfg.dailyKlineDays;
    const QString periodLabel = periodName(static_cast<KlinePeriod>(activePeriod()), language);
    const QString title = (m_name.isEmpty() ? m_code : m_name + " " + m_code)
        + " · " + i18n::t("popup.kline.title", language)
            .arg(periodLabel).arg(titleCount).arg(m_cfg.dailyKlineDays);
    p.drawText(QRect(bounds.left(), bounds.top(), bounds.width(), 24), Qt::AlignLeft | Qt::AlignVCenter,
        p.fontMetrics().elidedText(title, Qt::ElideRight, bounds.width()));
    if (!m_status.isEmpty() || m_bars.isEmpty()) {
        p.drawText(bounds.adjusted(0, 28, 0, 0), Qt::AlignCenter | Qt::TextWordWrap, m_status);
        return;
    }

    QMap<int, QVector<double>> averages;
    for (int period : m_cfg.dailyKlineMAPeriods) {
        averages.insert(period, daily_kline::movingAverage(m_bars, period));
    }
    const QFont originalFont = p.font();
    QFont legendFont = originalFont;
    legendFont.setPointSizeF(9);
    p.setFont(legendFont);
    int legendX = bounds.left(), legendY = bounds.top() + 26;
    for (auto it = averages.cbegin(); it != averages.cend(); ++it) {
        const double value = it.value().last();
        const QString text = QStringLiteral("MA%1: %2").arg(it.key())
            .arg(std::isfinite(value) ? QString::number(value, 'f', value < 10 ? 3 : 2) : QStringLiteral("--"));
        const int width = p.fontMetrics().horizontalAdvance(text);
        if (legendX > bounds.left() && legendX + width > bounds.right()) {
            legendX = bounds.left();
            legendY += 18;
        }
        p.setPen(daily_kline::movingAverageColor(it.key()));
        p.drawText(QRect(legendX, legendY, width, 18), Qt::AlignVCenter, text);
        legendX += width + 14;
    }
    p.setFont(originalFont);
    const int plotTop = averages.isEmpty() ? 36 : legendY - bounds.top() + 26;
    const QRect area = bounds.adjusted(58, plotTop, -8, -24);
    if (area.width() < 10 || area.height() < 70) return;
    const int volumeHeight = qBound(28, area.height() / 4, 64);
    const QRect volume(area.left(), area.bottom() - volumeHeight + 1, area.width(), volumeHeight);
    const QRect price(area.left(), area.top(), area.width(), area.height() - volumeHeight - 10);

    // Price markers and volume scale use only the visible candles. The earlier
    // history is retained solely to warm up MA calculations at the left edge.
    const auto extrema = daily_kline::visibleExtrema(m_bars, visibleStart);
    double lowest = extrema.low, highest = extrema.high, maxVolume = 0;
    for (int i = visibleStart; i < m_bars.size(); ++i) {
        if (std::isfinite(m_bars[i].volume)) maxVolume = std::max(maxVolume, m_bars[i].volume);
        for (auto it = averages.cbegin(); it != averages.cend(); ++it) {
            if (std::isfinite(it.value()[i])) {
                lowest = std::min(lowest, it.value()[i]);
                highest = std::max(highest, it.value()[i]);
            }
        }
    }
    const double padding = std::max((highest - lowest) * 0.10, highest * 0.002);
    lowest -= padding;
    highest += padding;
    const auto y = [&](double value) { return price.bottom() - (value - lowest) / (highest - lowest) * (price.height() - 1); };
    const double slot = static_cast<double>(price.width()) / visibleCount;
    const auto x = [&](int i) { return price.left() + (i - visibleStart + 0.5) * slot; };
    const double candleWidth = daily_kline::candleWidth(slot);
    m_priceRect = price;
    m_visibleStart = visibleStart;
    m_visibleCount = visibleCount;
    m_slot = slot;
    for (int i = 0; i <= 4; ++i) {
        const int yy = price.top() + (price.height() - 1) * i / 4;
        p.setPen(m_cfg.timelineChartTextColor);
        const double tick = highest - (highest - lowest) * i / 4;
        p.drawText(QRect(bounds.left(), yy - 9, 52, 18), Qt::AlignRight | Qt::AlignVCenter,
            QString::number(tick, 'f', highest < 10 ? 3 : 2));
    }
    p.save();
    p.setClipRect(price);
    for (const auto& gap : daily_kline::unfilledGaps(m_bars, visibleStart)) {
        const double left = x(gap.startIndex) - slot * 0.5;
        const QRectF zone(left, y(gap.upper), price.right() - left, y(gap.lower) - y(gap.upper));
        const QColor fill(160, 160, 160, 60);
        const QColor edge(160, 160, 160, 125);
        p.fillRect(zone, fill);
        const QString label = i18n::t("popup.daily.gap", language);
        const int width = p.fontMetrics().horizontalAdvance(label) + 8;
        if (zone.height() >= p.fontMetrics().height() + 2 && zone.width() >= width + 8) {
            p.setPen(edge);
            p.drawText(zone.adjusted(4, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, label);
        }
    }
    p.setClipRect(area);
    for (int i = visibleStart; i < m_bars.size(); ++i) {
        const auto& bar = m_bars[i];
        const QColor color = bar.close >= bar.open ? m_cfg.timelineChartUpColor : m_cfg.timelineChartDownColor;
        p.setPen(QPen(color, 1));
        p.setBrush(bar.close > bar.open ? Qt::NoBrush : QBrush(color));
        p.drawLine(QPointF(x(i), y(bar.high)), QPointF(x(i), y(bar.low)));
        const double top = std::min(y(bar.open), y(bar.close));
        p.drawRect(QRectF(x(i) - candleWidth / 2, top, candleWidth,
            std::max(1.0, std::abs(y(bar.open) - y(bar.close)))));
        if (std::isfinite(bar.volume) && bar.volume > 0 && maxVolume > 0) {
            const double height = std::max(1.0, bar.volume / maxVolume * (volume.height() - 1));
            p.fillRect(QRectF(x(i) - candleWidth / 2, volume.bottom() - height, candleWidth, height), color);
        }
    }
    p.setClipRect(price);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    for (auto it = averages.cbegin(); it != averages.cend(); ++it) {
        QPainterPath path;
        bool connected = false;
        // Include the previous close when available so the path enters the
        // visible area naturally rather than starting at the first candle's center.
        for (int i = qMax(0, visibleStart - 1); i < m_bars.size(); ++i) {
            const double value = it.value()[i];
            if (!std::isfinite(value)) { connected = false; continue; }
            if (connected) path.lineTo(x(i), y(value));
            else { path.moveTo(x(i), y(value)); connected = true; }
        }
        p.setPen(QPen(daily_kline::movingAverageColor(it.key()), 1.0));
        p.drawPath(path);
    }
    p.restore();

    // Point the arrow at the visible high/low wick. Put the price on the side
    // with room, so first/last-candle labels remain inside the chart.
    const auto drawMarker = [&](int index, double value, bool high) {
        if (index < 0) return;
        const QString number = QString::number(value, 'f', value < 10 ? 3 : 2);
        const int top = qBound(price.top(), qRound(y(value)) + (high ? -18 : 2), price.bottom() - 17);
        const auto marker = daily_kline::priceMarkerLayout(QRectF(price), QPointF(x(index), top + 9.0),
            number, QFontMetricsF(p.font()));
        const double tailX = marker.tip.x() + (marker.labelOnRight ? 5.0 : -5.0);
        const QPolygonF arrow{marker.tip, QPointF(tailX, marker.tip.y() - 3.0),
            QPointF(tailX, marker.tip.y() + 3.0)};
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m_cfg.timelineChartTextColor);
        p.drawPolygon(arrow);
        p.setPen(m_cfg.timelineChartTextColor);
        p.drawText(marker.label, Qt::TextSingleLine | Qt::AlignVCenter
            | (marker.labelOnRight ? Qt::AlignLeft : Qt::AlignRight), number);
        p.restore();
    };
    drawMarker(extrema.highIndex, extrema.high, true);
    drawMarker(extrema.lowIndex, extrema.low, false);

    p.setPen(m_cfg.timelineChartTextColor);
    const int labelY = area.bottom() + 5;
    p.drawText(QRect(area.left(), labelY, 90, 18), Qt::AlignLeft, m_bars[visibleStart].date);
    p.drawText(QRect(area.right() - 89, labelY, 90, 18), Qt::AlignRight, m_bars.last().date);

    if (m_hoverIndex >= visibleStart && m_hoverIndex < m_bars.size()) {
        const auto& bar = m_bars[m_hoverIndex];
        const double hx = x(m_hoverIndex);
        const double hy = y(bar.close);
        const double latestClose = m_bars.last().close;
        const double previousClose = m_hoverIndex > 0 ? m_bars[m_hoverIndex - 1].close : qQNaN();
        const double barPct = std::isfinite(previousClose) && std::abs(previousClose) > 1e-9
            ? (bar.close - previousClose) / previousClose * 100.0
            : qQNaN();
        const double sincePct = std::abs(bar.close) > 1e-9 && std::isfinite(latestClose)
            ? (latestClose - bar.close) / bar.close * 100.0
            : qQNaN();

        QColor crossColor = m_cfg.timelineChartTextColor;
        crossColor.setAlpha(120);
        QPen crossPen(crossColor, 1.0, Qt::CustomDashLine);
        crossPen.setDashPattern({4.0, 3.0});
        p.setPen(crossPen);
        p.drawLine(QPointF(hx, area.top()), QPointF(hx, area.bottom()));
        p.drawLine(QPointF(price.left(), hy), QPointF(price.right(), hy));

        const auto pctText = [](double value) {
            if (!std::isfinite(value)) return QStringLiteral("--");
            QString text = QString::number(value, 'f', 2);
            if (value > 0.0) text.prepend('+');
            return text + QLatin1Char('%');
        };
        const auto pctColor = [&](double value) {
            if (!std::isfinite(value) || value == 0.0) return m_cfg.timelineChartTextColor;
            return value > 0.0 ? m_cfg.timelineChartUpColor : m_cfg.timelineChartDownColor;
        };

        const QString openLabel = i18n::t("popup.kline.hoverOpen", language) + QStringLiteral(": ");
        const QString highLabel = i18n::t("popup.kline.hoverHigh", language) + QStringLiteral(": ");
        const QString lowLabel = i18n::t("popup.kline.hoverLow", language) + QStringLiteral(": ");
        const QString closeLabel = i18n::t("popup.kline.hoverClose", language) + QStringLiteral(": ");
        const QString changeLabel = i18n::t("popup.kline.hoverChange", language) + QStringLiteral(": ");
        const QString sinceLabel = i18n::t("popup.kline.hoverSince", language) + QStringLiteral(": ");
        const auto priceText = [&](double value) {
            return std::isfinite(value)
                ? QString::number(value, 'f', bar.close < 10 ? 3 : 2)
                : QStringLiteral("--");
        };
        const QString openText = priceText(bar.open);
        const QString highText = priceText(bar.high);
        const QString lowText = priceText(bar.low);
        const QString closeText = priceText(bar.close);
        const QString barPctText = pctText(barPct);
        const QString sincePctText = pctText(sincePct);

        const QFontMetrics fm(p.font());
        const int lineHeight = fm.height() + 1;
        const int padding = 6;
        const auto rowWidth = [&](const QString& label, const QString& value) {
            return fm.horizontalAdvance(label) + fm.horizontalAdvance(value);
        };
        const int boxWidth = qMax(fm.horizontalAdvance(bar.date),
            qMax(qMax(rowWidth(openLabel, openText), rowWidth(highLabel, highText)),
                qMax(qMax(rowWidth(lowLabel, lowText), rowWidth(closeLabel, closeText)),
                    qMax(rowWidth(changeLabel, barPctText), rowWidth(sinceLabel, sincePctText)))))
            + padding * 2;
        const int boxHeight = lineHeight * 7 + padding * 2;
        int boxX = m_hoverPos.x() + 14;
        int boxY = m_hoverPos.y() + 14;
        if (boxX + boxWidth > bounds.right()) boxX = m_hoverPos.x() - 14 - boxWidth;
        if (boxY + boxHeight > bounds.bottom()) boxY = m_hoverPos.y() - 14 - boxHeight;
        boxX = qBound(bounds.left(), boxX, qMax(bounds.left(), bounds.right() - boxWidth));
        boxY = qBound(bounds.top(), boxY, qMax(bounds.top(), bounds.bottom() - boxHeight));

        QColor boxBackground = m_cfg.timelineChartBgColor;
        boxBackground.setAlpha(235);
        QColor boxBorder = m_cfg.timelineChartGridColor;
        boxBorder.setAlpha(180);
        p.save();
        p.setPen(QPen(boxBorder, 1.0));
        p.setBrush(boxBackground);
        p.drawRoundedRect(QRect(boxX, boxY, boxWidth, boxHeight), 4, 4);
        p.setBrush(Qt::NoBrush);

        const auto drawRow = [&](int rowY, const QString& label, const QString& value, const QColor& valueColor) {
            p.setPen(m_cfg.timelineChartTextColor);
            p.drawText(QRect(boxX + padding, rowY, boxWidth - padding * 2, lineHeight),
                Qt::AlignLeft | Qt::AlignVCenter, label);
            const int labelWidth = fm.horizontalAdvance(label);
            p.setPen(valueColor);
            p.drawText(QRect(boxX + padding + labelWidth, rowY,
                    boxWidth - padding * 2 - labelWidth, lineHeight),
                Qt::AlignLeft | Qt::AlignVCenter, value);
        };
        int rowY = boxY + padding;
        p.setPen(m_cfg.timelineChartTextColor);
        p.drawText(QRect(boxX + padding, rowY, boxWidth - padding * 2, lineHeight),
            Qt::AlignLeft | Qt::AlignVCenter, bar.date);
        rowY += lineHeight;
        drawRow(rowY, openLabel, openText, m_cfg.timelineChartTextColor);
        rowY += lineHeight;
        drawRow(rowY, highLabel, highText, m_cfg.timelineChartUpColor);
        rowY += lineHeight;
        drawRow(rowY, lowLabel, lowText, m_cfg.timelineChartDownColor);
        rowY += lineHeight;
        drawRow(rowY, closeLabel, closeText, m_cfg.timelineChartTextColor);
        rowY += lineHeight;
        drawRow(rowY, changeLabel, barPctText, pctColor(barPct));
        rowY += lineHeight;
        drawRow(rowY, sinceLabel, sincePctText, pctColor(sincePct));
        p.restore();
        setProperty("hoverOpen", bar.open);
        setProperty("hoverHigh", bar.high);
        setProperty("hoverLow", bar.low);
        setProperty("hoverClose", bar.close);
    }
}

#include "daily_kline_chart.h"
#include "tdx_bridge.h"
#include "i18n.h"
#include "daily_kline_indicators.h"
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWheelEvent>
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
        } else if (event->type() == QEvent::MouseButtonPress && isVisible()) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && m_plotRect.contains(mouse->position().toPoint())) {
                m_dragging = true;
                m_dragOrigin = mouse->position().toPoint();
                m_dragOriginStart = m_viewStart;
                m_timeline->setCursor(Qt::ClosedHandCursor);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease && m_dragging) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_dragging = false;
                m_timeline->setCursor(Qt::OpenHandCursor);
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && isVisible()) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint pos = mouse->position().toPoint();
            if (m_dragging) {
                // The popup does not grab the mouse, so a release outside the
                // chart never reaches it. Any move without the left button held
                // must therefore end the drag.
                if (!(mouse->buttons() & Qt::LeftButton)) {
                    m_dragging = false;
                    updateCursor(pos);
                } else {
                    panTo(pos);
                    return true;
                }
            }
            if (!m_dragging) {
                updateCursor(pos);
                refreshHover(pos);
            }
        } else if (event->type() == QEvent::Wheel && isVisible()) {
            const auto* wheel = static_cast<QWheelEvent*>(event);
            // Mouse wheels report whole notches, trackpads report a long stream
            // of small deltas. Accumulate until a quarter notch is reached so
            // both zoom at a comparable, controllable rate.
            double steps = 0.0;
            if (wheel->angleDelta().y() != 0) steps = wheel->angleDelta().y() / 120.0;
            else if (wheel->pixelDelta().y() != 0) steps = wheel->pixelDelta().y() / 50.0;
            if (steps != 0.0) {
                m_wheelSteps += steps;
                if (std::abs(m_wheelSteps) >= 0.25) {
                    // Wheel up (away from the user) shows fewer candles, like
                    // the desktop trading clients.
                    zoomAt(wheel->position().toPoint(), std::pow(0.9, qBound(-1.0, m_wheelSteps, 1.0)));
                    m_wheelSteps = 0.0;
                }
                return true;
            }
        } else if (event->type() == QEvent::Leave && m_hoverIndex >= 0) {
            if (!m_dragging) m_timeline->unsetCursor();
            clearHover();
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

int DailyKlineChart::minViewCount() const {
    return qMin(daily_kline::kMinViewCount, qBound(1, m_cfg.dailyKlineDays, 800));
}

void DailyKlineChart::resetSeries() {
    ++m_generation;
    setProperty("barCount", 0);
    setProperty("viewStart", 0);
    setProperty("viewCount", 0);
    m_bars.clear();
    m_status.clear();
    m_pending = false;
    m_nextFetchMs = 0;
    m_priceRect = QRect();
    m_plotRect = QRect();
    m_viewStart = 0;
    m_viewCount = 0;
    m_viewTouched = false;
    m_dragging = false;
    m_wheelSteps = 0.0;
    clearHover();
    update();
}

void DailyKlineChart::clearHover() {
    m_hoverIndex = -1;
    setProperty("hoverIndex", -1);
    setProperty("hoverOpen", QVariant());
    setProperty("hoverHigh", QVariant());
    setProperty("hoverLow", QVariant());
    setProperty("hoverClose", QVariant());
}

void DailyKlineChart::updateCursor(const QPoint& pos) {
    if (m_dragging) return;
    // The overlay is transparent to mouse events, so the shared timeline widget
    // carries the cursor for the whole chart window.
    if (m_plotRect.contains(pos)) m_timeline->setCursor(Qt::OpenHandCursor);
    else m_timeline->unsetCursor();
}

void DailyKlineChart::refreshHover(const QPoint& pos) {
    int index = -1;
    // Derive the slot from the view rather than the last painted geometry:
    // panning updates the view before its repaint lands, and a stale slot
    // would report the neighbouring candle.
    if (m_priceRect.contains(pos) && m_viewCount > 0) {
        const double slot = double(m_priceRect.width()) / m_viewCount;
        const int offset = qBound(0, int((pos.x() - m_priceRect.left()) / slot), m_viewCount - 1);
        index = m_viewStart + offset;
    }
    if (index != m_hoverIndex || pos != m_hoverPos) {
        m_hoverIndex = index;
        m_hoverPos = pos;
        setProperty("hoverIndex", index);
        update();
    }
}

void DailyKlineChart::applyView(int start, int count, bool touched) {
    const int total = int(m_bars.size());
    const int minimum = minViewCount();
    const auto view = daily_kline::clampView({start, count}, total, minimum);
    const bool changed = view.start != m_viewStart || view.count != m_viewCount;
    m_viewStart = view.start;
    m_viewCount = view.count;
    // "Touched" means the view no longer matches the configured default, so
    // zooming back out to exactly that range restores both the plain title and
    // the follow-the-newest-candle behaviour.
    if (touched) {
        const auto defaults = daily_kline::defaultView(total, m_cfg.dailyKlineDays, minimum);
        m_viewTouched = view.start != defaults.start || view.count != defaults.count;
    } else {
        m_viewTouched = false;
    }
    if (changed) {
        setProperty("viewStart", m_viewStart);
        setProperty("viewCount", m_viewCount);
        setProperty("barCount", m_viewCount);
        refreshHover(m_hoverPos);
        update();
    }
}

void DailyKlineChart::panTo(const QPoint& pos) {
    if (m_viewCount <= 0 || m_bars.isEmpty() || m_priceRect.width() <= 0) return;
    // Dragging right moves the chart right, revealing older candles on the left.
    const double slot = double(m_priceRect.width()) / m_viewCount;
    const double delta = (m_dragOrigin.x() - pos.x()) / slot;
    m_hoverPos = pos;
    const auto view = daily_kline::panView(
        {m_dragOriginStart, m_viewCount}, int(m_bars.size()), delta, minViewCount());
    applyView(view.start, view.count, true);
}

void DailyKlineChart::zoomAt(const QPoint& pos, double factor) {
    if (m_bars.isEmpty() || m_viewCount <= 0 || m_priceRect.width() <= 0) return;
    const double slot = double(m_priceRect.width()) / m_viewCount;
    const double anchor = m_viewStart
        + qBound(0.0, (pos.x() - m_priceRect.left()) / slot, double(m_viewCount));
    const auto view = daily_kline::zoomView(
        {m_viewStart, m_viewCount}, int(m_bars.size()), anchor, factor, minViewCount());
    m_hoverPos = pos;
    applyView(view.start, view.count, true);
}

void DailyKlineChart::setControlHeld(bool held) {
    held = (held || m_pinnedPeriod != KlinePeriod::None) && m_timeline->isVisible() && !m_code.isEmpty();
    const bool wasHeld = m_controlHeld;
    if (wasHeld != held) {
        m_timeline->setProperty("dailyKlineVisible", held);
        m_timeline->update();
    }
    m_controlHeld = held;
    setVisible(held);
    if (held) {
        raise();
        // The Ctrl hover timer re-enters here every 50 ms; re-applying the
        // cursor during a drag would undo its closed-hand state.
        updateCursor(m_timeline->mapFromGlobal(QCursor::pos()));
        fetch();
    } else if (wasHeld) {
        m_dragging = false;
        m_wheelSteps = 0.0;
        m_timeline->unsetCursor();
        clearHover();
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
        {"name", m_name}, {"period", period}, {"count", daily_kline::kMaxHistoryCount}},
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
            if (bars.size() > daily_kline::kMaxHistoryCount) {
                bars = bars.mid(bars.size() - daily_kline::kMaxHistoryCount);
            }
            // A user who panned or zoomed keeps that window anchored to the
            // right edge, so a refresh appends candles instead of moving them.
            const auto previous = daily_kline::ViewWindow{m_viewStart, m_viewCount};
            const int oldTotal = int(m_bars.size());
            const bool touched = m_viewTouched;
            m_bars = bars;
            const auto view = daily_kline::refreshView(
                previous, oldTotal, int(m_bars.size()), m_cfg.dailyKlineDays, touched, minViewCount());
            applyView(view.start, view.count, touched);
            m_status = bars.isEmpty() ? i18n::t("popup.kline.empty", language).arg(periodLabel) : QString();
            update();
        });
}

void DailyKlineChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // The timeline parent paints the shared background once. This transparent
    // overlay paints chart content only, preserving the same configured alpha.
    const QRect bounds = rect().adjusted(12, 12, -12, -12);
    m_priceRect = QRect();
    m_plotRect = QRect();
    p.setPen(m_cfg.timelineChartTextColor);
    const QString language = i18n::resolveLanguage(m_cfg.language);
    const int total = int(m_bars.size());
    if (!m_bars.isEmpty() && m_viewCount <= 0) {
        const auto view = daily_kline::defaultView(total, m_cfg.dailyKlineDays, minViewCount());
        m_viewStart = view.start;
        m_viewCount = view.count;
    }
    const int visibleStart = m_viewStart;
    const int visibleCount = m_viewCount;
    const int titleCount = visibleCount > 0 ? visibleCount : m_cfg.dailyKlineDays;
    const QString periodLabel = periodName(static_cast<KlinePeriod>(activePeriod()), language);
    // Once the user pans or zooms, the visible count no longer matches the
    // configured range, so quoting both would read like a mismatch.
    const QString titleBody = m_viewTouched
        ? i18n::t("popup.kline.titleZoomed", language).arg(periodLabel).arg(titleCount)
        : i18n::t("popup.kline.title", language)
            .arg(periodLabel).arg(titleCount).arg(m_cfg.dailyKlineDays);
    const QString title = (m_name.isEmpty() ? m_code : m_name + " " + m_code) + " · " + titleBody;
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
    const int legendIndex = qBound(0, visibleStart + visibleCount - 1, total - 1);
    int legendX = bounds.left(), legendY = bounds.top() + 26;
    for (auto it = averages.cbegin(); it != averages.cend(); ++it) {
        const double value = it.value().at(legendIndex);
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
    const int visibleEnd = qMin(visibleStart + visibleCount, total);
    const auto extrema = daily_kline::visibleExtrema(m_bars, visibleStart, visibleEnd);
    double lowest = extrema.low, highest = extrema.high, maxVolume = 0;
    for (int i = visibleStart; i < visibleEnd; ++i) {
        if (std::isfinite(m_bars[i].volume)) maxVolume = std::max(maxVolume, m_bars[i].volume);
        for (auto it = averages.cbegin(); it != averages.cend(); ++it) {
            if (std::isfinite(it.value()[i])) {
                lowest = std::min(lowest, it.value()[i]);
                highest = std::max(highest, it.value()[i]);
            }
        }
    }
    if (!std::isfinite(lowest) || !std::isfinite(highest) || highest <= lowest) return;
    const double padding = std::max((highest - lowest) * 0.10, highest * 0.002);
    lowest -= padding;
    highest += padding;
    const auto y = [&](double value) { return price.bottom() - (value - lowest) / (highest - lowest) * (price.height() - 1); };
    const double slot = static_cast<double>(price.width()) / qMax(1, visibleCount);
    const auto x = [&](int i) { return price.left() + (i - visibleStart + 0.5) * slot; };
    const double candleWidth = daily_kline::candleWidth(slot);
    m_priceRect = price;
    m_plotRect = area;
    for (int i = 0; i <= 4; ++i) {
        const int yy = price.top() + (price.height() - 1) * i / 4;
        p.setPen(m_cfg.timelineChartTextColor);
        const double tick = highest - (highest - lowest) * i / 4;
        p.drawText(QRect(bounds.left(), yy - 9, 52, 18), Qt::AlignRight | Qt::AlignVCenter,
            QString::number(tick, 'f', highest < 10 ? 3 : 2));
    }
    p.save();
    p.setClipRect(price);
    for (const auto& gap : daily_kline::unfilledGaps(m_bars, visibleStart, visibleEnd)) {
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
    for (int i = visibleStart; i < visibleEnd; ++i) {
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
        for (int i = qMax(0, visibleStart - 1); i < qMin(visibleEnd + 1, total); ++i) {
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
    p.drawText(QRect(area.right() - 89, labelY, 90, 18), Qt::AlignRight, m_bars[visibleEnd - 1].date);

    if (m_hoverIndex >= visibleStart && m_hoverIndex < visibleEnd) {
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

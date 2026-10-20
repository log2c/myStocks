#pragma once

#include "types.h"
#include <QWidget>
#include <QTimer>

// Overlays a timeline widget while Control is held, including non-activating
// tool windows where key events can be delivered to another application.
enum class KlinePeriod : int {
    None = 0,
    Daily = 4,
    Weekly = 5,
    Monthly = 6,
};

class DailyKlineChart : public QWidget {
public:
    explicit DailyKlineChart(QWidget* timeline);
    void setPinnedDaily(bool pinned);
    void setPinnedPeriod(KlinePeriod period);
    void setContext(const AppConfig& cfg, const QString& code, const QString& name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent*) override;

private:
    struct Bar {
        QString date;
        double open, high, low, close, volume;
    };
    void setControlHeld(bool held);
    int activePeriod() const;
    // Zoom-in floor: never fewer candles than the readable minimum, but never
    // more than a deliberately small configured range (the setting allows 1).
    int minViewCount() const;
    void resetSeries();
    void fetch();
    // Pan/zoom state: which slice of m_bars is on screen. An untouched chart
    // follows the configured range and the newest candle.
    void applyView(int start, int count, bool touched);
    void panTo(const QPoint& pos);
    void zoomAt(const QPoint& pos, double factor);
    void updateCursor(const QPoint& pos);
    void refreshHover(const QPoint& pos);
    void clearHover();
    QWidget* m_timeline;
    QTimer m_modifierTimer;
    AppConfig m_cfg;
    QString m_code, m_name, m_status;
    QVector<Bar> m_bars;
    int m_generation = 0;
    bool m_controlHeld = false;
    KlinePeriod m_pinnedPeriod = KlinePeriod::None;
    bool m_pending = false;
    qint64 m_nextFetchMs = 0;
    QRect m_priceRect;
    QRect m_plotRect;
    int m_viewStart = 0;
    int m_viewCount = 0;
    bool m_viewTouched = false;
    bool m_dragging = false;
    QPoint m_dragOrigin;
    int m_dragOriginStart = 0;
    double m_wheelSteps = 0.0;
    QPoint m_hoverPos;
    int m_hoverIndex = -1;
};

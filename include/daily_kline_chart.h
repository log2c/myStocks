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
    void resetSeries();
    void fetch();
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
    int m_visibleStart = 0;
    int m_visibleCount = 0;
    double m_slot = 0.0;
    QPoint m_hoverPos;
    int m_hoverIndex = -1;
};

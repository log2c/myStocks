#pragma once

#include "types.h"
#include <QWidget>
#include <QTimer>

// Overlays a timeline widget while Control is held, including non-activating
// tool windows where key events can be delivered to another application.
class DailyKlineChart : public QWidget {
public:
    explicit DailyKlineChart(QWidget* timeline);
    void setPinnedDaily(bool pinned);
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
    void fetch();
    QWidget* m_timeline;
    QTimer m_modifierTimer;
    AppConfig m_cfg;
    QString m_code, m_name, m_status;
    QVector<Bar> m_bars;
    int m_generation = 0;
    bool m_controlHeld = false;
    bool m_pinnedDaily = false;
    bool m_pending = false;
    qint64 m_nextFetchMs = 0;
};

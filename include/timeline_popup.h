#pragma once

#include "types.h"

#include <QWidget>

bool isTimelinePopupSupportedCode(const QString& rawCode);

// Closes every K-line detail window that is currently on screen, whichever
// window opened it. The hover popup is left alone.
void hideAllKlineDetailWindows();
// True when the point lies inside any visible K-line detail window.
bool isPointInsideKlineDetailWindow(const QPoint& globalPos);

class SharedTimelineChartPopupPrivate;

class SharedTimelineChartPopup : public QWidget {
public:
    explicit SharedTimelineChartPopup(QWidget* parent = nullptr);
    ~SharedTimelineChartPopup() override;

    void applyConfig(const AppConfig& cfg);
    void showForStock(const QString& code, const QString& name, const QRect& anchorRect, int baseWidth, double cost = qQNaN(), bool daily = false);
    // Opens the interactive stock chart window with intraday, daily, weekly,
    // and monthly views. Unlike the hover popup, it stays open until closed.
    void showDetailForStock(const QString& code, const QString& name, const QRect& anchorRect, double cost = qQNaN());
    void hidePopup();

private:
    SharedTimelineChartPopupPrivate* d = nullptr;
};

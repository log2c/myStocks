#pragma once

#include "types.h"

#include <QWidget>

bool isTimelinePopupSupportedCode(const QString& rawCode);

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

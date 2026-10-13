#pragma once

#include "types.h"

#include <QWidget>
#include <QUrl>

QUrl timelineImageUrlForCode(const QString& rawCode);
bool isTimelinePopupSupportedCode(const QString& rawCode);

class SharedTimelineChartPopupPrivate;

class SharedTimelineChartPopup : public QWidget {
public:
    explicit SharedTimelineChartPopup(QWidget* parent = nullptr);
    ~SharedTimelineChartPopup() override;

    void applyConfig(const AppConfig& cfg);
    void showForStock(const QString& code, const QString& name, const QRect& anchorRect,
        int baseWidth, double cost = qQNaN(), double price = qQNaN());
    void updateQuoteForStock(const QString& code, double cost, double price);
    void hidePopup();

private:
    SharedTimelineChartPopupPrivate* d = nullptr;
};

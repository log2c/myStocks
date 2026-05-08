#pragma once

#include "types.h"

#include <QRect>
#include <QTimer>
#include <QWidget>

#include <functional>

class EastMoneyHotRankProvider;
class TonghuashunStockHeatProvider;
class HotRankConstituentDetailWindow;
class EastMoneyIndexQuoteProvider;
class SharedTimelineChartPopup;
class QResizeEvent;
class QWheelEvent;

class MarketBreadthDetailWindow : public QWidget {
public:
    explicit MarketBreadthDetailWindow(QWidget* parent = nullptr);
    ~MarketBreadthDetailWindow() override;

    void applyConfig(const AppConfig& cfg);
    void setLanguage(const QString& language);
    void setForceRefreshCallback(std::function<void()> callback);
    void setHotRankDetailWatchlistCallbacks(
        std::function<bool(const QString&)> containsCallback,
        std::function<bool(const QString&, const QString&, bool)> mutateCallback,
        std::function<void()> reloadCallback
    );

    void showCenteredForSnapshot(
        const MarketBreadthSnapshot& snapshot,
        const QVector<HotRankItem>& hotSectors,
        const QVector<HotRankItem>& hotConcepts,
        const QRect& referenceRect
    );
    void refreshSnapshot(
        const MarketBreadthSnapshot& snapshot,
        const QVector<HotRankItem>& hotSectors,
        const QVector<HotRankItem>& hotConcepts
    );

    bool isPinnedFromTray() const;
    QRect savedWindowRect() const;
    void setSavedWindowRect(const QRect& rect);
    void hidePopup();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    enum class HotRankTabMode {
        Auto = 0,
        Sector = 1,
        Concept = 2,
        HourHeat = 3,
        DayHeat = 4,
    };

    enum class BreadthChartTabMode {
        Distribution = 0,
        Trend = 1,
    };

    enum class HotRankSortField {
        Pct = 0,
        MainNetInflow = 1,
        YearPct = 2,
    };

    QRect resizeHandleRect() const;
    void updateCursorForPosition(const QPoint& pos);
    void applyResizeFromGlobalPos(const QPoint& globalPos);
    void triggerPopupRefresh(bool withFeedback);
    void startRefreshFeedback();
    bool isRefreshFeedbackActive(qint64* nowMsOut = nullptr) const;
    void startAutoRefreshTimer();
    void stopAutoRefreshTimer();
    void ensureSingleVisible();
    void ensureHotRankProviders();
    void ensureStockHeatProvider();
    void ensureHotRankDetailWindow();
    void ensureIndexQuoteProvider();
    void ensureTimelinePopup();
    int popupHotRankLimit(bool concept) const;
    void requestHotRankData(bool concept, bool forceRefresh = false);
    void requestStockHeatData(HotRankTabMode mode, bool forceRefresh = false);
    void requestIndexQuoteData(bool forceRefresh = false);
    HotRankTabMode resolvedHotRankTabMode() const;
    static bool isStockHeatTabMode(HotRankTabMode mode);
    const QVector<HotRankItem>& hotRankItemsForMode(HotRankTabMode mode) const;
    int& hotRankScrollIndexForMode(HotRankTabMode mode);
    int hotRankRowAt(const QPoint& pos) const;
    int hotStockActionAt(const QPoint& pos) const;
    int indexCellAt(const QPoint& pos) const;
    void openHotRankDetail(const HotRankItem& item);
    bool containsHotStockWatchlistItem(const HotRankItem& item) const;
    void toggleHotStockWatchlistAction(int actionIndex);
    void flushPendingHotStockWatchlistReloadIfNeeded();
    void showTimelinePopupForIndexCell(int index);
    void hideTimelinePopup();

    struct HotRankRowHitArea {
        QRect rect;
        HotRankItem item;
    };

    struct IndexCellHitArea {
        QRect rect;
        QString code;
        QString name;
    };

    QWidget* m_parentWindow = nullptr;
    AppConfig m_cfg;
    QString m_language = QStringLiteral("en_US");
    MarketBreadthSnapshot m_snapshot;
    QVector<HotRankItem> m_hotSectors;
    QVector<HotRankItem> m_hotConcepts;
    QVector<HotRankItem> m_hotSectorsRanked;
    QVector<HotRankItem> m_hotConceptsRanked;
    QVector<HotRankItem> m_hotHourStocks;
    QVector<HotRankItem> m_hotDayStocks;
    EastMoneyHotRankProvider* m_hotRankProvider = nullptr;
    TonghuashunStockHeatProvider* m_stockHeatProvider = nullptr;
    HotRankConstituentDetailWindow* m_hotRankDetailWindow = nullptr;
    EastMoneyIndexQuoteProvider* m_indexQuoteProvider = nullptr;
    SharedTimelineChartPopup* m_timelinePopup = nullptr;
    QVector<IndexQuoteItem> m_indexQuotes;
    QString m_lastHotRankError;
    QString m_lastStockHeatError;
    HotRankTabMode m_hotRankTabMode = HotRankTabMode::Auto;
    BreadthChartTabMode m_breadthChartTabMode = BreadthChartTabMode::Trend;
    bool m_pinnedFromTray = false;
    bool m_hasStoredGeometry = false;
    QRect m_closeButtonRect;
    QRect m_refreshButtonRect;
    QRect m_updatedTextRect;
    QRect m_hotSectorTabRect;
    QRect m_hotConceptTabRect;
    QRect m_hotHourTabRect;
    QRect m_hotDayTabRect;
    QRect m_hotNameHeaderRect;
    QRect m_hotPctHeaderRect;
    QRect m_hotInflowHeaderRect;
    QRect m_hotYearPctHeaderRect;
    QRect m_hotRankListViewportRect;
    QRect m_breadthDistributionTabRect;
    QRect m_breadthTrendTabRect;
    bool m_closeButtonHovered = false;
    bool m_closeButtonPressed = false;
    bool m_refreshButtonHovered = false;
    bool m_refreshButtonPressed = false;
    bool m_hotSectorTabHovered = false;
    bool m_hotConceptTabHovered = false;
    bool m_hotHourTabHovered = false;
    bool m_hotDayTabHovered = false;
    bool m_hotPctHeaderHovered = false;
    bool m_hotInflowHeaderHovered = false;
    bool m_hotYearPctHeaderHovered = false;
    bool m_breadthDistributionTabHovered = false;
    bool m_breadthTrendTabHovered = false;
    int m_pressedHotTab = 0;
    int m_pressedHotHeader = 0;
    int m_hoveredHotRowIndex = -1;
    int m_pressedHotRowIndex = -1;
    int m_pressedIndexCellIndex = -1;
    int m_hotSectorScrollIndex = 0;
    int m_hotConceptScrollIndex = 0;
    int m_hotHourScrollIndex = 0;
    int m_hotDayScrollIndex = 0;
    int m_pressedBreadthTab = 0;
    HotRankSortField m_hotRankSortField = HotRankSortField::Pct;
    bool m_hotRankSortDescending = true;
    bool m_dragging = false;
    bool m_resizing = false;
    bool m_adjustingResize = false;
    QPoint m_dragOffset;
    QPoint m_resizeStartGlobalPos;
    QSize m_resizeStartSize;
    QTimer* m_refreshFeedbackTimer = nullptr;
    QTimer* m_autoRefreshTimer = nullptr;
    qint64 m_refreshFeedbackStartedMs = 0;
    qint64 m_refreshFeedbackUntilMs = 0;
    std::function<void()> m_forceRefreshCallback;
    std::function<bool(const QString&)> m_hotRankDetailWatchlistContainsCallback;
    std::function<bool(const QString&, const QString&, bool)> m_hotRankDetailWatchlistMutateCallback;
    std::function<void()> m_hotRankDetailWatchlistReloadCallback;
    QVector<HotRankRowHitArea> m_hotRankRowHitAreas;
    QVector<QRect> m_hotStockActionButtonRects;
    int m_hoveredHotStockActionIndex = -1;
    int m_pressedHotStockActionIndex = -1;
    bool m_hotStockWatchlistDirty = false;
    QHash<QString, bool> m_hotStockWatchlistPresenceOverrides;
    QVector<IndexCellHitArea> m_indexCellHitAreas;
    bool m_timelineAppFilterInstalled = false;
    static MarketBreadthDetailWindow* s_visiblePopup;
};

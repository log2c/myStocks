#pragma once

#include "quote_model.h"
#include "types.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QPoint>
#include <QTableView>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

#include <functional>

class TimelineChartPopup;
class MarketBreadthDetailWindow;

class FloatingWindow : public QWidget {
    Q_OBJECT
public:
    explicit FloatingWindow(QuoteModel* model, QWidget* parent = nullptr);
    ~FloatingWindow() override;

    void applyConfig(const AppConfig& cfg);
    void toggleMarketBreadthDetailPopupFromTray();
    QRect marketBreadthDetailPopupGeometry() const;
    void setMarketBreadthDetailPopupGeometry(const QRect& rect);
    void setMarketBreadthDetailWatchlistCallbacks(
        std::function<bool(const QString&)> containsCallback,
        std::function<bool(const QString&, const QString&, bool)> mutateCallback,
        std::function<void()> reloadCallback
    );

    // Update displayed groups; allGroups includes "所有" at index 0.
    void setGroups(const QVector<StockGroup>& customGroups, int activeGroupIndex, int allGroupPosition = 0);
    void setActiveGroupIndex(int index);
    // Set the number of leading model rows that are "index" rows (displayed in top table).
    void setIndexCount(int count);
    // Release the locked column widths so they are recomputed on the next layout pass.
    void unlockWidth();

signals:
    void forceRefreshRequested();
    // Emitted when user clicks a group tab (0 = "所有", 1+ = custom group).
    void groupSwitchRequested(int index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool isCursorInsideWindow() const;
    bool isInteractionActivationPressed() const;
    bool shouldCaptureMouseInteraction() const;
    bool shouldAllowMouseInteraction() const;
    bool isDragTriggerButton(Qt::MouseButton button) const;
    bool isCurrentDragButtonHeld(Qt::MouseButtons buttons) const;
    void scheduleHoverReadingTimer();
    void updateHoverReadingState(bool animated);
    void refreshMousePassthroughState(bool force = false);
    bool setMousePassthroughActive(bool active);
    bool canShowTimelinePopup() const;
    void updateHoverPopupsForViewport(const QPoint& viewportPos, QTableView* sourceView = nullptr);
    void updateTimelinePopupForHover(const QPoint& viewportPos, QTableView* sourceView = nullptr);
    void hideTimelinePopup();
    void refreshMarketBreadthDetailPopup();
    void hideMarketBreadthDetailPopup();
    void enforceWindowLevel(bool activate = false);
    void applyStyle();
    void applyHoverReadingStyle();
    void setHoverReadingActive(bool active, bool animated);
    void applyInterpolatedStyle(qreal hoverProgress);
    void applyColumns();
    void adjustWindowSize();
    int autoColumnWidthFromContent(int column) const;
    void rebuildGroupBar();
    void applyGroupBarStyle();
    void applyRowVisibility();

private:
    QuoteModel* m_model = nullptr;
    QFrame* m_panel = nullptr;
    QTableView* m_table = nullptr;       // top section: index rows + header
    QTableView* m_stockTable = nullptr;  // bottom section: stock rows (group-filtered)
    TimelineChartPopup* m_timelinePopup = nullptr;
    MarketBreadthDetailWindow* m_marketBreadthDetailPopup = nullptr;
    QString m_timelineHoverCode;
    QString m_timelineHoverName;
    QRect m_marketBreadthDetailAnchorRect;

    AppConfig m_cfg;
    bool m_dragging = false;
    QPoint m_dragOffset;
    Qt::MouseButton m_dragButton = Qt::NoButton;

    QTimer* m_hoverTimer = nullptr;
    QTimer* m_mousePassthroughTimer = nullptr;
    QVariantAnimation* m_styleAnimation = nullptr;
    bool m_hoverReadingActive = false;
    qreal m_hoverReadingProgress = 0.0;
    bool m_mousePassthroughActive = false;

    // Group bar
    QWidget* m_groupBar = nullptr;
    QHBoxLayout* m_groupBarLayout = nullptr;
    QVector<StockGroup> m_customGroups;
    int m_activeGroupIndex = 0; // 0 = "所有"
    int m_indexCount = 0;       // number of leading model rows that are index rows
    int m_allGroupPosition = 0; // visual position of "所有" button in the group bar
    QHash<int, int> m_maxColumnWidths; // logical col → max pixel width seen; empty = no history yet
};

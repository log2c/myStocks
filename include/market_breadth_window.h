#pragma once

#include "types.h"

#include <QRect>
#include <QTimer>
#include <QWidget>

#include <functional>

class EastMoneyHotRankProvider;
class EastMoneyIndexQuoteProvider;

class MarketBreadthDetailWindow : public QWidget {
public:
    explicit MarketBreadthDetailWindow(QWidget* parent = nullptr);
    ~MarketBreadthDetailWindow() override;

    void applyConfig(const AppConfig& cfg);
    void setLanguage(const QString& language);
    void setForceRefreshCallback(std::function<void()> callback);

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
    void hidePopup();

protected:
    void hideEvent(QHideEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    enum class HotRankTabMode {
        Auto = 0,
        Sector = 1,
        Concept = 2,
    };

    void startRefreshFeedback();
    bool isRefreshFeedbackActive(qint64* nowMsOut = nullptr) const;
    void startLastUpdatedTextTimer();
    void stopLastUpdatedTextTimer();
    void ensureSingleVisible();
    void enforceAlwaysOnTop();
    void ensureHotRankProviders();
    void ensureIndexQuoteProvider();
    int popupHotRankLimit(bool concept) const;
    void requestHotRankData(bool concept, bool forceRefresh = false);
    void requestIndexQuoteData(bool forceRefresh = false);

    QWidget* m_parentWindow = nullptr;
    AppConfig m_cfg;
    QString m_language = QStringLiteral("en_US");
    MarketBreadthSnapshot m_snapshot;
    QVector<HotRankItem> m_hotSectors;
    QVector<HotRankItem> m_hotConcepts;
    QVector<HotRankItem> m_hotSectorsRanked;
    QVector<HotRankItem> m_hotConceptsRanked;
    EastMoneyHotRankProvider* m_hotRankProvider = nullptr;
    EastMoneyIndexQuoteProvider* m_indexQuoteProvider = nullptr;
    QVector<IndexQuoteItem> m_indexQuotes;
    QString m_lastHotRankError;
    HotRankTabMode m_hotRankTabMode = HotRankTabMode::Auto;
    bool m_pinnedFromTray = false;
    QRect m_closeButtonRect;
    QRect m_refreshButtonRect;
    QRect m_updatedTextRect;
    QRect m_hotSectorTabRect;
    QRect m_hotConceptTabRect;
    bool m_closeButtonHovered = false;
    bool m_closeButtonPressed = false;
    bool m_refreshButtonHovered = false;
    bool m_refreshButtonPressed = false;
    bool m_hotSectorTabHovered = false;
    bool m_hotConceptTabHovered = false;
    int m_pressedHotTab = 0;
    bool m_dragging = false;
    QPoint m_dragOffset;
    QTimer* m_refreshFeedbackTimer = nullptr;
    QTimer* m_lastUpdatedTextTimer = nullptr;
    qint64 m_refreshFeedbackStartedMs = 0;
    qint64 m_refreshFeedbackUntilMs = 0;
    std::function<void()> m_forceRefreshCallback;
    static MarketBreadthDetailWindow* s_visiblePopup;
};

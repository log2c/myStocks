#pragma once

#include "quote_model.h"
#include "types.h"

#include <QFrame>
#include <QPoint>
#include <QTableView>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

class FloatingWindow : public QWidget {
    Q_OBJECT
public:
    explicit FloatingWindow(QuoteModel* model, QWidget* parent = nullptr);

    void applyConfig(const AppConfig& cfg);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;
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
    void enforceWindowLevel(bool activate = false);
    void applyStyle();
    void applyHoverReadingStyle();
    void setHoverReadingActive(bool active, bool animated);
    void applyInterpolatedStyle(qreal hoverProgress);
    void applyColumns();
    void adjustWindowSize();
    int autoColumnWidthFromContent(int column) const;

private:
    QuoteModel* m_model = nullptr;
    QFrame* m_panel = nullptr;
    QTableView* m_table = nullptr;

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
};

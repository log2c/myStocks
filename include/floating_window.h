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
    void enforceWindowLevel(bool activate = false);
    void applyStyle();
    void applyHoverReadingStyle();
    void setHoverReadingActive(bool active, bool animated);
    void applyInterpolatedStyle(qreal progress, bool towardsHoverReading);
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

    QTimer* m_hoverTimer = nullptr;
    QVariantAnimation* m_styleAnimation = nullptr;
    bool m_hoverReadingActive = false;
};

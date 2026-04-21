#pragma once

#include "quote_model.h"
#include "types.h"

#include <QFrame>
#include <QPoint>
#include <QTableView>
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

private:
    void enforceWindowLevel(bool activate = false);
    void applyStyle();
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
};

#include "floating_window.h"

#include <QCursor>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPalette>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStyleFactory>
#include <QVBoxLayout>

#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QVector<int> normalizedColumnOrder(const QVector<int>& order) {
    QVector<int> out;
    out.reserve(ColCount);

    for (int logical : order) {
        if (logical < 0 || logical >= ColCount || out.contains(logical)) {
            continue;
        }
        out.push_back(logical);
    }

    for (int i = 0; i < ColCount; ++i) {
        if (out.contains(i)) {
            continue;
        }
        out.push_back(i);
    }

    return out;
}

QColor mixColor(const QColor& from, const QColor& to, qreal t) {
    const qreal clamped = qBound(0.0, t, 1.0);
    return QColor(
        qRound(from.red() + (to.red() - from.red()) * clamped),
        qRound(from.green() + (to.green() - from.green()) * clamped),
        qRound(from.blue() + (to.blue() - from.blue()) * clamped),
        qRound(from.alpha() + (to.alpha() - from.alpha()) * clamped)
    );
}

int mixInt(int from, int to, qreal t) {
    const qreal clamped = qBound(0.0, t, 1.0);
    return qRound(from + (to - from) * clamped);
}

} // namespace

FloatingWindow::FloatingWindow(QuoteModel* model, QWidget* parent)
    : QWidget(parent)
    , m_model(model) {
    Qt::WindowFlags flags = Qt::FramelessWindowHint;
#ifdef WIN32
    // Qt::Tool suppresses the taskbar entry on Windows.
    flags |= Qt::Tool;
#else
    // Use a normal top-level window on non-Windows to avoid tool-window stacking quirks.
    flags |= Qt::Window;
#endif
    if (m_cfg.floatingWindowAlwaysOnTop) {
        flags |= Qt::WindowStaysOnTopHint;
    } else {
        flags |= Qt::WindowStaysOnBottomHint;
    }
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground, true);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_panel = new QFrame(this);
    m_panel->setObjectName("panel");

    QVBoxLayout* panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);

    m_table = new QTableView(m_panel);
    m_table->setModel(m_model);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(false);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setFrameShape(QFrame::NoFrame);

#ifdef WIN32
    // Windows native style (QWindowsVistaStyle) ignores stylesheet color and
    // ForegroundRole. Force Fusion so our palette and stylesheet are respected.
    if (QStyle* fusion = QStyleFactory::create("Fusion")) {
        m_table->setStyle(fusion);
        m_table->horizontalHeader()->setStyle(fusion);
    }
#endif

    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);

    QHeaderView* header = m_table->horizontalHeader();
    header->setSectionsMovable(false);
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(0);
    header->setSectionResizeMode(QHeaderView::Fixed);

    panelLayout->addWidget(m_table);
    root->addWidget(m_panel);

    m_panel->installEventFilter(this);
    m_table->installEventFilter(this);
    m_table->viewport()->installEventFilter(this);
    m_table->horizontalHeader()->installEventFilter(this);

    connect(m_model, &QAbstractItemModel::dataChanged, this, [this]() {
        adjustWindowSize();
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        adjustWindowSize();
    });
    connect(m_model, &QAbstractItemModel::layoutChanged, this, [this]() {
        adjustWindowSize();
    });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this]() {
        adjustWindowSize();
    });
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, [this]() {
        adjustWindowSize();
    });

    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    connect(m_hoverTimer, &QTimer::timeout, this, [this]() {
        if (m_cfg.hoverReadingEnabled && !m_dragging) {
            setHoverReadingActive(true, true);
        }
    });

    m_styleAnimation = new QVariantAnimation(this);
    m_styleAnimation->setDuration(180);
    m_styleAnimation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_styleAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        applyInterpolatedStyle(value.toReal(), m_hoverReadingActive);
    });
}

bool FloatingWindow::eventFilter(QObject* watched, QEvent* event) {
    auto startHoverReadingTimerIfNeeded = [this]() {
        if (!m_cfg.hoverReadingEnabled || m_dragging || !m_hoverTimer || m_hoverReadingActive) {
            return;
        }
        const int ms = static_cast<int>(m_cfg.hoverReadingDelaySecs * 1000.0);
        m_hoverTimer->start(qMax(100, ms));
    };

    auto restoreNormalStyleIfCursorOutsideWindow = [this]() {
        const QPoint globalPos = QCursor::pos();
        if (frameGeometry().contains(globalPos)) {
            return;
        }
        if (m_hoverTimer) {
            m_hoverTimer->stop();
        }
        if (m_hoverReadingActive) {
            m_hoverReadingActive = false;
            applyStyle();
        }
    };

    switch (event->type()) {
    case QEvent::Enter:
    case QEvent::HoverEnter:
        if (watched == m_panel
            || watched == m_table
            || watched == m_table->viewport()
            || watched == m_table->horizontalHeader()) {
            startHoverReadingTimerIfNeeded();
        }
        break;
    case QEvent::Leave:
    case QEvent::HoverLeave:
        if (watched == m_panel
            || watched == m_table
            || watched == m_table->viewport()
            || watched == m_table->horizontalHeader()) {
            restoreNormalStyleIfCursorOutsideWindow();
        }
        break;
    case QEvent::MouseButtonPress: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            m_dragging = true;
            m_dragOffset = mouseEvent->globalPosition().toPoint() - frameGeometry().topLeft();
            grabMouse();
            return true;
        }
        break;
    }
    case QEvent::MouseMove: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (m_dragging && (mouseEvent->buttons() & Qt::LeftButton)) {
            move(mouseEvent->globalPosition().toPoint() - m_dragOffset);
            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            m_dragging = false;
            releaseMouse();
            enforceWindowLevel(false);
            return true;
        }
        break;
    }
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

void FloatingWindow::applyConfig(const AppConfig& cfg) {
    m_cfg = cfg;

    const bool topFlagChanged =
        windowFlags().testFlag(Qt::WindowStaysOnTopHint) != m_cfg.floatingWindowAlwaysOnTop;
    const bool bottomFlagChanged =
        windowFlags().testFlag(Qt::WindowStaysOnBottomHint) == m_cfg.floatingWindowAlwaysOnTop;
    const QRect oldGeometry = geometry();
    const bool wasVisible = isVisible();
    if (topFlagChanged) {
        setWindowFlag(Qt::WindowStaysOnTopHint, m_cfg.floatingWindowAlwaysOnTop);
    }
    if (bottomFlagChanged) {
        setWindowFlag(Qt::WindowStaysOnBottomHint, !m_cfg.floatingWindowAlwaysOnTop);
    }
    if (wasVisible && (topFlagChanged || bottomFlagChanged)) {
        show();
        setGeometry(oldGeometry);
    }

    const double effectiveOpacity = m_cfg.transparentBackgroundEnabled
        ? static_cast<double>(qBound(0, m_cfg.transparentBackgroundOpacity, 100)) / 100.0
        : qBound(0.0, m_cfg.opacity, 1.0);
    setWindowOpacity(qBound(0.0, effectiveOpacity, 1.0));

    // Reset hover reading state when config is reloaded
    if (m_hoverTimer) {
        m_hoverTimer->stop();
    }
    setHoverReadingActive(false, false);

    applyStyle();
    applyColumns();

    if (isVisible()) {
        enforceWindowLevel(false);
    }
}

void FloatingWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        grabMouse();
    }
    QWidget::mousePressEvent(event);
}

void FloatingWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void FloatingWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        releaseMouse();
        enforceWindowLevel(false);
    }
    QWidget::mouseReleaseEvent(event);
}

void FloatingWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

    enforceWindowLevel(true);
}

void FloatingWindow::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    if (m_cfg.hoverReadingEnabled && !m_dragging && m_hoverTimer && !m_hoverReadingActive) {
        const int ms = static_cast<int>(m_cfg.hoverReadingDelaySecs * 1000.0);
        m_hoverTimer->start(qMax(100, ms));
    }
}

void FloatingWindow::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    if (frameGeometry().contains(QCursor::pos())) {
        return;
    }
    if (m_hoverTimer) {
        m_hoverTimer->stop();
    }
    setHoverReadingActive(false, true);
}

void FloatingWindow::enforceWindowLevel(bool activate) {
    if (!isVisible()) {
        return;
    }

    const bool alwaysOnTop = m_cfg.floatingWindowAlwaysOnTop;

#ifdef WIN32
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        if (alwaysOnTop) {
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING
            );
        } else {
            SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING
            );
            SetWindowPos(
                hwnd,
                HWND_BOTTOM,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING
            );
        }
    }
#endif

    if (alwaysOnTop) {
        raise();
        if (activate) {
            activateWindow();
        }
    } else {
        lower();
    }
}

void FloatingWindow::applyStyle() {
    const QColor b = m_cfg.transparentBackgroundEnabled
        ? QColor(0, 0, 0, 0)
        : m_cfg.bgColor;
    const QColor t = m_cfg.textColor;
    const QColor g = m_cfg.gridColor;

    const QString css = QString(
        "QFrame#panel{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border-radius: 10px;"
        "}"
        "QTableView{"
        "background: transparent;"
        "border: none;"
        "color: rgb(%5,%6,%7);"
        "gridline-color: rgba(%8,%9,%10,%11);"
        "}"
        "QHeaderView::section{"
        "background: transparent;"
        "border: none;"
        "padding: 0 4px;"
        "font-weight: 600;"
        "color: rgb(%5,%6,%7);"
        "}"
        "QAbstractItemView::item{"
        "padding: 0 4px;"
        "}"
    )
        .arg(b.red())
        .arg(b.green())
        .arg(b.blue())
        .arg(b.alpha())
        .arg(t.red())
        .arg(t.green())
        .arg(t.blue())
        .arg(g.red())
        .arg(g.green())
        .arg(g.blue())
        .arg(g.alpha());

    m_panel->setStyleSheet(css);
    m_table->setShowGrid(m_cfg.showGrid);
    m_table->horizontalHeader()->setVisible(m_cfg.showHeader);

#ifdef WIN32
    // Sync palette so Fusion style picks up the correct text / base colors.
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Text, t);
    pal.setColor(QPalette::WindowText, t);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
#endif
}

void FloatingWindow::applyHoverReadingStyle() {
    const QColor t = m_cfg.textColor;
    const QColor g = m_cfg.gridColor;

    // Determine dark/light based on configured background color lightness
    const bool isDark = m_cfg.bgColor.lightness() < 128;

    QColor bg;
    QColor border;
    int radius;

#ifdef Q_OS_MAC
    // macOS: vibrancy style (design.md)
    if (isDark) {
        bg = QColor(30, 30, 30, qRound(0.72 * 255));       // rgba(30,30,30,0.72)
        border = QColor(255, 255, 255, qRound(0.08 * 255)); // rgba(255,255,255,0.08)
    } else {
        bg = QColor(255, 255, 255, qRound(0.72 * 255));     // rgba(255,255,255,0.72)
        border = QColor(255, 255, 255, qRound(0.35 * 255)); // rgba(255,255,255,0.35)
    }
    radius = 16;
#else
    // Windows: acrylic style (design.md)
    if (isDark) {
        bg = QColor(32, 32, 32, qRound(0.80 * 255));        // rgba(32,32,32,0.80)
        border = QColor(255, 255, 255, qRound(0.06 * 255)); // rgba(255,255,255,0.06)
    } else {
        bg = QColor(255, 255, 255, qRound(0.85 * 255));     // rgba(255,255,255,0.85)
        border = QColor(0, 0, 0, qRound(0.08 * 255));       // rgba(0,0,0,0.08)
    }
    radius = 10;
#endif

    const QString css = QString(
        "QFrame#panel{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border-radius: %5px;"
        "border: 1px solid rgba(%6,%7,%8,%9);"
        "}"
        "QTableView{"
        "background: transparent;"
        "border: none;"
        "color: rgb(%10,%11,%12);"
        "gridline-color: rgba(%13,%14,%15,%16);"
        "}"
        "QHeaderView::section{"
        "background: transparent;"
        "border: none;"
        "padding: 0 4px;"
        "font-weight: 600;"
        "color: rgb(%10,%11,%12);"
        "}"
        "QAbstractItemView::item{"
        "padding: 0 4px;"
        "}"
    )
        .arg(bg.red())
        .arg(bg.green())
        .arg(bg.blue())
        .arg(bg.alpha())
        .arg(radius)
        .arg(border.red())
        .arg(border.green())
        .arg(border.blue())
        .arg(border.alpha())
        .arg(t.red())
        .arg(t.green())
        .arg(t.blue())
        .arg(g.red())
        .arg(g.green())
        .arg(g.blue())
        .arg(g.alpha());

    m_panel->setStyleSheet(css);
    m_table->setShowGrid(m_cfg.showGrid);
    m_table->horizontalHeader()->setVisible(m_cfg.showHeader);

#ifdef WIN32
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Text, t);
    pal.setColor(QPalette::WindowText, t);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
#endif
}

void FloatingWindow::setHoverReadingActive(bool active, bool animated) {
    if (m_hoverReadingActive == active && (!m_styleAnimation || m_styleAnimation->state() != QAbstractAnimation::Running)) {
        return;
    }

    m_hoverReadingActive = active;

    if (!animated || !m_styleAnimation) {
        if (m_styleAnimation) {
            m_styleAnimation->stop();
        }
        if (m_hoverReadingActive) {
            applyHoverReadingStyle();
        } else {
            applyStyle();
        }
        return;
    }

    m_styleAnimation->stop();
    m_styleAnimation->setStartValue(0.0);
    m_styleAnimation->setEndValue(1.0);
    m_styleAnimation->start();
}

void FloatingWindow::applyInterpolatedStyle(qreal progress, bool towardsHoverReading) {
    const QColor normalBg = m_cfg.transparentBackgroundEnabled
        ? QColor(0, 0, 0, 0)
        : m_cfg.bgColor;
    const QColor text = m_cfg.textColor;
    const QColor grid = m_cfg.gridColor;

    const bool isDark = m_cfg.bgColor.lightness() < 128;
    QColor hoverBg;
    QColor hoverBorder;
    int hoverRadius;

#ifdef Q_OS_MAC
    if (isDark) {
        hoverBg = QColor(30, 30, 30, qRound(0.72 * 255));
        hoverBorder = QColor(255, 255, 255, qRound(0.08 * 255));
    } else {
        hoverBg = QColor(255, 255, 255, qRound(0.72 * 255));
        hoverBorder = QColor(255, 255, 255, qRound(0.35 * 255));
    }
    hoverRadius = 16;
#else
    if (isDark) {
        hoverBg = QColor(32, 32, 32, qRound(0.80 * 255));
        hoverBorder = QColor(255, 255, 255, qRound(0.06 * 255));
    } else {
        hoverBg = QColor(255, 255, 255, qRound(0.85 * 255));
        hoverBorder = QColor(0, 0, 0, qRound(0.08 * 255));
    }
    hoverRadius = 10;
#endif

    const int normalRadius = 10;
    const QColor normalBorder(0, 0, 0, 0);

    const QColor fromBg = towardsHoverReading ? normalBg : hoverBg;
    const QColor toBg = towardsHoverReading ? hoverBg : normalBg;
    const QColor fromBorder = towardsHoverReading ? normalBorder : hoverBorder;
    const QColor toBorder = towardsHoverReading ? hoverBorder : normalBorder;
    const int fromRadius = towardsHoverReading ? normalRadius : hoverRadius;
    const int toRadius = towardsHoverReading ? hoverRadius : normalRadius;

    const QColor bg = mixColor(fromBg, toBg, progress);
    const QColor border = mixColor(fromBorder, toBorder, progress);
    const int radius = mixInt(fromRadius, toRadius, progress);

    const QString css = QString(
        "QFrame#panel{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border-radius: %5px;"
        "border: 1px solid rgba(%6,%7,%8,%9);"
        "}"
        "QTableView{"
        "background: transparent;"
        "border: none;"
        "color: rgb(%10,%11,%12);"
        "gridline-color: rgba(%13,%14,%15,%16);"
        "}"
        "QHeaderView::section{"
        "background: transparent;"
        "border: none;"
        "padding: 0 4px;"
        "font-weight: 600;"
        "color: rgb(%10,%11,%12);"
        "}"
        "QAbstractItemView::item{"
        "padding: 0 4px;"
        "}"
    )
        .arg(bg.red())
        .arg(bg.green())
        .arg(bg.blue())
        .arg(bg.alpha())
        .arg(radius)
        .arg(border.red())
        .arg(border.green())
        .arg(border.blue())
        .arg(border.alpha())
        .arg(text.red())
        .arg(text.green())
        .arg(text.blue())
        .arg(grid.red())
        .arg(grid.green())
        .arg(grid.blue())
        .arg(grid.alpha());

    m_panel->setStyleSheet(css);
    m_table->setShowGrid(m_cfg.showGrid);
    m_table->horizontalHeader()->setVisible(m_cfg.showHeader);

#ifdef WIN32
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::WindowText, text);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
#endif
}

void FloatingWindow::applyColumns() {
    const QVector<int> columnOrder = normalizedColumnOrder(m_cfg.columnOrder);
    QHeaderView* header = m_table->horizontalHeader();

    // Apply column visual order.
    {
        QSignalBlocker blocker(header);
        header->setSectionsMovable(true);
        for (int visualIndex = 0; visualIndex < columnOrder.size(); ++visualIndex) {
            const int logical = columnOrder[visualIndex];
            const int from = header->visualIndex(logical);
            if (from >= 0 && from != visualIndex) {
                header->moveSection(from, visualIndex);
            }
        }
        header->setSectionsMovable(false);
    }

    // Set visibility for each column.
    for (int i = 0; i < ColCount; ++i) {
        const bool visible = m_cfg.visibleColumns.value(i, true);
        m_table->setColumnHidden(i, !visible);
    }

    adjustWindowSize();
}

void FloatingWindow::adjustWindowSize() {
    // Auto-size each visible column to its content.
    for (int i = 0; i < ColCount; ++i) {
        if (m_table->isColumnHidden(i)) {
            continue;
        }
        m_table->setColumnWidth(i, autoColumnWidthFromContent(i));
    }

    // Width = sum of visible column widths.
    int totalWidth = 0;
    for (int i = 0; i < ColCount; ++i) {
        if (m_table->isColumnHidden(i)) {
            continue;
        }
        totalWidth += m_table->columnWidth(i);
    }

    // Height = header + rows.
    int totalHeight = 0;
    if (m_table->horizontalHeader()->isVisible()) {
        totalHeight += m_table->horizontalHeader()->sizeHint().height();
    }
    totalHeight += m_model->rowCount() * m_table->verticalHeader()->defaultSectionSize();

    const int safeWidth = qMax(totalWidth, 1);
    const int safeHeight = qMax(totalHeight, 1);
    setFixedSize(safeWidth, safeHeight);
}

int FloatingWindow::autoColumnWidthFromContent(int column) const {
    const int minWidth = 16;
    int width = minWidth;

    const QString headerText =
        m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
    const QFontMetrics headerFm(m_table->horizontalHeader()->font());
    width = qMax(width, headerFm.horizontalAdvance(headerText) + 24);

    const QFontMetrics cellFm(m_table->font());
    const int rows = m_model->rowCount();
    for (int r = 0; r < rows; ++r) {
        const QString text = m_model->data(m_model->index(r, column), Qt::DisplayRole).toString();
        width = qMax(width, cellFm.horizontalAdvance(text) + 24);
    }

    const int maxW = m_cfg.columnMaxWidths.value(column, 0);
    if (maxW > 0) {
        width = qMin(width, qMax(maxW, minWidth));
    }

    return width;
}

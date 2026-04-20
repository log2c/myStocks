#include "floating_window.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPalette>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStyleFactory>
#include <QVBoxLayout>

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

} // namespace

FloatingWindow::FloatingWindow(QuoteModel* model, QWidget* parent)
    : QWidget(parent)
    , m_model(model) {
    // Qt::Tool suppresses the taskbar entry on Windows while keeping top-most behavior
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
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
}

bool FloatingWindow::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched);

    switch (event->type()) {
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

    const double effectiveOpacity = m_cfg.transparentBackgroundEnabled
        ? static_cast<double>(qBound(0, m_cfg.transparentBackgroundOpacity, 100)) / 100.0
        : qBound(0.0, m_cfg.opacity, 1.0);
    setWindowOpacity(qBound(0.0, effectiveOpacity, 1.0));

    applyStyle();
    applyColumns();
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
    }
    QWidget::mouseReleaseEvent(event);
}

void FloatingWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

    // Re-raise on show to keep the floating window above normal app windows.
    raise();
    activateWindow();
}

void FloatingWindow::applyStyle() {
    const QColor b = m_cfg.transparentBackgroundEnabled
        ? QColor(0, 0, 0, 0)
        : m_cfg.bgColor;
    const QColor t = m_cfg.textColor;

    const QString css = QString(
        "QFrame#panel{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border-radius: 10px;"
        "}"
        "QTableView{"
        "background: transparent;"
        "border: none;"
        "color: rgb(%5,%6,%7);"
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
        .arg(t.blue());

    m_panel->setStyleSheet(css);
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

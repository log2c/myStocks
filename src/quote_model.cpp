#include "quote_model.h"

#include "i18n.h"

#include <QColor>

#include <cmath>

namespace {

QVector<int> visibleLogicalColumns(const AppConfig& cfg) {
    QVector<int> columns;
    columns.reserve(ColCount);

    for (int logical : cfg.columnOrder) {
        if (logical < 0 || logical >= ColCount) {
            continue;
        }
        if (!cfg.visibleColumns.value(logical, true) || columns.contains(logical)) {
            continue;
        }
        columns.push_back(logical);
    }

    for (int logical = 0; logical < ColCount; ++logical) {
        if (!cfg.visibleColumns.value(logical, true) || columns.contains(logical)) {
            continue;
        }
        columns.push_back(logical);
    }

    return columns;
}

bool isHongKongCode(const QString& rawCode) {
    return rawCode.trimmed().toLower().startsWith("hk");
}

// Returns true for ChiNext (创业板 300/301) and STAR Market (科创板 688/689)
// which use a 20% limit but we show ▲▲▲/▼▼▼ at ±10% per display rules.
bool isHighLimitCode(const QString& rawCode) {
    QString code = rawCode.trimmed().toLower();
    if (code.startsWith("sh") || code.startsWith("sz")) {
        code = code.mid(2);
    }
    return code.startsWith("300") || code.startsWith("301")
        || code.startsWith("688") || code.startsWith("689");
}

QString indicatorSymbol(const QString& code, double pct) {
    if (std::isnan(pct)) {
        return QStringLiteral("--");
    }
    const double limitPct = isHighLimitCode(code) ? 10.0 : 9.9;
    if (pct >= limitPct)  return QStringLiteral("\u25b2\u25b2\u25b2"); // ▲▲▲
    if (pct >= 3.0)       return QStringLiteral("\u25b2\u25b2");      // ▲▲
    if (pct > 0.0)        return QStringLiteral("\u25b2");            // ▲
    if (pct < 0.0) {
        if (pct > -3.0)        return QStringLiteral("\u25bc");            // ▼
        if (pct > -limitPct)   return QStringLiteral("\u25bc\u25bc");      // ▼▼
        return QStringLiteral("\u25bc\u25bc\u25bc");                        // ▼▼▼
    }
    return QStringLiteral("-");
}

// Returns -3..+3: sign = direction, abs = intensity (1=light, 2=normal, 3=strong)
int indicatorLevel(const QString& code, double pct) {
    if (std::isnan(pct)) return 0;
    const double limitPct = isHighLimitCode(code) ? 10.0 : 9.9;
    if (pct >= limitPct)        return  3;
    if (pct >= 3.0)             return  2;
    if (pct > 0.0)              return  1;
    if (pct == 0.0)             return  0;
    if (pct > -3.0)             return -1;
    if (pct > -limitPct)        return -2;
    return -3;
}

// Adjust HSV value of a color by delta, clamped to [0, 255].
QColor adjustedValue(const QColor& base, int delta) {
    const QColor h = base.toHsv();
    const int v = qBound(0, h.value() + delta, 255);
    return QColor::fromHsv(h.hsvHue(), h.hsvSaturation(), v, h.alpha());
}

QString withHongKongNamePrefix(const QString& code, const QString& name) {
    if (!isHongKongCode(code)) {
        return name;
    }

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return name;
    }

    if (trimmed.startsWith("H ")) {
        return trimmed;
    }

    return "H " + trimmed;
}

QString formatNetInflowYi(double value) {
    if (!std::isfinite(value)) {
        return QStringLiteral("--");
    }

    const double yi = value / 100000000.0;
    QString text = QString::number(yi, 'f', 2);
    if (yi > 0.0) {
        text.prepend('+');
    }
    text.append(QStringLiteral("亿"));
    return text;
}

} // namespace

QuoteModel::QuoteModel(QObject* parent)
    : QAbstractTableModel(parent) {
    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(200);
    connect(m_blinkTimer, &QTimer::timeout, this, [this]() {
        QVector<int> done;
        for (auto it = m_blinkCounters.begin(); it != m_blinkCounters.end(); ++it) {
            --it.value();
            if (it.value() <= 0) {
                done.push_back(it.key());
            }
            const QModelIndex idx = index(it.key(), ColIndicator);
            emit dataChanged(idx, idx, {Qt::ForegroundRole});
        }
        for (int row : done) {
            m_blinkCounters.remove(row);
        }
        if (m_blinkCounters.isEmpty()) {
            m_blinkTimer->stop();
        }
    });
}

int QuoteModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size()
        + (hasHotSectorRow() ? 1 : 0)
        + (hasHotConceptRow() ? 1 : 0);
}

int QuoteModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return ColCount;
}

QVariant QuoteModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }

    if (orientation == Qt::Horizontal) {
        const QStringList headers = i18n::columnNames(m_language);
        if (section >= 0 && section < headers.size()) {
            return headers.at(section);
        }
    }

    return {};
}

QVariant QuoteModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const RowKind kind = rowKind(index.row());
    if (kind != RowKindQuote) {
        const int labelCol = firstVisibleLogicalColumn();
        const int contentCol = firstContentLogicalColumn();
        const QString label = specialRowLabel(index.row());
        const QString text = specialRowText(index.row());

        if (role == Qt::DisplayRole) {
            if (index.column() == labelCol) {
                if (contentCol == labelCol && !text.isEmpty()) {
                    return QStringLiteral("%1 %2").arg(label, text);
                }
                return label;
            }
            if (contentCol != labelCol && index.column() == contentCol) {
                return {};
            }
            return {};
        }

        if (role == Qt::TextAlignmentRole) {
            if (index.column() == labelCol) {
                return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
            }
            return specialRowHasData(index.row())
                ? static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter)
                : static_cast<int>(Qt::AlignCenter);
        }

        if (role == Qt::ForegroundRole) {
            const bool lightHoverText = m_hoverReadingVisualActive
                && m_cfg.hoverReadingEnabled
                && m_cfg.hoverReadingUiMode.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0;
            return lightHoverText
                ? QColor(QStringLiteral("#1F1F1F"))
                : m_cfg.textColor;
        }

        return {};
    }

    const QuoteItem& q = m_rows[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColCode:
            return q.code;
        case ColName:
            return withHongKongNamePrefix(q.code, q.name);
        case ColPrice:
            return std::isnan(q.price) ? QString("--") : QString::number(q.price, 'f', 3);
        case ColPct:
            return std::isnan(q.pct) ? QString("--") : formatSigned(q.pct, 2, true);
        case ColChange:
            return std::isnan(q.change) ? QString("--") : formatSigned(q.change, 3, false);
        case ColIndicator:
            return indicatorSymbol(q.code, q.pct);
        default:
            return {};
        }
    }

    if (role == Qt::TextAlignmentRole) {
        if (index.column() <= ColName) {
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }

    if (role == Qt::ForegroundRole) {
        const bool lightHoverText = m_hoverReadingVisualActive
            && m_cfg.hoverReadingEnabled
            && m_cfg.hoverReadingUiMode.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0;
        const QColor defaultText = lightHoverText
            ? QColor(QStringLiteral("#1F1F1F"))
            : m_cfg.textColor;

        if (index.column() == ColIndicator) {
            // Determine base color from direction
            const int level = indicatorLevel(q.code, q.pct);
            QColor base;
            if (level > 0)      base = m_cfg.upColor;
            else if (level < 0) base = m_cfg.downColor;
            else                base = m_cfg.flatColor;

            // Derive intensity: level 1 → dim, level 2 → normal, level 3 → bright
            const int absLevel = std::abs(level);
            QColor indicatorColor;
            if      (absLevel == 1) indicatorColor = adjustedValue(base, -50);
            else if (absLevel == 3) indicatorColor = adjustedValue(base, +50);
            else                    indicatorColor = base;

            // Blink: alternate between white and the indicator color
            if (m_blinkCounters.contains(index.row())) {
                const int count = m_blinkCounters.value(index.row());
                if (count % 2 == 0) {
                    return QColor(Qt::white);
                }
            }
            return indicatorColor;
        }
        if (m_cfg.simpleModeEnabled) {
            return defaultText;
        }
        if (index.column() >= ColPrice) {
            if (std::isnan(q.pct)) {
                return defaultText;
            }
            if (q.pct > 0.0) {
                return m_cfg.upColor;
            }
            if (q.pct < 0.0) {
                return m_cfg.downColor;
            }
            return m_cfg.flatColor;
        }
        return defaultText;
    }

    return {};
}

void QuoteModel::setStocks(const QVector<StockItem>& stocks) {
    beginResetModel();
    m_rows.clear();
    m_rowByCode.clear();

    m_rows.reserve(stocks.size());
    for (int i = 0; i < stocks.size(); ++i) {
        QuoteItem q;
        q.code = stocks[i].code;
        q.name = stocks[i].name;
        m_rows.push_back(q);
        m_rowByCode.insert(q.code, i);
    }

    endResetModel();
}

void QuoteModel::updateQuotes(const QVector<QuoteItem>& quotes) {
    for (const QuoteItem& q : quotes) {
        if (!m_rowByCode.contains(q.code)) {
            continue;
        }

        const int row = m_rowByCode.value(q.code);
        m_rows[row].name = q.name.isEmpty() ? m_rows[row].name : q.name;

        // Detect indicator change before updating pct
        const QString oldIndicator = indicatorSymbol(m_rows[row].code, m_rows[row].pct);
        const QString newIndicator = indicatorSymbol(q.code, q.pct);
        const bool indicatorChanged = (oldIndicator != newIndicator)
            && !std::isnan(m_rows[row].pct)  // skip initial load
            && !std::isnan(q.pct);

        m_rows[row].price = q.price;
        m_rows[row].pct = q.pct;
        m_rows[row].change = q.change;

        if (indicatorChanged && m_cfg.blinkReminderEnabled) {
            m_blinkCounters[row] = 4; // 4 half-periods = 2 full blinks
            if (!m_blinkTimer->isActive()) {
                m_blinkTimer->start();
            }
        }

        emit dataChanged(index(row, 0), index(row, ColCount - 1));
    }
}

void QuoteModel::setHotSectors(const QVector<HotRankItem>& items) {
    m_hotSectors = items;
    emitSpecialRowsChanged();
}

void QuoteModel::setHotConcepts(const QVector<HotRankItem>& items) {
    m_hotConcepts = items;
    emitSpecialRowsChanged();
}

void QuoteModel::setConfig(const AppConfig& cfg) {
    const bool hadHotSectorRow = hasHotSectorRow();
    const bool hadHotConceptRow = hasHotConceptRow();
    m_cfg = cfg;
    const bool rowCountChanged = hadHotSectorRow != hasHotSectorRow()
        || hadHotConceptRow != hasHotConceptRow();
    if (rowCountChanged) {
        beginResetModel();
        endResetModel();
        return;
    }
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, ColCount - 1), {Qt::ForegroundRole});
    }
    emitSpecialRowsChanged();
}

void QuoteModel::setHoverReadingVisualState(bool active) {
    if (m_hoverReadingVisualActive == active) {
        return;
    }

    m_hoverReadingVisualActive = active;
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, ColCount - 1), {Qt::ForegroundRole});
    }
}

void QuoteModel::setLanguage(const QString& language) {
    m_language = i18n::resolveLanguage(language);
    emit headerDataChanged(Qt::Horizontal, 0, ColCount - 1);
}

QString QuoteModel::trayTooltipText() const {
    QStringList lines;
    for (const QuoteItem& q : m_rows) {
        const QString price = std::isnan(q.price)
            ? QStringLiteral("--")
            : QString::number(q.price, 'f', 3);
        const QString pct = std::isnan(q.pct)
            ? QStringLiteral("--")
            : formatSigned(q.pct, 2, true);
        const QString indicator = indicatorSymbol(q.code, q.pct);
        lines.append(QString("%1%2-%3(%4)").arg(indicator, q.name, price, pct));
    }
    return lines.join('\n');
}

QuoteModel::RowKind QuoteModel::rowKind(int row) const {
    if (row < 0 || row >= rowCount()) {
        return RowKindQuote;
    }
    if (row < m_rows.size()) {
        return RowKindQuote;
    }
    if (hasHotSectorRow() && row == hotSectorRowIndex()) {
        return RowKindHotSector;
    }
    if (hasHotConceptRow() && row == hotConceptRowIndex()) {
        return RowKindHotConcept;
    }
    return RowKindQuote;
}

QString QuoteModel::specialRowLabel(int row) const {
    switch (rowKind(row)) {
    case RowKindHotSector:
        return i18n::t("quote.hotSector", m_language);
    case RowKindHotConcept:
        return i18n::t("quote.hotConcept", m_language);
    case RowKindQuote:
    default:
        return {};
    }
}

QString QuoteModel::specialRowText(int row) const {
    const QStringList entries = specialRowEntries(row);
    if (entries.isEmpty()) {
        return i18n::t("quote.noData", m_language);
    }

    return entries.join(QStringLiteral("    "));
}

QStringList QuoteModel::specialRowEntries(int row) const {
    switch (rowKind(row)) {
    case RowKindHotSector:
        return formatHotRankEntries(m_hotSectors);
    case RowKindHotConcept:
        return formatHotRankEntries(m_hotConcepts);
    case RowKindQuote:
    default:
        return {};
    }
}

int QuoteModel::specialRowEntryCount(int row) const {
    return specialRowEntries(row).size();
}

QString QuoteModel::specialRowEntryText(int row, int entryIndex) const {
    const QStringList entries = specialRowEntries(row);
    if (entryIndex < 0 || entryIndex >= entries.size()) {
        return {};
    }
    return entries.at(entryIndex);
}

QColor QuoteModel::specialRowEntryColor(int row, int entryIndex) const {
    const QVector<HotRankItem>* items = nullptr;
    switch (rowKind(row)) {
    case RowKindHotSector:
        items = &m_hotSectors;
        break;
    case RowKindHotConcept:
        items = &m_hotConcepts;
        break;
    case RowKindQuote:
    default:
        return m_cfg.textColor;
    }

    if (!items || entryIndex < 0 || entryIndex >= items->size()) {
        return m_cfg.textColor;
    }

    return hotRankColorForPct(items->at(entryIndex).pct);
}

bool QuoteModel::specialRowHasData(int row) const {
    switch (rowKind(row)) {
    case RowKindHotSector:
        return !m_hotSectors.isEmpty();
    case RowKindHotConcept:
        return !m_hotConcepts.isEmpty();
    case RowKindQuote:
    default:
        return false;
    }
}

int QuoteModel::firstVisibleLogicalColumn() const {
    const QVector<int> columns = visibleLogicalColumns(m_cfg);
    return columns.isEmpty() ? 0 : columns.first();
}

int QuoteModel::firstContentLogicalColumn() const {
    const QVector<int> columns = visibleLogicalColumns(m_cfg);
    if (columns.size() >= 2) {
        return columns.at(1);
    }
    return columns.isEmpty() ? 0 : columns.first();
}

QString QuoteModel::formatSigned(double value, int precision, bool percent) {
    QString s = QString::number(value, 'f', precision);
    if (value > 0.0) {
        s.prepend('+');
    }
    if (percent) {
        s.append('%');
    }
    return s;
}

QColor QuoteModel::hotRankColorForPct(double pct) const {
    if (std::isnan(pct)) {
        return m_cfg.flatColor;
    }
    if (pct > 0.0) {
        return m_cfg.upColor;
    }
    if (pct < 0.0) {
        return m_cfg.downColor;
    }
    return m_cfg.flatColor;
}

bool QuoteModel::hasHotSectorRow() const {
    return m_cfg.hotRankEnabled && m_cfg.hotSectorVisible;
}

bool QuoteModel::hasHotConceptRow() const {
    return m_cfg.hotRankEnabled && m_cfg.hotConceptVisible;
}

int QuoteModel::hotSectorRowIndex() const {
    return m_rows.size();
}

int QuoteModel::hotConceptRowIndex() const {
    return m_rows.size() + (hasHotSectorRow() ? 1 : 0);
}

void QuoteModel::emitSpecialRowsChanged() {
    if (hasHotSectorRow()) {
        const int row = hotSectorRowIndex();
        emit dataChanged(index(row, 0), index(row, ColCount - 1));
    }
    if (hasHotConceptRow()) {
        const int row = hotConceptRowIndex();
        emit dataChanged(index(row, 0), index(row, ColCount - 1));
    }
}

QStringList QuoteModel::formatHotRankEntries(const QVector<HotRankItem>& items) const {
    QStringList parts;
    parts.reserve(items.size());

    for (const HotRankItem& item : items) {
        const QString text = formatHotRankEntry(item);
        if (!text.isEmpty()) {
            parts.push_back(text);
        }
    }

    return parts;
}

QString QuoteModel::formatHotRankEntry(const HotRankItem& item) const {
    const QString name = item.name.trimmed();
    if (name.isEmpty()) {
        return {};
    }

    const QString pct = std::isnan(item.pct)
        ? QStringLiteral("--")
        : formatSigned(item.pct, 2, true);
    const QString netInflow = formatNetInflowYi(item.mainNetInflow);
    return QStringLiteral("%1 %2 %3").arg(name, pct, netInflow);
}

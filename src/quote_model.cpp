#include "quote_model.h"

#include "i18n.h"

#include <QColor>

#include <cmath>

namespace {

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
    return m_rows.size();
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
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
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
            return m_cfg.textColor;
        }
        if (index.column() >= ColPrice) {
            if (std::isnan(q.pct)) {
                return m_cfg.textColor;
            }
            if (q.pct > 0.0) {
                return m_cfg.upColor;
            }
            if (q.pct < 0.0) {
                return m_cfg.downColor;
            }
            return m_cfg.flatColor;
        }
        return m_cfg.textColor;
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

void QuoteModel::setConfig(const AppConfig& cfg) {
    m_cfg = cfg;
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

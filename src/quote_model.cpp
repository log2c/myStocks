#include "quote_model.h"

#include "i18n.h"

#include <QColor>

#include <cmath>

QuoteModel::QuoteModel(QObject* parent)
    : QAbstractTableModel(parent) {}

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
            return q.name;
        case ColPrice:
            return std::isnan(q.price) ? QString("--") : QString::number(q.price, 'f', 3);
        case ColPct:
            return std::isnan(q.pct) ? QString("--") : formatSigned(q.pct, 2, true);
        case ColChange:
            return std::isnan(q.change) ? QString("--") : formatSigned(q.change, 3, false);
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
        m_rows[row].price = q.price;
        m_rows[row].pct = q.pct;
        m_rows[row].change = q.change;

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

#pragma once

#include "types.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QTimer>

class QuoteModel : public QAbstractTableModel {
public:
    explicit QuoteModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setStocks(const QVector<StockItem>& stocks);
    void updateQuotes(const QVector<QuoteItem>& quotes);
    void setConfig(const AppConfig& cfg);
    void setLanguage(const QString& language);

private:
    static QString formatSigned(double value, int precision, bool percent);

private:
    QVector<QuoteItem> m_rows;
    QHash<QString, int> m_rowByCode;
    AppConfig m_cfg;
    QString m_language = "en_US";

    // blink state: row -> remaining half-periods (4 = 2 full blinks)
    QHash<int, int> m_blinkCounters;
    QTimer* m_blinkTimer = nullptr;
    bool m_blinkPhase = false; // true = highlight phase
};

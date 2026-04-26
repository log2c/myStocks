#pragma once

#include "types.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QHash>
#include <QTimer>

class QuoteModel : public QAbstractTableModel {
public:
    enum RowKind {
        RowKindQuote = 0,
        RowKindHotSector = 1,
        RowKindHotConcept = 2,
    };

    explicit QuoteModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setStocks(const QVector<StockItem>& stocks);
    void updateQuotes(const QVector<QuoteItem>& quotes);
    void setHotSectors(const QVector<HotRankItem>& items);
    void setHotConcepts(const QVector<HotRankItem>& items);
    void setConfig(const AppConfig& cfg);
    void setHoverReadingVisualState(bool active);
    void setLanguage(const QString& language);
    QString trayTooltipText() const;
    RowKind rowKind(int row) const;
    QString specialRowLabel(int row) const;
    QString specialRowText(int row) const;
    QStringList specialRowEntries(int row) const;
    int specialRowEntryCount(int row) const;
    QString specialRowEntryText(int row, int entryIndex) const;
    QColor specialRowEntryColor(int row, int entryIndex) const;
    bool specialRowHasData(int row) const;
    int firstVisibleLogicalColumn() const;
    int firstContentLogicalColumn() const;

private:
    static QString formatSigned(double value, int precision, bool percent);
    bool hasHotSectorRow() const;
    bool hasHotConceptRow() const;
    int hotSectorRowIndex() const;
    int hotConceptRowIndex() const;
    void emitSpecialRowsChanged();
    QColor hotRankColorForPct(double pct) const;
    QString formatHotRankEntry(const HotRankItem& item) const;
    QStringList formatHotRankEntries(const QVector<HotRankItem>& items) const;

private:
    QVector<QuoteItem> m_rows;
    QVector<HotRankItem> m_hotSectors;
    QVector<HotRankItem> m_hotConcepts;
    QHash<QString, int> m_rowByCode;
    AppConfig m_cfg;
    QString m_language = "en_US";

    // blink state: row -> remaining half-periods (4 = 2 full blinks)
    QHash<int, int> m_blinkCounters;
    QTimer* m_blinkTimer = nullptr;
    bool m_blinkPhase = false; // true = highlight phase
    bool m_hoverReadingVisualActive = false;
};

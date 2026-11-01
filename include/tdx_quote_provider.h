#pragma once
#include "quote_provider.h"

#include <QHash>
#include <QPair>

class QTimer;

class TdxQuoteProvider : public IQuoteProvider {
public:
    // `usePush` is false for the secondary instance that serves the market
    // breadth window: the bridge holds one push subscription per process, and
    // that one belongs to the watchlist.
    explicit TdxQuoteProvider(QObject* parent = nullptr, bool usePush = true);
    void applyConfig(const AppConfig& cfg) override;
    void fetchQuotes(const QVector<StockItem>& stocks, bool force = false) override;
private:
    void subscribePush(const QVector<StockItem>& stocks);
    void pollPush();
    void stopPush();

    AppConfig m_cfg;
    bool m_usePush = true;
    bool m_pending = false;
    int m_generation = 0;
    // The push channel keeps a dedicated 0x0547 socket open and caches the last
    // frame per instrument, so a poll here reads that cache instead of the
    // network. The signature is compared as plain text; the list rarely changes.
    QString m_pushSignature;
    bool m_pushStarting = false;
    bool m_pushPolling = false;
    bool m_pushHealthy = false;
    bool m_pushCoversAll = false;
    qint64 m_lastSnapshotMs = 0;
    QTimer* m_pushTimer = nullptr;
    // Last price and percentage already handed to the model, so a list whose
    // prices did not move is not repainted.
    QHash<QString, QPair<double, double>> m_pushSeen;
};

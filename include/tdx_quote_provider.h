#pragma once
#include "quote_provider.h"

class TdxQuoteProvider : public IQuoteProvider {
public:
    explicit TdxQuoteProvider(QObject* parent = nullptr);
    void applyConfig(const AppConfig& cfg) override;
    void fetchQuotes(const QVector<StockItem>& stocks) override;
private:
    AppConfig m_cfg;
    bool m_pending = false;
    int m_generation = 0;
};

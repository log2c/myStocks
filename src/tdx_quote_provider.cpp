#include "tdx_quote_provider.h"
#include "tdx_bridge.h"
#include <QJsonArray>

namespace {
double number(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toDouble() : qQNaN();
}
}
TdxQuoteProvider::TdxQuoteProvider(QObject* parent) : IQuoteProvider(parent) {}
void TdxQuoteProvider::applyConfig(const AppConfig& cfg) {
    IQuoteProvider::applyConfig(cfg);
    m_cfg = cfg;
    ++m_generation;
    m_pending = false;
}
void TdxQuoteProvider::fetchQuotes(const QVector<StockItem>& stocks) {
    if (m_pending || stocks.isEmpty()) return;
    QJsonArray items;
    QHash<QString, StockItem> originals;
    for (const StockItem& stock : stocks) {
        if (originals.contains(stock.code)) continue;
        items.append(QJsonObject{{QStringLiteral("code"), stock.code}, {QStringLiteral("name"), stock.name}});
        originals.insert(stock.code, stock);
    }
    m_pending = true;
    const int generation = m_generation;
    tdx_bridge::request(this, m_cfg,
        {{QStringLiteral("operation"), QStringLiteral("quotes")}, {QStringLiteral("stocks"), items}},
        [this, generation, originals](const QJsonObject& object, const QString& message) {
            if (generation != m_generation) return;
            m_pending = false;
            if (!message.isEmpty()) { emit error(message); return; }
            QVector<QuoteItem> result;
            for (const QJsonValue& value : object.value(QStringLiteral("quotes")).toArray()) {
                const QJsonObject row = value.toObject();
                const QString code = row.value(QStringLiteral("code")).toString();
                if (!originals.contains(code)) continue;
                QuoteItem item;
                item.code = code;
                item.name = row.value(QStringLiteral("name")).toString();
                item.price = number(row, QStringLiteral("price"));
                item.pct = number(row, QStringLiteral("pct"));
                item.change = number(row, QStringLiteral("change"));
                item.riseSpeed = number(row, QStringLiteral("riseSpeed"));
                item.cost = originals.value(code).cost;
                result.push_back(item);
            }
            if (!result.isEmpty()) emit quotesReady(result);
            QStringList errors;
            for (const auto& value : object.value(QStringLiteral("errors")).toArray()) errors << value.toString();
            if (!errors.isEmpty()) emit error(QStringLiteral("通达信: %1").arg(errors.join(QStringLiteral("; "))));
            else if (result.isEmpty()) emit error(QStringLiteral("通达信未返回行情"));
        });
}

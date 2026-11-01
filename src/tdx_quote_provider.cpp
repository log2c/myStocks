#include "tdx_quote_provider.h"
#include "tdx_bridge.h"
#include "watchlist_utils.h"
#include <QDateTime>
#include <QJsonArray>
#include <QTimer>
#include <cmath>

namespace {
double number(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toDouble() : qQNaN();
}

constexpr int kPushPollMs = 100;

// Prices compare exactly, but an unquoted indicator is NaN and must still count
// as unchanged or a pre-open list would repaint on every poll.
bool sameValue(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}
}

TdxQuoteProvider::TdxQuoteProvider(QObject* parent, bool usePush)
    : IQuoteProvider(parent), m_usePush(usePush) {
    if (!m_usePush) return;
    m_pushTimer = new QTimer(this);
    m_pushTimer->setInterval(kPushPollMs);
    connect(m_pushTimer, &QTimer::timeout, this, &TdxQuoteProvider::pollPush);
    m_pushTimer->start();
}

void TdxQuoteProvider::applyConfig(const AppConfig& cfg) {
    IQuoteProvider::applyConfig(cfg);
    m_cfg = cfg;
    ++m_generation;
    m_pending = false;
    // Servers, proxy, interface and the polling interval all feed the push
    // socket, and the bridge reads them only when it (re)starts a subscription.
    stopPush();
}

void TdxQuoteProvider::fetchQuotes(const QVector<StockItem>& stocks, bool force) {
    if (stocks.isEmpty()) return;
    if (m_usePush) subscribePush(stocks);
    if (m_pending) return;

    // When the push channel carries every price, the full snapshot only has to
    // keep names, rise speed and the reset/auction state current. Any instrument
    // it cannot carry keeps the snapshot on the fast cadence.
    const bool pushCarriesPrices = m_usePush && m_pushHealthy && m_pushCoversAll;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 snapshotMs = qint64(qBound(1, m_cfg.quoteSnapshotSecs, 300)) * 1000;
    if (!force && pushCarriesPrices && m_lastSnapshotMs > 0
        && now - m_lastSnapshotMs < snapshotMs) {
        return;
    }

    QJsonArray items;
    QHash<QString, StockItem> originals;
    for (const StockItem& stock : stocks) {
        if (!watchlist_utils::isTdxCompatibleCode(stock.code) || originals.contains(stock.code)) continue;
        items.append(QJsonObject{{QStringLiteral("code"), stock.code}, {QStringLiteral("name"), stock.name}});
        originals.insert(stock.code, stock);
    }
    if (items.isEmpty()) return;
    m_pending = true;
    m_lastSnapshotMs = now;
    const int generation = m_generation;
    // Only the primary watchlist instance asks for the summary board; the index
    // instance shares the bridge but has no breadth row to fill.
    const bool includeBreadth = m_usePush && m_cfg.marketBreadthEnabled;
    tdx_bridge::request(this, m_cfg,
        {{QStringLiteral("operation"), QStringLiteral("quotes")},
            {QStringLiteral("stocks"), items},
            {QStringLiteral("includeBreadth"), includeBreadth}},
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
                item.preClose = number(row, QStringLiteral("preClose"));
                item.resetData = row.value(QStringLiteral("resetData")).toBool();
                item.pct = number(row, QStringLiteral("pct"));
                item.change = number(row, QStringLiteral("change"));
                item.riseSpeed = number(row, QStringLiteral("riseSpeed"));
                item.cost = originals.value(code).cost;
                result.push_back(item);
            }
            if (!result.isEmpty()) emit quotesReady(result);
            const QJsonValue breadthValue = object.value(QStringLiteral("breadth"));
            if (breadthValue.isObject()) {
                const QJsonObject row = breadthValue.toObject();
                MarketBreadthCounts counts;
                counts.upCount = row.value(QStringLiteral("upCount")).toInt(-1);
                counts.flatCount = row.value(QStringLiteral("flatCount")).toInt(-1);
                counts.downCount = row.value(QStringLiteral("downCount")).toInt(-1);
                counts.limitUpCount = row.value(QStringLiteral("limitUpCount")).toInt();
                counts.limitDownCount = row.value(QStringLiteral("limitDownCount")).toInt();
                counts.lastUpdatedAtMs = qint64(row.value(QStringLiteral("lastUpdatedAtMs")).toDouble());
                if (counts.upCount >= 0 && counts.flatCount >= 0 && counts.downCount >= 0) {
                    emit breadthReady(counts);
                }
            }
            QStringList errors;
            for (const auto& value : object.value(QStringLiteral("errors")).toArray()) errors << value.toString();
            if (!errors.isEmpty()) emit error(QStringLiteral("通达信: %1").arg(errors.join(QStringLiteral("; "))));
            else if (result.isEmpty()) emit error(QStringLiteral("通达信未返回行情"));
        });
}

void TdxQuoteProvider::subscribePush(const QVector<StockItem>& stocks) {
    QJsonArray items;
    QStringList codes;
    for (const StockItem& stock : stocks) {
        if (!watchlist_utils::isTdxCompatibleCode(stock.code)) continue;
        codes << stock.code;
        items.append(QJsonObject{{QStringLiteral("code"), stock.code}, {QStringLiteral("name"), stock.name}});
    }
    if (items.isEmpty()) {
        stopPush();
        return;
    }
    const QString signature = codes.join(QLatin1Char('\n'));
    if (signature == m_pushSignature || m_pushStarting) return;
    m_pushSignature = signature;
    m_pushStarting = true;
    const int generation = m_generation;
    // The bridge clamps this again; sending the setting keeps one cadence knob.
    const int intervalMs = qBound(500, m_cfg.pollMs, 10000);
    tdx_bridge::request(this, m_cfg,
        {{QStringLiteral("operation"), QStringLiteral("push_start")},
            {QStringLiteral("stocks"), items},
            {QStringLiteral("pushIntervalMs"), intervalMs}},
        [this, generation](const QJsonObject& object, const QString& message) {
            m_pushStarting = false;
            if (generation != m_generation) return;
            if (!message.isEmpty()) {
                // A dead push socket only means the snapshot drives the list, so
                // folding the subscription is enough; there is nothing to report.
                m_pushSignature.clear();
                m_pushCoversAll = false;
                m_pushHealthy = false;
                return;
            }
            // Anything the channel could not take stays on the snapshot, which
            // must then keep the fast cadence.
            m_pushCoversAll = object.value(QStringLiteral("skipped")).toInt(1) == 0
                && object.value(QStringLiteral("overLimit")).toInt(1) == 0;
        });
}

void TdxQuoteProvider::pollPush() {
    if (m_pushSignature.isEmpty() || m_pushPolling || m_pushStarting) return;
    m_pushPolling = true;
    tdx_bridge::request(this, m_cfg,
        {{QStringLiteral("operation"), QStringLiteral("push_poll")}},
        [this](const QJsonObject& object, const QString& message) {
            m_pushPolling = false;
            if (!message.isEmpty()) {
                m_pushHealthy = false;
                return;
            }
            const qint64 ageMs = qint64(object.value(QStringLiteral("ageMs")).toDouble(-1.0));
            // A channel that refreshes every pollMs can legitimately be that
            // stale, so the cutoff scales with it instead of pinning to 5 s.
            const qint64 maxAgeMs = qMax<qint64>(3000, qint64(qBound(500, m_cfg.pollMs, 10000)) * 3);
            m_pushHealthy = object.value(QStringLiteral("connected")).toBool()
                && ageMs >= 0 && ageMs <= maxAgeMs;
            if (!m_pushHealthy) {
                // The snapshot is the fallback and already owns these rows.
                return;
            }
            QVector<QuoteItem> result;
            for (const QJsonValue& value : object.value(QStringLiteral("quotes")).toArray()) {
                const QJsonObject row = value.toObject();
                const QString code = row.value(QStringLiteral("code")).toString();
                if (code.isEmpty()) continue;
                const double price = number(row, QStringLiteral("price"));
                const double pct = number(row, QStringLiteral("pct"));
                if (!std::isfinite(price)) continue;
                const auto previous = m_pushSeen.constFind(code);
                if (previous != m_pushSeen.constEnd()
                    && sameValue(previous->first, price) && sameValue(previous->second, pct)) {
                    continue;
                }
                m_pushSeen.insert(code, {price, pct});
                QuoteItem item;
                item.code = code;
                item.price = price;
                item.preClose = number(row, QStringLiteral("preClose"));
                item.pct = pct;
                item.change = number(row, QStringLiteral("change"));
                // A refresh frame carries no name and no rise speed, so the row
                // keeps the values its last snapshot supplied.
                item.partial = true;
                result.push_back(item);
            }
            if (!result.isEmpty()) emit quotesReady(result);
        });
}

void TdxQuoteProvider::stopPush() {
    m_pushSeen.clear();
    m_pushHealthy = false;
    m_pushCoversAll = false;
    m_lastSnapshotMs = 0;
    if (m_pushSignature.isEmpty()) return;
    m_pushSignature.clear();
    tdx_bridge::request(this, m_cfg,
        {{QStringLiteral("operation"), QStringLiteral("push_stop")}},
        [](const QJsonObject&, const QString&) {});
}

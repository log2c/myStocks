#include "limit_pool_provider.h"

#include "app_constants.h"
#include "network_logger.h"
#include "network_utils.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>

namespace {
QDate chinaToday() {
    return QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("Asia/Shanghai")).date();
}
}

TonghuashunLimitPoolProvider::TonghuashunLimitPoolProvider(QObject* parent, QNetworkAccessManager* manager)
    : QObject(parent), m_nam(manager ? manager : new QNetworkAccessManager(this)) {
    m_nam->setParent(this);
    applyConfig(AppConfig{});
}

void TonghuashunLimitPoolProvider::applyConfig(const AppConfig& cfg) {
    ++m_generation;
    for (auto& state : m_pools) {
        if (state.reply) state.reply->abort();
        state = PoolState{};
    }
    m_nam->clearConnectionCache();
    m_nam->setProxy(network_utils::proxyFromConfig(cfg));
    m_userAgent = network_utils::effectiveUserAgent(cfg);
}

void TonghuashunLimitPoolProvider::fetch(bool limitDown, const QDate& tradeDate, bool forceRefresh) {
    auto& state = m_pools[limitDown ? 1 : 0];
    if (state.reply && state.requestedDate == tradeDate && !forceRefresh) return;
    const QDate today = chinaToday();
    if (!forceRefresh && state.requestedDate == tradeDate && state.calendarDate == today
        && QDateTime::currentMSecsSinceEpoch() < state.expiresAtMs) {
        emit dataReady(limitDown, state.cached);
        return;
    }
    // Invalidate the old request before abort(), whose finished signal can be synchronous.
    auto* previous = state.reply;
    state = PoolState{};
    if (previous) previous->abort();
    state.requestedDate = tradeDate;
    state.calendarDate = today;
    if (tradeDate.isValid() && tradeDate > today) {
        fail(limitDown, QStringLiteral("limit pool trading date is in the future"));
        return;
    }
    requestPage(limitDown, m_generation);
}

void TonghuashunLimitPoolProvider::fail(bool limitDown, const QString& message) {
    auto& state = m_pools[limitDown ? 1 : 0];
    state.expiresAtMs = 0;
    state.pending = {};
    state.codes.clear();
    emit error(limitDown, message);
}

void TonghuashunLimitPoolProvider::requestPage(bool limitDown, int generation) {
    const auto& state = m_pools[limitDown ? 1 : 0];
    const QString path = limitDown ? QStringLiteral("lower_limit_pool") : QStringLiteral("limit_up_pool");
    QUrl url(QStringLiteral("https://data.10jqka.com.cn/dataapi/limit_up/") + path);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("page"), QString::number(state.page));
    query.addQueryItem(QStringLiteral("limit"), QString::number(state.pageSize));
    query.addQueryItem(QStringLiteral("field"), QStringLiteral("199112"));
    const QDate date = state.pending.tradeDate.isValid() ? state.pending.tradeDate : state.requestedDate;
    if (date.isValid()) query.addQueryItem(QStringLiteral("date"), date.toString(QStringLiteral("yyyyMMdd")));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", m_userAgent.toUtf8());
    request.setRawHeader("Referer", "https://data.10jqka.com.cn/");
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);
    const auto trace = network_logger::logRequestStart(
        QStringLiteral("tonghuashun-") + path, QStringLiteral("GET"), request, m_nam->proxy());
    auto* reply = m_nam->get(request);
    m_pools[limitDown ? 1 : 0].reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, limitDown, generation, trace]() {
        const QByteArray body = reply->readAll();
        network_logger::logRequestFinish(trace, reply, body.size(), body);
        reply->deleteLater();
        auto& state = m_pools[limitDown ? 1 : 0];
        if (generation != m_generation || state.reply != reply) return;
        state.reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            fail(limitDown, reply->errorString());
            return;
        }
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            fail(limitDown, QStringLiteral("invalid limit pool JSON"));
            return;
        }
        const auto root = doc.object();
        if (root.value(QStringLiteral("status_code")).toInt(-1) != 0
            || !root.value(QStringLiteral("data")).isObject()) {
            fail(limitDown, QStringLiteral("limit pool request failed: %1").arg(root.value(QStringLiteral("status_msg")).toString()));
            return;
        }
        const auto data = root.value(QStringLiteral("data")).toObject();
        const QDate date = QDate::fromString(data.value(QStringLiteral("date")).toString(), QStringLiteral("yyyyMMdd"));
        if (!date.isValid() || date > state.calendarDate
            || (state.requestedDate.isValid() && date != state.requestedDate)
            || (state.pending.tradeDate.isValid() && date != state.pending.tradeDate)) {
            fail(limitDown, QStringLiteral("limit pool trading date mismatch"));
            return;
        }
        const auto page = data.value(QStringLiteral("page")).toObject();
        const int total = page.value(QStringLiteral("total")).toInt(-1);
        const int pageSize = page.value(QStringLiteral("limit")).toInt(-1);
        if (total < 0 || total > 10000 || pageSize < 1 || pageSize > 200
            || page.value(QStringLiteral("page")).toInt(-1) != state.page
            || (state.total >= 0 && (state.total != total || state.pageSize != pageSize))
            || !data.value(QStringLiteral("info")).isArray()) {
            fail(limitDown, QStringLiteral("invalid limit pool pagination"));
            return;
        }
        state.total = total;
        state.pageSize = pageSize;
        state.pending.tradeDate = date;
        const auto rows = data.value(QStringLiteral("info")).toArray();
        const int expectedRows = qMin(pageSize, total - (state.page - 1) * pageSize);
        if (rows.size() != expectedRows) {
            fail(limitDown, QStringLiteral("limit pool page size does not match its total"));
            return;
        }
        for (const auto& value : rows) {
            const auto row = value.toObject();
            const QString name = row.value(QStringLiteral("name")).toString().trimmed();
            const QString code = row.value(QStringLiteral("code")).toString().trimmed();
            if (name.isEmpty() || code.isEmpty() || state.codes.contains(code)) {
                fail(limitDown, QStringLiteral("limit pool contains an invalid or duplicate stock"));
                return;
            }
            state.codes.insert(code);
            const double changeRate = row.value(QStringLiteral("change_rate")).toDouble(qQNaN());
            state.pending.stocks.append(LimitPoolStock{code, name, changeRate});
        }
        if (state.pending.stocks.size() > total || (rows.isEmpty() && state.pending.stocks.size() < total)) {
            fail(limitDown, QStringLiteral("incomplete limit pool page"));
            return;
        }
        if (state.pending.stocks.size() < total) {
            ++state.page;
            requestPage(limitDown, generation);
            return;
        }
        state.cached = state.pending;
        state.expiresAtMs = QDateTime::currentMSecsSinceEpoch() + app_constants::kNetworkCacheTtlMs;
        emit dataReady(limitDown, state.cached);
    });
}

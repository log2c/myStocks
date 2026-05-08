#pragma once

#include "types.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QObject>
#include <QStringList>

class IQuoteProvider : public QObject {
    Q_OBJECT
public:
    explicit IQuoteProvider(QObject* parent = nullptr);
    ~IQuoteProvider() override;

    virtual void fetchQuotes(const QVector<StockItem>& stocks) = 0;
    virtual void setLanguage(const QString& language);
    virtual void applyConfig(const AppConfig& cfg);

signals:
    void quotesReady(const QVector<QuoteItem>& quotes);
    void error(const QString& message);

protected:
    QString m_userAgent = defaultChrome100UserAgent();
    QNetworkProxy m_proxy = QNetworkProxy(QNetworkProxy::NoProxy);
};

class EastMoneyQuoteProvider : public IQuoteProvider {
public:
    explicit EastMoneyQuoteProvider(QObject* parent = nullptr);

    void fetchQuotes(const QVector<StockItem>& stocks) override;

private:
    static QString toSecId(const QString& rawCode);
    static double firstNumber(const QJsonObject& obj, const QStringList& keys);
    void handleResponse(
        const QHash<QString, StockItem>& requestMap,
        const QByteArray& body,
        const QString& errorText
    );

private:
    QNetworkAccessManager m_nam;
    int m_pendingRequests = 0;
    QStringList m_errors;
    QHash<QString, QuoteItem> m_buffer;
};

class EastMoneyHotRankProvider : public QObject {
    Q_OBJECT
public:
    explicit EastMoneyHotRankProvider(QObject* parent = nullptr);

    void applyConfig(const AppConfig& cfg);
    void fetchHotSectors(
        int limit,
        const QString& sortField,
        const QString& sortOrder,
        bool forceRefresh = false
    );
    void fetchHotConcepts(
        int limit,
        const QString& sortField,
        const QString& sortOrder,
        bool forceRefresh = false
    );

signals:
    void hotSectorsReady(const QVector<HotRankItem>& items);
    void hotConceptsReady(const QVector<HotRankItem>& items);
    void error(const QString& message);

private:
    void fetchHotList(
        bool concept,
        int limit,
        const QString& sortField,
        const QString& sortOrder,
        bool forceRefresh
    );
    void handleHotListResponse(
        bool concept,
        const QByteArray& body,
        const QString& errorText,
        const QString& requestKey
    );

private:
    QString m_userAgent = defaultChrome100UserAgent();
    QNetworkProxy m_proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkAccessManager m_nam;
    QString m_lastError;
    QNetworkReply* m_hotSectorReply = nullptr;
    QNetworkReply* m_hotConceptReply = nullptr;
    QVector<HotRankItem> m_cachedHotSectors;
    QVector<HotRankItem> m_cachedHotConcepts;
    QString m_hotSectorCacheRequestKey;
    QString m_hotConceptCacheRequestKey;
    QString m_hotSectorInFlightRequestKey;
    QString m_hotConceptInFlightRequestKey;
    qint64 m_hotSectorCacheExpiresAtMs = 0;
    qint64 m_hotConceptCacheExpiresAtMs = 0;
    bool m_hotSectorCacheValid = false;
    bool m_hotConceptCacheValid = false;
};

class TonghuashunStockHeatProvider : public QObject {
    Q_OBJECT
public:
    explicit TonghuashunStockHeatProvider(QObject* parent = nullptr);

    void applyConfig(const AppConfig& cfg);
    void fetchHourHotStocks(int limit, bool forceRefresh = false);
    void fetchDayHotStocks(int limit, bool forceRefresh = false);

signals:
    void hourHotStocksReady(const QVector<HotRankItem>& items);
    void dayHotStocksReady(const QVector<HotRankItem>& items);
    void error(const QString& message);

private:
    void fetchHotStocks(const QString& periodType, int limit, bool forceRefresh);
    void handleHotStocksResponse(
        const QString& periodType,
        const QByteArray& body,
        const QString& errorText,
        const QString& requestKey
    );

    QString m_userAgent = defaultChrome100UserAgent();
    QNetworkProxy m_proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkAccessManager m_nam;
    QString m_lastError;
    QNetworkReply* m_hourReply = nullptr;
    QNetworkReply* m_dayReply = nullptr;
    QVector<HotRankItem> m_cachedHourItems;
    QVector<HotRankItem> m_cachedDayItems;
    QString m_hourCacheRequestKey;
    QString m_dayCacheRequestKey;
    QString m_hourInFlightRequestKey;
    QString m_dayInFlightRequestKey;
    qint64 m_hourCacheExpiresAtMs = 0;
    qint64 m_dayCacheExpiresAtMs = 0;
    bool m_hourCacheValid = false;
    bool m_dayCacheValid = false;
};

class EastMoneyHotRankDetailProvider : public QObject {
    Q_OBJECT
public:
    explicit EastMoneyHotRankDetailProvider(QObject* parent = nullptr);

    void applyConfig(const AppConfig& cfg);
    void fetch(const QString& fs, int limit = 300, bool forceRefresh = false);

signals:
    void dataReady(const QString& fs, const QVector<HotRankDetailItem>& items);
    void error(const QString& fs, const QString& message);

private:
    void handleResponse(const QString& fs, const QByteArray& body, const QString& errorText);

    QString m_userAgent = defaultChrome100UserAgent();
    QNetworkProxy m_proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkAccessManager m_nam;
    QNetworkReply* m_reply = nullptr;
    QString m_inFlightFs;
    QString m_cachedFs;
    QVector<HotRankDetailItem> m_cachedItems;
    qint64 m_cacheExpiresAtMs = 0;
    bool m_cacheValid = false;
    QString m_lastError;
};

class AshareMarketBreadthProvider : public QObject {
    Q_OBJECT
public:
    explicit AshareMarketBreadthProvider(QObject* parent = nullptr);

    void applyConfig(const AppConfig& cfg);
    void fetch(bool forceRefresh = false);

signals:
    void dataReady(const MarketBreadthSnapshot& snapshot);
    void error(const QString& message);

private:
    void finalizeFetch(int token);

    QString m_userAgent = defaultChrome100UserAgent();
    QNetworkProxy m_proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkAccessManager m_nam;
    QString m_lastError;
    QNetworkReply* m_overviewReply = nullptr;
    QNetworkReply* m_distributionReply = nullptr;
    QNetworkReply* m_turnoverReply = nullptr;
    int m_requestToken = 0;
    MarketBreadthSnapshot m_pendingSnapshot;
    MarketBreadthSnapshot m_cachedSnapshot;
    qint64 m_cacheExpiresAtMs = 0;
    bool m_cacheValid = false;
    bool m_overviewDone = false;
    bool m_distributionDone = false;
    bool m_turnoverDone = false;
    QStringList m_pendingErrors;
};

class EastMoneyIndexQuoteProvider : public QObject {
    Q_OBJECT
public:
    explicit EastMoneyIndexQuoteProvider(QObject* parent = nullptr);

    void applyConfig(const AppConfig& cfg);
    void fetch(bool forceRefresh = false);

signals:
    void dataReady(const QVector<IndexQuoteItem>& items);
    void error(const QString& message);

private:
    void handleResponse(const QByteArray& body, const QString& errorText);

    QString m_userAgent = defaultChrome100UserAgent();
    QNetworkProxy m_proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkAccessManager m_nam;
    QNetworkReply* m_reply = nullptr;
    QVector<IndexQuoteItem> m_cachedItems;
    qint64 m_cacheExpiresAtMs = 0;
    bool m_cacheValid = false;
    QString m_lastError;
};

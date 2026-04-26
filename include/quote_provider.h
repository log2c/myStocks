#pragma once

#include "types.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QObject>
#include <QSet>
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

class MockQuoteProvider : public IQuoteProvider {
public:
    explicit MockQuoteProvider(QObject* parent = nullptr);

    void fetchQuotes(const QVector<StockItem>& stocks) override;

private:
    QHash<QString, double> m_lastPrice;
};

class XTickQuoteProvider : public IQuoteProvider {
public:
    explicit XTickQuoteProvider(const QString& token, QObject* parent = nullptr);

    void setToken(const QString& token);
    void fetchQuotes(const QVector<StockItem>& stocks) override;
    void setLanguage(const QString& language) override;

private:
    static QString normalizeCode(const QString& raw);
    static int inferType(const QString& rawCode);
    static QString makeKey(int type, const QString& code);

    void handleResponse(int reqType, const QByteArray& body, const QString& errorText);

private:
    QString m_token;
    QString m_language = "en_US";
    QNetworkAccessManager m_nam;

    int m_pendingRequests = 0;
    QStringList m_errors;
    QString m_lastError;

    QHash<QString, StockItem> m_reqMap;
    QHash<QString, StockItem> m_codeFallback;
    QHash<QString, QuoteItem> m_buffer;
};

class SinaQuoteProvider : public IQuoteProvider {
public:
    explicit SinaQuoteProvider(QObject* parent = nullptr);

    void fetchQuotes(const QVector<StockItem>& stocks) override;

private:
    static QString toCnSymbol(const QString& rawCode);
    static double parseFieldNumber(const QList<QByteArray>& fields, int index);
    void handleResponse(const QByteArray& body, const QString& errorText);

private:
    QNetworkAccessManager m_nam;
    QHash<QString, StockItem> m_symbolMap;
};

class TencentQuoteProvider : public IQuoteProvider {
public:
    explicit TencentQuoteProvider(QObject* parent = nullptr);

    void fetchQuotes(const QVector<StockItem>& stocks) override;

private:
    static QString toCnSymbol(const QString& rawCode);
    static double parseFieldNumber(const QList<QByteArray>& fields, int index);
    void handleResponse(const QByteArray& body, const QString& errorText);

private:
    QNetworkAccessManager m_nam;
    QHash<QString, StockItem> m_symbolMap;
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
    void fetchHotSectors(int limit, const QString& sortField, const QString& sortOrder);
    void fetchHotConcepts(int limit, const QString& sortField, const QString& sortOrder);

signals:
    void hotSectorsReady(const QVector<HotRankItem>& items);
    void hotConceptsReady(const QVector<HotRankItem>& items);
    void error(const QString& message);

private:
    void fetchHotList(
        bool concept,
        int limit,
        const QString& sortField,
        const QString& sortOrder
    );
    void handleHotListResponse(bool concept, const QByteArray& body, const QString& errorText);

private:
    QString m_userAgent = defaultChrome100UserAgent();
    QNetworkProxy m_proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkAccessManager m_nam;
    QString m_lastError;
};

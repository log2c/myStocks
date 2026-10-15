#pragma once

#include "types.h"

#include <QDate>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <array>

struct LimitPoolStock {
    QString code;
    QString name;
    double changeRate = qQNaN(); // Percentage units: 10.0158 means 10.0158%.
};

struct LimitPoolSnapshot {
    QDate tradeDate;
    QVector<LimitPoolStock> stocks;
};

class TonghuashunLimitPoolProvider : public QObject {
    Q_OBJECT
public:
    // An injected manager is owned by the provider, like the default manager.
    explicit TonghuashunLimitPoolProvider(QObject* parent = nullptr, QNetworkAccessManager* manager = nullptr);
    void applyConfig(const AppConfig& cfg);
    void fetch(bool limitDown, const QDate& tradeDate = {}, bool forceRefresh = false);

signals:
    void dataReady(bool limitDown, const LimitPoolSnapshot& snapshot);
    void error(bool limitDown, const QString& message);

private:
    struct PoolState {
        QNetworkReply* reply = nullptr;
        QDate requestedDate;
        QDate calendarDate;
        LimitPoolSnapshot pending;
        LimitPoolSnapshot cached;
        QSet<QString> codes;
        int page = 1;
        int pageSize = 200;
        int total = -1;
        qint64 expiresAtMs = 0;
    };
    void requestPage(bool limitDown, int generation);
    void fail(bool limitDown, const QString& message);

    QNetworkAccessManager* m_nam = nullptr;
    QString m_userAgent;
    int m_generation = 0;
    std::array<PoolState, 2> m_pools;
};

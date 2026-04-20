#pragma once

#include "types.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>

class FloatingWindow;
class IQuoteProvider;
#ifdef WIN32
class QHotkey;
#endif
class QuoteModel;
class QSystemTrayIcon;
class QTimer;

class AppController : public QObject {
public:
    explicit AppController(QObject* parent = nullptr);

private:
    QString findDataYaml() const;
    void toggleWindow();
    void openSettings();
    void reloadStocksFromYaml();
    void refreshQuotes(bool force = false);
    void onProviderError(const QString& message);
    void setupTray();
    void setupHotkey();
    void rebuildProvider();
    int writeApiNamesToDataYaml();
    QHash<QString, QString> currentApiNamesByCode() const;
    bool shouldPollNow();
    bool hasHongKongStocks() const;
    bool isWithinTradingSession(const QDateTime& bjNow) const;
    bool isWithinHongKongTradingSession(const QDateTime& bjNow) const;
    bool probeTradingDay(const QDate& bjDate);
    QString probeTradingDateText(const QByteArray& body) const;

private:
    AppConfig m_cfg;
    QString m_resolvedLanguage;
    QVector<StockItem> m_stocks;
    QHash<QString, QString> m_apiNamesByCode;

    QuoteModel* m_model = nullptr;
    FloatingWindow* m_window = nullptr;
    IQuoteProvider* m_provider = nullptr;

    QTimer* m_timer = nullptr;
    QSystemTrayIcon* m_tray = nullptr;
#ifdef WIN32
    QHotkey* m_hotkey = nullptr;
#endif

    QNetworkAccessManager m_probeNam;
    QDate m_probeDate;
    QDateTime m_probeCheckedAt;
    bool m_probeTradingDay = true;
};

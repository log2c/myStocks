#pragma once

#include "types.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class FloatingWindow;
class IQuoteProvider;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
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
    void resetFloatingWindowPosition();
    void openSettings();
    void reloadStocksFromYaml();
    void refreshQuotes(bool force = false);
    void onProviderError(const QString& message);
    void setupTray();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    void setupHotkey();
#endif
    void rebuildProvider();
    int writeApiNamesToDataYaml();
    QHash<QString, QString> currentApiNamesByCode() const;
    void loadExtraWatchItems();
    void saveExtraWatchItems() const;
    QVector<StockItem> mergedWatchItems() const;
    QVector<StockItem> filterYamlStocks(
        const QVector<StockItem>& loaded,
        QStringList* ignoredCodes = nullptr
    ) const;
    static bool isSectorCode(const QString& code);
    bool shouldPollNow();
    bool hasHongKongStocks() const;
    bool isWithinTradingSession(const QDateTime& bjNow) const;
    bool isWithinHongKongTradingSession(const QDateTime& bjNow) const;
    bool probeTradingDay(const QDate& bjDate);
    QString probeTradingDateText(const QByteArray& body) const;
    void updateTrayTooltip();

private:
    AppConfig m_cfg;
    QString m_resolvedLanguage;
    QVector<StockItem> m_stocks;
    QVector<StockItem> m_indexes;
    QVector<StockItem> m_sectors;
    QHash<QString, QString> m_apiNamesByCode;
    QStringList m_lastIgnoredYamlIndexCodes;
    QString m_lastTrayErrorMessage;
    QDateTime m_lastTrayErrorAt;

    QuoteModel* m_model = nullptr;
    FloatingWindow* m_window = nullptr;
    IQuoteProvider* m_provider = nullptr;
    IQuoteProvider* m_sectorProvider = nullptr;

    QTimer* m_timer = nullptr;
    QSystemTrayIcon* m_tray = nullptr;

    QNetworkAccessManager m_probeNam;
    QDate m_probeDate;
    QDateTime m_probeCheckedAt;
    bool m_probeTradingDay = true;

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    QHotkey* m_hotkey = nullptr;
#endif
};
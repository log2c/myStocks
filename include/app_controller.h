#pragma once

#include "types.h"
#include "updater.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class FloatingWindow;
class EastMoneyHotRankProvider;
class AshareMarketBreadthProvider;
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
    void toggleMarketBreadthDetailWindow();
    void resetFloatingWindowPosition();
    void openSettings();
    void reloadStocksFromYaml();
    void refreshQuotes(bool force = false);
    void onProviderError(const QString& message);
    void showTrayErrorBadge();
    void restoreTrayIcon();
    void setupTray();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    void setupHotkey();
#endif
    void rebuildProvider();
    void rebuildHotRankProvider();
    QHash<QString, QString> currentApiNamesByCode() const;
    void loadExtraWatchItems();
    void saveExtraWatchItems() const;
    QVector<StockItem> mergedWatchItems() const;
    QVector<StockItem> filterYamlStocks(
        const QVector<StockItem>& loaded,
        QStringList* ignoredCodes = nullptr
    ) const;
    static bool isFutureCode(const QString& code);
    bool shouldPollNow();
    bool hasHongKongStocks() const;
    bool hasHongKongTradingScheduleItems() const;
    bool isWithinTradingSession(const QDateTime& bjNow) const;
    bool isWithinHongKongTradingSession(const QDateTime& bjNow) const;
    bool isWithinAshareTradingSession(const QDateTime& bjNow) const;
    bool probeTradingDay(const QDate& bjDate);
    bool probeTradingDay(const QDate& bjDate, bool useHongKongProbe);
    bool shouldPollHotRanksNow();
    bool shouldPollMarketBreadthNow();
    void refreshHotRanks(bool force = false);
    void refreshMarketBreadth(bool force = false);
    QString probeTradingDateText(const QByteArray& body) const;
    void updateTrayTooltip();
    void captureRuntimeWindowGeometries(AppConfig* cfg) const;
    bool isYamlWatchStockTracked(const QString& code) const;
    bool updateYamlWatchStock(const QString& code, const QString& name, bool add);

private:
    AppConfig m_cfg;
    QString m_resolvedLanguage;
    QVector<StockItem> m_stocks;
    QVector<StockItem> m_indexes;
    QHash<QString, QString> m_apiNamesByCode;
    QStringList m_lastIgnoredYamlIndexCodes;
    QString m_lastTrayErrorMessage;
    QDateTime m_lastTrayErrorAt;

    QuoteModel* m_model = nullptr;
    FloatingWindow* m_window = nullptr;
    IQuoteProvider* m_provider = nullptr;
    EastMoneyHotRankProvider* m_hotRankProvider = nullptr;
    AshareMarketBreadthProvider* m_marketBreadthProvider = nullptr;

    QTimer* m_timer = nullptr;
    QTimer* m_hotRankTimer = nullptr;
    QTimer* m_marketBreadthTimer = nullptr;
    QSystemTrayIcon* m_tray = nullptr;
    QIcon m_trayBaseIcon;
    QTimer* m_trayErrorBadgeTimer = nullptr;

    QNetworkAccessManager m_probeNam;
    QDate m_probeDate;
    QDateTime m_probeCheckedAt;
    bool m_probeTradingDay = true;
    QDate m_hotProbeDate;
    QDateTime m_hotProbeCheckedAt;
    bool m_hotProbeTradingDay = true;

    Updater* m_updater = nullptr;

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    QHotkey* m_hotkey = nullptr;
    QHotkey* m_marketBreadthHotkey = nullptr;
#endif
};

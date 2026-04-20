#pragma once

#include "types.h"

#include <QDate>
#include <QDateTime>
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
    void refreshQuotes();
    void onProviderError(const QString& message);
    void setupTray();
    void setupHotkey();
    void rebuildProvider();
    bool shouldPollNow();
    bool isWithinTradingSession(const QDateTime& bjNow) const;
    bool probeTradingDay(const QDate& bjDate);
    QString probeTradingDateText(const QByteArray& body) const;

private:
    AppConfig m_cfg;
    QString m_resolvedLanguage;
    QVector<StockItem> m_stocks;

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

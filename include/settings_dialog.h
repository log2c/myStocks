#pragma once

#include "types.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QHash>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVector>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTableWidget;
class QTimer;

class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(
        const AppConfig& cfg,
        const QVector<StockItem>& stocks,
        const QVector<StockItem>& indexes,
        const QVector<StockItem>& sectors,
        const QVector<StockItem>& futures,
        const QHash<QString, QString>& apiNamesByCode,
        const QString& dataYamlPath,
        QWidget* parent = nullptr
    );

    AppConfig config() const;
    QVector<StockItem> selectedIndexes() const;
    QVector<StockItem> selectedSectors() const;
    QVector<StockItem> selectedFutures() const;

private:
    QString trText(const QString& key) const;

    static void paintColorButton(QPushButton* btn, const QColor& c);
    static QPushButton* createColorButton(
        QWidget* parent,
        const QColor& color,
        const QString& pickTitle
    );
    static QColor buttonColor(QPushButton* btn);

    QWidget* buildGeneralTab();
    QWidget* buildNetworkTab();
    QWidget* buildDisplayTab();
    QWidget* buildOtherTab();
    QWidget* buildStocksTab();
    QWidget* buildIndexSectorTab();
    QWidget* buildFuturesTab();
    QWidget* buildAboutTab();

    void updateHotkeyIndicator(const QKeySequence& seq);

    void parseSinaSearchResult(const QByteArray& data);
    void doStockSearch(bool forceSearch = false);
    void parseSectorSuggestResult(const QByteArray& data);
    void doSectorSearch(bool forceSearch = false);
    void parseFutureSuggestResult(const QByteArray& data);
    void doFutureSearch(bool forceSearch = false);

private:
    AppConfig m_cfg;
    QVector<StockItem> m_stocks;
    QVector<StockItem> m_indexes;
    QVector<StockItem> m_sectors;
    QVector<StockItem> m_futures;
    QHash<QString, QString> m_apiNamesByCode;
    QString m_dataYamlPath;
    QString m_uiLanguage;

    QSpinBox* m_pollSpin = nullptr;
    QKeySequenceEdit* m_hotkeyEdit = nullptr;
    QLabel* m_hotkeyIndicator = nullptr;
    QPushButton* m_hotkeyClearBtn = nullptr;
    QCheckBox* m_startupShowFloatingWindowCheck = nullptr;
    QLineEdit* m_userAgentEdit = nullptr;
    QComboBox* m_proxyTypeCombo = nullptr;
    QLineEdit* m_proxyHostEdit = nullptr;
    QSpinBox* m_proxyPortSpin = nullptr;
    QLineEdit* m_proxyUserEdit = nullptr;
    QCheckBox* m_debugIgnoreTradingTimeCheck = nullptr;
    QCheckBox* m_logEnabledCheck = nullptr;
    QComboBox* m_logLevelCombo = nullptr;
    QPushButton* m_openLogDirButton = nullptr;

    QCheckBox* m_transparentBackgroundCheck = nullptr;
    QSlider* m_transparentOpacitySlider = nullptr;
    QLabel* m_transparentOpacityLabel = nullptr;
    QLineEdit* m_proxyPasswordEdit = nullptr;
    QComboBox* m_languageCombo = nullptr;
    QFontComboBox* m_fontFamilyCombo = nullptr;
    QSpinBox* m_fontSizeSpin = nullptr;
    QCheckBox* m_fontBoldCheck = nullptr;

    QPushButton* m_bgBtn = nullptr;
    QPushButton* m_textBtn = nullptr;
    QPushButton* m_upBtn = nullptr;
    QPushButton* m_downBtn = nullptr;
    QPushButton* m_flatBtn = nullptr;

    QCheckBox* m_showHeaderCheck = nullptr;
    QCheckBox* m_showGridCheck = nullptr;
    QPushButton* m_gridColorBtn = nullptr;
    QCheckBox* m_hotRankEnabledCheck = nullptr;
    QSpinBox* m_hotRankPollSecsSpin = nullptr;
    QDoubleSpinBox* m_hotRankFlipSecsSpin = nullptr;
    QCheckBox* m_hotSectorVisibleCheck = nullptr;
    QSpinBox* m_hotSectorCountSpin = nullptr;
    QComboBox* m_hotSectorSortFieldCombo = nullptr;
    QComboBox* m_hotSectorSortOrderCombo = nullptr;
    QCheckBox* m_hotConceptVisibleCheck = nullptr;
    QSpinBox* m_hotConceptCountSpin = nullptr;
    QComboBox* m_hotConceptSortFieldCombo = nullptr;
    QComboBox* m_hotConceptSortOrderCombo = nullptr;
    QCheckBox* m_floatingTopMostCheck = nullptr;
    QCheckBox* m_simpleModeCheck = nullptr;
    QCheckBox* m_blinkReminderCheck = nullptr;
    QCheckBox* m_trayTooltipCheck = nullptr;
    QCheckBox* m_hoverReadingCheck = nullptr;
    QDoubleSpinBox* m_hoverReadingDelaySpin = nullptr;
    QComboBox* m_hoverReadingModeCombo = nullptr;
    QCheckBox* m_hoverReadingTransparentBackgroundCheck = nullptr;
    QCheckBox* m_timelineChartEnabledCheck = nullptr;
    QSpinBox* m_timelineChartRefreshSecsSpin = nullptr;
    QCheckBox* m_marketBreadthEnabledCheck = nullptr;
    QSpinBox* m_marketBreadthRefreshSecsSpin = nullptr;
    QCheckBox* m_timelineChartFixedRangeCheck = nullptr;
    QPushButton* m_timelineChartBgBtn = nullptr;
    QPushButton* m_timelineChartGridBtn = nullptr;
    QPushButton* m_timelineChartPriceLineBtn = nullptr;
    QPushButton* m_timelineChartAvgLineBtn = nullptr;
    QPushButton* m_timelineChartTextBtn = nullptr;
    QPushButton* m_timelineChartUpBtn = nullptr;
    QPushButton* m_timelineChartDownBtn = nullptr;
    QCheckBox* m_mousePassthroughCheck = nullptr;
    QComboBox* m_mousePassthroughKeyCombo = nullptr;
    QCheckBox* m_doubleClickCloseWindowCheck = nullptr;
    QDoubleSpinBox* m_windowPaddingSpin = nullptr;
    QListWidget* m_columnList = nullptr;
    QVector<QSpinBox*> m_columnMaxWidthSpins;
    bool m_normalizingHotkeySequence = false;

    // Stocks tab
    QComboBox* m_stockMarketCombo = nullptr;
    QLineEdit* m_stockSearchEdit = nullptr;
    QPushButton* m_stockSearchBtn = nullptr;
    QTimer* m_stockSearchDebounce = nullptr;
    QNetworkAccessManager* m_stockSearchNam = nullptr;
    QNetworkReply* m_stockSearchReply = nullptr;
    QListWidget* m_stockSuggestList = nullptr;
    QTableWidget* m_stockTable = nullptr;

    // Index / sector tab
    QListWidget* m_indexList = nullptr;
    QLineEdit* m_sectorSearchEdit = nullptr;
    QPushButton* m_sectorSearchBtn = nullptr;
    QTimer* m_sectorSearchDebounce = nullptr;
    QNetworkAccessManager* m_sectorSearchNam = nullptr;
    QNetworkReply* m_sectorSearchReply = nullptr;
    QListWidget* m_sectorSuggestList = nullptr;
    QTableWidget* m_sectorTable = nullptr;

    // Futures tab
    QLineEdit* m_futureSearchEdit = nullptr;
    QPushButton* m_futureSearchBtn = nullptr;
    QTimer* m_futureSearchDebounce = nullptr;
    QNetworkAccessManager* m_futureSearchNam = nullptr;
    QNetworkReply* m_futureSearchReply = nullptr;
    QListWidget* m_futureSuggestList = nullptr;
    QTableWidget* m_futureTable = nullptr;
};

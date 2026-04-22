#pragma once

#include "types.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
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
        const QHash<QString, QString>& apiNamesByCode,
        const QString& dataYamlPath,
        std::function<void()> onWriteStockNames,
        QWidget* parent = nullptr
    );

    AppConfig config() const;
    QVector<StockItem> selectedIndexes() const;
    QVector<StockItem> selectedSectors() const;

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
    QWidget* buildAboutTab();

    void updateHotkeyIndicator(const QKeySequence& seq);

    void parseSinaSearchResult(const QByteArray& data);
    void doStockSearch(bool forceSearch = false);
    void parseSectorSuggestResult(const QByteArray& data);
    void doSectorSearch(bool forceSearch = false);

private:
    AppConfig m_cfg;
    QVector<StockItem> m_stocks;
    QVector<StockItem> m_indexes;
    QVector<StockItem> m_sectors;
    QHash<QString, QString> m_apiNamesByCode;
    QString m_dataYamlPath;
    std::function<void()> m_onWriteStockNames;
    QString m_uiLanguage;

    QSpinBox* m_pollSpin = nullptr;
    QKeySequenceEdit* m_hotkeyEdit = nullptr;
    QLabel* m_hotkeyIndicator = nullptr;
    QPushButton* m_hotkeyClearBtn = nullptr;
    QComboBox* m_sourceCombo = nullptr;
    QLineEdit* m_tokenEdit = nullptr;
    QPushButton* m_tokenCheckBtn = nullptr;
    QWidget* m_tokenRowWidget = nullptr;
    QWidget* m_tokenRowLabel = nullptr;
    QNetworkAccessManager* m_tokenCheckNam = nullptr;
    QNetworkReply* m_tokenCheckReply = nullptr;
    QLineEdit* m_userAgentEdit = nullptr;
    QComboBox* m_proxyTypeCombo = nullptr;
    QLineEdit* m_proxyHostEdit = nullptr;
    QSpinBox* m_proxyPortSpin = nullptr;
    QLineEdit* m_proxyUserEdit = nullptr;
    QCheckBox* m_debugIgnoreTradingTimeCheck = nullptr;
    QCheckBox* m_logEnabledCheck = nullptr;
    QComboBox* m_logLevelCombo = nullptr;
    QPushButton* m_openLogDirButton = nullptr;
    QPushButton* m_writeStockNamesButton = nullptr;

    QCheckBox* m_transparentBackgroundCheck = nullptr;
    QSlider* m_transparentOpacitySlider = nullptr;
    QLabel* m_transparentOpacityLabel = nullptr;
    QLineEdit* m_proxyPasswordEdit = nullptr;
    QComboBox* m_languageCombo = nullptr;

    QPushButton* m_bgBtn = nullptr;
    QPushButton* m_textBtn = nullptr;
    QPushButton* m_upBtn = nullptr;
    QPushButton* m_downBtn = nullptr;
    QPushButton* m_flatBtn = nullptr;

    QCheckBox* m_showHeaderCheck = nullptr;
    QCheckBox* m_showGridCheck = nullptr;
    QPushButton* m_gridColorBtn = nullptr;
    QCheckBox* m_floatingTopMostCheck = nullptr;
    QCheckBox* m_simpleModeCheck = nullptr;
    QCheckBox* m_blinkReminderCheck = nullptr;
    QCheckBox* m_trayTooltipCheck = nullptr;
    QCheckBox* m_hoverReadingCheck = nullptr;
    QDoubleSpinBox* m_hoverReadingDelaySpin = nullptr;
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
};

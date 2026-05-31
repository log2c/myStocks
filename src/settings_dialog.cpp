#include "settings_dialog.h"

#include "app_logging.h"
#include "config_manager.h"
#include "i18n.h"
#include "network_logger.h"
#include "network_utils.h"
#include "updater.h"
#include "watchlist_utils.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QColorDialog>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QDropEvent>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPalette>
#include <QPixmap>
#include <QProgressBar>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <utility>

namespace {

using watchlist_utils::normalizeApiWatchCode;
using watchlist_utils::watchCodeKey;

// Event filter that ignores wheel events, letting them propagate to a parent
// scroll area. Used to avoid nested-scroll-area conflict on the Other tab.
class IgnoreWheelFilter : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject*, QEvent* ev) override {
        if (ev->type() == QEvent::Wheel) {
            ev->ignore();
            return true;
        }
        return false;
    }
};

struct IndexPreset {
    QString code;
    QString name;
    QString note;
};

const QVector<IndexPreset>& indexPresets() {
    static const QVector<IndexPreset> presets {
        {QStringLiteral("1.000001"), QStringLiteral("上证指数"), QStringLiteral("沪市主板综合指数")},
        {QStringLiteral("0.399001"), QStringLiteral("深证成指"), QStringLiteral("深市核心宽基指数")},
        {QStringLiteral("0.399006"), QStringLiteral("创业板指"), QStringLiteral("创业板市场代表指数")},
        {QStringLiteral("1.000688"), QStringLiteral("科创50"), QStringLiteral("科创板龙头 50 只")},
        {QStringLiteral("1.000300"), QStringLiteral("沪深300"), QStringLiteral("沪深两市核心蓝筹")},
        {QStringLiteral("1.000905"), QStringLiteral("中证500"), QStringLiteral("A股中盘代表指数")},
        {QStringLiteral("100.HSI"), QStringLiteral("恒生指数"), QStringLiteral("港股大盘基准指数")},
        {QStringLiteral("124.HSTECH"), QStringLiteral("恒生科技指数"), QStringLiteral("港股科技龙头指数")},
        {QStringLiteral("100.XIN9"), QStringLiteral("富时中国A50"), QStringLiteral("离岸A50指数")},
    };

    return presets;
}

bool isPredefinedIndexCode(const QString& rawCode) {
    return watchlist_utils::isPredefinedIndexCode(rawCode);
}

namespace headers {

inline constexpr auto kUserAgent = "User-Agent";
inline constexpr auto kReferer = "Referer";
inline constexpr auto kAccept = "Accept";
inline constexpr auto kConnection = "Connection";
inline constexpr auto kContentType = "Content-Type";

inline constexpr auto kEastMoneyReferer = "https://quote.eastmoney.com/";
inline constexpr auto kEastMoneyContentType = "application/json;charset=UTF-8";

} // namespace headers

inline constexpr int kStockSearchDebounceMs = 400;
inline constexpr int kStockSearchMinLength = 2;
inline constexpr int kStockSearchResultLimit = 20;

QIcon dialogWindowIcon(QWidget* parent) {
    if (parent && !parent->windowIcon().isNull()) {
        return parent->windowIcon();
    }
    if (qApp && !qApp->windowIcon().isNull()) {
        return qApp->windowIcon();
    }
    return {};
}

QMessageBox::StandardButton showIconMessageBox(
    QWidget* parent,
    QMessageBox::Icon icon,
    const QString& title,
    const QString& text,
    QMessageBox::StandardButtons buttons = QMessageBox::Ok,
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton
) {
    QMessageBox box(icon, title, text, buttons, parent);
    const QIcon windowIcon = dialogWindowIcon(parent);
    if (!windowIcon.isNull()) {
        box.setWindowIcon(windowIcon);
        box.setIconPixmap(windowIcon.pixmap(36, 36));
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    box.setOption(QMessageBox::Option::DontUseNativeDialog, true);
#endif
    if (defaultButton != QMessageBox::NoButton) {
        box.setDefaultButton(defaultButton);
    }
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

class StockTableWidget : public QTableWidget {
public:
    explicit StockTableWidget(QWidget* parent = nullptr)
        : QTableWidget(0, 5, parent) {
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDragDropMode(QAbstractItemView::NoDragDrop);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setAlternatingRowColors(true);
        verticalHeader()->setVisible(false);
        verticalHeader()->setDefaultSectionSize(24);
        horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
        horizontalHeader()->setStretchLastSection(false);
        setColumnWidth(4, 64);
    }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        const int row = currentRow();
        if (e->key() == Qt::Key_Up && (e->modifiers() & Qt::AltModifier) == 0) {
            if (row > 0) {
                const int newRow = moveRowUp(row);
                if (newRow >= 0) setCurrentCell(newRow, 0);
            }
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Down && (e->modifiers() & Qt::AltModifier) == 0) {
            if (row >= 0 && row < rowCount() - 1) {
                const int newRow = moveRowDown(row);
                if (newRow >= 0) setCurrentCell(newRow, 0);
            }
            e->accept();
            return;
        }
        QTableWidget::keyPressEvent(e);
    }

public:
    // confirmDelete: receives display text (code + name), returns true to proceed
    void setConfirmDelete(std::function<bool(const QString&)> fn) {
        m_confirmDelete = std::move(fn);
    }

    void addStockRow(const QString& code, const QString& name, double cost = qQNaN()) {
        const int row = rowCount();
        insertRow(row);
        populateRow(row, code, name, cost);
        renumberRows();
    }

    bool containsCode(const QString& code) const {
        for (int r = 0; r < rowCount(); ++r) {
            if (item(r, 1) && item(r, 1)->text() == code) {
                return true;
            }
        }
        return false;
    }

    QVector<StockItem> stocks() const {
        QVector<StockItem> result;
        result.reserve(rowCount());
        for (int r = 0; r < rowCount(); ++r) {
            const QString code = item(r, 1) ? item(r, 1)->text() : QString();
            const QString name = item(r, 2) ? item(r, 2)->text() : QString();
            if (!code.isEmpty()) {
                double cost = qQNaN();
                if (QLineEdit* le = qobject_cast<QLineEdit*>(cellWidget(r, 3))) {
                    bool ok = false;
                    const double v = le->text().trimmed().toDouble(&ok);
                    if (ok && std::isfinite(v) && v > 0.0) cost = v;
                }
                StockItem s;
                s.code = code;
                s.name = name;
                s.cost = cost;
                result.push_back(s);
            }
        }
        return result;
    }

    // Returns the new row index, or -1 if nothing moved.
    int moveRowUp(int row) {
        if (row <= 0 || row >= rowCount()) return -1;
        return swapRows(row, row - 1);
    }

    int moveRowDown(int row) {
        if (row < 0 || row >= rowCount() - 1) return -1;
        return swapRows(row, row + 1);
    }

    int moveRowTop(int row) {
        if (row <= 0 || row >= rowCount()) return -1;
        const QString code = item(row, 1) ? item(row, 1)->text() : QString();
        const QString name = item(row, 2) ? item(row, 2)->text() : QString();
        double cost = qQNaN();
        if (QLineEdit* le = qobject_cast<QLineEdit*>(cellWidget(row, 3))) {
            bool ok = false;
            const double v = le->text().trimmed().toDouble(&ok);
            if (ok && std::isfinite(v) && v > 0.0) cost = v;
        }
        removeRow(row);
        insertRow(0);
        populateRow(0, code, name, cost);
        renumberRows();
        selectRow(0);
        return 0;
    }

    int moveRowBottom(int row) {
        if (row < 0 || row >= rowCount() - 1) return -1;
        const QString code = item(row, 1) ? item(row, 1)->text() : QString();
        const QString name = item(row, 2) ? item(row, 2)->text() : QString();
        double cost = qQNaN();
        if (QLineEdit* le = qobject_cast<QLineEdit*>(cellWidget(row, 3))) {
            bool ok = false;
            const double v = le->text().trimmed().toDouble(&ok);
            if (ok && std::isfinite(v) && v > 0.0) cost = v;
        }
        removeRow(row);
        const int newRow = rowCount();
        insertRow(newRow);
        populateRow(newRow, code, name, cost);
        renumberRows();
        selectRow(newRow);
        return newRow;
    }

private:
    int swapRows(int a, int b) {
        // b is always a+1 or a-1; do a single remove+insert to keep cell widgets intact
        const int lo = qMin(a, b);
        const int hi = qMax(a, b);
        const QString codeA = item(lo, 1) ? item(lo, 1)->text() : QString();
        const QString nameA = item(lo, 2) ? item(lo, 2)->text() : QString();
        double costA = qQNaN();
        if (QLineEdit* le = qobject_cast<QLineEdit*>(cellWidget(lo, 3))) {
            bool ok = false; const double v = le->text().trimmed().toDouble(&ok);
            if (ok && std::isfinite(v) && v > 0.0) costA = v;
        }
        const QString codeB = item(hi, 1) ? item(hi, 1)->text() : QString();
        const QString nameB = item(hi, 2) ? item(hi, 2)->text() : QString();
        double costB = qQNaN();
        if (QLineEdit* le = qobject_cast<QLineEdit*>(cellWidget(hi, 3))) {
            bool ok = false; const double v = le->text().trimmed().toDouble(&ok);
            if (ok && std::isfinite(v) && v > 0.0) costB = v;
        }

        removeRow(hi);
        removeRow(lo);
        insertRow(lo);
        populateRow(lo, codeB, nameB, costB);
        insertRow(hi);
        populateRow(hi, codeA, nameA, costA);

        renumberRows();
        // a moved to: if a was the higher row (moved up), it's now at lo; if lower (moved down), at hi
        const int newCurrent = (a > b) ? lo : hi;
        selectRow(newCurrent);
        return newCurrent;
    }

    void populateRow(int row, const QString& code, const QString& name, double cost = qQNaN()) {
        QTableWidgetItem* seqItem = new QTableWidgetItem(QString::number(row + 1));
        seqItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        setItem(row, 0, seqItem);

        QTableWidgetItem* codeItem = new QTableWidgetItem(code);
        codeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        setItem(row, 1, codeItem);

        QTableWidgetItem* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        setItem(row, 2, nameItem);

        // Cost column (index 3)
        const bool costEditable = watchlist_utils::isCostEditableCode(code);
        if (costEditable) {
            QLineEdit* costEdit = new QLineEdit();
            costEdit->setValidator(new QDoubleValidator(0.0, 1e9, 3, costEdit));
            costEdit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (std::isfinite(cost) && cost > 0.0) {
                costEdit->setText(QString::number(cost, 'f', 3));
            }
            costEdit->setPlaceholderText(QStringLiteral("--"));
            setCellWidget(row, 3, costEdit);
        } else {
            QTableWidgetItem* costItem = new QTableWidgetItem(QStringLiteral("--"));
            costItem->setFlags(Qt::ItemIsEnabled);
            costItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            setItem(row, 3, costItem);
        }

        QPushButton* delBtn = new QPushButton(QStringLiteral("\u2715 Del"));
        delBtn->setFlat(true);
        setCellWidget(row, 4, delBtn);

        connect(delBtn, &QPushButton::clicked, this, [this]() {
            QPushButton* btn = qobject_cast<QPushButton*>(sender());
            if (!btn) {
                return;
            }
            for (int r = 0; r < rowCount(); ++r) {
                if (cellWidget(r, 4) == btn) {
                    const QString code = item(r, 1) ? item(r, 1)->text() : QString();
                    const QString name = item(r, 2) ? item(r, 2)->text() : QString();
                    const QString display = code + (name.isEmpty() ? QString() : (QStringLiteral(" ") + name));
                    if (m_confirmDelete && !m_confirmDelete(display)) {
                        return;
                    }
                    removeRow(r);
                    renumberRows();
                    return;
                }
            }
        });
    }

    void renumberRows() {
        for (int r = 0; r < rowCount(); ++r) {
            if (QTableWidgetItem* it = item(r, 0)) {
                it->setText(QString::number(r + 1));
            }
        }
    }

    std::function<bool(const QString&)> m_confirmDelete;
};

QKeySequence normalizedHotkeySequence(const QKeySequence& sequence) {
    if (sequence.isEmpty()) {
        return {};
    }

    const QKeyCombination combo = sequence[0];
    const Qt::Key key = combo.key();
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();

    if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
        return {};
    }

    const bool hasModifier = mods.testFlag(Qt::ControlModifier)
        || mods.testFlag(Qt::AltModifier)
        || mods.testFlag(Qt::ShiftModifier)
        || mods.testFlag(Qt::MetaModifier);

    if (!hasModifier) {
        return {};
    }

    return sequence;
}

void applyCompactFormLayout(QFormLayout* form) {
    if (!form) {
        return;
    }

    form->setContentsMargins(10, 8, 10, 8);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(9);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop);
}

void addCompactFormRow(QFormLayout* form, QWidget* widget) {
    if (!form || !widget) {
        return;
    }

    form->addRow(QString(), widget);
}

void applyNumericSpinBoxWidth(QSpinBox* spin) {
    if (!spin) {
        return;
    }

    // Keep purely numeric spin boxes readable in compact form layouts.
    spin->setMinimumWidth(96);
}

void applyNumericSpinBoxWidth(QDoubleSpinBox* spin) {
    if (!spin) {
        return;
    }

    // Keep purely numeric spin boxes readable in compact form layouts.
    spin->setMinimumWidth(96);
}

QWidget* makeScrollableTab(QWidget* content, QWidget* parent) {
    if (!content) {
        return new QWidget(parent);
    }

    QScrollArea* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidget(content);
    return scroll;
}

} // namespace

SettingsDialog::SettingsDialog(
    const AppConfig& cfg,
    const QVector<StockItem>& stocks,
    const QVector<StockItem>& indexes,
    const QHash<QString, QString>& apiNamesByCode,
    const QString& dataYamlPath,
    const QVector<StockGroup>& groups,
    QWidget* parent
)
    : QDialog(parent)
    , m_cfg(cfg)
    , m_stocks(stocks)
    , m_indexes(indexes)
    , m_groups(groups)
    , m_apiNamesByCode(apiNamesByCode)
    , m_dataYamlPath(dataYamlPath)
    , m_uiLanguage(i18n::resolveLanguage(cfg.language)) {
    setWindowTitle(trText("settings.title"));
    const QIcon windowIcon = dialogWindowIcon(this);
    if (!windowIcon.isNull()) {
        setWindowIcon(windowIcon);
    }
    setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);
    setFixedSize(700, 560);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(8);

    QTabWidget* tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->addTab(makeScrollableTab(buildGeneralTab(), tabs), trText("settings.tab.general"));
    tabs->addTab(makeScrollableTab(buildNetworkTab(), tabs), trText("settings.tab.network"));
    tabs->addTab(makeScrollableTab(buildDisplayTab(), tabs), trText("settings.tab.display"));
    tabs->addTab(makeScrollableTab(buildStocksTab(), tabs), trText("settings.tab.data"));
    tabs->addTab(buildIndexSectorTab(), trText("settings.tab.indexSector"));
    tabs->addTab(buildGroupsTab(), QStringLiteral("\u5206\u7ec4")); // "分组"
    tabs->addTab(makeScrollableTab(buildOtherTab(), tabs), trText("settings.tab.other"));
    tabs->addTab(makeScrollableTab(buildAboutTab(), tabs), trText("settings.tab.about"));
    root->addWidget(tabs);

    QDialogButtonBox* box = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this
    );
    // Disable AutoDefault so pressing Enter does not close the dialog
    if (auto* okBtn = box->button(QDialogButtonBox::Ok)) {
        okBtn->setAutoDefault(false);
        okBtn->setDefault(false);
    }
    if (auto* cancelBtn = box->button(QDialogButtonBox::Cancel)) {
        cancelBtn->setAutoDefault(false);
        cancelBtn->setDefault(false);
    }
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(box);

#ifdef WIN32
    setStyleSheet(
        QStringLiteral(
            "QLineEdit, QAbstractSpinBox {"
            "background-color: palette(base);"
            "color: palette(text);"
            "selection-background-color: palette(highlight);"
            "selection-color: palette(highlighted-text);"
            "}"
        )
    );
#endif

}

AppConfig SettingsDialog::config() const {
    AppConfig out = m_cfg;

    out.pollMs = m_pollSpin->value();
    out.hotkey = normalizedHotkeySequence(m_hotkeyEdit->keySequence()).toString(QKeySequence::PortableText);
    out.marketBreadthHotkey = normalizedHotkeySequence(
        m_marketBreadthHotkeyEdit->keySequence()
    ).toString(QKeySequence::PortableText);
    out.startupShowFloatingWindow = m_startupShowFloatingWindowCheck->isChecked();
    out.userAgent = m_userAgentEdit->text().trimmed();
    out.proxyType = m_proxyTypeCombo->currentData().toString();
    out.proxyHost = m_proxyHostEdit->text().trimmed();
    out.proxyPort = m_proxyPortSpin->value();
    out.proxyUser = m_proxyUserEdit->text().trimmed();
    out.debugIgnoreTradingTime = m_debugIgnoreTradingTimeCheck->isChecked();
    out.acceptBetaUpdates = m_acceptBetaUpdatesCheck->isChecked();
    out.autoCheckUpdates = m_autoCheckUpdatesCheck->isChecked();
    out.logEnabled = m_logEnabledCheck ? m_logEnabledCheck->isChecked() : out.logEnabled;
    out.logLevel = app_logging::normalizeLogLevel(m_logLevelCombo->currentData().toString());
    out.transparentBackgroundEnabled = m_transparentBackgroundCheck->isChecked();
    out.transparentBackgroundOpacity = m_transparentOpacitySlider->value();
    {
        QFont defaultFont = font();
        const QString defaultFamily = defaultFloatingWindowFontFamily();
        if (!defaultFamily.isEmpty()) {
            defaultFont.setFamily(defaultFamily);
        }
        const QString selectedFamily = m_fontFamilyCombo
            ? m_fontFamilyCombo->currentFont().family().trimmed()
            : QString();
        out.floatingWindowFontFamily =
            (selectedFamily.isEmpty() || selectedFamily == defaultFont.family())
            ? defaultFont.family()
            : selectedFamily;
        out.floatingWindowFontSize = m_fontSizeSpin
            ? ((m_fontSizeSpin->value() == qMax(1, defaultFont.pointSize()))
                ? 0
                : qBound(0, m_fontSizeSpin->value(), 72))
            : out.floatingWindowFontSize;
        out.floatingWindowFontBold = m_fontBoldCheck && m_fontBoldCheck->isChecked();
    }
    out.proxyPassword = m_proxyPasswordEdit->text();
    out.language = m_languageCombo->currentData().toString();

    out.bgColor = buttonColor(m_bgBtn);
    out.textColor = buttonColor(m_textBtn);
    out.upColor = buttonColor(m_upBtn);
    out.downColor = buttonColor(m_downBtn);
    out.flatColor = buttonColor(m_flatBtn);
    out.costLineColor = buttonColor(m_costLineColorBtn);

    out.showHeader = m_showHeaderCheck->isChecked();
    out.showGrid = m_showGridCheck->isChecked();
    out.gridColor = buttonColor(m_gridColorBtn);
    out.floatingWindowAlwaysOnTop = m_floatingTopMostCheck->isChecked();
    out.floatingWindowPaddingPx = m_windowPaddingSpin
        ? qMax(0.0, m_windowPaddingSpin->value())
        : out.floatingWindowPaddingPx;
    out.simpleModeEnabled = m_simpleModeCheck->isChecked();
    out.blinkReminderEnabled = m_blinkReminderCheck->isChecked();
    out.trayTooltipEnabled = m_trayTooltipCheck->isChecked();
    out.hoverReadingEnabled = m_hoverReadingCheck->isChecked();
    out.hoverReadingDelaySecs = m_hoverReadingDelaySpin->value();
    out.hoverReadingUiMode = normalizeHoverReadingUiMode(
        m_hoverReadingModeCombo ? m_hoverReadingModeCombo->currentData().toString() : QString()
    );
    out.hoverReadingTransparentBackgroundEnabled = m_hoverReadingTransparentBackgroundCheck
        ? m_hoverReadingTransparentBackgroundCheck->isChecked()
        : out.hoverReadingTransparentBackgroundEnabled;
    out.timelineChartEnabled = m_timelineChartEnabledCheck
        ? m_timelineChartEnabledCheck->isChecked()
        : out.timelineChartEnabled;
    out.timelineChartRefreshSecs = m_timelineChartRefreshSecsSpin
        ? qBound(10, m_timelineChartRefreshSecsSpin->value(), 3600)
        : out.timelineChartRefreshSecs;
    out.marketBreadthEnabled = m_marketBreadthEnabledCheck
        ? m_marketBreadthEnabledCheck->isChecked()
        : out.marketBreadthEnabled;
    out.marketBreadthRefreshSecs = m_marketBreadthRefreshSecsSpin
        ? qBound(10, m_marketBreadthRefreshSecsSpin->value(), 3600)
        : out.marketBreadthRefreshSecs;
    out.timelineChartFixedRangeEnabled = m_timelineChartFixedRangeCheck
        ? m_timelineChartFixedRangeCheck->isChecked()
        : out.timelineChartFixedRangeEnabled;
    out.timelineChartBgColor = m_timelineChartBgBtn
        ? buttonColor(m_timelineChartBgBtn)
        : out.timelineChartBgColor;
    out.timelineChartGridColor = m_timelineChartGridBtn
        ? buttonColor(m_timelineChartGridBtn)
        : out.timelineChartGridColor;
    out.timelineChartPriceLineColor = m_timelineChartPriceLineBtn
        ? buttonColor(m_timelineChartPriceLineBtn)
        : out.timelineChartPriceLineColor;
    out.timelineChartAvgLineColor = m_timelineChartAvgLineBtn
        ? buttonColor(m_timelineChartAvgLineBtn)
        : out.timelineChartAvgLineColor;
    out.timelineChartTextColor = m_timelineChartTextBtn
        ? buttonColor(m_timelineChartTextBtn)
        : out.timelineChartTextColor;
    out.timelineChartUpColor = m_timelineChartUpBtn
        ? buttonColor(m_timelineChartUpBtn)
        : out.timelineChartUpColor;
    out.timelineChartDownColor = m_timelineChartDownBtn
        ? buttonColor(m_timelineChartDownBtn)
        : out.timelineChartDownColor;
    out.hotRankEnabled = false;
    out.hotSectorVisible = false;
    out.hotConceptVisible = false;
    out.mousePassthroughEnabled = m_mousePassthroughCheck
        && m_mousePassthroughCheck->isChecked();
    out.mousePassthroughActivationKey = normalizeMousePassthroughActivationKey(
        m_mousePassthroughKeyCombo
            ? m_mousePassthroughKeyCombo->currentData().toString()
            : out.mousePassthroughActivationKey
    );
    out.floatingWindowDoubleClickToHide = m_doubleClickCloseWindowCheck
        && m_doubleClickCloseWindowCheck->isChecked()
        && !out.mousePassthroughEnabled;
    out.floatingWindowDoubleClickStockDetail = m_doubleClickStockDetailCheck
        && m_doubleClickStockDetailCheck->isChecked();

    if (m_trayIconBtnGroup) {
        const int id = m_trayIconBtnGroup->checkedId();
        out.trayIconPath = (id >= 0 && id < m_trayIconPaths.size())
            ? m_trayIconPaths[id]
            : QString();
    }

    if (m_gistTokenEdit) {
        out.gistToken = m_gistTokenEdit->text().trimmed();
    }
    if (m_gistIdEdit) {
        out.gistId = m_gistIdEdit->text().trimmed();
    }
    // gistLastSyncTime is updated in-place when sync succeeds; reflect via m_cfg
    out.gistLastSyncTime = m_cfg.gistLastSyncTime;

    for (int i = 0; i < ColCount; ++i) {
        out.visibleColumns[i] = false;
    }

    out.columnOrder.clear();
    for (int row = 0; row < m_columnList->count(); ++row) {
        const QListWidgetItem* item = m_columnList->item(row);
        if (!item) {
            continue;
        }

        const int logical = item->data(Qt::UserRole).toInt();
        if (logical < 0 || logical >= ColCount || out.columnOrder.contains(logical)) {
            continue;
        }

        out.columnOrder.push_back(logical);
        out.visibleColumns[logical] = (item->checkState() == Qt::Checked);
    }
    out.columnOrder = watchlist_utils::normalizedColumnOrder(out.columnOrder);

    for (int i = 0; i < ColCount; ++i) {
        out.columnMaxWidths[i] = m_columnMaxWidthSpins[i]->value();
    }

    return out;
}

QVector<StockItem> SettingsDialog::selectedIndexes() const {
    if (!m_indexList) {
        return m_indexes;
    }

    QVector<StockItem> out;
    out.reserve(m_indexList->count());

    QSet<QString> seen;
    for (int row = 0; row < m_indexList->count(); ++row) {
        const QListWidgetItem* item = m_indexList->item(row);
        if (!item || item->checkState() != Qt::Checked) {
            continue;
        }

        const QString code = item->data(Qt::UserRole).toString().trimmed();
        const QString name = item->data(Qt::UserRole + 1).toString().trimmed();
        if (code.isEmpty()) {
            continue;
        }

        const QString key = watchCodeKey(code);
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);

        out.push_back({code, name});
    }

    return out;
}

QVector<StockGroup> SettingsDialog::groups() const {
    return m_groups;
}

void SettingsDialog::rebuildGroupList() {
    if (!m_groupList) {
        return;
    }

    const QListWidgetItem* currentItem = m_groupList->currentItem();
    const bool keepAllGroupSelected = currentItem
        && currentItem->data(Qt::UserRole).toInt() == -1;
    const QString currentGroupName = (!keepAllGroupSelected && currentItem)
        ? currentItem->text().trimmed()
        : QString();

    int selectedRow = -1;
    {
        QSignalBlocker blocker(m_groupList);
        m_groupList->clear();

        const int allPos = qBound(0, m_cfg.groupAllPosition, m_groups.size());
        m_cfg.groupAllPosition = allPos;
        for (int visualIndex = 0; visualIndex <= m_groups.size(); ++visualIndex) {
            if (visualIndex == allPos) {
                QListWidgetItem* item = new QListWidgetItem(
                    trText("settings.group.allGroup"),
                    m_groupList
                );
                item->setData(Qt::UserRole, -1);
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                QFont font = item->font();
                font.setItalic(true);
                item->setFont(font);
                if (keepAllGroupSelected) {
                    selectedRow = visualIndex;
                }
                continue;
            }

            const int groupIndex = visualIndex < allPos ? visualIndex : visualIndex - 1;
            QListWidgetItem* item = new QListWidgetItem(m_groups.at(groupIndex).name, m_groupList);
            item->setData(Qt::UserRole, groupIndex);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            if (!currentGroupName.isEmpty() && item->text().trimmed() == currentGroupName) {
                selectedRow = visualIndex;
            }
        }

        if (selectedRow < 0 && m_groupList->count() > 0) {
            if (m_groups.isEmpty()) {
                selectedRow = qBound(0, allPos, m_groupList->count() - 1);
            } else {
                selectedRow = (allPos == 0) ? 1 : 0;
            }
        }
        if (selectedRow >= 0) {
            m_groupList->setCurrentRow(selectedRow);
        }
    }

    refreshCurrentGroupSelection();
}

void SettingsDialog::reloadDialogDataFromYaml() {
    if (m_dataYamlPath.trimmed().isEmpty()) {
        return;
    }

    const QVector<StockItem> reloadedStocks = ConfigManager::loadStocksFromYaml(m_dataYamlPath);
    QVector<StockItem> filteredStocks;
    filteredStocks.reserve(reloadedStocks.size());
    for (const StockItem& stock : reloadedStocks) {
        if (!isPredefinedIndexCode(stock.code)) {
            filteredStocks.push_back(stock);
        }
    }
    m_stocks = std::move(filteredStocks);

    QSet<QString> stockKeys;
    for (const StockItem& stock : m_stocks) {
        stockKeys.insert(watchCodeKey(stock.code));
    }

    m_groups = ConfigManager::loadGroupsFromYaml(m_dataYamlPath);
    for (StockGroup& group : m_groups) {
        QStringList reloadedCodes;
        for (const QString& code : group.stockCodes) {
            if (stockKeys.contains(watchCodeKey(code))) {
                reloadedCodes.append(code);
            }
        }
        group.stockCodes = std::move(reloadedCodes);
    }

    if (m_stockTable) {
        QSignalBlocker blocker(m_stockTable);
        m_stockTable->setRowCount(0);

        StockTableWidget* table = static_cast<StockTableWidget*>(m_stockTable);
        for (const StockItem& stock : m_stocks) {
            const QString displayName = m_apiNamesByCode.value(stock.code, stock.name);
            table->addStockRow(stock.code, displayName, stock.cost);
        }
        if (m_stockTable->rowCount() > 0) {
            m_stockTable->setCurrentCell(0, 0);
        }
    }

    if (m_stockSuggestList) {
        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
    }
    if (m_stockSearchEdit) {
        m_stockSearchEdit->clear();
    }

    refreshGroupStockChoices();
    rebuildGroupList();
}

QString SettingsDialog::trText(const QString& key) const {
    return i18n::t(key, m_uiLanguage);
}

void SettingsDialog::updateHotkeyIndicator(QLabel* indicator, const QKeySequence& seq) {
    if (!indicator) {
        return;
    }

    if (seq.isEmpty()) {
        indicator->clear();
        indicator->setToolTip(QString());
        return;
    }

    // Valid format: has at least one modifier key + a real key → green ✓
    indicator->setText(QStringLiteral("\u2713"));
    indicator->setStyleSheet(QStringLiteral("color: green; font-weight: bold; font-size: 14px;"));
    indicator->setToolTip(trText("hotkey.available"));
}

void SettingsDialog::paintColorButton(QPushButton* btn, const QColor& c) {
    btn->setText(QString("%1,%2,%3,%4").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()));
    const QString fg = (c.lightness() < 128) ? "white" : "black";
    btn->setStyleSheet(
        QString("background-color: rgba(%1,%2,%3,%4); color:%5;")
            .arg(c.red())
            .arg(c.green())
            .arg(c.blue())
            .arg(c.alpha())
            .arg(fg)
    );
}

QPushButton* SettingsDialog::createColorButton(
    QWidget* parent,
    const QColor& color,
    const QString& pickTitle
) {
    QPushButton* btn = new QPushButton(parent);
    btn->setProperty("pickColor", color);
    paintColorButton(btn, color);

    QObject::connect(btn, &QPushButton::clicked, parent, [btn, parent, pickTitle]() {
        const QColor current = btn->property("pickColor").value<QColor>();
        QColorDialog dialog(current, parent);
        dialog.setWindowTitle(pickTitle);
        const QIcon windowIcon = dialogWindowIcon(parent);
        if (!windowIcon.isNull()) {
            dialog.setWindowIcon(windowIcon);
        }
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QColor picked = dialog.selectedColor();
        if (!picked.isValid()) {
            return;
        }
        btn->setProperty("pickColor", picked);
        paintColorButton(btn, picked);
    });

    return btn;
}

QColor SettingsDialog::buttonColor(QPushButton* btn) {
    return btn->property("pickColor").value<QColor>();
}

QWidget* SettingsDialog::buildGeneralTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(w);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    QWidget* baseSection = new QWidget(w);
    QFormLayout* form = new QFormLayout(baseSection);
    applyCompactFormLayout(form);

    m_pollSpin = new QSpinBox(w);
    m_pollSpin->setRange(3000, 60000);
    m_pollSpin->setSingleStep(10);
    m_pollSpin->setValue(qMax(3000, m_cfg.pollMs));
    applyNumericSpinBoxWidth(m_pollSpin);

    auto buildHotkeyEditorWidget = [this, w](
        const QString& configuredHotkey,
        QKeySequenceEdit*& edit,
        QLabel*& indicator,
        QPushButton*& clearBtn
    ) {
        edit = new QKeySequenceEdit(QKeySequence(configuredHotkey), w);
        edit->setToolTip(trText("settings.general.hotkeyHint"));

        indicator = new QLabel(w);
        indicator->setMinimumWidth(22);
        indicator->setAlignment(Qt::AlignCenter);

        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
            [this, edit, indicator](const QKeySequence& sequence) {
                if (m_normalizingHotkeySequence || !edit) {
                    return;
                }

                const QKeySequence normalized = normalizedHotkeySequence(sequence);
                if (normalized != sequence) {
                    m_normalizingHotkeySequence = true;
                    edit->setKeySequence(normalized);
                    m_normalizingHotkeySequence = false;
                }

                updateHotkeyIndicator(indicator, normalized);
            }
        );

        const QKeySequence initialNormalized = normalizedHotkeySequence(edit->keySequence());
        if (initialNormalized != edit->keySequence()) {
            edit->setKeySequence(initialNormalized);
        }
        updateHotkeyIndicator(indicator, normalizedHotkeySequence(edit->keySequence()));

        clearBtn = new QPushButton(QStringLiteral("✕"), w);
        clearBtn->setFixedWidth(28);
        clearBtn->setToolTip(trText("settings.general.hotkeyClear"));
        connect(clearBtn, &QPushButton::clicked, this, [edit]() {
            if (edit) {
                edit->setKeySequence(QKeySequence());
            }
        });

        QWidget* widget = new QWidget(w);
        QHBoxLayout* layout = new QHBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        layout->addWidget(edit, 1);
        layout->addWidget(indicator);
        layout->addWidget(clearBtn);
        return widget;
    };

    QWidget* hotkeyWidget = buildHotkeyEditorWidget(
        m_cfg.hotkey,
        m_hotkeyEdit,
        m_hotkeyIndicator,
        m_hotkeyClearBtn
    );

    QWidget* marketBreadthHotkeyWidget = buildHotkeyEditorWidget(
        m_cfg.marketBreadthHotkey,
        m_marketBreadthHotkeyEdit,
        m_marketBreadthHotkeyIndicator,
        m_marketBreadthHotkeyClearBtn
    );

    m_startupShowFloatingWindowCheck = new QCheckBox(
        trText("settings.general.startupShowFloatingWindow"),
        w
    );
    m_startupShowFloatingWindowCheck->setChecked(m_cfg.startupShowFloatingWindow);

    m_languageCombo = new QComboBox(w);
    m_languageCombo->addItem(trText("settings.language.auto"), "auto");
    m_languageCombo->addItem(trText("settings.language.zh"), "zh_CN");
    m_languageCombo->addItem(trText("settings.language.en"), "en_US");
    const int languageIndex = m_languageCombo->findData(i18n::normalizeLanguage(m_cfg.language));
    if (languageIndex >= 0) {
        m_languageCombo->setCurrentIndex(languageIndex);
    }

    form->addRow(trText("settings.general.poll"), m_pollSpin);
    addCompactFormRow(form, m_startupShowFloatingWindowCheck);
    form->addRow(trText("settings.general.language"), m_languageCombo);
    root->addWidget(baseSection);

    QGroupBox* hotkeyGroup = new QGroupBox(trText("settings.general.groupHotkey"), w);
    QFormLayout* hotkeyForm = new QFormLayout(hotkeyGroup);
    applyCompactFormLayout(hotkeyForm);
    hotkeyForm->setContentsMargins(6, 6, 6, 6);
    hotkeyForm->setVerticalSpacing(8);
    hotkeyForm->addRow(trText("settings.general.floatingWindowHotkey"), hotkeyWidget);
    hotkeyForm->addRow(trText("settings.general.marketBreadthHotkey"), marketBreadthHotkeyWidget);

    // ── Group switch hotkey prefix ─────────────────────────────────────────
    {
        const QVector<QPair<QString,QString>> modOptions = {
            {QStringLiteral("—"), QStringLiteral("")},   // — (None)
#if defined(Q_OS_MACOS)
            {QStringLiteral("Shift"),   QStringLiteral("Shift")},
            {QStringLiteral("Control"), QStringLiteral("Meta")},
            {QStringLiteral("Option"),  QStringLiteral("Alt")},
            {QStringLiteral("Command"), QStringLiteral("Ctrl")},
#else
            {QStringLiteral("Shift"),   QStringLiteral("Shift")},
            {QStringLiteral("Ctrl"),    QStringLiteral("Ctrl")},
            {QStringLiteral("Alt"),     QStringLiteral("Alt")},
            {QStringLiteral("Win"),     QStringLiteral("Meta")},
#endif
        };

        const auto fillCombo = [&modOptions](QComboBox* cb) {
            for (const auto& e : modOptions)
                cb->addItem(e.first, e.second);
        };
        const auto findModIndex = [&modOptions](const QString& qtName) -> int {
            for (int i = 0; i < modOptions.size(); ++i)
                if (modOptions[i].second == qtName) return i;
            return 0;
        };

        m_groupMod1Combo = new QComboBox(hotkeyGroup);
        fillCombo(m_groupMod1Combo);
        m_groupMod2Combo = new QComboBox(hotkeyGroup);
        fillCombo(m_groupMod2Combo);

        {
            const QStringList prefParts =
                m_cfg.groupSwitchHotkeyPrefix.split(QLatin1Char('+'), Qt::SkipEmptyParts);
            if (prefParts.size() >= 1) m_groupMod1Combo->setCurrentIndex(findModIndex(prefParts[0]));
            if (prefParts.size() >= 2) m_groupMod2Combo->setCurrentIndex(findModIndex(prefParts[1]));
        }

        m_groupHotkeyPreviewLabel = new QLabel(hotkeyGroup);
        m_groupHotkeyPreviewLabel->setWordWrap(true);

        QWidget* groupHotkeyWidget = new QWidget(hotkeyGroup);
        QHBoxLayout* groupHotkeyLayout = new QHBoxLayout(groupHotkeyWidget);
        groupHotkeyLayout->setContentsMargins(0, 0, 0, 0);
        groupHotkeyLayout->setSpacing(6);
        groupHotkeyLayout->addWidget(m_groupMod1Combo);
        groupHotkeyLayout->addWidget(m_groupMod2Combo);
        groupHotkeyLayout->addWidget(m_groupHotkeyPreviewLabel, 1);
        hotkeyForm->addRow(trText("settings.general.groupSwitchHotkey"), groupHotkeyWidget);

        const auto updatePreview = [this]() {
            if (!m_groupHotkeyPreviewLabel) return;
            const QString& prefix = m_cfg.groupSwitchHotkeyPrefix;
            if (prefix.isEmpty() || m_groups.isEmpty()) {
                m_groupHotkeyPreviewLabel->setText({});
                return;
            }
            const int total = 1 + m_groups.size();
            QStringList lines;
            for (int vi = 0; vi < qMin(total, 10); ++vi) {
                QString name;
                if (vi == m_cfg.groupAllPosition) {
                    name = trText("settings.group.allGroup");
                } else {
                    const int gi = vi < m_cfg.groupAllPosition ? vi : vi - 1;
                    name = (gi >= 0 && gi < m_groups.size())
                        ? m_groups.at(gi).name : QStringLiteral("?");
                }
                lines << QStringLiteral("%1+F%2 (%3)").arg(prefix).arg(vi + 1).arg(name);
            }
            m_groupHotkeyPreviewLabel->setText(lines.join(QStringLiteral("  ")));
        };

        const auto updatePrefixFromCombos = [this, updatePreview]() {
            const QString qt1 = m_groupMod1Combo->currentData().toString();
            const QString qt2 = m_groupMod2Combo->currentData().toString();
            if (!qt1.isEmpty() && !qt2.isEmpty() && qt1 != qt2) {
                m_cfg.groupSwitchHotkeyPrefix = qt1 + QLatin1Char('+') + qt2;
            } else {
                m_cfg.groupSwitchHotkeyPrefix.clear();
            }
            updatePreview();
        };

        updatePreview();

        connect(m_groupMod1Combo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, updatePrefixFromCombos);
        connect(m_groupMod2Combo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, updatePrefixFromCombos);
    }
    root->addWidget(hotkeyGroup);

    QGroupBox* fontGroup = new QGroupBox(trText("settings.general.groupFont"), w);
    QFormLayout* fontForm = new QFormLayout(fontGroup);
    applyCompactFormLayout(fontForm);
    fontForm->setContentsMargins(6, 6, 6, 6);
    fontForm->setVerticalSpacing(8);

    QFont effectiveBaseFont = w->font();
    const QString defaultFamily = defaultFloatingWindowFontFamily();
    if (!defaultFamily.isEmpty()) {
        effectiveBaseFont.setFamily(defaultFamily);
    }
    m_fontFamilyCombo = new QFontComboBox(fontGroup);
    if (!m_cfg.floatingWindowFontFamily.trimmed().isEmpty()) {
        m_fontFamilyCombo->setCurrentFont(QFont(m_cfg.floatingWindowFontFamily));
    } else {
        m_fontFamilyCombo->setCurrentFont(effectiveBaseFont);
    }

    m_fontSizeSpin = new QSpinBox(fontGroup);
    m_fontSizeSpin->setRange(6, 72);
    m_fontSizeSpin->setValue(
        qBound(6, m_cfg.floatingWindowFontSize > 0
            ? m_cfg.floatingWindowFontSize
            : qMax(6, effectiveBaseFont.pointSize()), 72)
    );
    applyNumericSpinBoxWidth(m_fontSizeSpin);

    m_fontBoldCheck = new QCheckBox(trText("settings.general.fontBold"), fontGroup);
    m_fontBoldCheck->setChecked(m_cfg.floatingWindowFontBold);

    QPushButton* resetFontButton = new QPushButton(
        trText("settings.general.resetFont"),
        fontGroup
    );
    connect(resetFontButton, &QPushButton::clicked, this, [this, effectiveBaseFont]() {
        if (m_fontFamilyCombo) {
            m_fontFamilyCombo->setCurrentFont(effectiveBaseFont);
        }
        if (m_fontSizeSpin) {
            m_fontSizeSpin->setValue(qMax(6, effectiveBaseFont.pointSize()));
        }
        if (m_fontBoldCheck) {
            m_fontBoldCheck->setChecked(false);
        }
    });

    fontForm->addRow(trText("settings.general.fontFamily"), m_fontFamilyCombo);
    fontForm->addRow(trText("settings.general.fontSize"), m_fontSizeSpin);
    addCompactFormRow(fontForm, m_fontBoldCheck);
    addCompactFormRow(fontForm, resetFontButton);
    root->addWidget(fontGroup);
    root->addStretch(1);

    return w;
}

QWidget* SettingsDialog::buildNetworkTab() {
    QWidget* w = new QWidget(this);
    QFormLayout* form = new QFormLayout(w);
    applyCompactFormLayout(form);

    m_userAgentEdit = new QLineEdit(m_cfg.userAgent, w);
    if (m_userAgentEdit->text().trimmed().isEmpty()) {
        m_userAgentEdit->setText(defaultChrome100UserAgent());
    }

    m_proxyTypeCombo = new QComboBox(w);
    m_proxyTypeCombo->addItem(trText("settings.proxy.none"), "none");
    m_proxyTypeCombo->addItem(trText("settings.proxy.http"), "http");
    m_proxyTypeCombo->addItem(trText("settings.proxy.socks5"), "socks5");
    int proxyTypeIndex = m_proxyTypeCombo->findData(m_cfg.proxyType.trimmed().toLower());
    if (proxyTypeIndex < 0) {
        proxyTypeIndex = 0;
    }
    m_proxyTypeCombo->setCurrentIndex(proxyTypeIndex);

    m_proxyHostEdit = new QLineEdit(m_cfg.proxyHost, w);

    m_proxyPortSpin = new QSpinBox(w);
    m_proxyPortSpin->setRange(0, 65535);
    m_proxyPortSpin->setValue(qBound(0, m_cfg.proxyPort, 65535));
    applyNumericSpinBoxWidth(m_proxyPortSpin);

    m_proxyUserEdit = new QLineEdit(m_cfg.proxyUser, w);

    m_proxyPasswordEdit = new QLineEdit(m_cfg.proxyPassword, w);
    m_proxyPasswordEdit->setEchoMode(QLineEdit::Password);

    form->addRow(trText("settings.general.userAgent"), m_userAgentEdit);
    form->addRow(trText("settings.general.proxyType"), m_proxyTypeCombo);
    form->addRow(trText("settings.general.proxyHost"), m_proxyHostEdit);
    form->addRow(trText("settings.general.proxyPort"), m_proxyPortSpin);
    form->addRow(trText("settings.general.proxyUser"), m_proxyUserEdit);
    form->addRow(trText("settings.general.proxyPassword"), m_proxyPasswordEdit);

    return w;
}

QWidget* SettingsDialog::buildDisplayTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(w);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    QGroupBox* windowGroup = new QGroupBox(trText("settings.display.groupWindow"), w);
    QFormLayout* windowForm = new QFormLayout(windowGroup);
    applyCompactFormLayout(windowForm);
    windowForm->setContentsMargins(6, 6, 6, 6);
    windowForm->setVerticalSpacing(8);

    QGroupBox* interactionGroup = new QGroupBox(trText("settings.display.groupInteraction"), w);
    QFormLayout* interactionForm = new QFormLayout(interactionGroup);
    applyCompactFormLayout(interactionForm);
    interactionForm->setContentsMargins(6, 6, 6, 6);
    interactionForm->setVerticalSpacing(8);

    QGroupBox* timelineGroup = new QGroupBox(trText("settings.display.groupTimeline"), w);
    QFormLayout* timelineForm = new QFormLayout(timelineGroup);
    applyCompactFormLayout(timelineForm);
    timelineForm->setContentsMargins(6, 6, 6, 6);
    timelineForm->setVerticalSpacing(8);

    QGroupBox* columnsGroup = new QGroupBox(trText("settings.display.groupColumns"), w);
    QFormLayout* columnsForm = new QFormLayout(columnsGroup);
    applyCompactFormLayout(columnsForm);
    columnsForm->setContentsMargins(6, 6, 6, 6);
    columnsForm->setVerticalSpacing(8);

    m_floatingTopMostCheck = new QCheckBox(trText("settings.display.alwaysOnTop"), w);
    m_floatingTopMostCheck->setChecked(m_cfg.floatingWindowAlwaysOnTop);
    addCompactFormRow(windowForm, m_floatingTopMostCheck);

    m_doubleClickCloseWindowCheck = new QCheckBox(
        trText("settings.display.doubleClickCloseWindow"),
        w
    );
    m_doubleClickCloseWindowCheck->setChecked(
        m_cfg.floatingWindowDoubleClickToHide && !m_cfg.mousePassthroughEnabled
    );
    m_doubleClickCloseWindowCheck->setEnabled(!m_cfg.mousePassthroughEnabled);
    addCompactFormRow(windowForm, m_doubleClickCloseWindowCheck);

    m_doubleClickStockDetailCheck = new QCheckBox(
        trText("settings.display.doubleClickStockDetail"),
        w
    );
    m_doubleClickStockDetailCheck->setChecked(m_cfg.floatingWindowDoubleClickStockDetail);
    addCompactFormRow(windowForm, m_doubleClickStockDetailCheck);

    m_showHeaderCheck = new QCheckBox(trText("settings.display.showHeader"), w);
    m_showHeaderCheck->setChecked(m_cfg.showHeader);
    addCompactFormRow(windowForm, m_showHeaderCheck);

    m_showGridCheck = new QCheckBox(trText("settings.display.showGrid"), w);
    m_showGridCheck->setChecked(m_cfg.showGrid);
    m_gridColorBtn = createColorButton(w, m_cfg.gridColor, trText("settings.color.pick"));
    m_gridColorBtn->setEnabled(m_cfg.showGrid);
    connect(m_showGridCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_gridColorBtn->setEnabled(checked);
    });
    addCompactFormRow(windowForm, m_showGridCheck);
    windowForm->addRow(trText("settings.display.gridColor"), m_gridColorBtn);

    m_windowPaddingSpin = new QDoubleSpinBox(w);
    m_windowPaddingSpin->setRange(0.0, 120.0);
    m_windowPaddingSpin->setSingleStep(0.5);
    m_windowPaddingSpin->setDecimals(1);
    m_windowPaddingSpin->setSuffix(trText("settings.display.pxSuffix"));
    m_windowPaddingSpin->setValue(qMax(0.0, m_cfg.floatingWindowPaddingPx));
    applyNumericSpinBoxWidth(m_windowPaddingSpin);
    windowForm->addRow(trText("settings.display.padding"), m_windowPaddingSpin);

    m_transparentBackgroundCheck = new QCheckBox(
        trText("settings.general.transparentBackground"),
        w
    );
    m_transparentBackgroundCheck->setChecked(m_cfg.transparentBackgroundEnabled);

    m_transparentOpacitySlider = new QSlider(Qt::Horizontal, w);
    m_transparentOpacitySlider->setRange(0, 100);
    m_transparentOpacitySlider->setValue(qBound(0, m_cfg.transparentBackgroundOpacity, 100));

    m_transparentOpacityLabel = new QLabel(w);

    QWidget* transparentOpacityWidget = new QWidget(w);
    QHBoxLayout* transparentOpacityLayout = new QHBoxLayout(transparentOpacityWidget);
    transparentOpacityLayout->setContentsMargins(0, 0, 0, 0);
    transparentOpacityLayout->setSpacing(6);
    transparentOpacityLayout->addWidget(m_transparentOpacitySlider, 1);
    transparentOpacityLayout->addWidget(m_transparentOpacityLabel);

    const QString pickColorTitle = trText("settings.color.pick");
    m_bgBtn = createColorButton(w, m_cfg.bgColor, pickColorTitle);
    m_textBtn = createColorButton(w, m_cfg.textColor, pickColorTitle);
    m_upBtn = createColorButton(w, m_cfg.upColor, pickColorTitle);
    m_downBtn = createColorButton(w, m_cfg.downColor, pickColorTitle);
    m_flatBtn = createColorButton(w, m_cfg.flatColor, pickColorTitle);
    m_costLineColorBtn = createColorButton(w, m_cfg.costLineColor, pickColorTitle);

    const AppConfig defaultCfg;
    QPushButton* resetColorsButton = new QPushButton(
        trText("settings.general.resetColors"),
        w
    );
    connect(resetColorsButton, &QPushButton::clicked, this, [this, defaultCfg]() {
        m_bgBtn->setProperty("pickColor", defaultCfg.bgColor);
        m_textBtn->setProperty("pickColor", defaultCfg.textColor);
        m_upBtn->setProperty("pickColor", defaultCfg.upColor);
        m_downBtn->setProperty("pickColor", defaultCfg.downColor);
        m_flatBtn->setProperty("pickColor", defaultCfg.flatColor);
        m_costLineColorBtn->setProperty("pickColor", defaultCfg.costLineColor);

        paintColorButton(m_bgBtn, defaultCfg.bgColor);
        paintColorButton(m_textBtn, defaultCfg.textColor);
        paintColorButton(m_upBtn, defaultCfg.upColor);
        paintColorButton(m_downBtn, defaultCfg.downColor);
        paintColorButton(m_flatBtn, defaultCfg.flatColor);
        paintColorButton(m_costLineColorBtn, defaultCfg.costLineColor);
    });

    connect(m_transparentOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        m_transparentOpacityLabel->setText(
            trText("settings.general.transparentOpacityValue").arg(value)
        );
    });

    connect(m_transparentBackgroundCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        m_transparentOpacitySlider->setEnabled(enabled);
        m_transparentOpacityLabel->setEnabled(enabled);
        m_bgBtn->setEnabled(!enabled);
    });

    m_transparentOpacityLabel->setText(
        trText("settings.general.transparentOpacityValue")
            .arg(m_transparentOpacitySlider->value())
    );
    const bool transparentModeEnabled = m_transparentBackgroundCheck->isChecked();
    m_transparentOpacitySlider->setEnabled(transparentModeEnabled);
    m_transparentOpacityLabel->setEnabled(transparentModeEnabled);
    m_bgBtn->setEnabled(!transparentModeEnabled);

    addCompactFormRow(windowForm, m_transparentBackgroundCheck);
    windowForm->addRow(trText("settings.general.transparentOpacity"), transparentOpacityWidget);
    windowForm->addRow(trText("settings.general.background"), m_bgBtn);
    windowForm->addRow(trText("settings.general.text"), m_textBtn);
    windowForm->addRow(trText("settings.general.up"), m_upBtn);
    windowForm->addRow(trText("settings.general.down"), m_downBtn);
    windowForm->addRow(trText("settings.general.flat"), m_flatBtn);
    windowForm->addRow(trText("settings.display.costLineColor"), m_costLineColorBtn);
    addCompactFormRow(windowForm, resetColorsButton);

    m_simpleModeCheck = new QCheckBox(trText("settings.display.simpleMode"), w);
    m_simpleModeCheck->setChecked(m_cfg.simpleModeEnabled);
    addCompactFormRow(interactionForm, m_simpleModeCheck);

    m_blinkReminderCheck = new QCheckBox(trText("settings.display.blinkReminder"), w);
    m_blinkReminderCheck->setChecked(m_cfg.blinkReminderEnabled);
    m_blinkReminderCheck->setEnabled(m_cfg.simpleModeEnabled);
    connect(m_simpleModeCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_blinkReminderCheck->setEnabled(checked);
    });
    addCompactFormRow(interactionForm, m_blinkReminderCheck);

    m_trayTooltipCheck = new QCheckBox(trText("settings.display.trayTooltip"), w);
    m_trayTooltipCheck->setChecked(m_cfg.trayTooltipEnabled);
    addCompactFormRow(interactionForm, m_trayTooltipCheck);

    m_hoverReadingCheck = new QCheckBox(trText("settings.display.hoverReading"), w);
    m_hoverReadingCheck->setChecked(m_cfg.hoverReadingEnabled);
    addCompactFormRow(interactionForm, m_hoverReadingCheck);

    m_hoverReadingDelaySpin = new QDoubleSpinBox(w);
    m_hoverReadingDelaySpin->setRange(0.1, 60.0);
    m_hoverReadingDelaySpin->setSingleStep(0.1);
    m_hoverReadingDelaySpin->setDecimals(1);
    m_hoverReadingDelaySpin->setSuffix(trText("settings.display.hoverReadingDelaySuffix"));
    m_hoverReadingDelaySpin->setValue(qBound(0.1, m_cfg.hoverReadingDelaySecs, 60.0));
    applyNumericSpinBoxWidth(m_hoverReadingDelaySpin);
    m_hoverReadingDelaySpin->setEnabled(
        m_cfg.hoverReadingEnabled && !m_cfg.mousePassthroughEnabled
    );

    m_hoverReadingModeCombo = new QComboBox(w);
    m_hoverReadingModeCombo->addItem(trText("settings.display.hoverReadingModeLight"), "light");
    m_hoverReadingModeCombo->addItem(trText("settings.display.hoverReadingModeDark"), "dark");
    int hoverReadingModeIndex = m_hoverReadingModeCombo->findData(
        normalizeHoverReadingUiMode(m_cfg.hoverReadingUiMode)
    );
    if (hoverReadingModeIndex < 0) {
        hoverReadingModeIndex = m_hoverReadingModeCombo->findData(QStringLiteral("dark"));
    }
    if (hoverReadingModeIndex >= 0) {
        m_hoverReadingModeCombo->setCurrentIndex(hoverReadingModeIndex);
    }
    m_hoverReadingModeCombo->setEnabled(m_cfg.hoverReadingEnabled);

    m_hoverReadingTransparentBackgroundCheck = new QCheckBox(
        trText("settings.display.hoverReadingTransparentBackground"),
        w
    );
    m_hoverReadingTransparentBackgroundCheck->setChecked(
        m_cfg.hoverReadingTransparentBackgroundEnabled
    );
    m_hoverReadingTransparentBackgroundCheck->setEnabled(m_cfg.hoverReadingEnabled);

    m_mousePassthroughCheck = new QCheckBox(trText("settings.display.mousePassthrough"), w);
    m_mousePassthroughCheck->setChecked(m_cfg.mousePassthroughEnabled);
    addCompactFormRow(interactionForm, m_mousePassthroughCheck);

    m_mousePassthroughKeyCombo = new QComboBox(w);
    m_mousePassthroughKeyCombo->addItem(
        trText("settings.display.mousePassthroughKeyCtrl"),
        QStringLiteral("ctrl")
    );
    m_mousePassthroughKeyCombo->addItem(
        trText("settings.display.mousePassthroughKeyShift"),
        QStringLiteral("shift")
    );
#if defined(Q_OS_MACOS)
    m_mousePassthroughKeyCombo->addItem(
        trText("settings.display.mousePassthroughKeyCommand"),
        QStringLiteral("command")
    );
#endif
    m_mousePassthroughKeyCombo->addItem(
        trText("settings.display.mousePassthroughKeyAlt"),
        QStringLiteral("alt")
    );
    int mousePassthroughKeyIndex = m_mousePassthroughKeyCombo->findData(
        normalizeMousePassthroughActivationKey(m_cfg.mousePassthroughActivationKey)
    );
    if (mousePassthroughKeyIndex < 0) {
        mousePassthroughKeyIndex = m_mousePassthroughKeyCombo->findData(QStringLiteral("ctrl"));
    }
    if (mousePassthroughKeyIndex >= 0) {
        m_mousePassthroughKeyCombo->setCurrentIndex(mousePassthroughKeyIndex);
    }
    m_mousePassthroughKeyCombo->setEnabled(m_cfg.mousePassthroughEnabled);

    interactionForm->addRow(
        trText("settings.display.mousePassthroughTrigger"),
        m_mousePassthroughKeyCombo
    );
    interactionForm->addRow(trText("settings.display.hoverReadingDelay"), m_hoverReadingDelaySpin);
    interactionForm->addRow(trText("settings.display.hoverReadingMode"), m_hoverReadingModeCombo);
    addCompactFormRow(interactionForm, m_hoverReadingTransparentBackgroundCheck);

    QWidget* hoverReadingDelayLabel = interactionForm->labelForField(m_hoverReadingDelaySpin);
    QWidget* hoverReadingModeLabel = interactionForm->labelForField(m_hoverReadingModeCombo);
    QWidget* mousePassthroughKeyLabel = interactionForm->labelForField(m_mousePassthroughKeyCombo);
    QWidget* doubleClickCloseWindowLabel = windowForm->labelForField(m_doubleClickCloseWindowCheck);
    if (hoverReadingDelayLabel) {
        hoverReadingDelayLabel->setEnabled(
            m_cfg.hoverReadingEnabled && !m_cfg.mousePassthroughEnabled
        );
    }
    if (hoverReadingModeLabel) {
        hoverReadingModeLabel->setEnabled(m_cfg.hoverReadingEnabled);
    }
    if (mousePassthroughKeyLabel) {
        mousePassthroughKeyLabel->setEnabled(m_cfg.mousePassthroughEnabled);
    }
    if (doubleClickCloseWindowLabel) {
        doubleClickCloseWindowLabel->setEnabled(!m_cfg.mousePassthroughEnabled);
    }

    connect(m_hoverReadingCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_hoverReadingDelaySpin->setEnabled(checked && !m_cfg.mousePassthroughEnabled);
        if (m_hoverReadingModeCombo) {
            m_hoverReadingModeCombo->setEnabled(checked);
        }
        if (m_hoverReadingTransparentBackgroundCheck) {
            m_hoverReadingTransparentBackgroundCheck->setEnabled(checked);
        }
    });
    connect(
        m_hoverReadingCheck,
        &QCheckBox::toggled,
        this,
        [this, hoverReadingDelayLabel](bool checked) {
        if (hoverReadingDelayLabel) {
            hoverReadingDelayLabel->setEnabled(checked && !m_cfg.mousePassthroughEnabled);
        }
    });
    connect(m_hoverReadingCheck, &QCheckBox::toggled, this, [hoverReadingModeLabel](bool checked) {
        if (hoverReadingModeLabel) {
            hoverReadingModeLabel->setEnabled(checked);
        }
    });
    connect(m_mousePassthroughCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_cfg.mousePassthroughEnabled = checked;
        if (m_hoverReadingDelaySpin) {
            const bool hoverReadingEnabled = m_hoverReadingCheck && m_hoverReadingCheck->isChecked();
            m_hoverReadingDelaySpin->setEnabled(hoverReadingEnabled && !checked);
        }
        if (m_mousePassthroughKeyCombo) {
            m_mousePassthroughKeyCombo->setEnabled(checked);
        }
        if (m_doubleClickCloseWindowCheck) {
            if (checked) {
                m_doubleClickCloseWindowCheck->setChecked(false);
            }
            m_doubleClickCloseWindowCheck->setEnabled(!checked);
        }
    });
    connect(
        m_mousePassthroughCheck,
        &QCheckBox::toggled,
        this,
        [this, hoverReadingDelayLabel, mousePassthroughKeyLabel, doubleClickCloseWindowLabel](bool checked) {
            if (hoverReadingDelayLabel) {
                const bool hoverReadingEnabled = m_hoverReadingCheck
                    && m_hoverReadingCheck->isChecked();
                hoverReadingDelayLabel->setEnabled(hoverReadingEnabled && !checked);
            }
            if (mousePassthroughKeyLabel) {
                mousePassthroughKeyLabel->setEnabled(checked);
            }
            if (doubleClickCloseWindowLabel) {
                doubleClickCloseWindowLabel->setEnabled(!checked);
            }
        }
    );

    m_timelineChartEnabledCheck = new QCheckBox(trText("settings.display.timelineEnabled"), w);
    m_timelineChartEnabledCheck->setChecked(m_cfg.timelineChartEnabled);
    addCompactFormRow(timelineForm, m_timelineChartEnabledCheck);

    m_marketBreadthEnabledCheck = new QCheckBox(trText("settings.display.marketBreadthEnabled"), w);
    m_marketBreadthEnabledCheck->setChecked(m_cfg.marketBreadthEnabled);
    addCompactFormRow(timelineForm, m_marketBreadthEnabledCheck);

    m_marketBreadthRefreshSecsSpin = new QSpinBox(w);
    m_marketBreadthRefreshSecsSpin->setRange(10, 3600);
    m_marketBreadthRefreshSecsSpin->setSingleStep(1);
    m_marketBreadthRefreshSecsSpin->setSuffix(trText("settings.display.marketBreadthRefreshSuffix"));
    m_marketBreadthRefreshSecsSpin->setValue(qBound(10, m_cfg.marketBreadthRefreshSecs, 3600));
    applyNumericSpinBoxWidth(m_marketBreadthRefreshSecsSpin);
    timelineForm->addRow(trText("settings.display.marketBreadthRefresh"), m_marketBreadthRefreshSecsSpin);

    m_timelineChartRefreshSecsSpin = new QSpinBox(w);
    m_timelineChartRefreshSecsSpin->setRange(10, 3600);
    m_timelineChartRefreshSecsSpin->setSingleStep(1);
    m_timelineChartRefreshSecsSpin->setSuffix(trText("settings.display.timelineRefreshSuffix"));
    m_timelineChartRefreshSecsSpin->setValue(qBound(10, m_cfg.timelineChartRefreshSecs, 3600));
    applyNumericSpinBoxWidth(m_timelineChartRefreshSecsSpin);
    timelineForm->addRow(trText("settings.display.timelineRefresh"), m_timelineChartRefreshSecsSpin);

    m_timelineChartFixedRangeCheck = new QCheckBox(trText("settings.display.timelineFixedRange"), w);
    m_timelineChartFixedRangeCheck->setChecked(m_cfg.timelineChartFixedRangeEnabled);
    addCompactFormRow(timelineForm, m_timelineChartFixedRangeCheck);

    const QString timelinePickColorTitle = trText("settings.color.pick");
    m_timelineChartBgBtn = createColorButton(
        w,
        m_cfg.timelineChartBgColor,
        timelinePickColorTitle
    );
    m_timelineChartGridBtn = createColorButton(
        w,
        m_cfg.timelineChartGridColor,
        timelinePickColorTitle
    );
    m_timelineChartPriceLineBtn = createColorButton(
        w,
        m_cfg.timelineChartPriceLineColor,
        timelinePickColorTitle
    );
    m_timelineChartAvgLineBtn = createColorButton(
        w,
        m_cfg.timelineChartAvgLineColor,
        timelinePickColorTitle
    );
    m_timelineChartTextBtn = createColorButton(
        w,
        m_cfg.timelineChartTextColor,
        timelinePickColorTitle
    );
    m_timelineChartUpBtn = createColorButton(
        w,
        m_cfg.timelineChartUpColor,
        timelinePickColorTitle
    );
    m_timelineChartDownBtn = createColorButton(
        w,
        m_cfg.timelineChartDownColor,
        timelinePickColorTitle
    );

    timelineForm->addRow(trText("settings.display.timelineBg"), m_timelineChartBgBtn);
    timelineForm->addRow(trText("settings.display.timelineGrid"), m_timelineChartGridBtn);
    timelineForm->addRow(trText("settings.display.timelinePriceLine"), m_timelineChartPriceLineBtn);
    timelineForm->addRow(trText("settings.display.timelineAvgLine"), m_timelineChartAvgLineBtn);
    timelineForm->addRow(trText("settings.display.timelineText"), m_timelineChartTextBtn);
    timelineForm->addRow(trText("settings.display.timelineUp"), m_timelineChartUpBtn);
    timelineForm->addRow(trText("settings.display.timelineDown"), m_timelineChartDownBtn);

    QPushButton* resetTimelineColorsButton = new QPushButton(
        trText("settings.display.timelineResetColors"),
        w
    );
    const AppConfig timelineDefaultCfg;
    connect(resetTimelineColorsButton, &QPushButton::clicked, this, [this, timelineDefaultCfg]() {
        m_timelineChartBgBtn->setProperty("pickColor", timelineDefaultCfg.timelineChartBgColor);
        m_timelineChartGridBtn->setProperty("pickColor", timelineDefaultCfg.timelineChartGridColor);
        m_timelineChartPriceLineBtn->setProperty("pickColor", timelineDefaultCfg.timelineChartPriceLineColor);
        m_timelineChartAvgLineBtn->setProperty("pickColor", timelineDefaultCfg.timelineChartAvgLineColor);
        m_timelineChartTextBtn->setProperty("pickColor", timelineDefaultCfg.timelineChartTextColor);
        m_timelineChartUpBtn->setProperty("pickColor", timelineDefaultCfg.timelineChartUpColor);
        m_timelineChartDownBtn->setProperty("pickColor", timelineDefaultCfg.timelineChartDownColor);

        paintColorButton(m_timelineChartBgBtn, timelineDefaultCfg.timelineChartBgColor);
        paintColorButton(m_timelineChartGridBtn, timelineDefaultCfg.timelineChartGridColor);
        paintColorButton(m_timelineChartPriceLineBtn, timelineDefaultCfg.timelineChartPriceLineColor);
        paintColorButton(m_timelineChartAvgLineBtn, timelineDefaultCfg.timelineChartAvgLineColor);
        paintColorButton(m_timelineChartTextBtn, timelineDefaultCfg.timelineChartTextColor);
        paintColorButton(m_timelineChartUpBtn, timelineDefaultCfg.timelineChartUpColor);
        paintColorButton(m_timelineChartDownBtn, timelineDefaultCfg.timelineChartDownColor);
    });
    addCompactFormRow(timelineForm, resetTimelineColorsButton);

    QWidget* timelineRefreshLabel = timelineForm->labelForField(m_timelineChartRefreshSecsSpin);
    QWidget* marketBreadthRefreshLabel = timelineForm->labelForField(m_marketBreadthRefreshSecsSpin);
    const auto updateTimelineControlsState = [this, timelineRefreshLabel, marketBreadthRefreshLabel]() {
        const bool enabled = m_timelineChartEnabledCheck && m_timelineChartEnabledCheck->isChecked();
        const bool marketBreadthEnabled = m_marketBreadthEnabledCheck
            && m_marketBreadthEnabledCheck->isChecked();
        if (m_timelineChartRefreshSecsSpin) {
            m_timelineChartRefreshSecsSpin->setEnabled(enabled);
        }
        if (m_marketBreadthRefreshSecsSpin) {
            m_marketBreadthRefreshSecsSpin->setEnabled(marketBreadthEnabled);
        }
        if (m_timelineChartFixedRangeCheck) {
            m_timelineChartFixedRangeCheck->setEnabled(enabled);
        }
        if (timelineRefreshLabel) {
            timelineRefreshLabel->setEnabled(enabled);
        }
        if (marketBreadthRefreshLabel) {
            marketBreadthRefreshLabel->setEnabled(marketBreadthEnabled);
        }
        if (m_timelineChartBgBtn) {
            m_timelineChartBgBtn->setEnabled(enabled);
        }
        if (m_timelineChartGridBtn) {
            m_timelineChartGridBtn->setEnabled(enabled);
        }
        if (m_timelineChartPriceLineBtn) {
            m_timelineChartPriceLineBtn->setEnabled(enabled);
        }
        if (m_timelineChartAvgLineBtn) {
            m_timelineChartAvgLineBtn->setEnabled(enabled);
        }
        if (m_timelineChartTextBtn) {
            m_timelineChartTextBtn->setEnabled(enabled);
        }
        if (m_timelineChartUpBtn) {
            m_timelineChartUpBtn->setEnabled(enabled);
        }
        if (m_timelineChartDownBtn) {
            m_timelineChartDownBtn->setEnabled(enabled);
        }
    };
    connect(
        m_timelineChartEnabledCheck,
        &QCheckBox::toggled,
        this,
        [updateTimelineControlsState](bool) {
            updateTimelineControlsState();
        }
    );
    connect(
        m_marketBreadthEnabledCheck,
        &QCheckBox::toggled,
        this,
        [updateTimelineControlsState](bool) {
            updateTimelineControlsState();
        }
    );
    updateTimelineControlsState();

    const QStringList names = i18n::columnNames(m_uiLanguage);
    const QVector<int> columnOrder = watchlist_utils::normalizedColumnOrder(m_cfg.columnOrder);

    m_columnList = new QListWidget(w);
    m_columnList->setDragDropMode(QAbstractItemView::InternalMove);
    m_columnList->setDefaultDropAction(Qt::MoveAction);
    m_columnList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_columnList->setDragEnabled(true);
    m_columnList->setAcceptDrops(true);
    m_columnList->setDropIndicatorShown(true);
    m_columnList->setMinimumHeight(160);

    for (int logical : columnOrder) {
        QListWidgetItem* item = new QListWidgetItem(names.value(logical), m_columnList);
        item->setData(Qt::UserRole, logical);
        item->setFlags(
            item->flags()
            | Qt::ItemIsSelectable
            | Qt::ItemIsEnabled
            | Qt::ItemIsDragEnabled
            | Qt::ItemIsUserCheckable
        );
        item->setCheckState(m_cfg.visibleColumns.value(logical, true) ? Qt::Checked : Qt::Unchecked);
    }

    columnsForm->addRow(m_columnList);

    QLabel* columnsHint = new QLabel(trText("settings.display.columnsHint"), w);
    columnsHint->setWordWrap(true);
    addCompactFormRow(columnsForm, columnsHint);

    m_columnMaxWidthSpins.resize(ColCount);

    const QString maxSuffix = trText("settings.display.pxSuffix");
    const QString autoText = trText("settings.display.auto");

    for (int i = 0; i < ColCount; ++i) {
        m_columnMaxWidthSpins[i] = new QSpinBox(w);
        m_columnMaxWidthSpins[i]->setRange(0, 1200);
        m_columnMaxWidthSpins[i]->setSpecialValueText(autoText);
        m_columnMaxWidthSpins[i]->setSuffix(maxSuffix);
        m_columnMaxWidthSpins[i]->setValue(qMax(0, m_cfg.columnMaxWidths.value(i, 0)));
        applyNumericSpinBoxWidth(m_columnMaxWidthSpins[i]);

        columnsForm->addRow(
            trText("settings.display.columnMaxNameFmt").arg(names.value(i)),
            m_columnMaxWidthSpins[i]
        );
    }

    root->addWidget(windowGroup);
    root->addWidget(timelineGroup);
    root->addWidget(interactionGroup);
    root->addWidget(columnsGroup, 1);

    return w;
}

QWidget* SettingsDialog::buildOtherTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(w);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // --- Config sync group ---
    m_syncNam = new QNetworkAccessManager(this);
    {
        QGroupBox* syncGroup = new QGroupBox(trText("settings.sync.group"), w);
        QVBoxLayout* syncLayout = new QVBoxLayout(syncGroup);
        syncLayout->setContentsMargins(8, 10, 8, 8);
        syncLayout->setSpacing(6);

        QFormLayout* syncForm = new QFormLayout;
        syncForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        syncForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        syncForm->setHorizontalSpacing(8);
        syncForm->setVerticalSpacing(4);

        m_gistTokenEdit = new QLineEdit(syncGroup);
        m_gistTokenEdit->setEchoMode(QLineEdit::Password);
        m_gistTokenEdit->setPlaceholderText(QStringLiteral("ghp_..."));
        m_gistTokenEdit->setText(m_cfg.gistToken);

        m_gistIdEdit = new QLineEdit(syncGroup);
        m_gistIdEdit->setPlaceholderText(QStringLiteral("e.g. a1b2c3d4e5f6..."));
        m_gistIdEdit->setText(m_cfg.gistId);

        m_gistLastSyncLabel = new QLabel(syncGroup);
        {
            const QString ts = m_cfg.gistLastSyncTime.trimmed();
            if (ts.isEmpty()) {
                m_gistLastSyncLabel->setText(trText("settings.sync.never"));
            } else {
                m_gistLastSyncLabel->setText(ts);
            }
        }

        syncForm->addRow(trText("settings.sync.gistToken"), m_gistTokenEdit);
        syncForm->addRow(trText("settings.sync.gistId"), m_gistIdEdit);
        syncForm->addRow(trText("settings.sync.lastSync"), m_gistLastSyncLabel);
        syncLayout->addLayout(syncForm);

        QWidget* syncBtnRow = new QWidget(syncGroup);
        QHBoxLayout* syncBtnLayout = new QHBoxLayout(syncBtnRow);
        syncBtnLayout->setContentsMargins(0, 2, 0, 0);
        syncBtnLayout->setSpacing(8);

        QPushButton* downloadBtn = new QPushButton(trText("settings.sync.download"), syncBtnRow);
        QPushButton* uploadBtn   = new QPushButton(trText("settings.sync.upload"),   syncBtnRow);
        syncBtnLayout->addStretch();
        syncBtnLayout->addWidget(downloadBtn);
        syncBtnLayout->addWidget(uploadBtn);
        syncLayout->addWidget(syncBtnRow);

        root->addWidget(syncGroup);

        auto validateFields = [this]() -> bool {
            if (m_gistTokenEdit->text().trimmed().isEmpty()
                || m_gistIdEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(this,
                    trText("settings.sync.group"),
                    trText("settings.sync.missingFields"));
                return false;
            }
            return true;
        };

        // Unified sync handler: fetch remote info first, then present a choice dialog.
        auto doSync = [this, uploadBtn, downloadBtn, validateFields]() {
            if (!validateFields()) return;
            const QString token = m_gistTokenEdit->text().trimmed();
            const QString gistId = m_gistIdEdit->text().trimmed();

            // Persist token and gist ID immediately (before network request)
            m_cfg.gistToken = token;
            m_cfg.gistId = gistId;
            {
                auto s = ConfigManager::createAppSettings();
                s->setValue(QStringLiteral("sync/gistToken"), token);
                s->setValue(QStringLiteral("sync/gistId"), gistId);
            }

            uploadBtn->setEnabled(false);
            downloadBtn->setEnabled(false);

            // GET gist to obtain remote updated_at before showing the choice dialog.
            QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/gists/") + gistId));
            req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
            req.setRawHeader("Accept", "application/vnd.github.v3+json");
            req.setRawHeader("User-Agent", "myStocks-app");
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);

            QNetworkReply* fetchReply = m_syncNam->get(req);
            connect(fetchReply, &QNetworkReply::finished, this,
                [this, fetchReply, uploadBtn, downloadBtn, token, gistId]() {
                    fetchReply->deleteLater();
                    uploadBtn->setEnabled(true);
                    downloadBtn->setEnabled(true);

                    const int httpStatus = fetchReply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (fetchReply->error() != QNetworkReply::NoError
                        || httpStatus < 200 || httpStatus >= 300) {
                        const QString err = fetchReply->error() != QNetworkReply::NoError
                            ? fetchReply->errorString()
                            : QStringLiteral("HTTP %1").arg(httpStatus);
                        QMessageBox::critical(this,
                            trText("settings.sync.group"),
                            trText("settings.sync.fetchFail").arg(err));
                        return;
                    }

                    const QJsonDocument fetchDoc = QJsonDocument::fromJson(fetchReply->readAll());
                    const QJsonObject root = fetchDoc.object();
                    const QString remoteUpdatedAt = root[QStringLiteral("updated_at")].toString();

                    // Format remote timestamp (ISO 8601 → local time string)
                    QString remoteDisplay;
                    if (!remoteUpdatedAt.isEmpty()) {
                        const QDateTime dt =
                            QDateTime::fromString(remoteUpdatedAt, Qt::ISODate).toLocalTime();
                        remoteDisplay = dt.isValid()
                            ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                            : remoteUpdatedAt;
                    } else {
                        remoteDisplay = trText("settings.sync.never");
                    }

                    const QString localDisplay = m_cfg.gistLastSyncTime.trimmed().isEmpty()
                        ? trText("settings.sync.never")
                        : m_cfg.gistLastSyncTime.trimmed();

                    // Build the choice dialog.
                    QDialog dlg(this);
                    dlg.setWindowTitle(trText("settings.sync.group"));
                    dlg.setWindowModality(Qt::WindowModal);
                    QVBoxLayout* vlay = new QVBoxLayout(&dlg);
                    vlay->setSpacing(12);
                    vlay->setContentsMargins(20, 16, 20, 16);

                    QLabel* infoLabel = new QLabel(&dlg);
                    infoLabel->setText(trText("settings.sync.syncDialog.info")
                        .arg(remoteDisplay, localDisplay));
                    infoLabel->setWordWrap(true);
                    vlay->addWidget(infoLabel);

                    QHBoxLayout* btnLay = new QHBoxLayout;
                    btnLay->setSpacing(8);
                    QPushButton* useRemoteBtn =
                        new QPushButton(trText("settings.sync.syncDialog.useRemote"), &dlg);
                    QPushButton* useLocalBtn =
                        new QPushButton(trText("settings.sync.syncDialog.useLocal"), &dlg);
                    QPushButton* ignoreBtn =
                        new QPushButton(trText("settings.sync.syncDialog.ignore"), &dlg);
                    ignoreBtn->setDefault(true);
                    btnLay->addStretch();
                    btnLay->addWidget(useRemoteBtn);
                    btnLay->addWidget(useLocalBtn);
                    btnLay->addWidget(ignoreBtn);
                    vlay->addLayout(btnLay);

                    enum SyncChoice { ChooseRemote, ChooseLocal, ChooseIgnore };
                    SyncChoice syncChoice = ChooseIgnore;
                    connect(useRemoteBtn, &QPushButton::clicked, &dlg,
                        [&syncChoice, &dlg]() { syncChoice = ChooseRemote; dlg.accept(); });
                    connect(useLocalBtn, &QPushButton::clicked, &dlg,
                        [&syncChoice, &dlg]() { syncChoice = ChooseLocal; dlg.accept(); });
                    connect(ignoreBtn, &QPushButton::clicked, &dlg,
                        [&dlg]() { dlg.reject(); });

                    ignoreBtn->setFocus();
                    dlg.exec();

                    if (syncChoice == ChooseIgnore) return;

                    uploadBtn->setEnabled(false);
                    downloadBtn->setEnabled(false);

                    if (syncChoice == ChooseRemote) {
                        // Use remote: write already-fetched gist content to local data.yaml.
                        const QJsonObject files = root[QStringLiteral("files")].toObject();
                        QString yamlContent;
                        for (const QString& key : files.keys()) {
                            if (key.compare(QStringLiteral("data.yaml"),
                                            Qt::CaseInsensitive) == 0) {
                                yamlContent =
                                    files[key].toObject()[QStringLiteral("content")].toString();
                                break;
                            }
                        }
                        uploadBtn->setEnabled(true);
                        downloadBtn->setEnabled(true);
                        if (yamlContent.isEmpty()) {
                            QMessageBox::critical(this,
                                trText("settings.sync.group"),
                                trText("settings.sync.downloadFail").arg(
                                    QStringLiteral("data.yaml not found in gist")));
                            return;
                        }
                        QString yamlError;
                        if (!ConfigManager::saveDataYamlText(
                                m_dataYamlPath, yamlContent, &yamlError)) {
                            QMessageBox::critical(this,
                                trText("settings.sync.group"),
                                trText("settings.sync.downloadFail").arg(yamlError));
                            return;
                        }
                        const QString now = QDateTime::currentDateTime()
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                        m_cfg.gistLastSyncTime = now;
                        if (!remoteUpdatedAt.isEmpty()) {
                            m_cfg.gistRemoteUpdatedAt = remoteUpdatedAt;
                        }
                        m_gistLastSyncLabel->setText(now);
                        {
                            auto s = ConfigManager::createAppSettings();
                            s->setValue(QStringLiteral("sync/gistLastSyncTime"), now);
                            s->setValue(QStringLiteral("sync/gistRemoteUpdatedAt"),
                                        m_cfg.gistRemoteUpdatedAt);
                        }
                        reloadDialogDataFromYaml();
                        QMessageBox::information(this,
                            trText("settings.sync.group"),
                            trText("settings.sync.downloadOk"));
                    } else {
                        // Use local: PATCH upload local data.yaml to remote.
                        QString yamlError;
                        const QString yamlContent = ConfigManager::loadDataYamlText(
                            m_dataYamlPath, &yamlError);
                        if (!yamlError.isEmpty()) {
                            uploadBtn->setEnabled(true);
                            downloadBtn->setEnabled(true);
                            QMessageBox::critical(this,
                                trText("settings.sync.group"),
                                trText("settings.sync.uploadFail").arg(yamlError));
                            return;
                        }

                        QJsonObject filesObj;
                        QJsonObject fileEntry;
                        fileEntry[QStringLiteral("content")] = yamlContent;
                        filesObj[QStringLiteral("data.yaml")] = fileEntry;
                        QJsonObject body;
                        body[QStringLiteral("files")] = filesObj;

                        QNetworkRequest patchReq(
                            QUrl(QStringLiteral("https://api.github.com/gists/") + gistId));
                        patchReq.setHeader(QNetworkRequest::ContentTypeHeader,
                                           QStringLiteral("application/json"));
                        patchReq.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
                        patchReq.setRawHeader("Accept", "application/vnd.github.v3+json");
                        patchReq.setRawHeader("User-Agent", "myStocks-app");
                        patchReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                              QNetworkRequest::NoLessSafeRedirectPolicy);

                        QNetworkReply* patchReply = m_syncNam->sendCustomRequest(
                            patchReq, "PATCH",
                            QJsonDocument(body).toJson(QJsonDocument::Compact));
                        connect(patchReply, &QNetworkReply::finished, this,
                            [this, patchReply, uploadBtn, downloadBtn]() {
                                patchReply->deleteLater();
                                uploadBtn->setEnabled(true);
                                downloadBtn->setEnabled(true);
                                const int status = patchReply->attribute(
                                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                                if (patchReply->error() != QNetworkReply::NoError
                                    || status < 200 || status >= 300) {
                                    const QString err =
                                        patchReply->error() != QNetworkReply::NoError
                                        ? patchReply->errorString()
                                        : QStringLiteral("HTTP %1").arg(status);
                                    QMessageBox::critical(this,
                                        trText("settings.sync.group"),
                                        trText("settings.sync.uploadFail").arg(err));
                                    return;
                                }
                                const QJsonDocument doc2 =
                                    QJsonDocument::fromJson(patchReply->readAll());
                                const QString newRemoteUpdatedAt =
                                    doc2.object()[QStringLiteral("updated_at")].toString();
                                const QString now = QDateTime::currentDateTime()
                                    .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                                m_cfg.gistLastSyncTime = now;
                                if (!newRemoteUpdatedAt.isEmpty()) {
                                    m_cfg.gistRemoteUpdatedAt = newRemoteUpdatedAt;
                                }
                                m_gistLastSyncLabel->setText(now);
                                {
                                    auto s = ConfigManager::createAppSettings();
                                    s->setValue(QStringLiteral("sync/gistLastSyncTime"), now);
                                    s->setValue(QStringLiteral("sync/gistRemoteUpdatedAt"),
                                                m_cfg.gistRemoteUpdatedAt);
                                }
                                QMessageBox::information(this,
                                    trText("settings.sync.group"),
                                    trText("settings.sync.uploadOk"));
                            }
                        );
                    }
                }
            );
        };
        connect(uploadBtn,   &QPushButton::clicked, this, doSync);
        connect(downloadBtn, &QPushButton::clicked, this, doSync);
    }

    // --- Debug/Log group ---
    QWidget* debugWidget = new QWidget(w);
    QFormLayout* form = new QFormLayout(debugWidget);
    applyCompactFormLayout(form);

    m_acceptBetaUpdatesCheck = new QCheckBox(debugWidget);
    m_acceptBetaUpdatesCheck->setChecked(m_cfg.acceptBetaUpdates);

    m_autoCheckUpdatesCheck = new QCheckBox(debugWidget);
    m_autoCheckUpdatesCheck->setChecked(m_cfg.autoCheckUpdates);

    m_debugIgnoreTradingTimeCheck = new QCheckBox(debugWidget);
    m_debugIgnoreTradingTimeCheck->setChecked(m_cfg.debugIgnoreTradingTime);

    m_logLevelCombo = new QComboBox(debugWidget);
    m_logLevelCombo->addItem(trText("settings.other.logLevel.debug"), "debug");
    m_logLevelCombo->addItem(trText("settings.other.logLevel.info"), "info");
    m_logLevelCombo->addItem(trText("settings.other.logLevel.warn"), "warn");
    m_logLevelCombo->addItem(trText("settings.other.logLevel.error"), "error");

    int logLevelIndex = m_logLevelCombo->findData(app_logging::normalizeLogLevel(m_cfg.logLevel));
    if (logLevelIndex < 0) {
        logLevelIndex = m_logLevelCombo->findData("info");
    }
    if (logLevelIndex >= 0) {
        m_logLevelCombo->setCurrentIndex(logLevelIndex);
    }

    form->addRow(trText("settings.other.acceptBetaUpdates"), m_acceptBetaUpdatesCheck);
    form->addRow(trText("settings.other.autoCheckUpdates"), m_autoCheckUpdatesCheck);
    form->addRow(trText("settings.other.debugMode"), m_debugIgnoreTradingTimeCheck);
    form->addRow(trText("settings.other.logLevel"), m_logLevelCombo);
    root->addWidget(debugWidget);

    // --- Tray icon group ---
    QGroupBox* trayIconGroup = new QGroupBox(trText("settings.other.trayIconGroup"), w);
    QVBoxLayout* trayLayout = new QVBoxLayout(trayIconGroup);
    trayLayout->setContentsMargins(6, 8, 6, 6);
    trayLayout->setSpacing(4);

    QScrollArea* trayScroll = new QScrollArea(trayIconGroup);
    trayScroll->setWidgetResizable(true);
    trayScroll->setFrameShape(QFrame::NoFrame);
    trayScroll->setFixedHeight(230);
    trayScroll->viewport()->installEventFilter(new IgnoreWheelFilter(trayScroll));

    QWidget* trayGrid = new QWidget;
    const int kCols = 7;
    QGridLayout* trayGridLayout = new QGridLayout(trayGrid);
    trayGridLayout->setSpacing(6);
    trayGridLayout->setContentsMargins(4, 4, 4, 4);

    m_trayIconBtnGroup = new QButtonGroup(this);
    m_trayIconBtnGroup->setExclusive(true);
    m_trayIconPaths.clear();

    const QStringList trayIconNames = {
        QStringLiteral("apple-002-app2.png"),
        QStringLiteral("apple-003-apple-1-copy.png"),
        QStringLiteral("apple-004-apple-2.png"),
        QStringLiteral("apple-005-game.png"),
        QStringLiteral("apple-006-apple-3.png"),
        QStringLiteral("apple-007-player.png"),
        QStringLiteral("apple-008-apple-4.png"),
        QStringLiteral("apple-009-apple-5.png"),
        QStringLiteral("buffalo.png"),
        QStringLiteral("chicken.png"),
        QStringLiteral("crab.png"),
        QStringLiteral("delicious.png"),
        QStringLiteral("dog.png"),
        QStringLiteral("dribbble.png"),
        QStringLiteral("github2.png"),
        QStringLiteral("google-drive.png"),
        QStringLiteral("happy.png"),
        QStringLiteral("instagram.png"),
        QStringLiteral("lion.png"),
        QStringLiteral("logo.png"),
        QStringLiteral("telegram.png"),
        QStringLiteral("wechat1.png"),
        QStringLiteral("wps1.png"),
        QStringLiteral("xiaomi1.png"),
        QStringLiteral("yoga-ball.png"),
        QStringLiteral("youtube.png")
    };

    auto addTrayIconCell = [&](const QString& resourcePath, const QString& tooltip) {
        const int id = m_trayIconPaths.size();
        m_trayIconPaths.append(resourcePath);

        QWidget* cell = new QWidget(trayGrid);
#ifdef WIN32
        cell->setObjectName(QStringLiteral("trayIconCell"));
        cell->setAttribute(Qt::WA_StyledBackground, true);
#endif
        QVBoxLayout* cl = new QVBoxLayout(cell);
        cl->setContentsMargins(2, 2, 2, 2);
        cl->setSpacing(2);
        cl->setAlignment(Qt::AlignCenter);

        QLabel* iconLabel = new QLabel(cell);
        const QString imgPath = resourcePath.isEmpty()
            ? QStringLiteral(":/icon.png")
            : resourcePath;
        QPixmap pm(imgPath);
        iconLabel->setPixmap(pm.scaled(36, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setToolTip(tooltip);

        QRadioButton* rb = new QRadioButton(cell); // no text
        rb->setToolTip(tooltip);
#ifdef WIN32
        rb->setObjectName(QStringLiteral("trayIconRadio"));

        const auto updateTrayIconSelectionStyle = [this, cell, rb]() {
            const QPalette pal = this->palette();
            QColor cellBorder = pal.color(QPalette::Mid);
            QColor cellBackground(0, 0, 0, 0);
            if (rb->isChecked()) {
                cellBorder = pal.color(QPalette::Highlight);
                cellBackground = pal.color(QPalette::Highlight);
                cellBackground.setAlpha(56);
            }

            cell->setStyleSheet(
                QStringLiteral(
                    "#trayIconCell {"
                    "border: %1px solid %2;"
                    "border-radius: 8px;"
                    "background-color: %3;"
                    "}"
                )
                    .arg(rb->isChecked() ? 2 : 1)
                    .arg(cellBorder.name(QColor::HexArgb))
                    .arg(cellBackground.name(QColor::HexArgb))
            );

            rb->setStyleSheet(
                QStringLiteral(
                    "QRadioButton::indicator {"
                    "width: 15px;"
                    "height: 15px;"
                    "border-radius: 7px;"
                    "border: 2px solid %1;"
                    "background-color: %2;"
                    "}"
                    "QRadioButton::indicator:checked {"
                    "border: 2px solid %3;"
                    "background-color: %3;"
                    "}"
                )
                    .arg(pal.color(QPalette::Mid).name(QColor::HexArgb))
                    .arg(pal.color(QPalette::Base).name(QColor::HexArgb))
                    .arg(pal.color(QPalette::Highlight).name(QColor::HexArgb))
            );
        };

        connect(rb, &QRadioButton::toggled, cell, [updateTrayIconSelectionStyle](bool) {
            updateTrayIconSelectionStyle();
        });
        updateTrayIconSelectionStyle();
#endif

        cl->addWidget(iconLabel, 0, Qt::AlignCenter);
        cl->addWidget(rb, 0, Qt::AlignCenter);

        trayGridLayout->addWidget(cell, id / kCols, id % kCols);
        m_trayIconBtnGroup->addButton(rb, id);
    };

    // First: default icon
    addTrayIconCell(QString(), trText("settings.other.trayIconDefault"));
    // Then all tray icons
    for (const QString& name : trayIconNames) {
        const QString resourcePath = QStringLiteral(":/tray_icons/") + name;
        QString tip = name;
        const int dot = tip.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) tip.truncate(dot);
        addTrayIconCell(resourcePath, tip);
    }

    // Select the currently configured icon
    {
        const QString currentPath = m_cfg.trayIconPath;
        int selectedId = 0;
        for (int i = 0; i < m_trayIconPaths.size(); ++i) {
            if (m_trayIconPaths[i] == currentPath) {
                selectedId = i;
                break;
            }
        }
        QAbstractButton* btn = m_trayIconBtnGroup->button(selectedId);
        if (btn) btn->setChecked(true);
    }

    trayScroll->setWidget(trayGrid);
    trayLayout->addWidget(trayScroll);
    root->addWidget(trayIconGroup);

    root->addStretch();
    return w;
}

QWidget* SettingsDialog::buildStocksTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setSpacing(5);
    vbox->setContentsMargins(6, 6, 6, 6);

    // --- Search row ---
    QWidget* searchRow = new QWidget(w);
    QHBoxLayout* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(5);

    m_stockSearchEdit = new QLineEdit(searchRow);
    m_stockSearchEdit->setPlaceholderText(trText("settings.stocks.searchPlaceholder"));

    m_stockSearchBtn = new QPushButton(trText("settings.stocks.search"), searchRow);
    m_stockSearchBtn->setMinimumWidth(76);

    searchLayout->addWidget(m_stockSearchEdit, 1);
    searchLayout->addWidget(m_stockSearchBtn);
    vbox->addWidget(searchRow);

    // --- Suggestion list (floating overlay, not in layout) ---
    m_stockSuggestList = new QListWidget(this);
    m_stockSuggestList->setMaximumHeight(160);
    m_stockSuggestList->setFrameShape(QFrame::StyledPanel);
    m_stockSuggestList->hide();
    // Not added to vbox — positioned absolutely when shown

    // --- Stock table + order buttons ---
    StockTableWidget* table = new StockTableWidget(w);
    table->setHorizontalHeaderLabels({
        trText("settings.stocks.colSeq"),
        trText("settings.stocks.colCode"),
        trText("settings.stocks.colName"),
        trText("settings.stocks.colCost"),
        trText("settings.stocks.colDel")
    });
    table->setMinimumHeight(220);

    for (const StockItem& s : m_stocks) {
        // Use API name if available, fall back to stored name
        const QString displayName = m_apiNamesByCode.value(s.code, s.name);
        table->addStockRow(s.code, displayName, s.cost);
    }

    m_stockTable = table;
    table->setConfirmDelete([this](const QString& display) -> bool {
        const QMessageBox::StandardButton ret = showIconMessageBox(
            this,
            QMessageBox::Question,
            trText("app.name"),
            trText("settings.stocks.delConfirm").arg(display),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        return ret == QMessageBox::Yes;
    });

    // Order buttons column on the right
    QWidget* tableArea = new QWidget(w);
    QHBoxLayout* tableAreaLayout = new QHBoxLayout(tableArea);
    tableAreaLayout->setContentsMargins(0, 0, 0, 0);
    tableAreaLayout->setSpacing(6);
    tableAreaLayout->addWidget(m_stockTable, 1);

    QWidget* orderBtnCol = new QWidget(tableArea);
    QVBoxLayout* orderBtnLayout = new QVBoxLayout(orderBtnCol);
    orderBtnLayout->setContentsMargins(0, 0, 0, 0);
    orderBtnLayout->setSpacing(5);

    auto makeOrderBtn = [&](const QString& label) -> QPushButton* {
        QPushButton* btn = new QPushButton(label, orderBtnCol);
        btn->setFixedWidth(56);
        return btn;
    };

    QPushButton* btnTop    = makeOrderBtn(trText("settings.stocks.moveTop"));
    QPushButton* btnUp     = makeOrderBtn(trText("settings.stocks.moveUp"));
    QPushButton* btnDown   = makeOrderBtn(trText("settings.stocks.moveDown"));
    QPushButton* btnBottom = makeOrderBtn(trText("settings.stocks.moveBottom"));

    orderBtnLayout->addStretch();
    orderBtnLayout->addWidget(btnTop);
    orderBtnLayout->addWidget(btnUp);
    orderBtnLayout->addWidget(btnDown);
    orderBtnLayout->addWidget(btnBottom);
    orderBtnLayout->addStretch();

    tableAreaLayout->addWidget(orderBtnCol);
    vbox->addWidget(tableArea, 1);

    // --- Buttons row ---
    QWidget* btnRow = new QWidget(w);
    QHBoxLayout* btnLayout = new QHBoxLayout(btnRow);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(6);
    QPushButton* saveBtn = new QPushButton(trText("settings.stocks.save"), btnRow);
    saveBtn->setMinimumWidth(82);
    btnLayout->addStretch();
    btnLayout->addWidget(saveBtn);
    vbox->addWidget(btnRow);

    // --- Network setup ---
    m_stockSearchNam = new QNetworkAccessManager(this);
    m_stockSearchNam->setProxy(network_utils::proxyFromConfig(m_cfg));

    m_stockSearchDebounce = new QTimer(this);
    m_stockSearchDebounce->setSingleShot(true);
    m_stockSearchDebounce->setInterval(kStockSearchDebounceMs);

    // --- Connections ---

    // Order buttons
    auto currentTableRow = [this]() -> int {
        return m_stockTable->currentRow();
    };
    connect(btnTop, &QPushButton::clicked, this, [this, currentTableRow]() {
        const int newRow = static_cast<StockTableWidget*>(m_stockTable)->moveRowTop(currentTableRow());
        m_stockTable->setCurrentCell(newRow, 0);
    });
    connect(btnUp, &QPushButton::clicked, this, [this, currentTableRow]() {
        const int newRow = static_cast<StockTableWidget*>(m_stockTable)->moveRowUp(currentTableRow());
        m_stockTable->setCurrentCell(newRow, 0);
    });
    connect(btnDown, &QPushButton::clicked, this, [this, currentTableRow]() {
        const int newRow = static_cast<StockTableWidget*>(m_stockTable)->moveRowDown(currentTableRow());
        m_stockTable->setCurrentCell(newRow, 0);
    });
    connect(btnBottom, &QPushButton::clicked, this, [this, currentTableRow]() {
        const int newRow = static_cast<StockTableWidget*>(m_stockTable)->moveRowBottom(currentTableRow());
        m_stockTable->setCurrentCell(newRow, 0);
    });

    // Filter spaces; trigger debounce on 3+ chars
    connect(m_stockSearchEdit, &QLineEdit::textEdited, this, [this](const QString& rawText) {
        QString text = rawText;
        text.remove(QLatin1Char(' '));
        if (text != rawText) {
            m_stockSearchEdit->setText(text);
        }
        if (text.length() < kStockSearchMinLength) {
            m_stockSearchDebounce->stop();
            m_stockSuggestList->clear();
            m_stockSuggestList->hide();
            return;
        }
        m_stockSearchDebounce->start();
    });

    // Manual search button: fire immediately (bypass debounce and min-length)
    connect(m_stockSearchBtn, &QPushButton::clicked, this, [this]() {
        m_stockSearchDebounce->stop();
        doStockSearch(true);
    });

    // Debounce fires: make network request
    connect(m_stockSearchDebounce, &QTimer::timeout, this, [this]() {
        doStockSearch();
    });

    // Click on suggestion: add to table
    connect(m_stockSuggestList, &QListWidget::itemClicked, this, [this](QListWidgetItem* listItem) {
        if (!listItem) {
            return;
        }
        const QString code = listItem->data(Qt::UserRole).toString();
        const QString name = listItem->data(Qt::UserRole + 1).toString();
        if (code.isEmpty()) {
            return;
        }

        StockTableWidget* tbl = static_cast<StockTableWidget*>(m_stockTable);
        if (tbl->containsCode(code)) {
            showIconMessageBox(
                this,
                QMessageBox::Information,
                trText("app.name"),
                trText("settings.stocks.duplicate")
            );
        } else {
            tbl->addStockRow(code, name);
        }

        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
        m_stockSearchEdit->clear();
    });

    // Save button: write current table to data.yaml
    connect(saveBtn, &QPushButton::clicked, this, [this]() {
        if (m_dataYamlPath.isEmpty()) {
            qWarning() << "[StocksTab] save failed: empty yaml path.";
            showIconMessageBox(
                this,
                QMessageBox::Warning,
                trText("app.name"),
                trText("settings.stocks.saveFail")
            );
            return;
        }
        const QVector<StockItem> stocks =
            static_cast<StockTableWidget*>(m_stockTable)->stocks();
        qInfo() << "[StocksTab] save requested path=" << m_dataYamlPath << "count=" << stocks.size();
        if (ConfigManager::saveStocksToYaml(m_dataYamlPath, stocks)) {
            const QVector<StockItem> reloadedStocks = ConfigManager::loadStocksFromYaml(m_dataYamlPath);
            QVector<StockItem> filteredStocks;
            filteredStocks.reserve(reloadedStocks.size());
            for (const StockItem& stock : reloadedStocks) {
                if (!isPredefinedIndexCode(stock.code)) {
                    filteredStocks.push_back(stock);
                }
            }
            m_stocks = std::move(filteredStocks);

            QSet<QString> stockKeys;
            for (const StockItem& stock : m_stocks) {
                stockKeys.insert(watchCodeKey(stock.code));
            }
            for (StockGroup& group : m_groups) {
                QStringList prunedCodes;
                for (const QString& code : group.stockCodes) {
                    if (stockKeys.contains(watchCodeKey(code))) {
                        prunedCodes.append(code);
                    }
                }
                group.stockCodes = std::move(prunedCodes);
            }
            refreshGroupStockChoices();
            refreshCurrentGroupSelection();

            qInfo() << "[StocksTab] save success path=" << m_dataYamlPath;
            showIconMessageBox(
                this,
                QMessageBox::Information,
                trText("app.name"),
                trText("settings.stocks.saveOk")
            );
        } else {
            qWarning() << "[StocksTab] save failed path=" << m_dataYamlPath;
            showIconMessageBox(
                this,
                QMessageBox::Warning,
                trText("app.name"),
                trText("settings.stocks.saveFail")
            );
        }
    });

    return w;
}

QWidget* SettingsDialog::buildIndexSectorTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(w);
    root->setSpacing(8);
    root->setContentsMargins(6, 6, 6, 6);

    // --- Index section ---
    QGroupBox* indexGroup = new QGroupBox(trText("settings.indexSector.indexGroup"), w);
    QVBoxLayout* indexLayout = new QVBoxLayout(indexGroup);
    indexLayout->setContentsMargins(6, 6, 6, 6);
    indexLayout->setSpacing(5);

    QLabel* indexHint = new QLabel(trText("settings.indexSector.indexHint"), indexGroup);
    indexHint->setWordWrap(true);

    m_indexList = new QListWidget(indexGroup);
    m_indexList->setDragDropMode(QAbstractItemView::InternalMove);
    m_indexList->setDefaultDropAction(Qt::MoveAction);
    m_indexList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_indexList->setDragEnabled(true);
    m_indexList->setAcceptDrops(true);
    m_indexList->setDropIndicatorShown(true);
    m_indexList->setMinimumHeight(120);

    QSet<QString> checkedKeys;
    QStringList orderedKeys;
    for (const StockItem& selected : m_indexes) {
        const QString key = watchCodeKey(selected.code);
        if (key.isEmpty()) {
            continue;
        }
        checkedKeys.insert(key);
        orderedKeys.push_back(key);
    }

    QHash<QString, IndexPreset> presetByKey;
    for (const IndexPreset& preset : indexPresets()) {
        const QString key = watchCodeKey(preset.code);
        presetByKey.insert(key, preset);
        if (!orderedKeys.contains(key)) {
            orderedKeys.push_back(key);
        }
    }

    for (const QString& key : orderedKeys) {
        if (!presetByKey.contains(key)) {
            continue;
        }

        const IndexPreset preset = presetByKey.value(key);
        QListWidgetItem* item = new QListWidgetItem(
            trText("settings.indexSector.indexDisplayFmt").arg(preset.name, preset.code),
            m_indexList
        );
        item->setData(Qt::UserRole, preset.code);
        item->setData(Qt::UserRole + 1, preset.name);
        item->setToolTip(preset.note);
        item->setFlags(
            item->flags()
            | Qt::ItemIsSelectable
            | Qt::ItemIsEnabled
            | Qt::ItemIsDragEnabled
            | Qt::ItemIsUserCheckable
        );
        item->setCheckState(checkedKeys.contains(key) ? Qt::Checked : Qt::Unchecked);
    }

    indexLayout->addWidget(indexHint);
    indexLayout->addWidget(m_indexList, 1);
    root->addWidget(indexGroup, 1);

    return w;
}

void SettingsDialog::parseSearchResult(const QByteArray& data) {
    if (!m_stockSuggestList || !m_stockSearchEdit || !m_stockSearchBtn) {
        return;
    }

    if (data.isEmpty()) {
        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[StockSearch] parse failed:" << parseError.errorString();
        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray items = root.value(QStringLiteral("Data")).toArray();

    m_stockSuggestList->clear();
    QSet<QString> addedCodes;

    for (const QJsonValue& value : items) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject obj = value.toObject();
        QString quoteId = obj.value(QStringLiteral("QuoteID")).toString().trimmed();

        if (quoteId.isEmpty()) {
            QString market;
            const QJsonValue marketValue = obj.value(QStringLiteral("MktNum"));
            if (marketValue.isString()) {
                market = marketValue.toString().trimmed();
            } else if (marketValue.isDouble()) {
                market = QString::number(static_cast<int>(marketValue.toDouble()));
            }

            const QString symbol = obj.value(QStringLiteral("Code")).toString().trimmed();
            if (!market.isEmpty() && !symbol.isEmpty()) {
                quoteId = market + QStringLiteral(".") + symbol;
            } else {
                quoteId = symbol;
            }
        }

        QString normalizedCode = normalizeApiWatchCode(quoteId);
        if (normalizedCode.isEmpty()) {
            normalizedCode = normalizeApiWatchCode(obj.value(QStringLiteral("Code")).toString());
        }
        if (normalizedCode.isEmpty()) {
            continue;
        }

        const QString key = watchCodeKey(normalizedCode);
        if (addedCodes.contains(key)) {
            continue;
        }
        addedCodes.insert(key);

        const QString name = obj.value(QStringLiteral("Name")).toString().trimmed();
        const QString securityType = obj.value(QStringLiteral("SecurityTypeName")).toString().trimmed();
        QString displayText = name.isEmpty()
            ? normalizedCode
            : (normalizedCode + QStringLiteral(" - ") + name);
        if (!securityType.isEmpty()) {
            displayText += QStringLiteral(" [") + securityType + QStringLiteral("]");
        }

        QListWidgetItem* item = new QListWidgetItem(displayText, m_stockSuggestList);
        item->setData(Qt::UserRole, normalizedCode);
        item->setData(Qt::UserRole + 1, name);
    }

    if (m_stockSuggestList->count() > 0) {
        const QPoint topLeft = m_stockSearchEdit->mapTo(this,
            QPoint(0, m_stockSearchEdit->height() + 2));
        const int w = m_stockSearchEdit->width() + 6 + m_stockSearchBtn->width();
        const int itemH = m_stockSuggestList->sizeHintForRow(0);
        const int h = qMin(m_stockSuggestList->count() * (itemH > 0 ? itemH : 22) + 6, 160);
        m_stockSuggestList->setGeometry(topLeft.x(), topLeft.y(), w, h);
        m_stockSuggestList->raise();
        m_stockSuggestList->setVisible(true);
    } else {
        m_stockSuggestList->setVisible(false);
    }
}

void SettingsDialog::doStockSearch(bool forceSearch) {
    if (!m_stockSearchEdit || !m_stockSearchNam) {
        return;
    }

    const QString keyword = m_stockSearchEdit->text().trimmed();
    if (!forceSearch && keyword.length() < kStockSearchMinLength) {
        return;
    }
    if (keyword.isEmpty()) {
        return;
    }

    if (m_stockSearchReply) {
        m_stockSearchReply->abort();
        m_stockSearchReply->deleteLater();
        m_stockSearchReply = nullptr;
    }

    QUrl url(QStringLiteral("https://searchapi.eastmoney.com/api/Info/Search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("14"));
    query.addQueryItem(
        QStringLiteral("and14"),
        QStringLiteral("MultiMatch/Name,Code,PinYin/%1/true").arg(keyword)
    );
    query.addQueryItem(QStringLiteral("pageIndex14"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pageSize14"), QString::number(kStockSearchResultLimit));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader(headers::kUserAgent, network_utils::effectiveUserAgent(m_cfg).toUtf8());
    req.setRawHeader(headers::kReferer, headers::kEastMoneyReferer);
    req.setRawHeader(headers::kAccept, "*/*");
    req.setRawHeader(headers::kConnection, "keep-alive");
    req.setRawHeader(headers::kContentType, headers::kEastMoneyContentType);
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        QStringLiteral("eastmoney-search"),
        QStringLiteral("GET"),
        req,
        m_stockSearchNam->proxy()
    );

    m_stockSearchReply = m_stockSearchNam->get(req);
    connect(m_stockSearchReply, &QNetworkReply::finished, this, [this, trace]() {
        if (!m_stockSearchReply) {
            return;
        }

        const QNetworkReply::NetworkError netErr = m_stockSearchReply->error();
        const QByteArray data = m_stockSearchReply->readAll();

        network_logger::logRequestFinish(trace, m_stockSearchReply, data.size(), data);

        m_stockSearchReply->deleteLater();
        m_stockSearchReply = nullptr;

        if (netErr != QNetworkReply::NoError) {
            qWarning() << "[StockSearch] network error" << netErr;
            m_stockSuggestList->clear();
            m_stockSuggestList->hide();
            return;
        }

        parseSearchResult(data);
    });
}

QString SettingsDialog::groupMemberDisplayName(const QString& code) const {
    const QString key = watchCodeKey(code);
    for (const StockItem& stock : m_stocks) {
        if (watchCodeKey(stock.code) != key) {
            continue;
        }
        const QString name = stock.name.trimmed();
        return name.isEmpty()
            ? stock.code
            : QStringLiteral("%1 %2").arg(stock.code, name);
    }
    return code;
}

void SettingsDialog::refreshGroupStockChoices() {
    if (!m_groupStockList) {
        return;
    }

    QSignalBlocker blocker(m_groupStockList);
    m_groupStockList->clear();
    for (const StockItem& stock : m_stocks) {
        if (stock.code.isEmpty()) {
            continue;
        }
        const QString name = stock.name.trimmed();
        QListWidgetItem* item = new QListWidgetItem(
            name.isEmpty()
                ? stock.code
                : QStringLiteral("%1 %2").arg(stock.code, name),
            m_groupStockList
        );
        item->setData(Qt::UserRole, stock.code);
        item->setCheckState(Qt::Unchecked);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    }
}

void SettingsDialog::refreshCurrentGroupSelection() {
    if (!m_groupList || !m_groupStockList || !m_groupMemberList) {
        return;
    }

    const QListWidgetItem* current = m_groupList->currentItem();
    if (!current) {
        m_groupMemberList->setEnabled(false);
        m_groupMemberList->clear();
        m_groupStockList->setEnabled(false);
        QSignalBlocker blocker(m_groupStockList);
        for (int index = 0; index < m_groupStockList->count(); ++index) {
            m_groupStockList->item(index)->setCheckState(Qt::Unchecked);
        }
        return;
    }

    const int groupIndex = current->data(Qt::UserRole).toInt();
    if (groupIndex < 0) {
        m_groupMemberList->setEnabled(false);
        m_groupMemberList->clear();
        m_groupStockList->setEnabled(false);
        QSignalBlocker blocker(m_groupStockList);
        for (int index = 0; index < m_groupStockList->count(); ++index) {
            m_groupStockList->item(index)->setCheckState(Qt::Unchecked);
        }
        return;
    }
    if (groupIndex >= m_groups.size()) {
        return;
    }

    const StockGroup& group = m_groups.at(groupIndex);
    {
        QSignalBlocker blocker(m_groupMemberList);
        m_groupMemberList->clear();
        m_groupMemberList->setEnabled(true);
        for (const QString& code : group.stockCodes) {
            QListWidgetItem* item = new QListWidgetItem(
                groupMemberDisplayName(code),
                m_groupMemberList
            );
            item->setData(Qt::UserRole, code);
        }
    }

    {
        QSet<QString> keys;
        for (const QString& code : group.stockCodes) {
            keys.insert(watchCodeKey(code));
        }
        QSignalBlocker blocker(m_groupStockList);
        m_groupStockList->setEnabled(true);
        for (int index = 0; index < m_groupStockList->count(); ++index) {
            QListWidgetItem* item = m_groupStockList->item(index);
            item->setCheckState(
                keys.contains(watchCodeKey(item->data(Qt::UserRole).toString()))
                    ? Qt::Checked
                    : Qt::Unchecked
            );
        }
    }
}

QWidget* SettingsDialog::buildGroupsTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(10, 10, 10, 10);
    vbox->setSpacing(8);

    // ── Main body ─────────────────────────────────────────────────────────
    QHBoxLayout* mainRow = new QHBoxLayout;
    vbox->addLayout(mainRow, 1);

    // Helpers
    const auto refreshListUserRoles = [this]() {
        int ci = 0;
        for (int i = 0; i < m_groupList->count(); ++i) {
            QListWidgetItem* it = m_groupList->item(i);
            if (it->data(Qt::UserRole).toInt() != -1)
                it->setData(Qt::UserRole, ci++);
        }
    };
    const auto findAllGroupRow = [this]() -> int {
        for (int i = 0; i < m_groupList->count(); ++i)
            if (m_groupList->item(i)->data(Qt::UserRole).toInt() == -1) return i;
        return 0;
    };

    // ── Left: group list ──────────────────────────────────────────────────
    {
        QVBoxLayout* leftVbox = new QVBoxLayout;
        leftVbox->setSpacing(4);
        mainRow->addLayout(leftVbox, 0);
        leftVbox->addWidget(new QLabel(trText("settings.group.list"), w));

        m_groupList = new QListWidget(w);
        m_groupList->setMaximumWidth(180);
        m_groupList->setMinimumHeight(240);
        leftVbox->addWidget(m_groupList, 1);

        // Populate list: "所有" at groupAllPosition, custom groups elsewhere
        {
            const int allPos = qBound(0, m_cfg.groupAllPosition, m_groups.size());
            m_cfg.groupAllPosition = allPos;
            for (int vi = 0; vi <= m_groups.size(); ++vi) {
                if (vi == allPos) {
                    QListWidgetItem* it = new QListWidgetItem(
                        trText("settings.group.allGroup"), m_groupList);
                    it->setData(Qt::UserRole, -1);
                    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
                    QFont f = it->font(); f.setItalic(true); it->setFont(f);
                } else {
                    const int gi = vi < allPos ? vi : vi - 1;
                    QListWidgetItem* it = new QListWidgetItem(m_groups.at(gi).name, m_groupList);
                    it->setData(Qt::UserRole, gi);
                    it->setFlags(it->flags() | Qt::ItemIsEditable);
                }
            }
        }

        QHBoxLayout* btnRow = new QHBoxLayout;
        btnRow->setSpacing(4);
        leftVbox->addLayout(btnRow);
        QPushButton* addBtn  = new QPushButton(QStringLiteral("+"), w); addBtn->setMaximumWidth(32);
        QPushButton* delBtn  = new QPushButton(QStringLiteral("\u2212"), w); delBtn->setMaximumWidth(32);
        QPushButton* upBtn   = new QPushButton(QStringLiteral("\u2191"), w); upBtn->setMaximumWidth(32);
        QPushButton* downBtn = new QPushButton(QStringLiteral("\u2193"), w); downBtn->setMaximumWidth(32);
        btnRow->addWidget(addBtn); btnRow->addWidget(delBtn);
        btnRow->addWidget(upBtn);  btnRow->addWidget(downBtn);
        btnRow->addStretch();

        connect(addBtn, &QPushButton::clicked, this, [this, refreshListUserRoles]() {
            const QString name = QStringLiteral("\u65b0\u5206\u7ec4");
            StockGroup g; g.name = name;
            m_groups.append(g);
            QListWidgetItem* it = new QListWidgetItem(name, m_groupList);
            it->setData(Qt::UserRole, m_groups.size() - 1);
            it->setFlags(it->flags() | Qt::ItemIsEditable);
            refreshListUserRoles();
            m_groupList->setCurrentItem(it);
            m_groupList->editItem(it);
        });

        connect(delBtn, &QPushButton::clicked, this,
            [this, refreshListUserRoles, findAllGroupRow]() {
                const int row = m_groupList->currentRow();
                if (row < 0) return;
                const int ur = m_groupList->item(row)->data(Qt::UserRole).toInt();
                if (ur == -1) return; // protect "所有"
                m_groups.removeAt(ur);
                if (row < findAllGroupRow())
                    m_cfg.groupAllPosition = qMax(0, m_cfg.groupAllPosition - 1);
                delete m_groupList->takeItem(row);
                refreshListUserRoles();
                if (m_groupMemberList) { m_groupMemberList->clear(); m_groupMemberList->setEnabled(false); }
                if (m_groupStockList) {
                    m_groupStockList->setEnabled(false);
                    QSignalBlocker b(m_groupStockList);
                    for (int i = 0; i < m_groupStockList->count(); ++i)
                        m_groupStockList->item(i)->setCheckState(Qt::Unchecked);
                }
            }
        );

        connect(upBtn, &QPushButton::clicked, this,
            [this, refreshListUserRoles]() {
                const int row = m_groupList->currentRow();
                if (row <= 0) return;
                const int ur  = m_groupList->item(row)->data(Qt::UserRole).toInt();
                const int pur = m_groupList->item(row - 1)->data(Qt::UserRole).toInt();
                if (ur >= 0 && pur >= 0) m_groups.swapItemsAt(ur, pur);
                if (ur  == -1) m_cfg.groupAllPosition = row - 1;
                else if (pur == -1) m_cfg.groupAllPosition = row;
                QListWidgetItem* it = m_groupList->takeItem(row);
                m_groupList->insertItem(row - 1, it);
                m_groupList->setCurrentRow(row - 1);
                refreshListUserRoles();
            }
        );

        connect(downBtn, &QPushButton::clicked, this,
            [this, refreshListUserRoles]() {
                const int row = m_groupList->currentRow();
                if (row < 0 || row >= m_groupList->count() - 1) return;
                const int ur  = m_groupList->item(row)->data(Qt::UserRole).toInt();
                const int nur = m_groupList->item(row + 1)->data(Qt::UserRole).toInt();
                if (ur >= 0 && nur >= 0) m_groups.swapItemsAt(ur, nur);
                if (ur  == -1) m_cfg.groupAllPosition = row + 1;
                else if (nur == -1) m_cfg.groupAllPosition = row;
                QListWidgetItem* it = m_groupList->takeItem(row);
                m_groupList->insertItem(row + 1, it);
                m_groupList->setCurrentRow(row + 1);
                refreshListUserRoles();
            }
        );

        connect(m_groupList, &QListWidget::itemChanged, this,
            [this](QListWidgetItem* it) {
                const int ur = it->data(Qt::UserRole).toInt();
                if (ur < 0 || ur >= m_groups.size()) return;
                m_groups[ur].name = it->text().trimmed();
            }
        );

        // Checkbox: "所有" group shows only ungrouped members
        QCheckBox* ungroupedOnlyCheck = new QCheckBox(
            trText("settings.group.allGroupUngroupedOnly"), w);
        ungroupedOnlyCheck->setChecked(m_cfg.allGroupShowUngroupedOnly);
        leftVbox->addWidget(ungroupedOnlyCheck);
        connect(ungroupedOnlyCheck, &QCheckBox::toggled, this, [this](bool checked) {
            m_cfg.allGroupShowUngroupedOnly = checked;
        });
    }

    // ── Right: ordered members + stock checkboxes ─────────────────────────
    {
        QVBoxLayout* rightVbox = new QVBoxLayout;
        rightVbox->setSpacing(4);
        mainRow->addLayout(rightVbox, 1);

        // Member order section
        {
            QHBoxLayout* mhdr = new QHBoxLayout;
            mhdr->addWidget(new QLabel(trText("settings.group.orderedMembers"), w));
            mhdr->addStretch();
            QPushButton* mUp   = new QPushButton(QStringLiteral("\u2191"), w); mUp->setMaximumWidth(28);
            QPushButton* mDown = new QPushButton(QStringLiteral("\u2193"), w); mDown->setMaximumWidth(28);
            mhdr->addWidget(mUp); mhdr->addWidget(mDown);
            rightVbox->addLayout(mhdr);

            m_groupMemberList = new QListWidget(w);
            m_groupMemberList->setEnabled(false);
            m_groupMemberList->setMaximumHeight(130);
            rightVbox->addWidget(m_groupMemberList);

            connect(mUp, &QPushButton::clicked, this, [this]() {
                if (!m_groupMemberList->isEnabled()) return;
                const int row = m_groupMemberList->currentRow();
                if (row <= 0) return;
                const QListWidgetItem* cur = m_groupList->currentItem();
                const int gi = cur ? cur->data(Qt::UserRole).toInt() : -1;
                if (gi < 0 || gi >= m_groups.size()) return;
                m_groups[gi].stockCodes.swapItemsAt(row, row - 1);
                QListWidgetItem* it = m_groupMemberList->takeItem(row);
                m_groupMemberList->insertItem(row - 1, it);
                m_groupMemberList->setCurrentRow(row - 1);
            });

            connect(mDown, &QPushButton::clicked, this, [this]() {
                if (!m_groupMemberList->isEnabled()) return;
                const int row = m_groupMemberList->currentRow();
                if (row < 0 || row >= m_groupMemberList->count() - 1) return;
                const QListWidgetItem* cur = m_groupList->currentItem();
                const int gi = cur ? cur->data(Qt::UserRole).toInt() : -1;
                if (gi < 0 || gi >= m_groups.size()) return;
                m_groups[gi].stockCodes.swapItemsAt(row, row + 1);
                QListWidgetItem* it = m_groupMemberList->takeItem(row);
                m_groupMemberList->insertItem(row + 1, it);
                m_groupMemberList->setCurrentRow(row + 1);
            });
        }

        // All stocks section
        {
            rightVbox->addWidget(new QLabel(
                trText("settings.group.members")
                    + QStringLiteral(" (") + trText("settings.group.membersHint") + QStringLiteral(")"),
                w));
            m_groupStockList = new QListWidget(w);
            m_groupStockList->setEnabled(false);
            rightVbox->addWidget(m_groupStockList, 1);
            refreshGroupStockChoices();
        }
    }

    // ── Group selection → populate right panels ───────────────────────────
    connect(m_groupList, &QListWidget::currentRowChanged, this,
        [this](int) {
            refreshCurrentGroupSelection();
        }
    );

    // ── Stock checkbox → update group members ─────────────────────────────
    connect(m_groupStockList, &QListWidget::itemChanged, this,
        [this](QListWidgetItem* it) {
            if (!m_groupList || !m_groupStockList->isEnabled()) return;
            const QListWidgetItem* cur = m_groupList->currentItem();
            if (!cur) return;
            const int gi = cur->data(Qt::UserRole).toInt();
            if (gi < 0 || gi >= m_groups.size()) return;

            const QString code = it->data(Qt::UserRole).toString();
            QStringList& codes = m_groups[gi].stockCodes;

            QSignalBlocker bl(m_groupMemberList);
            if (it->checkState() == Qt::Checked) {
                if (!codes.contains(code)) {
                    codes.append(code);
                    QListWidgetItem* mi = new QListWidgetItem(
                        groupMemberDisplayName(code), m_groupMemberList);
                    mi->setData(Qt::UserRole, code);
                }
            } else {
                codes.removeAll(code);
                for (int i = 0; i < m_groupMemberList->count(); ++i) {
                    if (watchCodeKey(m_groupMemberList->item(i)->data(Qt::UserRole).toString())
                            == watchCodeKey(code)) {
                        delete m_groupMemberList->takeItem(i);
                        break;
                    }
                }
            }
        }
    );

    return w;
}

QWidget* SettingsDialog::buildAboutTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setAlignment(Qt::AlignCenter);
    vbox->setSpacing(12);
    vbox->setContentsMargins(24, 32, 24, 32);

    // Icon
    QLabel* iconLabel = new QLabel(w);
    QPixmap pm(QLatin1String(":/icon.png"));
    if (!pm.isNull()) {
        iconLabel->setPixmap(pm.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    iconLabel->setAlignment(Qt::AlignCenter);
    vbox->addWidget(iconLabel);

    // App name
    QLabel* nameLabel = new QLabel(trText("app.name"), w);
    QFont nameFont = nameLabel->font();
    nameFont.setPointSize(nameFont.pointSize() + 6);
    nameFont.setBold(true);
    nameLabel->setFont(nameFont);
    nameLabel->setAlignment(Qt::AlignCenter);
    vbox->addWidget(nameLabel);

    // Version
    const QString versionStr = QString::fromLatin1(APP_VERSION_STRING);
    QLabel* versionLabel = new QLabel(
        trText("settings.about.version") + QLatin1String(": ") + versionStr, w
    );
    versionLabel->setAlignment(Qt::AlignCenter);
    vbox->addWidget(versionLabel);

    vbox->addSpacing(8);

    // GitHub link
    const QString githubUrl = QString::fromLatin1(APP_GITHUB_URL);
    QLabel* linkLabel = new QLabel(w);
    linkLabel->setText(
        trText("settings.about.github") +
        QLatin1String(": <a href=\"") + githubUrl + QLatin1String("\">") +
        githubUrl + QLatin1String("</a>")
    );
    linkLabel->setAlignment(Qt::AlignCenter);
    linkLabel->setOpenExternalLinks(true);
    linkLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    vbox->addWidget(linkLabel);

    vbox->addSpacing(16);

    // --- Update section ---
    m_checkUpdateBtn = new QPushButton(trText("settings.about.checkUpdate"), w);
    m_checkUpdateBtn->setFixedWidth(160);
    vbox->addWidget(m_checkUpdateBtn, 0, Qt::AlignCenter);

    m_checkUpdateStatus = new QLabel(w);
    m_checkUpdateStatus->setAlignment(Qt::AlignCenter);
    m_checkUpdateStatus->setWordWrap(true);
    vbox->addWidget(m_checkUpdateStatus);

    m_downloadProgress = new QProgressBar(w);
    m_downloadProgress->setRange(0, 100);
    m_downloadProgress->setFixedWidth(300);
    m_downloadProgress->setVisible(false);
    vbox->addWidget(m_downloadProgress, 0, Qt::AlignCenter);

    // --- Updater setup ---
    m_updater = new Updater(this);
    m_updater->setConfig(m_cfg);

    connect(m_checkUpdateBtn, &QPushButton::clicked, this, [this]() {
        m_checkUpdateBtn->setEnabled(false);
        m_checkUpdateStatus->setText(trText("settings.about.checking"));
        m_downloadProgress->setVisible(false);
        m_updater->setConfig(m_cfg);
        m_updater->checkForUpdates();
    });

    connect(m_updater, &Updater::noUpdateAvailable, this, [this]() {
        m_checkUpdateBtn->setEnabled(true);
        m_checkUpdateStatus->setText(trText("settings.about.upToDate"));
    });

    connect(m_updater, &Updater::checkFailed, this, [this](const QString&) {
        m_checkUpdateBtn->setEnabled(true);
        m_checkUpdateStatus->setText(trText("settings.about.checkFailed"));
    });

    connect(m_updater, &Updater::updateAvailable, this,
        [this](const Updater::ReleaseInfo& info) {
            m_checkUpdateBtn->setEnabled(true);
            m_checkUpdateStatus->setText(
                trText("settings.about.updateAvailable").arg(info.tagName)
            );

            const Updater::ReleaseAsset asset = info.platformAsset();
            const bool hasAsset = !asset.browserDownloadUrl.isEmpty();

            QMessageBox msgBox(this);
            msgBox.setWindowTitle(trText("settings.about.updateAvailableTitle"));
            msgBox.setText(
                trText("settings.about.updateAvailableMsg").arg(info.tagName)
            );
            const QIcon windowIcon = dialogWindowIcon(this);
            if (!windowIcon.isNull()) {
                msgBox.setWindowIcon(windowIcon);
            }
            msgBox.setIcon(QMessageBox::Information);

            QPushButton* openPageBtn = msgBox.addButton(
                trText("settings.about.openReleasePage"), QMessageBox::ActionRole
            );
            QPushButton* downloadBtn = nullptr;
            if (hasAsset) {
                downloadBtn = msgBox.addButton(
                    trText("settings.about.download"), QMessageBox::AcceptRole
                );
            }
            msgBox.addButton(QMessageBox::Cancel);

            msgBox.exec();

            if (msgBox.clickedButton() == openPageBtn) {
                QDesktopServices::openUrl(QUrl(info.htmlUrl));
            } else if (downloadBtn && msgBox.clickedButton() == downloadBtn) {
                m_checkUpdateBtn->setEnabled(false);
                m_downloadProgress->setValue(0);
                m_downloadProgress->setVisible(true);
                m_checkUpdateStatus->setText(trText("settings.about.downloading").arg(0));
                m_updater->downloadAsset(asset);
            }
        }
    );

    connect(m_updater, &Updater::downloadProgress, this,
        [this](qint64 received, qint64 total) {
            if (total > 0) {
                const int pct = static_cast<int>(received * 100 / total);
                m_downloadProgress->setValue(pct);
                m_checkUpdateStatus->setText(trText("settings.about.downloading").arg(pct));
            }
        }
    );

    connect(m_updater, &Updater::downloadFinished, this, [this](const QString& filePath) {
        m_checkUpdateBtn->setEnabled(true);
        m_downloadProgress->setVisible(false);
        m_checkUpdateStatus->setText(trText("settings.about.downloadDone"));
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    });

    connect(m_updater, &Updater::downloadFailed, this, [this](const QString&) {
        m_checkUpdateBtn->setEnabled(true);
        m_downloadProgress->setVisible(false);
        m_checkUpdateStatus->setText(trText("settings.about.downloadFailed"));
    });

    vbox->addStretch();
    return w;
}

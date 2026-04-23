#include "settings_dialog.h"

#include "app_logging.h"
#include "config_manager.h"
#include "i18n.h"
#include "network_logger.h"
#include "network_utils.h"

#include <QAbstractItemView>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDropEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QSet>
#include <QStringConverter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

struct IndexPreset {
    QString code;
    QString name;
    QString note;
};

const QVector<IndexPreset>& indexPresets() {
    static const QVector<IndexPreset> presets {
        {QStringLiteral("sh000001"), QStringLiteral("上证指数（上证综指）"), QStringLiteral("沪市主板整体")},
        {QStringLiteral("sz399001"), QStringLiteral("深证成指"), QStringLiteral("深市主板 + 创业板核心 500 只")},
        {QStringLiteral("sh000300"), QStringLiteral("沪深300"), QStringLiteral("两市大盘核心资产")},
        {QStringLiteral("sh000016"), QStringLiteral("上证50"), QStringLiteral("沪市超大盘龙头")},
        {QStringLiteral("sh000905"), QStringLiteral("中证500"), QStringLiteral("中盘成长代表")},
        {QStringLiteral("sh000852"), QStringLiteral("中证1000"), QStringLiteral("小盘成长")},
        {QStringLiteral("sz399006"), QStringLiteral("创业板指"), QStringLiteral("创业板整体（新能源、医药为主）")},
        {QStringLiteral("sz399673"), QStringLiteral("创业板50"), QStringLiteral("创业板龙头 50 只")},
        {QStringLiteral("sh000688"), QStringLiteral("科创50（科创板指）"), QStringLiteral("科创板硬科技龙头 50 只")},
        {QStringLiteral("sh931643"), QStringLiteral("科创创业50（双创50）"), QStringLiteral("科创板 + 创业板龙头 50 只")},
        {QStringLiteral("sz399431"), QStringLiteral("中证银行指数"), QStringLiteral("银行板块")},
        {QStringLiteral("sz399975"), QStringLiteral("中证券商指数"), QStringLiteral("证券 / 券商板块")},
        {QStringLiteral("sh000808"), QStringLiteral("中证医药生物"), QStringLiteral("医药全板块")},
        {QStringLiteral("sh000932"), QStringLiteral("中证消费指数"), QStringLiteral("主要消费（食品饮料、家电等）")},
        {QStringLiteral("sz399808"), QStringLiteral("中证新能源指数"), QStringLiteral("光伏、锂电、风电等")},
        {QStringLiteral("sh980017"), QStringLiteral("中证半导体指数"), QStringLiteral("芯片 / 半导体")},
        {QStringLiteral("100.HSI"), QStringLiteral("恒生指数"), QStringLiteral("港股大盘基准指数")},
        {QStringLiteral("124.HSTECH"), QStringLiteral("恒生科技指数"), QStringLiteral("港股科技龙头指数")},
    };

    return presets;
}

QString watchCodeKey(const QString& code) {
    return code.trimmed().toLower();
}

QString normalizeSectorCode(const QString& rawCode) {
    QString code = rawCode.trimmed();
    if (code.isEmpty()) {
        return {};
    }

    if (code.startsWith(QStringLiteral("90."), Qt::CaseInsensitive)) {
        code = code.mid(3);
    }

    if (code.startsWith(QStringLiteral("bk"), Qt::CaseInsensitive)) {
        return code.toUpper();
    }

    return {};
}

const QSet<QString>& predefinedIndexAliases() {
    static QSet<QString> aliases;
    if (!aliases.isEmpty()) {
        return aliases;
    }

    for (const IndexPreset& preset : indexPresets()) {
        const QString presetCode = watchCodeKey(preset.code);
        aliases.insert(presetCode);

        QString plainCode = presetCode;
        if (presetCode.startsWith(QStringLiteral("sh"))
            || presetCode.startsWith(QStringLiteral("sz"))
            || presetCode.startsWith(QStringLiteral("hk"))) {
            plainCode = presetCode.mid(2);
        }
        if (!plainCode.isEmpty() && plainCode != QStringLiteral("000001") && plainCode != presetCode) {
            aliases.insert(plainCode);
        }
    }

    // Additional aliases from requirement table.
    aliases.insert(QStringLiteral("sz399300"));
    aliases.insert(QStringLiteral("sh932133"));
    aliases.insert(QStringLiteral("hsi"));
    aliases.insert(QStringLiteral("hstech"));
    aliases.insert(QStringLiteral("100.hsi"));
    aliases.insert(QStringLiteral("124.hstech"));

    return aliases;
}

bool isPredefinedIndexCode(const QString& rawCode) {
    const QString code = watchCodeKey(rawCode);
    if (code.isEmpty()) {
        return false;
    }
    return predefinedIndexAliases().contains(code);
}

class StockTableWidget : public QTableWidget {
public:
    explicit StockTableWidget(QWidget* parent = nullptr)
        : QTableWidget(0, 4, parent) {
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDragDropMode(QAbstractItemView::NoDragDrop);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        verticalHeader()->setVisible(false);
        horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
        horizontalHeader()->setStretchLastSection(false);
        setColumnWidth(3, 64);
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

    void addStockRow(const QString& code, const QString& name) {
        const int row = rowCount();
        insertRow(row);
        populateRow(row, code, name);
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
                result.push_back({code, name});
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
        removeRow(row);
        insertRow(0);
        populateRow(0, code, name);
        renumberRows();
        selectRow(0);
        return 0;
    }

    int moveRowBottom(int row) {
        if (row < 0 || row >= rowCount() - 1) return -1;
        const QString code = item(row, 1) ? item(row, 1)->text() : QString();
        const QString name = item(row, 2) ? item(row, 2)->text() : QString();
        removeRow(row);
        const int newRow = rowCount();
        insertRow(newRow);
        populateRow(newRow, code, name);
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
        const QString codeB = item(hi, 1) ? item(hi, 1)->text() : QString();
        const QString nameB = item(hi, 2) ? item(hi, 2)->text() : QString();

        removeRow(hi);
        removeRow(lo);
        insertRow(lo);
        populateRow(lo, codeB, nameB);
        insertRow(hi);
        populateRow(hi, codeA, nameA);

        renumberRows();
        // a moved to: if a was the higher row (moved up), it's now at lo; if lower (moved down), at hi
        const int newCurrent = (a > b) ? lo : hi;
        selectRow(newCurrent);
        return newCurrent;
    }

    void populateRow(int row, const QString& code, const QString& name) {
        QTableWidgetItem* seqItem = new QTableWidgetItem(QString::number(row + 1));
        seqItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        setItem(row, 0, seqItem);

        QTableWidgetItem* codeItem = new QTableWidgetItem(code);
        codeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        setItem(row, 1, codeItem);

        QTableWidgetItem* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        setItem(row, 2, nameItem);

        QPushButton* delBtn = new QPushButton(QStringLiteral("\u2715 Del"));
        delBtn->setFlat(true);
        setCellWidget(row, 3, delBtn);

        connect(delBtn, &QPushButton::clicked, this, [this]() {
            QPushButton* btn = qobject_cast<QPushButton*>(sender());
            if (!btn) {
                return;
            }
            for (int r = 0; r < rowCount(); ++r) {
                if (cellWidget(r, 3) == btn) {
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

QVector<int> normalizedColumnOrder(const QVector<int>& order) {
    QVector<int> out;
    out.reserve(ColCount);

    for (int logical : order) {
        if (logical < 0 || logical >= ColCount || out.contains(logical)) {
            continue;
        }
        out.push_back(logical);
    }

    for (int i = 0; i < ColCount; ++i) {
        if (out.contains(i)) {
            continue;
        }
        out.push_back(i);
    }

    return out;
}

QKeySequence normalizedHotkeySequence(const QKeySequence& sequence) {
    if (sequence.isEmpty()) {
        return {};
    }

    const int keyCombo = sequence[0];
    const int key = keyCombo & ~Qt::KeyboardModifierMask;
    const Qt::KeyboardModifiers mods = Qt::KeyboardModifiers(keyCombo & Qt::KeyboardModifierMask);

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

QString normalizeHoverReadingUiMode(const QString& rawMode) {
    const QString mode = rawMode.trimmed().toLower();
    if (mode == QLatin1String("light") || mode == QLatin1String("dark")) {
        return mode;
    }
    return QStringLiteral("dark");
}

QStringList normalizeSinaCodesForMarket(const QString& rawCode, const QString& marketFilter) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty()) {
        return {};
    }

    if (marketFilter == QLatin1String("a")) {
        if (code.startsWith(QLatin1String("sh")) || code.startsWith(QLatin1String("sz"))) {
            return {code};
        }
        return {};
    }

    if (marketFilter == QLatin1String("hk")) {
        if (code.startsWith(QLatin1String("hk"))) {
            return {code};
        }
        return {};
    }

    if (marketFilter == QLatin1String("etf")) {
        if (!code.startsWith(QLatin1String("of")) || code.size() <= 2) {
            return {};
        }
        return {code};
    }

    return {};
}

QString xtickResponseMessage(const QJsonObject& obj) {
    const QStringList keys {
        QStringLiteral("message"),
        QStringLiteral("msg"),
        QStringLiteral("error"),
        QStringLiteral("err"),
    };

    for (const QString& key : keys) {
        const QString message = obj.value(key).toString().trimmed();
        if (!message.isEmpty()) {
            return message;
        }
    }

    return {};
}

bool isTokenClearlyInvalid(const QString& message) {
    const QString msg = message.trimmed();
    if (msg.isEmpty()) {
        return false;
    }

    return msg.contains(QStringLiteral("请带Token访问"), Qt::CaseInsensitive)
        || msg.contains(QStringLiteral("登录后可获取"), Qt::CaseInsensitive)
        || msg.contains(QStringLiteral("token"), Qt::CaseInsensitive);
}

bool isTokenQuotaExceeded(const QString& message) {
    const QString msg = message.trimmed();
    return msg.contains(QStringLiteral("超出最大请求次数"), Qt::CaseInsensitive);
}

} // namespace

SettingsDialog::SettingsDialog(
    const AppConfig& cfg,
    const QVector<StockItem>& stocks,
    const QVector<StockItem>& indexes,
    const QVector<StockItem>& sectors,
    const QHash<QString, QString>& apiNamesByCode,
    const QString& dataYamlPath,
    std::function<void()> onWriteStockNames,
    QWidget* parent
)
    : QDialog(parent)
    , m_cfg(cfg)
    , m_stocks(stocks)
    , m_indexes(indexes)
    , m_sectors(sectors)
    , m_apiNamesByCode(apiNamesByCode)
    , m_dataYamlPath(dataYamlPath)
    , m_onWriteStockNames(std::move(onWriteStockNames))
    , m_uiLanguage(i18n::resolveLanguage(cfg.language)) {
    setWindowTitle(trText("settings.title"));
    setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);
    resize(600, 560);

    QVBoxLayout* root = new QVBoxLayout(this);
    QTabWidget* tabs = new QTabWidget(this);
    tabs->addTab(buildGeneralTab(), trText("settings.tab.general"));
    tabs->addTab(buildNetworkTab(), trText("settings.tab.network"));
    tabs->addTab(buildDisplayTab(), trText("settings.tab.display"));
    tabs->addTab(buildStocksTab(), trText("settings.tab.stocks"));
    tabs->addTab(buildIndexSectorTab(), trText("settings.tab.indexSector"));
    tabs->addTab(buildOtherTab(), trText("settings.tab.other"));
    tabs->addTab(buildAboutTab(), trText("settings.tab.about"));
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

    // Prevent user from resizing the dialog
    layout()->setSizeConstraint(QLayout::SetFixedSize);
}

AppConfig SettingsDialog::config() const {
    AppConfig out = m_cfg;

    out.pollMs = m_pollSpin->value();
    out.hotkey = normalizedHotkeySequence(m_hotkeyEdit->keySequence()).toString(QKeySequence::PortableText);
    out.startupShowFloatingWindow = m_startupShowFloatingWindowCheck->isChecked();
    out.apiSource = m_sourceCombo->currentData().toString();
    out.xtickToken = m_tokenEdit->text().trimmed();
    out.userAgent = m_userAgentEdit->text().trimmed();
    out.proxyType = m_proxyTypeCombo->currentData().toString();
    out.proxyHost = m_proxyHostEdit->text().trimmed();
    out.proxyPort = m_proxyPortSpin->value();
    out.proxyUser = m_proxyUserEdit->text().trimmed();
    out.debugIgnoreTradingTime = m_debugIgnoreTradingTimeCheck->isChecked();
    out.logEnabled = m_logEnabledCheck->isChecked();
    out.logLevel = app_logging::normalizeLogLevel(m_logLevelCombo->currentData().toString());
    out.transparentBackgroundEnabled = m_transparentBackgroundCheck->isChecked();
    out.transparentBackgroundOpacity = m_transparentOpacitySlider->value();
    out.proxyPassword = m_proxyPasswordEdit->text();
    out.language = m_languageCombo->currentData().toString();

    out.bgColor = buttonColor(m_bgBtn);
    out.textColor = buttonColor(m_textBtn);
    out.upColor = buttonColor(m_upBtn);
    out.downColor = buttonColor(m_downBtn);
    out.flatColor = buttonColor(m_flatBtn);

    out.showHeader = m_showHeaderCheck->isChecked();
    out.showGrid = m_showGridCheck->isChecked();
    out.gridColor = buttonColor(m_gridColorBtn);
    out.floatingWindowAlwaysOnTop = m_floatingTopMostCheck->isChecked();
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
    out.columnOrder = normalizedColumnOrder(out.columnOrder);

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

QVector<StockItem> SettingsDialog::selectedSectors() const {
    if (!m_sectorTable) {
        return m_sectors;
    }

    QVector<StockItem> out;
    QSet<QString> seen;

    const QVector<StockItem> sectors = static_cast<StockTableWidget*>(m_sectorTable)->stocks();
    for (const StockItem& sector : sectors) {
        const QString code = normalizeSectorCode(sector.code);
        if (code.isEmpty()) {
            continue;
        }

        const QString key = watchCodeKey(code);
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);

        const QString name = sector.name.trimmed();
        out.push_back({code, name.isEmpty() ? code : name});
    }

    return out;
}

QString SettingsDialog::trText(const QString& key) const {
    return i18n::t(key, m_uiLanguage);
}

void SettingsDialog::updateHotkeyIndicator(const QKeySequence& seq) {
    if (!m_hotkeyIndicator) {
        return;
    }

    if (seq.isEmpty()) {
        m_hotkeyIndicator->clear();
        m_hotkeyIndicator->setToolTip(QString());
        return;
    }

    // Valid format: has at least one modifier key + a real key → green ✓
    m_hotkeyIndicator->setText(QStringLiteral("\u2713"));
    m_hotkeyIndicator->setStyleSheet(QStringLiteral("color: green; font-weight: bold; font-size: 14px;"));
    m_hotkeyIndicator->setToolTip(trText("hotkey.available"));
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
        const QColor picked = QColorDialog::getColor(current, parent, pickTitle);
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
    QFormLayout* form = new QFormLayout(w);

    m_pollSpin = new QSpinBox(w);
    m_pollSpin->setRange(500, 60000);
    m_pollSpin->setValue(m_cfg.pollMs);

    m_hotkeyEdit = new QKeySequenceEdit(QKeySequence(m_cfg.hotkey), w);
    m_hotkeyEdit->setToolTip(trText("settings.general.hotkeyHint"));
    connect(m_hotkeyEdit, &QKeySequenceEdit::keySequenceChanged, this, [this](const QKeySequence& sequence) {
        if (m_normalizingHotkeySequence || !m_hotkeyEdit) {
            return;
        }

        const QKeySequence normalized = normalizedHotkeySequence(sequence);
        if (normalized != sequence) {
            m_normalizingHotkeySequence = true;
            m_hotkeyEdit->setKeySequence(normalized);
            m_normalizingHotkeySequence = false;
        }

        updateHotkeyIndicator(normalized);
    });

    {
        const QKeySequence initialNormalized = normalizedHotkeySequence(m_hotkeyEdit->keySequence());
        if (initialNormalized != m_hotkeyEdit->keySequence()) {
            m_hotkeyEdit->setKeySequence(initialNormalized);
        }
    }

    m_hotkeyIndicator = new QLabel(w);
    m_hotkeyIndicator->setMinimumWidth(22);
    m_hotkeyIndicator->setAlignment(Qt::AlignCenter);
    updateHotkeyIndicator(normalizedHotkeySequence(m_hotkeyEdit->keySequence()));

    m_hotkeyClearBtn = new QPushButton(QStringLiteral("✕"), w);
    m_hotkeyClearBtn->setFixedWidth(28);
    m_hotkeyClearBtn->setToolTip(trText("settings.general.hotkeyClear"));
    connect(m_hotkeyClearBtn, &QPushButton::clicked, this, [this]() {
        m_hotkeyEdit->setKeySequence(QKeySequence());
    });

    QWidget* hotkeyWidget = new QWidget(w);
    QHBoxLayout* hotkeyLayout = new QHBoxLayout(hotkeyWidget);
    hotkeyLayout->setContentsMargins(0, 0, 0, 0);
    hotkeyLayout->setSpacing(6);
    hotkeyLayout->addWidget(m_hotkeyEdit, 1);
    hotkeyLayout->addWidget(m_hotkeyIndicator);
    hotkeyLayout->addWidget(m_hotkeyClearBtn);

    m_startupShowFloatingWindowCheck = new QCheckBox(
        trText("settings.general.startupShowFloatingWindow"),
        w
    );
    m_startupShowFloatingWindowCheck->setChecked(m_cfg.startupShowFloatingWindow);

    m_sourceCombo = new QComboBox(w);
    m_sourceCombo->addItem(trText("settings.source.tencent"), "tencent");
    m_sourceCombo->addItem(trText("settings.source.mock"), "mock");
    m_sourceCombo->addItem(trText("settings.source.xtick"), "xtick");
    m_sourceCombo->addItem(trText("settings.source.sina"), "sina");
    m_sourceCombo->addItem(trText("settings.source.eastmoney"), "eastmoney");
    const int sourceIndex = m_sourceCombo->findData(m_cfg.apiSource);
    if (sourceIndex >= 0) {
        m_sourceCombo->setCurrentIndex(sourceIndex);
    }

    m_tokenEdit = new QLineEdit(m_cfg.xtickToken, w);
    m_tokenEdit->setPlaceholderText(trText("settings.general.token"));

    m_tokenCheckBtn = new QPushButton(trText("settings.general.tokenCheck"), w);

    m_tokenRowWidget = new QWidget(w);
    QHBoxLayout* tokenLayout = new QHBoxLayout(m_tokenRowWidget);
    tokenLayout->setContentsMargins(0, 0, 0, 0);
    tokenLayout->setSpacing(6);
    tokenLayout->addWidget(m_tokenEdit, 1);
    tokenLayout->addWidget(m_tokenCheckBtn);

    if (!m_tokenCheckNam) {
        m_tokenCheckNam = new QNetworkAccessManager(this);
    }

    connect(m_tokenCheckBtn, &QPushButton::clicked, this, [this]() {
        if (!m_tokenEdit || !m_tokenCheckBtn || !m_tokenCheckNam) {
            return;
        }

        const QString token = m_tokenEdit->text().trimmed();
        if (token.isEmpty()) {
            QMessageBox::warning(
                this,
                trText("app.name"),
                trText("settings.general.tokenEmpty")
            );
            return;
        }

        qInfo() << "[TokenCheck] start tokenLength=" << token.size();

        if (m_tokenCheckReply) {
            m_tokenCheckReply->abort();
            m_tokenCheckReply->deleteLater();
            m_tokenCheckReply = nullptr;
        }

        const AppConfig currentCfg = config();
        m_tokenCheckNam->setProxy(network_utils::proxyFromConfig(currentCfg));

        QUrl url(QStringLiteral("http://api.xtick.top/doc/order/time"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("type"), QStringLiteral("10"));
        query.addQueryItem(QStringLiteral("code"), QStringLiteral("000001"));
        query.addQueryItem(QStringLiteral("period"), QStringLiteral("lv1"));
        query.addQueryItem(QStringLiteral("token"), token);
        url.setQuery(query);

        QNetworkRequest req(url);
        req.setRawHeader("User-Agent", network_utils::effectiveUserAgent(currentCfg).toUtf8());
        req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

        const QNetworkProxy proxy = network_utils::proxyFromConfig(currentCfg);
        const network_logger::RequestTrace trace = network_logger::logRequestStart(
            "xtick-token-check",
            "GET",
            req,
            proxy
        );

        m_tokenCheckBtn->setEnabled(false);
        m_tokenCheckBtn->setText(trText("settings.general.tokenChecking"));

        m_tokenCheckReply = m_tokenCheckNam->get(req);
        connect(m_tokenCheckReply, &QNetworkReply::finished, this, [this, trace]() {
            if (!m_tokenCheckReply) {
                return;
            }

            QNetworkReply* reply = m_tokenCheckReply;
            m_tokenCheckReply = nullptr;

            const QByteArray body = reply->readAll();
            const QString netError = (reply->error() == QNetworkReply::NoError)
                ? QString()
                : reply->errorString();

            network_logger::logRequestFinish(trace, reply, body.size(), body);

            reply->deleteLater();

            m_tokenCheckBtn->setEnabled(true);
            m_tokenCheckBtn->setText(trText("settings.general.tokenCheck"));

            if (!netError.isEmpty()) {
                QMessageBox::warning(
                    this,
                    trText("app.name"),
                    trText("settings.general.tokenCheckFailedFmt").arg(netError)
                );
                return;
            }

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                QMessageBox::warning(
                    this,
                    trText("app.name"),
                    trText("settings.general.tokenCheckFailedFmt")
                        .arg(QStringLiteral("json parse error: ") + parseError.errorString())
                );
                return;
            }

            QString message;
            bool tokenLooksValid = false;
            bool quotaExceeded = false;

            if (doc.isArray()) {
                tokenLooksValid = true;
            } else if (doc.isObject()) {
                const QJsonObject obj = doc.object();
                message = xtickResponseMessage(obj);

                const int code = obj.value(QStringLiteral("code")).toInt(0);
                if (obj.contains(QStringLiteral("lastPrice"))
                    || obj.contains(QStringLiteral("price"))
                    || obj.contains(QStringLiteral("close"))
                    || obj.value(QStringLiteral("data")).isArray()
                    || obj.value(QStringLiteral("result")).isArray()) {
                    tokenLooksValid = true;
                } else if (code == 0) {
                    tokenLooksValid = true;
                } else if (isTokenQuotaExceeded(message)) {
                    tokenLooksValid = true;
                    quotaExceeded = true;
                } else if (isTokenClearlyInvalid(message)) {
                    tokenLooksValid = false;
                } else if (!message.isEmpty()) {
                    tokenLooksValid = false;
                }
            } else {
                message = QString::fromUtf8(body).trimmed();
            }

            if (tokenLooksValid) {
                const QString resultMessage = quotaExceeded
                    ? trText("settings.general.tokenValidWithQuota")
                    : trText("settings.general.tokenValid");
                qInfo() << "[TokenCheck] valid token result quotaExceeded=" << quotaExceeded;
                QMessageBox::information(this, trText("app.name"), resultMessage);
                return;
            }

            const QString detail = message.isEmpty()
                ? QString::fromUtf8(body).trimmed()
                : message;
            const QString fallback = trText("settings.general.tokenInvalid");
            QMessageBox::warning(
                this,
                trText("app.name"),
                trText("settings.general.tokenCheckFailedFmt").arg(
                    detail.isEmpty() ? fallback : detail
                )
            );
            qWarning() << "[TokenCheck] invalid token detail=" << (detail.isEmpty() ? fallback : detail);
        });
    });

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
    transparentOpacityLayout->setSpacing(8);
    transparentOpacityLayout->addWidget(m_transparentOpacitySlider, 1);
    transparentOpacityLayout->addWidget(m_transparentOpacityLabel);

    m_languageCombo = new QComboBox(w);
    m_languageCombo->addItem(trText("settings.language.auto"), "auto");
    m_languageCombo->addItem(trText("settings.language.zh"), "zh_CN");
    m_languageCombo->addItem(trText("settings.language.en"), "en_US");
    const int languageIndex = m_languageCombo->findData(i18n::normalizeLanguage(m_cfg.language));
    if (languageIndex >= 0) {
        m_languageCombo->setCurrentIndex(languageIndex);
    }

    const QString pickColorTitle = trText("settings.color.pick");
    m_bgBtn = createColorButton(w, m_cfg.bgColor, pickColorTitle);
    m_textBtn = createColorButton(w, m_cfg.textColor, pickColorTitle);
    m_upBtn = createColorButton(w, m_cfg.upColor, pickColorTitle);
    m_downBtn = createColorButton(w, m_cfg.downColor, pickColorTitle);
    m_flatBtn = createColorButton(w, m_cfg.flatColor, pickColorTitle);

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

        paintColorButton(m_bgBtn, defaultCfg.bgColor);
        paintColorButton(m_textBtn, defaultCfg.textColor);
        paintColorButton(m_upBtn, defaultCfg.upColor);
        paintColorButton(m_downBtn, defaultCfg.downColor);
        paintColorButton(m_flatBtn, defaultCfg.flatColor);
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

    form->addRow(trText("settings.general.poll"), m_pollSpin);
    form->addRow(trText("settings.general.hotkey"), hotkeyWidget);
    form->addRow(m_startupShowFloatingWindowCheck);
    form->addRow(trText("settings.general.apiSource"), m_sourceCombo);
    form->addRow(trText("settings.general.token"), m_tokenRowWidget);
    m_tokenRowLabel = form->labelForField(m_tokenRowWidget);

    const auto syncTokenRowVisibility = [this]() {
        const bool showToken = m_sourceCombo
            && m_sourceCombo->currentData().toString() == QLatin1String("xtick");

        if (m_tokenRowWidget) {
            m_tokenRowWidget->setVisible(showToken);
        }
        if (m_tokenRowLabel) {
            m_tokenRowLabel->setVisible(showToken);
        }
    };

    connect(
        m_sourceCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [syncTokenRowVisibility](int) {
            syncTokenRowVisibility();
        }
    );
    syncTokenRowVisibility();

    form->addRow(m_transparentBackgroundCheck);
    form->addRow(trText("settings.general.transparentOpacity"), transparentOpacityWidget);
    form->addRow(trText("settings.general.language"), m_languageCombo);
    form->addRow(trText("settings.general.background"), m_bgBtn);
    form->addRow(trText("settings.general.text"), m_textBtn);
    form->addRow(trText("settings.general.up"), m_upBtn);
    form->addRow(trText("settings.general.down"), m_downBtn);
    form->addRow(trText("settings.general.flat"), m_flatBtn);
    form->addRow(resetColorsButton);

    return w;
}

QWidget* SettingsDialog::buildNetworkTab() {
    QWidget* w = new QWidget(this);
    QFormLayout* form = new QFormLayout(w);

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
    QFormLayout* form = new QFormLayout(w);

    m_floatingTopMostCheck = new QCheckBox(trText("settings.display.alwaysOnTop"), w);
    m_floatingTopMostCheck->setChecked(m_cfg.floatingWindowAlwaysOnTop);
    form->addRow(m_floatingTopMostCheck);

    m_showHeaderCheck = new QCheckBox(trText("settings.display.showHeader"), w);
    m_showHeaderCheck->setChecked(m_cfg.showHeader);
    form->addRow(m_showHeaderCheck);

    m_showGridCheck = new QCheckBox(trText("settings.display.showGrid"), w);
    m_showGridCheck->setChecked(m_cfg.showGrid);
    m_gridColorBtn = createColorButton(w, m_cfg.gridColor, trText("settings.color.pick"));
    m_gridColorBtn->setEnabled(m_cfg.showGrid);
    connect(m_showGridCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_gridColorBtn->setEnabled(checked);
    });
    form->addRow(m_showGridCheck);
    form->addRow(trText("settings.display.gridColor"), m_gridColorBtn);

    m_simpleModeCheck = new QCheckBox(trText("settings.display.simpleMode"), w);
    m_simpleModeCheck->setChecked(m_cfg.simpleModeEnabled);
    form->addRow(m_simpleModeCheck);

    m_blinkReminderCheck = new QCheckBox(trText("settings.display.blinkReminder"), w);
    m_blinkReminderCheck->setChecked(m_cfg.blinkReminderEnabled);
    m_blinkReminderCheck->setEnabled(m_cfg.simpleModeEnabled);
    connect(m_simpleModeCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_blinkReminderCheck->setEnabled(checked);
    });
    form->addRow(m_blinkReminderCheck);

    m_trayTooltipCheck = new QCheckBox(trText("settings.display.trayTooltip"), w);
    m_trayTooltipCheck->setChecked(m_cfg.trayTooltipEnabled);
    form->addRow(m_trayTooltipCheck);

    m_hoverReadingCheck = new QCheckBox(trText("settings.display.hoverReading"), w);
    m_hoverReadingCheck->setChecked(m_cfg.hoverReadingEnabled);
    m_hoverReadingDelaySpin = new QDoubleSpinBox(w);
    m_hoverReadingDelaySpin->setRange(0.1, 60.0);
    m_hoverReadingDelaySpin->setSingleStep(0.1);
    m_hoverReadingDelaySpin->setDecimals(1);
    m_hoverReadingDelaySpin->setSuffix(trText("settings.display.hoverReadingDelaySuffix"));
    m_hoverReadingDelaySpin->setValue(qBound(0.1, m_cfg.hoverReadingDelaySecs, 60.0));
    m_hoverReadingDelaySpin->setEnabled(m_cfg.hoverReadingEnabled);

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

    QLabel* hoverReadingDelayLabel = new QLabel(trText("settings.display.hoverReadingDelay"), w);
    QLabel* hoverReadingModeLabel = new QLabel(trText("settings.display.hoverReadingMode"), w);
    hoverReadingDelayLabel->setEnabled(m_cfg.hoverReadingEnabled);
    hoverReadingModeLabel->setEnabled(m_cfg.hoverReadingEnabled);

    connect(m_hoverReadingCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_hoverReadingDelaySpin->setEnabled(checked);
        if (m_hoverReadingModeCombo) {
            m_hoverReadingModeCombo->setEnabled(checked);
        }
        if (m_hoverReadingTransparentBackgroundCheck) {
            m_hoverReadingTransparentBackgroundCheck->setEnabled(checked);
        }
    });
    connect(m_hoverReadingCheck, &QCheckBox::toggled, hoverReadingDelayLabel, &QLabel::setEnabled);
    connect(m_hoverReadingCheck, &QCheckBox::toggled, hoverReadingModeLabel, &QLabel::setEnabled);

    {
        QWidget* hoverReadingWidget = new QWidget(w);
        QHBoxLayout* hoverReadingLayout = new QHBoxLayout(hoverReadingWidget);
        hoverReadingLayout->setContentsMargins(0, 0, 0, 0);
        hoverReadingLayout->setSpacing(8);
        hoverReadingLayout->addWidget(m_hoverReadingCheck);
        hoverReadingLayout->addStretch(1);
        hoverReadingLayout->addWidget(hoverReadingDelayLabel);
        hoverReadingLayout->addWidget(m_hoverReadingDelaySpin);
        hoverReadingLayout->addWidget(hoverReadingModeLabel);
        hoverReadingLayout->addWidget(m_hoverReadingModeCombo);
        hoverReadingLayout->addWidget(m_hoverReadingTransparentBackgroundCheck);
        form->addRow(hoverReadingWidget);
    }

    const QStringList names = i18n::columnNames(m_uiLanguage);
    const QVector<int> columnOrder = normalizedColumnOrder(m_cfg.columnOrder);

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

    form->addRow(trText("settings.display.columns"), m_columnList);

    QLabel* columnsHint = new QLabel(trText("settings.display.columnsHint"), w);
    columnsHint->setWordWrap(true);
    form->addRow(columnsHint);

    m_columnMaxWidthSpins.resize(ColCount);

    const QString maxSuffix = trText("settings.display.pxSuffix");
    const QString autoText = trText("settings.display.auto");

    for (int i = 0; i < ColCount; ++i) {
        m_columnMaxWidthSpins[i] = new QSpinBox(w);
        m_columnMaxWidthSpins[i]->setRange(0, 1200);
        m_columnMaxWidthSpins[i]->setSpecialValueText(autoText);
        m_columnMaxWidthSpins[i]->setSuffix(maxSuffix);
        m_columnMaxWidthSpins[i]->setValue(qMax(0, m_cfg.columnMaxWidths.value(i, 0)));

        form->addRow(
            trText("settings.display.columnMaxNameFmt").arg(names.value(i)),
            m_columnMaxWidthSpins[i]
        );
    }

    return w;
}

QWidget* SettingsDialog::buildOtherTab() {
    QWidget* w = new QWidget(this);
    QFormLayout* form = new QFormLayout(w);

    m_debugIgnoreTradingTimeCheck = new QCheckBox(
        trText("settings.general.debugIgnoreTradingTime"),
        w
    );
    m_debugIgnoreTradingTimeCheck->setChecked(m_cfg.debugIgnoreTradingTime);

    m_logEnabledCheck = new QCheckBox(trText("settings.other.logEnabled"), w);
    m_logEnabledCheck->setChecked(m_cfg.logEnabled);

    m_logLevelCombo = new QComboBox(w);
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

    m_openLogDirButton = new QPushButton(trText("settings.other.openLogFolder"), w);
    connect(m_openLogDirButton, &QPushButton::clicked, this, []() {
        const QString logDir = app_logging::logDirectoryPath();
        QDir().mkpath(logDir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(logDir));
    });

    m_writeStockNamesButton = new QPushButton(trText("settings.other.writeStockNames"), w);
    connect(m_writeStockNamesButton, &QPushButton::clicked, this, [this]() {
        if (!m_onWriteStockNames) {
            QMessageBox::warning(
                this,
                trText("app.name"),
                trText("settings.other.writeStockNamesUnavailable")
            );
            return;
        }
        m_onWriteStockNames();
    });

    connect(m_logEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        m_logLevelCombo->setEnabled(enabled);
    });

    m_logLevelCombo->setEnabled(m_logEnabledCheck->isChecked());

    form->addRow(m_debugIgnoreTradingTimeCheck);
    form->addRow(m_logEnabledCheck);
    form->addRow(trText("settings.other.logLevel"), m_logLevelCombo);
    form->addRow(m_openLogDirButton);
    form->addRow(m_writeStockNamesButton);

    return w;
}

QWidget* SettingsDialog::buildStocksTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setSpacing(6);
    vbox->setContentsMargins(8, 8, 8, 8);

    // --- Search row ---
    QWidget* searchRow = new QWidget(w);
    QHBoxLayout* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(6);

    m_stockMarketCombo = new QComboBox(searchRow);
    m_stockMarketCombo->addItem(trText("settings.stocks.market1"), QStringLiteral("a"));
    m_stockMarketCombo->addItem(trText("settings.stocks.market2"), QStringLiteral("hk"));
    m_stockMarketCombo->addItem(trText("settings.stocks.market6"), QStringLiteral("etf"));

    m_stockSearchEdit = new QLineEdit(searchRow);
    m_stockSearchEdit->setPlaceholderText(trText("settings.stocks.searchPlaceholder"));

    m_stockSearchBtn = new QPushButton(trText("settings.stocks.search"), searchRow);

    searchLayout->addWidget(m_stockMarketCombo);
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
        trText("settings.stocks.colDel")
    });
    table->setMinimumHeight(220);

    for (const StockItem& s : m_stocks) {
        // Use API name if available, fall back to stored name
        const QString displayName = m_apiNamesByCode.value(s.code, s.name);
        table->addStockRow(s.code, displayName);
    }

    m_stockTable = table;
    table->setConfirmDelete([this](const QString& display) -> bool {
        const int ret = QMessageBox::question(
            this,
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
    tableAreaLayout->setSpacing(4);
    tableAreaLayout->addWidget(m_stockTable, 1);

    QWidget* orderBtnCol = new QWidget(tableArea);
    QVBoxLayout* orderBtnLayout = new QVBoxLayout(orderBtnCol);
    orderBtnLayout->setContentsMargins(0, 0, 0, 0);
    orderBtnLayout->setSpacing(4);

    auto makeOrderBtn = [&](const QString& label) -> QPushButton* {
        QPushButton* btn = new QPushButton(label, orderBtnCol);
        btn->setFixedWidth(60);
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
    QPushButton* saveBtn = new QPushButton(trText("settings.stocks.save"), btnRow);
    QPushButton* resetBtn = new QPushButton(trText("settings.stocks.reset"), btnRow);
    btnLayout->addStretch();
    btnLayout->addWidget(saveBtn);
    btnLayout->addWidget(resetBtn);
    vbox->addWidget(btnRow);

    // --- Network setup ---
    m_stockSearchNam = new QNetworkAccessManager(this);
    m_stockSearchNam->setProxy(network_utils::proxyFromConfig(m_cfg));

    m_stockSearchDebounce = new QTimer(this);
    m_stockSearchDebounce->setSingleShot(true);
    m_stockSearchDebounce->setInterval(400);

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
        qInfo() << "[StockSearch] textEdited:" << rawText;
        QString text = rawText;
        text.remove(QLatin1Char(' '));
        if (text != rawText) {
            m_stockSearchEdit->setText(text);
        }
        if (text.length() < 3) {
            m_stockSearchDebounce->stop();
            m_stockSuggestList->clear();
            m_stockSuggestList->hide();
            return;
        }
        qInfo() << "[StockSearch] starting debounce for:" << text;
        m_stockSearchDebounce->start();
    });

    // When market type changes, re-trigger search if text is ready
    connect(m_stockMarketCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_stockSearchEdit->text().length() >= 3) {
            m_stockSearchDebounce->start();
        }
    });

    // Manual search button: fire immediately (bypass debounce and min-length)
    connect(m_stockSearchBtn, &QPushButton::clicked, this, [this]() {
        qInfo() << "[StockSearch] button clicked, keyword:" << m_stockSearchEdit->text();
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
            QMessageBox::information(
                this,
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
            QMessageBox::warning(this, trText("app.name"), trText("settings.stocks.saveFail"));
            return;
        }
        const QVector<StockItem> stocks =
            static_cast<StockTableWidget*>(m_stockTable)->stocks();
        qInfo() << "[StocksTab] save requested path=" << m_dataYamlPath << "count=" << stocks.size();
        if (ConfigManager::saveStocksToYaml(m_dataYamlPath, stocks)) {
            qInfo() << "[StocksTab] save success path=" << m_dataYamlPath;
            QMessageBox::information(this, trText("app.name"), trText("settings.stocks.saveOk"));
        } else {
            qWarning() << "[StocksTab] save failed path=" << m_dataYamlPath;
            QMessageBox::warning(this, trText("app.name"), trText("settings.stocks.saveFail"));
        }
    });

    // Reset button: reload from data.yaml and repopulate
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        qInfo() << "[StocksTab] reset requested path=" << m_dataYamlPath;

        // Clear search state
        m_stockSearchEdit->clear();
        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
        if (m_stockSearchDebounce) {
            m_stockSearchDebounce->stop();
        }
        if (m_stockSearchReply) {
            m_stockSearchReply->abort();
            m_stockSearchReply->deleteLater();
            m_stockSearchReply = nullptr;
        }

        // Reload stocks from yaml
        QVector<StockItem> loaded;
        if (!m_dataYamlPath.isEmpty()) {
            loaded = ConfigManager::loadStocksFromYaml(m_dataYamlPath);
        }

        qInfo() << "[StocksTab] reset loaded count=" << loaded.size();

        QStringList ignoredIndexes;

        // Rebuild table
        StockTableWidget* tbl = static_cast<StockTableWidget*>(m_stockTable);
        tbl->setRowCount(0);
        for (const StockItem& s : loaded) {
            if (isPredefinedIndexCode(s.code)) {
                ignoredIndexes.push_back(s.code);
                continue;
            }
            tbl->addStockRow(s.code, s.name);
        }

        ignoredIndexes.removeDuplicates();
        if (!ignoredIndexes.isEmpty()) {
            QMessageBox::information(
                this,
                trText("app.name"),
                trText("settings.indexSector.ignoreYamlIndexFmt")
                    .arg(ignoredIndexes.join(QStringLiteral(", ")))
            );
        }
    });

    return w;
}

QWidget* SettingsDialog::buildIndexSectorTab() {
    QWidget* w = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(w);
    root->setSpacing(8);
    root->setContentsMargins(8, 8, 8, 8);

    // --- Index section ---
    QGroupBox* indexGroup = new QGroupBox(trText("settings.indexSector.indexGroup"), w);
    QVBoxLayout* indexLayout = new QVBoxLayout(indexGroup);
    indexLayout->setContentsMargins(8, 8, 8, 8);
    indexLayout->setSpacing(6);

    QLabel* indexHint = new QLabel(trText("settings.indexSector.indexHint"), indexGroup);
    indexHint->setWordWrap(true);

    m_indexList = new QListWidget(indexGroup);
    m_indexList->setDragDropMode(QAbstractItemView::InternalMove);
    m_indexList->setDefaultDropAction(Qt::MoveAction);
    m_indexList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_indexList->setDragEnabled(true);
    m_indexList->setAcceptDrops(true);
    m_indexList->setDropIndicatorShown(true);
    m_indexList->setMinimumHeight(170);

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

    // --- Sector section ---
    QGroupBox* sectorGroup = new QGroupBox(trText("settings.indexSector.sectorGroup"), w);
    QVBoxLayout* sectorLayout = new QVBoxLayout(sectorGroup);
    sectorLayout->setContentsMargins(8, 8, 8, 8);
    sectorLayout->setSpacing(6);

    QLabel* sectorHint = new QLabel(trText("settings.indexSector.sectorHint"), sectorGroup);
    sectorHint->setWordWrap(true);
    sectorLayout->addWidget(sectorHint);

    QWidget* searchRow = new QWidget(sectorGroup);
    QHBoxLayout* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(6);

    m_sectorSearchEdit = new QLineEdit(searchRow);
    m_sectorSearchEdit->setPlaceholderText(trText("settings.indexSector.sectorSearchPlaceholder"));
    m_sectorSearchBtn = new QPushButton(trText("settings.stocks.search"), searchRow);

    searchLayout->addWidget(m_sectorSearchEdit, 1);
    searchLayout->addWidget(m_sectorSearchBtn);
    sectorLayout->addWidget(searchRow);

    m_sectorSuggestList = new QListWidget(this);
    m_sectorSuggestList->setMaximumHeight(180);
    m_sectorSuggestList->setFrameShape(QFrame::StyledPanel);
    m_sectorSuggestList->hide();

    StockTableWidget* table = new StockTableWidget(sectorGroup);
    table->setHorizontalHeaderLabels({
        trText("settings.stocks.colSeq"),
        trText("settings.stocks.colCode"),
        trText("settings.stocks.colName"),
        trText("settings.stocks.colDel")
    });
    table->setMinimumHeight(180);

    for (const StockItem& sector : m_sectors) {
        const QString code = normalizeSectorCode(sector.code);
        if (code.isEmpty() || table->containsCode(code)) {
            continue;
        }
        table->addStockRow(code, sector.name);
    }

    m_sectorTable = table;
    table->setConfirmDelete([this](const QString& display) -> bool {
        const int ret = QMessageBox::question(
            this,
            trText("app.name"),
            trText("settings.indexSector.sectorDelConfirm").arg(display),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        return ret == QMessageBox::Yes;
    });

    QWidget* tableArea = new QWidget(sectorGroup);
    QHBoxLayout* tableAreaLayout = new QHBoxLayout(tableArea);
    tableAreaLayout->setContentsMargins(0, 0, 0, 0);
    tableAreaLayout->setSpacing(4);
    tableAreaLayout->addWidget(m_sectorTable, 1);

    QWidget* orderBtnCol = new QWidget(tableArea);
    QVBoxLayout* orderBtnLayout = new QVBoxLayout(orderBtnCol);
    orderBtnLayout->setContentsMargins(0, 0, 0, 0);
    orderBtnLayout->setSpacing(4);

    auto makeOrderBtn = [&](const QString& label) -> QPushButton* {
        QPushButton* btn = new QPushButton(label, orderBtnCol);
        btn->setFixedWidth(60);
        return btn;
    };

    QPushButton* btnTop = makeOrderBtn(trText("settings.stocks.moveTop"));
    QPushButton* btnUp = makeOrderBtn(trText("settings.stocks.moveUp"));
    QPushButton* btnDown = makeOrderBtn(trText("settings.stocks.moveDown"));
    QPushButton* btnBottom = makeOrderBtn(trText("settings.stocks.moveBottom"));

    orderBtnLayout->addStretch();
    orderBtnLayout->addWidget(btnTop);
    orderBtnLayout->addWidget(btnUp);
    orderBtnLayout->addWidget(btnDown);
    orderBtnLayout->addWidget(btnBottom);
    orderBtnLayout->addStretch();

    tableAreaLayout->addWidget(orderBtnCol);
    sectorLayout->addWidget(tableArea, 1);
    root->addWidget(sectorGroup, 1);

    m_sectorSearchNam = new QNetworkAccessManager(this);
    m_sectorSearchNam->setProxy(network_utils::proxyFromConfig(m_cfg));

    m_sectorSearchDebounce = new QTimer(this);
    m_sectorSearchDebounce->setSingleShot(true);
    m_sectorSearchDebounce->setInterval(400);

    auto currentTableRow = [this]() -> int {
        return m_sectorTable ? m_sectorTable->currentRow() : -1;
    };
    connect(btnTop, &QPushButton::clicked, this, [this, currentTableRow]() {
        const int newRow = static_cast<StockTableWidget*>(m_sectorTable)->moveRowTop(currentTableRow());
        m_sectorTable->setCurrentCell(newRow, 0);
    });
    connect(btnUp, &QPushButton::clicked, this, [this, currentTableRow]() {
        const int newRow = static_cast<StockTableWidget*>(m_sectorTable)->moveRowUp(currentTableRow());
        m_sectorTable->setCurrentCell(newRow, 0);
    });
    connect(btnDown, &QPushButton::clicked, this, [this, currentTableRow]() {
        const int newRow = static_cast<StockTableWidget*>(m_sectorTable)->moveRowDown(currentTableRow());
        m_sectorTable->setCurrentCell(newRow, 0);
    });
    connect(btnBottom, &QPushButton::clicked, this, [this, currentTableRow]() {
        const int newRow = static_cast<StockTableWidget*>(m_sectorTable)->moveRowBottom(currentTableRow());
        m_sectorTable->setCurrentCell(newRow, 0);
    });

    connect(m_sectorSearchEdit, &QLineEdit::textEdited, this, [this](const QString& rawText) {
        QString text = rawText;
        text.remove(QLatin1Char(' '));
        if (text != rawText) {
            m_sectorSearchEdit->setText(text);
        }

        if (text.length() < 2) {
            m_sectorSearchDebounce->stop();
            if (m_sectorSearchReply) {
                m_sectorSearchReply->abort();
                m_sectorSearchReply->deleteLater();
                m_sectorSearchReply = nullptr;
            }
            m_sectorSuggestList->clear();
            m_sectorSuggestList->hide();
            return;
        }

        m_sectorSearchDebounce->start();
    });

    connect(m_sectorSearchBtn, &QPushButton::clicked, this, [this]() {
        if (m_sectorSearchDebounce) {
            m_sectorSearchDebounce->stop();
        }
        doSectorSearch(true);
    });

    connect(m_sectorSearchDebounce, &QTimer::timeout, this, [this]() {
        doSectorSearch();
    });

    connect(m_sectorSuggestList, &QListWidget::itemClicked, this, [this](QListWidgetItem* listItem) {
        if (!listItem) {
            return;
        }

        const QString code = normalizeSectorCode(listItem->data(Qt::UserRole).toString());
        const QString name = listItem->data(Qt::UserRole + 1).toString().trimmed();
        if (code.isEmpty()) {
            return;
        }

        StockTableWidget* tbl = static_cast<StockTableWidget*>(m_sectorTable);
        if (tbl->containsCode(code)) {
            QMessageBox::information(
                this,
                trText("app.name"),
                trText("settings.indexSector.sectorDuplicate")
            );
        } else {
            tbl->addStockRow(code, name);
        }

        m_sectorSuggestList->clear();
        m_sectorSuggestList->hide();
        m_sectorSearchEdit->clear();
    });

    return w;
}

void SettingsDialog::parseSectorSuggestResult(const QByteArray& data) {
    if (!m_sectorSuggestList || !m_sectorSearchEdit || !m_sectorSearchBtn) {
        return;
    }

    if (data.isEmpty()) {
        m_sectorSuggestList->clear();
        m_sectorSuggestList->hide();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[SectorSearch] parse failed:" << parseError.errorString();
        m_sectorSuggestList->clear();
        m_sectorSuggestList->hide();
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonObject tableObj = root.value(QStringLiteral("QuotationCodeTable")).toObject();
    const QJsonArray items = tableObj.value(QStringLiteral("Data")).toArray();

    m_sectorSuggestList->clear();

    QSet<QString> addedCodes;
    for (const QJsonValue& value : items) {
        if (!value.isObject()) {
            continue;
        }

        static const QString kSectorSecurityType = QStringLiteral("9");

        const QJsonObject obj = value.toObject();
        if (obj.value(QStringLiteral("SecurityType")).toString() != kSectorSecurityType) {
            continue;
        }
        const QString code = normalizeSectorCode(obj.value(QStringLiteral("Code")).toString());
        const QString name = obj.value(QStringLiteral("Name")).toString().trimmed();
        if (code.isEmpty()) {
            continue;
        }

        const QString key = watchCodeKey(code);
        if (addedCodes.contains(key)) {
            continue;
        }
        addedCodes.insert(key);

        const QString displayText = name.isEmpty()
            ? code
            : (code + QStringLiteral(" - ") + name);
        QListWidgetItem* item = new QListWidgetItem(displayText, m_sectorSuggestList);
        item->setData(Qt::UserRole, code);
        item->setData(Qt::UserRole + 1, name);
    }

    if (m_sectorSuggestList->count() > 0) {
        const QPoint topLeft = m_sectorSearchEdit->mapTo(this,
            QPoint(0, m_sectorSearchEdit->height() + 2));
        const int w = m_sectorSearchEdit->width() + 6 + m_sectorSearchBtn->width();
        const int itemH = m_sectorSuggestList->sizeHintForRow(0);
        const int h = qMin(m_sectorSuggestList->count() * (itemH > 0 ? itemH : 22) + 6, 180);
        m_sectorSuggestList->setGeometry(topLeft.x(), topLeft.y(), w, h);
        m_sectorSuggestList->raise();
        m_sectorSuggestList->setVisible(true);
    } else {
        m_sectorSuggestList->setVisible(false);
    }
}

void SettingsDialog::doSectorSearch(bool forceSearch) {
    if (!m_sectorSearchEdit || !m_sectorSearchNam) {
        return;
    }

    const QString keyword = m_sectorSearchEdit->text().trimmed();
    if (!forceSearch && keyword.length() < 2) {
        return;
    }
    if (keyword.isEmpty()) {
        return;
    }

    if (m_sectorSearchReply) {
        m_sectorSearchReply->abort();
        m_sectorSearchReply->deleteLater();
        m_sectorSearchReply = nullptr;
    }

    static constexpr int kSectorSearchCount = 10;

    QUrl url(QStringLiteral("https://searchapi.eastmoney.com/api/suggest/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("input"), keyword);
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("14"));
    query.addQueryItem(QStringLiteral("count"), QString::number(kSectorSearchCount));
    url.setQuery(query);

    qInfo() << "[SectorSearch] GET" << url.toString();

    QNetworkRequest req(url);
    req.setHeader(
        QNetworkRequest::UserAgentHeader,
        network_utils::effectiveUserAgent(m_cfg)
    );
    req.setRawHeader("Referer", "https://quote.eastmoney.com");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        "eastmoney-sector-search",
        "GET",
        req,
        m_sectorSearchNam->proxy()
    );

    m_sectorSearchReply = m_sectorSearchNam->get(req);
    connect(m_sectorSearchReply, &QNetworkReply::finished, this, [this, trace]() {
        if (!m_sectorSearchReply) {
            return;
        }

        const QNetworkReply::NetworkError netErr = m_sectorSearchReply->error();
        const QByteArray data = m_sectorSearchReply->readAll();

        network_logger::logRequestFinish(trace, m_sectorSearchReply, data.size(), data);

        m_sectorSearchReply->deleteLater();
        m_sectorSearchReply = nullptr;

        if (netErr != QNetworkReply::NoError) {
            qWarning() << "[SectorSearch] network error" << netErr;
            m_sectorSuggestList->clear();
            m_sectorSuggestList->hide();
            return;
        }

        parseSectorSuggestResult(data);
    });
}

void SettingsDialog::parseSinaSearchResult(const QByteArray& data) {
    if (data.isEmpty()) {
        qInfo() << "[StockSearch] parseSinaSearchResult: empty data";
        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
        return;
    }

    // Sina suggest API returns GBK-encoded text; decode accordingly.
    QString response;
    auto decoder = QStringDecoder(QStringDecoder::System);
    // Try GB18030 first (superset of GBK/GB2312)
    auto gb18030 = QStringDecoder("GB18030");
    if (gb18030.isValid()) {
        response = gb18030.decode(data);
    } else {
        // Fallback: system encoding or UTF-8
        response = decoder.isValid() ? decoder.decode(data) : QString::fromUtf8(data);
    }

    qInfo() << "[StockSearch] parseSinaSearchResult response:" << response.left(300);

    QString content = response.trimmed();
    const QString prefix = QStringLiteral("var suggestvalue=\"");
    const QString suffix = QStringLiteral("\";");
    if (content.startsWith(prefix)) {
        content.remove(0, prefix.size());
    }
    if (content.endsWith(suffix)) {
        content.chop(suffix.size());
    }
    content = content.trimmed();

    if (content.isEmpty()) {
        qInfo() << "[StockSearch] Empty parsed content";
        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
        return;
    }

    qInfo() << "[StockSearch] Parsed content:" << content.left(300);

    const QStringList entries = content.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    const QString marketFilter = m_stockMarketCombo
        ? m_stockMarketCombo->currentData().toString().trimmed().toLower()
        : QStringLiteral("a");
    QSet<QString> addedCodes;

    m_stockSuggestList->clear();

    for (const QString& entry : entries) {
        const QStringList fields = entry.split(QLatin1Char(','));
        qInfo() << "[StockSearch] entry fields count:" << fields.size() << fields;
        if (fields.isEmpty()) {
            continue;
        }

        const QString rawCode = fields.at(0).trimmed();
        const QString name = (fields.size() > 4) ? fields.at(4).trimmed() : QString();
        const QStringList normalizedCodes = normalizeSinaCodesForMarket(rawCode, marketFilter);
        for (const QString& code : normalizedCodes) {
            if (addedCodes.contains(code)) {
                continue;
            }
            addedCodes.insert(code);

            const QString displayText = name.isEmpty()
                ? code
                : (code + QStringLiteral(" - ") + name);
            QListWidgetItem* item = new QListWidgetItem(displayText, m_stockSuggestList);
            item->setData(Qt::UserRole, code);
            item->setData(Qt::UserRole + 1, name);
        }
    }

    qInfo() << "[StockSearch] suggestion count:" << m_stockSuggestList->count();

    if (m_stockSuggestList->count() > 0) {
        // Position floating list below the search edit, overlaying the table
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
    qInfo() << "[StockSearch] doStockSearch called"
             << "edit:" << (m_stockSearchEdit ? "ok" : "null")
             << "nam:" << (m_stockSearchNam ? "ok" : "null");
    if (!m_stockSearchEdit || !m_stockSearchNam) {
        return;
    }

    const QString keyword = m_stockSearchEdit->text().trimmed();
    qInfo() << "[StockSearch] keyword:" << keyword << "length:" << keyword.length();
    if (!forceSearch && keyword.length() < 3) {
        return;
    }
    if (keyword.isEmpty()) {
        return;
    }

    // Cancel in-flight request
    if (m_stockSearchReply) {
        m_stockSearchReply->abort();
        m_stockSearchReply->deleteLater();
        m_stockSearchReply = nullptr;
    }

    const QUrl url(
        QStringLiteral("http://suggest3.sinajs.cn/suggest/type=2&key=")
        + QString::fromUtf8(QUrl::toPercentEncoding(keyword))
        + QStringLiteral("&name=suggestvalue")
    );

    qInfo() << "[StockSearch] GET" << url.toString();

    QNetworkRequest req(url);
    req.setHeader(
        QNetworkRequest::UserAgentHeader,
        network_utils::effectiveUserAgent(m_cfg)
    );
    req.setRawHeader("Referer", "https://finance.sina.com.cn");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        "sina-stock-search",
        "GET",
        req,
        m_stockSearchNam->proxy()
    );

    m_stockSearchReply = m_stockSearchNam->get(req);
    connect(m_stockSearchReply, &QNetworkReply::finished, this, [this, trace]() {
        if (!m_stockSearchReply) {
            return;
        }
        const int httpStatus = m_stockSearchReply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute
        ).toInt();
        const QNetworkReply::NetworkError netErr = m_stockSearchReply->error();
        const QByteArray data = m_stockSearchReply->readAll();

        network_logger::logRequestFinish(trace, m_stockSearchReply, data.size(), data);

        m_stockSearchReply->deleteLater();
        m_stockSearchReply = nullptr;

        if (netErr != QNetworkReply::NoError) {
            qWarning() << "[StockSearch] Network error" << netErr
                       << "HTTP status" << httpStatus;
        } else {
            qInfo() << "[StockSearch] HTTP" << httpStatus
                     << "body size" << data.size()
                     << "preview:" << data.left(200);
        }

        parseSinaSearchResult(data);
    });
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

    vbox->addStretch();
    return w;
}

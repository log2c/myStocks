#include "settings_dialog.h"

#include "app_logging.h"
#include "config_manager.h"
#include "i18n.h"
#include "network_utils.h"

#include <QAbstractItemView>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDropEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QStringConverter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

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

} // namespace

SettingsDialog::SettingsDialog(
    const AppConfig& cfg,
    const QVector<StockItem>& stocks,
    const QHash<QString, QString>& apiNamesByCode,
    const QString& dataYamlPath,
    std::function<void()> onWriteStockNames,
    QWidget* parent
)
    : QDialog(parent)
    , m_cfg(cfg)
    , m_stocks(stocks)
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
    form->addRow(trText("settings.general.apiSource"), m_sourceCombo);
    form->addRow(trText("settings.general.token"), m_tokenEdit);
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
    m_stockMarketCombo->addItem(trText("settings.stocks.market1"), QStringLiteral("1"));
    m_stockMarketCombo->addItem(trText("settings.stocks.market2"), QStringLiteral("2"));
    m_stockMarketCombo->addItem(trText("settings.stocks.market3"), QStringLiteral("3"));
    m_stockMarketCombo->addItem(trText("settings.stocks.market4"), QStringLiteral("4"));
    m_stockMarketCombo->addItem(trText("settings.stocks.market5"), QStringLiteral("5"));
    m_stockMarketCombo->addItem(trText("settings.stocks.market6"), QString());

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
            QMessageBox::warning(this, trText("app.name"), trText("settings.stocks.saveFail"));
            return;
        }
        const QVector<StockItem> stocks =
            static_cast<StockTableWidget*>(m_stockTable)->stocks();
        if (ConfigManager::saveStocksToYaml(m_dataYamlPath, stocks)) {
            QMessageBox::information(this, trText("app.name"), trText("settings.stocks.saveOk"));
        } else {
            QMessageBox::warning(this, trText("app.name"), trText("settings.stocks.saveFail"));
        }
    });

    // Reset button: reload from data.yaml and repopulate
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
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

        // Rebuild table
        StockTableWidget* tbl = static_cast<StockTableWidget*>(m_stockTable);
        tbl->setRowCount(0);
        for (const StockItem& s : loaded) {
            tbl->addStockRow(s.code, s.name);
        }
    });

    return w;
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

    // Response format: var suggestvalue="name,type,short,name,pinyin,fullcode,...|..."
    const int startQuote = response.indexOf(QLatin1Char('"'));
    const int endQuote = response.lastIndexOf(QLatin1Char('"'));
    if (startQuote < 0 || endQuote <= startQuote) {
        qInfo() << "[StockSearch] No quoted content found in response";
        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
        return;
    }

    const QString content = response.mid(startQuote + 1, endQuote - startQuote - 1).trimmed();
    if (content.isEmpty()) {
        qInfo() << "[StockSearch] Quoted content is empty";
        m_stockSuggestList->clear();
        m_stockSuggestList->hide();
        return;
    }

    qInfo() << "[StockSearch] Parsed content:" << content.left(300);

    const QStringList entries = content.split(QLatin1Char(';'), Qt::SkipEmptyParts);

    // When market type is empty (ETF mode), only keep type=203 entries
    const bool etfModeOnly = m_stockMarketCombo->currentData().toString().isEmpty();

    m_stockSuggestList->clear();

    for (const QString& entry : entries) {
        const QStringList fields = entry.split(QLatin1Char(','));
        qInfo() << "[StockSearch] entry fields count:" << fields.size() << fields;
        if (fields.size() < 4) {
            continue;
        }
        const QString typeStr = fields.at(1).trimmed();

        // ETF (type=203): name,type,shortcode,fullcode,...  → [0]=name, [3]=fullcode
        // Others (type=1/2/3/4/5): fullcode,type,shortcode,fullcode,name,...  → [0]=fullcode, [4]=name
        QString fullCode, name;
        if (typeStr == QLatin1String("203")) {
            fullCode = fields.at(3).trimmed();
            name     = fields.at(0).trimmed();
        } else {
            if (etfModeOnly) {
                continue;  // ETF mode: skip non-ETF entries
            }
            if (fields.size() < 5) {
                continue;
            }
            fullCode = fields.at(0).trimmed();
            name     = fields.at(4).trimmed();
        }

        if (fullCode.isEmpty()) {
            continue;
        }

        const QString displayText = fullCode + QStringLiteral(" - ") + name;
        QListWidgetItem* item = new QListWidgetItem(displayText, m_stockSuggestList);
        item->setData(Qt::UserRole, fullCode);
        item->setData(Qt::UserRole + 1, name);
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

    const QString marketType = m_stockMarketCombo->currentData().toString();
    const QUrl url(
        QStringLiteral("http://suggest3.sinajs.cn/suggest/type=")
        + marketType
        + QStringLiteral("&key=")
        + QString::fromUtf8(QUrl::toPercentEncoding(keyword))
        + QStringLiteral("&name=myStocksApp")
    );

    qInfo() << "[StockSearch] GET" << url.toString();

    QNetworkRequest req(url);
    req.setHeader(
        QNetworkRequest::UserAgentHeader,
        network_utils::effectiveUserAgent(m_cfg)
    );
    req.setRawHeader("Referer", "https://finance.sina.com.cn");

    m_stockSearchReply = m_stockSearchNam->get(req);
    connect(m_stockSearchReply, &QNetworkReply::finished, this, [this]() {
        if (!m_stockSearchReply) {
            return;
        }
        const int httpStatus = m_stockSearchReply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute
        ).toInt();
        const QNetworkReply::NetworkError netErr = m_stockSearchReply->error();
        const QByteArray data = m_stockSearchReply->readAll();
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

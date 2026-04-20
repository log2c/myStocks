#include "settings_dialog.h"

#include "app_logging.h"
#include "i18n.h"

#include <QAbstractItemView>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

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

} // namespace

SettingsDialog::SettingsDialog(const AppConfig& cfg, QWidget* parent)
    : QDialog(parent)
    , m_cfg(cfg)
    , m_uiLanguage(i18n::resolveLanguage(cfg.language)) {
    setWindowTitle(trText("settings.title"));
    resize(560, 440);

    QVBoxLayout* root = new QVBoxLayout(this);
    QTabWidget* tabs = new QTabWidget(this);
    tabs->addTab(buildGeneralTab(), trText("settings.tab.general"));
    tabs->addTab(buildNetworkTab(), trText("settings.tab.network"));
    tabs->addTab(buildDisplayTab(), trText("settings.tab.display"));
    tabs->addTab(buildOtherTab(), trText("settings.tab.other"));
    root->addWidget(tabs);

    QDialogButtonBox* box = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this
    );
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(box);
}

AppConfig SettingsDialog::config() const {
    AppConfig out = m_cfg;

    out.pollMs = m_pollSpin->value();
    out.opacity = m_opacitySpin->value();
    out.hotkey = m_hotkeyEdit->text().trimmed();
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

    m_opacitySpin = new QDoubleSpinBox(w);
    m_opacitySpin->setRange(0.2, 1.0);
    m_opacitySpin->setSingleStep(0.05);
    m_opacitySpin->setDecimals(2);
    m_opacitySpin->setValue(m_cfg.opacity);

    m_hotkeyEdit = new QLineEdit(m_cfg.hotkey, w);
    m_hotkeyEdit->setPlaceholderText(trText("settings.general.hotkeyHint"));

    m_sourceCombo = new QComboBox(w);
    m_sourceCombo->addItem(trText("settings.source.mock"), "mock");
    m_sourceCombo->addItem(trText("settings.source.xtick"), "xtick");
    m_sourceCombo->addItem(trText("settings.source.sina"), "sina");
    m_sourceCombo->addItem(trText("settings.source.tencent"), "tencent");
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

    connect(m_transparentOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        m_transparentOpacityLabel->setText(
            trText("settings.general.transparentOpacityValue").arg(value)
        );
    });

    connect(m_transparentBackgroundCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        m_transparentOpacitySlider->setEnabled(enabled);
        m_transparentOpacityLabel->setEnabled(enabled);
        m_bgBtn->setEnabled(!enabled);
        m_opacitySpin->setEnabled(!enabled);
    });

    m_transparentOpacityLabel->setText(
        trText("settings.general.transparentOpacityValue")
            .arg(m_transparentOpacitySlider->value())
    );
    const bool transparentModeEnabled = m_transparentBackgroundCheck->isChecked();
    m_transparentOpacitySlider->setEnabled(transparentModeEnabled);
    m_transparentOpacityLabel->setEnabled(transparentModeEnabled);
    m_bgBtn->setEnabled(!transparentModeEnabled);
    m_opacitySpin->setEnabled(!transparentModeEnabled);

    form->addRow(trText("settings.general.poll"), m_pollSpin);
    form->addRow(trText("settings.general.opacity"), m_opacitySpin);
    form->addRow(trText("settings.general.hotkey"), m_hotkeyEdit);
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

    m_showHeaderCheck = new QCheckBox(trText("settings.display.showHeader"), w);
    m_showHeaderCheck->setChecked(m_cfg.showHeader);
    form->addRow(m_showHeaderCheck);

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

    connect(m_logEnabledCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        m_logLevelCombo->setEnabled(enabled);
    });

    m_logLevelCombo->setEnabled(m_logEnabledCheck->isChecked());

    form->addRow(m_debugIgnoreTradingTimeCheck);
    form->addRow(m_logEnabledCheck);
    form->addRow(trText("settings.other.logLevel"), m_logLevelCombo);
    form->addRow(m_openLogDirButton);

    return w;
}

#pragma once

#include "types.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVector>

class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(const AppConfig& cfg, QWidget* parent = nullptr);

    AppConfig config() const;

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
    QWidget* buildDisplayTab();

private:
    AppConfig m_cfg;
    QString m_uiLanguage;

    QSpinBox* m_pollSpin = nullptr;
    QDoubleSpinBox* m_opacitySpin = nullptr;
    QLineEdit* m_hotkeyEdit = nullptr;
    QComboBox* m_sourceCombo = nullptr;
    QLineEdit* m_tokenEdit = nullptr;
    QLineEdit* m_userAgentEdit = nullptr;
    QComboBox* m_proxyTypeCombo = nullptr;
    QLineEdit* m_proxyHostEdit = nullptr;
    QSpinBox* m_proxyPortSpin = nullptr;
    QLineEdit* m_proxyUserEdit = nullptr;
    QCheckBox* m_debugIgnoreTradingTimeCheck = nullptr;
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
    QListWidget* m_columnList = nullptr;
    QVector<QSpinBox*> m_columnMaxWidthSpins;
};

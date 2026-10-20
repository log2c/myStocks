#include "startup_splash.h"
#include "i18n.h"

#include <QGuiApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

StartupSplash::StartupSplash(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
        | Qt::WindowDoesNotAcceptFocus) {
    setObjectName(QStringLiteral("startupSplash"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_StyledBackground);
    // Reduce all visual dimensions to two thirds of the original splash.
    setFixedWidth(qRound(380 * 2.0 / 3.0));
    QFont compactFont = font();
    if (compactFont.pixelSize() > 0) compactFont.setPixelSize(qMax(1, qRound(compactFont.pixelSize() * 2.0 / 3.0)));
    else compactFont.setPointSizeF(compactFont.pointSizeF() * 2.0 / 3.0);
    setFont(compactFont);
    setStyleSheet(QStringLiteral(
        "#startupSplash { background: #16202d; border: 1px solid #42556e; border-radius: 8px; }"
        "QLabel { color: #edf3fc; background: transparent; border: none; }"
        "QLabel#splashDetail, QLabel#splashRoute { color: #b5c5d9; font-size: 8px; }"
        "QLabel#splashWarning { color: #ffd185; font-size: 8px; }"
        "QProgressBar { border: none; background: #2a3a4f; border-radius: 2px; height: 4px; }"
        "QProgressBar::chunk { background: #72b8ff; border-radius: 2px; }"
        "QPushButton { color: #edf3fc; background: #2b405a; border: 1px solid #516984;"
        " border-radius: 3px; padding: 4px 8px; }"
        "QPushButton:hover { background: #3a5676; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(15, 13, 15, 13);
    root->setSpacing(8);
    auto* brand = new QLabel(QStringLiteral("MyStocks  /  通达信"), this);
    brand->setStyleSheet(QStringLiteral("font-size: 8px; color: #83bdff; font-weight: 600;"));
    root->addWidget(brand);
    m_heading = new QLabel(this);
    m_heading->setWordWrap(true);
    m_heading->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600;"));
    root->addWidget(m_heading);
    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("splashDetail"));
    m_detail->setWordWrap(true);
    root->addWidget(m_detail);
    for (auto*& label : m_routes) {
        label = new QLabel(this);
        label->setObjectName(QStringLiteral("splashRoute"));
        label->setWordWrap(true);
        root->addWidget(label);
    }
    m_warning = new QLabel(this);
    m_warning->setObjectName(QStringLiteral("splashWarning"));
    m_warning->setWordWrap(true);
    m_warning->hide();
    root->addWidget(m_warning);
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(false);
    m_progress->setRange(0, 0);
    root->addWidget(m_progress);
    m_actions = new QWidget(this);
    auto* buttons = new QHBoxLayout(m_actions);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(4);
    m_retry = new QPushButton(m_actions);
    m_offline = new QPushButton(m_actions);
    buttons->addWidget(m_retry);
    buttons->addWidget(m_offline);
    root->addWidget(m_actions);
    m_actions->hide();
    connect(m_retry, &QPushButton::clicked, this, &StartupSplash::retryRequested);
    connect(m_offline, &QPushButton::clicked, this, &StartupSplash::offlineRequested);
    trackPrimaryScreen();
    connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, [this]() {
        trackPrimaryScreen();
        updatePlacement();
    });
}

void StartupSplash::setLanguage(const QString& language) {
    m_retry->setText(i18n::t("splash.retry", language));
    m_offline->setText(i18n::t("splash.offline", language));
}

QRect StartupSplash::placement(const QSize& size, const QRect& available) {
    constexpr int margin = 16;
    const QSize bounded(qMin(size.width(), available.width()), qMin(size.height(), available.height()));
    QRect rect(QPoint(), bounded);
    rect.moveBottomRight(available.bottomRight() - QPoint(margin, margin));
    rect.moveLeft(qBound(available.left(), rect.left(), available.right() - rect.width() + 1));
    rect.moveTop(qBound(available.top(), rect.top(), available.bottom() - rect.height() + 1));
    return rect;
}

void StartupSplash::showFor(QWidget* floatingWindow) {
    Q_UNUSED(floatingWindow);
    updatePlacement();
    show();
    raise();
}

void StartupSplash::trackPrimaryScreen() {
    disconnect(m_screenGeometryConnection);
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        m_screenGeometryConnection = connect(screen, &QScreen::availableGeometryChanged,
            this, [this]() { updatePlacement(); });
    }
}

void StartupSplash::updatePlacement() {
    adjustSize();
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        move(placement(size(), screen->availableGeometry()).topLeft());
    }
}

void StartupSplash::setStatus(const QString& title, const QString& detail, int progress) {
    m_heading->setText(title);
    m_detail->setText(detail);
    m_actions->hide();
    m_progress->setVisible(true);
    m_progress->setRange(0, progress < 0 ? 0 : 100);
    if (progress >= 0) m_progress->setValue(progress);
    updatePlacement();
}

void StartupSplash::setRouteStatus(bool extended, const QString& text) {
    m_routes[extended ? 1 : 0]->setText(text);
    updatePlacement();
}

void StartupSplash::setWarning(const QString& text) {
    m_warning->setText(text);
    m_warning->setVisible(!text.isEmpty());
    updatePlacement();
}

void StartupSplash::setFailed(const QString& detail) {
    m_detail->setText(detail);
    m_progress->hide();
    m_actions->show();
    updatePlacement();
}


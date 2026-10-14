#include "startup_splash.h"
#include "i18n.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <QWindow>

StartupSplash::StartupSplash(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
        | Qt::WindowDoesNotAcceptFocus) {
    setObjectName(QStringLiteral("startupSplash"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_StyledBackground);
    setFixedWidth(380);
    setStyleSheet(QStringLiteral(
        "#startupSplash { background: #16202d; border: 1px solid #42556e; border-radius: 12px; }"
        "QLabel { color: #edf3fc; background: transparent; border: none; }"
        "QLabel#splashDetail, QLabel#splashRoute { color: #b5c5d9; font-size: 12px; }"
        "QLabel#splashWarning { color: #ffd185; font-size: 12px; }"
        "QProgressBar { border: none; background: #2a3a4f; border-radius: 3px; height: 6px; }"
        "QProgressBar::chunk { background: #72b8ff; border-radius: 3px; }"
        "QPushButton { color: #edf3fc; background: #2b405a; border: 1px solid #516984;"
        " border-radius: 5px; padding: 6px 12px; }"
        "QPushButton:hover { background: #3a5676; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 20);
    root->setSpacing(12);
    auto* brand = new QLabel(QStringLiteral("MyStocks  /  通达信"), this);
    brand->setStyleSheet(QStringLiteral("font-size: 12px; color: #83bdff; font-weight: 600;"));
    root->addWidget(brand);
    m_heading = new QLabel(this);
    m_heading->setWordWrap(true);
    m_heading->setStyleSheet(QStringLiteral("font-size: 19px; font-weight: 600;"));
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
    m_retry = new QPushButton(m_actions);
    m_offline = new QPushButton(m_actions);
    buttons->addWidget(m_retry);
    buttons->addWidget(m_offline);
    root->addWidget(m_actions);
    m_actions->hide();
    connect(m_retry, &QPushButton::clicked, this, &StartupSplash::retryRequested);
    connect(m_offline, &QPushButton::clicked, this, &StartupSplash::offlineRequested);
}

void StartupSplash::setLanguage(const QString& language) {
    m_retry->setText(i18n::t("splash.retry", language));
    m_offline->setText(i18n::t("splash.offline", language));
}

QRect StartupSplash::placement(const QSize& size, const QRect& available, const QRect& anchor) {
    constexpr int margin = 16;
    const QSize bounded(qMin(size.width(), available.width()), qMin(size.height(), available.height()));
    QRect rect(QPoint(), bounded);
    if (anchor.isValid()) rect.moveCenter(anchor.center());
    else rect.moveBottomRight(available.bottomRight() - QPoint(margin, margin));
    rect.moveLeft(qBound(available.left(), rect.left(), available.right() - rect.width() + 1));
    rect.moveTop(qBound(available.top(), rect.top(), available.bottom() - rect.height() + 1));
    return rect;
}

void StartupSplash::showFor(QWidget* floatingWindow) {
    if (m_floatingWindow) m_floatingWindow->removeEventFilter(this);
    m_floatingWindow = floatingWindow;
    if (m_floatingWindow) m_floatingWindow->installEventFilter(this);
    updatePlacement();
    show();
    if (windowHandle()) {
        windowHandle()->setTransientParent(m_floatingWindow && m_floatingWindow->isVisible()
            ? m_floatingWindow->windowHandle() : nullptr);
    }
    raise();
}

void StartupSplash::updatePlacement() {
    adjustSize();
    const QRect anchor = m_floatingWindow && m_floatingWindow->isVisible()
        ? m_floatingWindow->frameGeometry() : QRect();
    QScreen* screen = anchor.isValid() ? QGuiApplication::screenAt(anchor.center()) : QGuiApplication::primaryScreen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) move(placement(size(), screen->availableGeometry(), anchor).topLeft());
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

bool StartupSplash::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_floatingWindow && isVisible()
        && (event->type() == QEvent::Move || event->type() == QEvent::Resize
            || event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
        updatePlacement();
        raise();
    }
    return QWidget::eventFilter(watched, event);
}


#pragma once

#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;

class StartupSplash : public QWidget {
    Q_OBJECT
public:
    explicit StartupSplash(QWidget* parent = nullptr);
    void setLanguage(const QString& language);
    void showFor(QWidget* floatingWindow);
    void setStatus(const QString& title, const QString& detail, int progress = -1);
    void setRouteStatus(bool extended, const QString& text);
    void setWarning(const QString& text);
    void setFailed(const QString& detail);
    static QRect placement(const QSize& size, const QRect& available);

signals:
    void retryRequested();
    void offlineRequested();

private:
    void updatePlacement();
    void trackPrimaryScreen();
    QMetaObject::Connection m_screenGeometryConnection;
    QLabel* m_heading = nullptr;
    QLabel* m_detail = nullptr;
    QLabel* m_routes[2] = {};
    QLabel* m_warning = nullptr;
    QProgressBar* m_progress = nullptr;
    QWidget* m_actions = nullptr;
    QPushButton* m_retry = nullptr;
    QPushButton* m_offline = nullptr;
};

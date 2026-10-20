#include "right_click_close.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <algorithm>

namespace {

struct Registration {
    QPointer<QWidget> window;
    std::function<bool()> enabled;
    std::function<void()> close;
};

// Closes registered top-level windows when the right button is clicked inside
// them. A press and release must land on the same window, and a right-button
// drag (window move, chart pan) is ignored so it cannot close anything.
class RightClickCloseFilter : public QObject {
public:
    static RightClickCloseFilter* instance() {
        static RightClickCloseFilter* filter = nullptr;
        if (!filter) {
            filter = new RightClickCloseFilter();
            qApp->installEventFilter(filter);
        }
        return filter;
    }

    void add(QWidget* window, std::function<bool()> enabled, std::function<void()> close) {
        if (!window) {
            return;
        }
        for (Registration& entry : m_windows) {
            if (entry.window == window) {
                entry.enabled = std::move(enabled);
                entry.close = std::move(close);
                return;
            }
        }
        m_windows.push_back({window, std::move(enabled), std::move(close)});
        QObject::connect(window, &QObject::destroyed, this, [this, window]() {
            m_windows.erase(
                std::remove_if(m_windows.begin(), m_windows.end(),
                    [window](const Registration& entry) {
                        return entry.window.isNull() || entry.window == window;
                    }),
                m_windows.end()
            );
        });
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (!event) {
            return false;
        }

        const QEvent::Type type = event->type();
        if (type != QEvent::MouseButtonPress
            && type != QEvent::MouseMove
            && type != QEvent::MouseButtonRelease) {
            return false;
        }

        auto* mouse = static_cast<QMouseEvent*>(event);
        if (type == QEvent::MouseMove) {
            if (m_pressWindow && (mouse->buttons() & Qt::RightButton)
                && (mouse->globalPosition().toPoint() - m_pressGlobal).manhattanLength()
                    >= QApplication::startDragDistance()) {
                m_pressWindow = nullptr;
            }
            return false;
        }

        if (mouse->button() != Qt::RightButton) {
            return false;
        }

        // The receiving widget identifies the window: a press on a table
        // viewport and its release on the window itself are the same window.
        QWidget* target = windowFor(watched);
        if (type == QEvent::MouseButtonPress) {
            m_pressGlobal = mouse->globalPosition().toPoint();
            m_pressWindow = target;
            return false;
        }

        const QPointer<QWidget> pressed = m_pressWindow;
        m_pressWindow = nullptr;
        if (!target || pressed != target) {
            return false;
        }
        for (const Registration& entry : m_windows) {
            if (entry.window != pressed) {
                continue;
            }
            if (entry.enabled && !entry.enabled()) {
                return false;
            }
            if (entry.close) {
                entry.close();
            }
            break;
        }
        return false;
    }

private:
    QWidget* windowFor(QObject* receiver) const {
        QWidget* widget = qobject_cast<QWidget*>(receiver);
        QWidget* top = widget ? widget->window() : nullptr;
        for (const Registration& entry : m_windows) {
            if (entry.window == top) {
                return top;
            }
        }
        return nullptr;
    }

    QVector<Registration> m_windows;
    QPointer<QWidget> m_pressWindow;
    QPoint m_pressGlobal;
};

} // namespace

namespace right_click_close {

void registerWindow(
    QWidget* window,
    std::function<bool()> enabled,
    std::function<void()> close
) {
    RightClickCloseFilter::instance()->add(window, std::move(enabled), std::move(close));
}

} // namespace right_click_close

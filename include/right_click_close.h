#pragma once

#include <functional>

class QWidget;

namespace right_click_close {

// Registers a top-level window for the global right-click close shortcut.
// `enabled` is polled on every release, so a settings change applies without
// re-registering. `close` performs the window's own dismissal (hidePopup() and
// friends) instead of a bare hide.
void registerWindow(
    QWidget* window,
    std::function<bool()> enabled,
    std::function<void()> close
);

} // namespace right_click_close

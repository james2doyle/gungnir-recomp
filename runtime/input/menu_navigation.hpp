#pragma once

// Menu-overlay navigation from a gamepad (T11f). Pure state: SDL button edges
// are translated to actions, the UI thread drains them, and the selection is
// clamped at both ends. No SDL, Java or Android dependency, so the contracts
// run on the host.

#include <deque>
#include <mutex>
#include <optional>

namespace gungnir::input {

// Values are packed into the JNI poll result, so keep them stable.
enum class MenuAction {
    None = 0,
    Toggle = 1,
    Move = 2,
    Activate = 3,
};

// Back/Select is deliberately absent: it stays the guest's Select button.
enum class MenuButton {
    Guide,
    Up,
    Down,
    South,
};

struct MenuEvent {
    MenuAction action{MenuAction::None};
    int index{};

    bool operator==(const MenuEvent &) const = default;
};

class MenuNavigation {
public:
    // A UI that stops draining must not grow the queue without bound; the
    // oldest events are dropped because navigation is transient.
    static constexpr int kMaxPending = 8;

    // The overlay opened with `rows` selectable rows. Selection starts at 0.
    void begin(int rows);
    void end();

    [[nodiscard]] bool open() const;
    [[nodiscard]] int rows() const;
    [[nodiscard]] int index() const;

    // Called from the input thread on a button-down edge.
    void press(MenuButton button);

    // Called from the UI thread; returns and clears the oldest pending event.
    std::optional<MenuEvent> take();

private:
    void enqueue(MenuAction action, int index);

    mutable std::mutex mutex_;
    bool open_{false};
    int rows_{0};
    int index_{0};
    std::deque<MenuEvent> pending_;
};

MenuNavigation &menu_navigation();

} // namespace gungnir::input

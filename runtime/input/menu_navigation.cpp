#include "input/menu_navigation.hpp"

#include <algorithm>

namespace gungnir::input {

void MenuNavigation::begin(int rows) {
    std::lock_guard lock(mutex_);
    open_ = true;
    rows_ = std::max(rows, 1);
    index_ = 0;
    pending_.clear();
}

void MenuNavigation::end() {
    std::lock_guard lock(mutex_);
    open_ = false;
    rows_ = 0;
    index_ = 0;
    pending_.clear();
}

bool MenuNavigation::open() const {
    std::lock_guard lock(mutex_);
    return open_;
}

int MenuNavigation::rows() const {
    std::lock_guard lock(mutex_);
    return rows_;
}

int MenuNavigation::index() const {
    std::lock_guard lock(mutex_);
    return index_;
}

void MenuNavigation::press(MenuButton button) {
    std::lock_guard lock(mutex_);
    switch (button) {
    case MenuButton::Guide:
        // Toggle is valid in both states: the UI decides open vs close.
        enqueue(MenuAction::Toggle, index_);
        break;
    case MenuButton::Up:
        if (!open_) return;
        index_ = std::max(index_ - 1, 0);
        enqueue(MenuAction::Move, index_);
        break;
    case MenuButton::Down:
        if (!open_) return;
        index_ = std::min(index_ + 1, rows_ - 1);
        enqueue(MenuAction::Move, index_);
        break;
    case MenuButton::South:
        if (!open_) return;
        enqueue(MenuAction::Activate, index_);
        break;
    }
}

std::optional<MenuEvent> MenuNavigation::take() {
    std::lock_guard lock(mutex_);
    if (pending_.empty()) return std::nullopt;
    const MenuEvent event = pending_.front();
    pending_.pop_front();
    return event;
}

void MenuNavigation::enqueue(MenuAction action, int index) {
    if (static_cast<int>(pending_.size()) >= kMaxPending) pending_.pop_front();
    pending_.push_back(MenuEvent{action, index});
}

MenuNavigation &menu_navigation() {
    static MenuNavigation navigation;
    return navigation;
}

} // namespace gungnir::input

// T11f menu-navigation contracts: gamepad button edges drive the settings
// overlay selection, with a clamp at both ends, an open/closed gate, and a
// bounded pending queue drained by the UI thread.
//
// Usage: menu_navigation_tests
// Exit 0 on success, 1 on a failed contract.

#include "input/menu_navigation.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

using gungnir::input::MenuAction;
using gungnir::input::MenuButton;
using gungnir::input::MenuEvent;
using gungnir::input::MenuNavigation;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

MenuEvent next(MenuNavigation &menu, const char *contract) {
    const std::optional<MenuEvent> event = menu.take();
    check(event.has_value(), contract);
    return *event;
}

void closed_gate() {
    MenuNavigation menu;
    menu.end();
    check(!menu.open(), "a fresh navigation is closed");

    menu.press(MenuButton::Up);
    menu.press(MenuButton::Down);
    menu.press(MenuButton::South);
    check(!menu.take().has_value(), "movement and activation are ignored while closed");

    menu.press(MenuButton::Back);
    const MenuEvent back = next(menu, "Back opens from the closed state");
    check(back.action == MenuAction::Toggle, "Back reports a toggle while closed");

    menu.press(MenuButton::Guide);
    const MenuEvent guide = next(menu, "Guide opens from the closed state");
    check(guide.action == MenuAction::Toggle, "Guide reports a toggle while closed");
}

void moves_with_clamp() {
    MenuNavigation menu;
    menu.begin(3);
    check(menu.open(), "begin opens the overlay");
    check(menu.rows() == 3, "begin records the row count");
    check(menu.index() == 0, "begin selects the first row");

    menu.press(MenuButton::Down);
    check(menu.index() == 1, "Down advances one row");
    MenuEvent event = next(menu, "Down reports a move");
    check(event.action == MenuAction::Move, "Down reports Move");
    check(event.index == 1, "the move carries the new index");

    menu.press(MenuButton::Down);
    menu.press(MenuButton::Down);
    check(menu.index() == 2, "Down clamps at the last row");
    (void)next(menu, "clamped Down is still consumed");
    (void)next(menu, "clamped Down is still consumed (second)");

    menu.press(MenuButton::Up);
    check(menu.index() == 1, "Up moves back one row");
    (void)next(menu, "Up reports a move");

    menu.press(MenuButton::Up);
    menu.press(MenuButton::Up);
    check(menu.index() == 0, "Up clamps at the first row");
    (void)next(menu, "clamped Up is consumed");
    (void)next(menu, "clamped Up is consumed (second)");
}

void activate_uses_selection() {
    MenuNavigation menu;
    menu.begin(4);
    menu.press(MenuButton::Down);
    menu.press(MenuButton::Down);
    (void)next(menu, "first Down");
    (void)next(menu, "second Down");

    menu.press(MenuButton::South);
    const MenuEvent event = next(menu, "South reports an activation");
    check(event.action == MenuAction::Activate, "South reports Activate");
    check(event.index == 2, "activation uses the highlighted row");
    check(menu.index() == 2, "activation does not move the highlight");
}

void toggle_while_open() {
    MenuNavigation menu;
    menu.begin(2);
    menu.press(MenuButton::Back);
    const MenuEvent back = next(menu, "Back closes from the open state");
    check(back.action == MenuAction::Toggle, "Back reports a toggle while open");

    menu.press(MenuButton::Guide);
    const MenuEvent guide = next(menu, "Guide closes from the open state");
    check(guide.action == MenuAction::Toggle, "Guide reports a toggle while open");
}

void begin_and_end_reset() {
    MenuNavigation menu;
    menu.begin(9);
    menu.press(MenuButton::Down);
    (void)next(menu, "Down before reset");
    menu.end();
    check(!menu.open(), "end closes the navigation");
    check(!menu.take().has_value(), "end clears pending actions");

    menu.begin(9);
    check(menu.index() == 0, "begin resets the selection to the first row");

    menu.begin(0);
    check(menu.rows() == 1, "an empty row count keeps one selectable row");
    menu.press(MenuButton::Down);
    check(menu.index() == 0, "a single row cannot move");
}

void pending_queue_is_bounded() {
    MenuNavigation menu;
    menu.begin(20);
    for (int i = 0; i < 20; ++i) menu.press(MenuButton::Down);

    int count = 0;
    int last = -1;
    while (const std::optional<MenuEvent> event = menu.take()) {
        ++count;
        last = event->index;
    }
    check(count == MenuNavigation::kMaxPending, "the pending queue is bounded");
    check(last == menu.index(), "the newest event survives the bound");
    check(menu.index() == 19, "all presses moved the selection");
}

} // namespace

int main() {
    try {
        closed_gate();
        moves_with_clamp();
        activate_uses_selection();
        toggle_while_open();
        begin_and_end_reset();
        pending_queue_is_bounded();
    } catch (const std::exception &error) {
        std::cerr << "menu_navigation_tests: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "menu_navigation_tests: OK\n";
    return EXIT_SUCCESS;
}

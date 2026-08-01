/**
 * @file nav.h
 *
 * Keyboard-navigation helpers for the terminal UI.
 */
#ifndef BUTCHER_TUI_NAV_H
#define BUTCHER_TUI_NAV_H

#include "ftxui/component/component.hpp"

/**
 * Pass every event to @p child except the ones an enclosing container needs
 * in order to move between fields.
 *
 * FTXUI's Input handles ArrowUp and ArrowDown itself, moving the caret within
 * the text. Inside a vertical container that traps focus: pressing Down in a
 * name field slides the caret to the end of the name instead of moving to the
 * next field. This wrapper declines those two keys so the container sees them.
 */
ftxui::Component NavEscape(ftxui::Component child);

#endif /* BUTCHER_TUI_NAV_H */

#pragma once

// The "Controls" pages of the in-game options menu: Overview, Gamepad, Keyboard & Mouse, Aiming. Rows are built
// into the game-styled list from game_ui.h; state lives in aim_settings.h.

#include "game_ui.h"

namespace controls_menu {

inline constexpr int kPageCount = 4;
const char* PageName(int page);

// Builds the rows of one page (0 Overview, 1 Gamepad, 2 Keyboard & Mouse, 3 Aiming).
void BuildPage(game_ui::ListView& view, int page);
// Devices page row: the active device and scheme in effect (the scheme itself is set on the Overview page).
void BuildSchemeRow(game_ui::ListView& view);

// Press-to-bind dialog. Call once per frame after the menu has been drawn.
void DrawModals(game_ui::Frame& input);

// Brief toast when the Automatic scheme switches (gamepad <-> mouse/keyboard). Call once per frame.
void DrawHud();


// True while a press-to-bind capture is running (the overlay must not treat Esc/Back as menu keys).
bool Capturing();

// Restores one page / every controls option (aiming, mouse, scheme, bindings) to defaults and saves.
void ResetPage(int page);
void ResetAll();

} // namespace controls_menu

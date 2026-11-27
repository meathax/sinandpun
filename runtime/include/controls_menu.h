#pragma once

// The "Controls" pages of the in-game options menu: Gamepad (sticks), Gyro, Buttons, Mouse. Rows are built
// into the game-styled list from game_ui.h; state lives in aim_settings.h.

#include "game_ui.h"

namespace controls_menu {

inline constexpr int kPageCount = 4;
const char* PageName(int page);

// Builds the rows of one page (0 Gamepad, 1 Gyro, 2 Buttons, 3 Mouse).
void BuildPage(game_ui::ListView& view, int page);

// Press-to-bind dialog. Call once per frame after the menu has been drawn.
void DrawModals(game_ui::Frame& input);



// True while a press-to-bind capture is running (the overlay must not treat Esc/Back as menu keys).
bool Capturing();

// Restores one page / every controls option (aiming, mouse, scheme, bindings) to defaults and saves.
void ResetPage(int page);
void ResetAll();

} // namespace controls_menu

#pragma once

// The "Controls" pages of the in-game options menu: Aiming, Mapping, Mouse & Keyboard. Rows are built into the
// game-styled list from game_ui.h; state lives in aim_settings.h.

#include "game_ui.h"

namespace controls_menu {

inline constexpr int kPageCount = 3;
const char* PageName(int page);

// Builds the rows of one page (0 Aiming, 1 Mapping, 2 Mouse & Keyboard).
void BuildPage(game_ui::ListView& view, int page);
// "Controller scheme" row (Classic Controller / Wii Remote + Nunchuk pointer); lives on the Devices page.
void BuildSchemeRow(game_ui::ListView& view);

// Press-to-bind dialog. Call once per frame after the menu has been drawn.
void DrawModals(game_ui::Frame& input);

// True when a changed controls option only takes effect after a restart (the controller scheme).
bool RestartPending();

// True while a press-to-bind capture is running (the overlay must not treat Esc/Back as menu keys).
bool Capturing();

// Restores one page / every controls option (aiming, mouse, scheme, bindings) to defaults and saves.
void ResetPage(int page);
void ResetAll();

} // namespace controls_menu

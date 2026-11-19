#pragma once

// The "Controls" page set of the F10 settings menu: Aiming, Mapping, Mouse & Keyboard, Gamepad.
// Drawn inside settings_overlay.cpp's menu bar; state lives in aim_settings.h.

namespace controls_menu {

// Draws the tab bar. `drawGamepadPage` renders the existing device/GameCube-pad page.
void Draw(void (*drawGamepadPage)());

// True while a press-to-bind capture is running (the overlay must not treat Esc/F10 as menu keys).
bool Capturing();

// Two presses within three seconds; works with every input device. True on the confirming press.
bool ResetConfirmButton(const char* label, const char* id);

// Restores every controls option (aiming, mouse, scheme, bindings) to defaults and saves.
void ResetAll();

} // namespace controls_menu

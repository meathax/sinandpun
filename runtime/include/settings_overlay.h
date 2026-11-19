#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>

namespace settings_overlay {
// Apply persistent controller settings once Aurora has discovered host devices.
void InitializeRuntimeSettings() noexcept;
// Pass Aurora events to the options menu (called before each frame).
void HandleEvents(const AuroraEvent* events) noexcept;
// Draw the options menu, HUD overlays and dialogs before each Aurora present.
void Draw() noexcept;
bool StartupScreenVisible() noexcept;
void NotifyStrapInputAccepted() noexcept;
void AdvancePresentedFrame() noexcept;
// Put host controllers back to a neutral state before the process ends.
void ReleaseControllers() noexcept;

// ---- In-game hook API (runtime/src/hle/game_menu_hook.cpp) -----------------------------------------
// Opens the options menu in "game mode": no F10 hint, B on the root closes it, game input is blocked
// through InputBindings::InputBlocked() and, when a pause callback is registered, the game is paused.
// All three calls are thread safe; the request is applied on the next Draw() (the present thread).
void OpenFromGame() noexcept;
void CloseFromGame() noexcept;
// True from the moment the menu was opened with OpenFromGame() until its fade-out has finished, including
// when the player closed it with B. Poll this to learn that the menu has gone.
bool IsOpenFromGame() noexcept;
// Optional: called with true when a game-mode menu opens and with false when it has fully closed. Runs on the
// thread that calls Draw(). Pass nullptr to unregister. A menu opened with F10/F1/Back+Start never pauses.
void SetPauseCallback(void (*callback)(bool paused)) noexcept;
} // namespace settings_overlay

#pragma once

// SDL glue for the controls: tracks keyboard/mouse/wheel/focus from the event stream (main thread), samples
// them together with the gamepad for the virtual remote (any thread), manages the relative mouse mode, and
// implements press-to-bind capture for the settings menu.

#include "input_map.h"

#include <SDL3/SDL_events.h>

namespace AimInput {

// Installs SDL key names for binding (idempotent).
void Install();

void HandleSdlEvent(const SDL_Event& event);

// False while the window lacks focus: every key/button is treated as released.
bool Focused();

// Keyboard, mouse and wheel plus gamepad 0 (the same pad the game's PAD HLE reads).
InputMap::RawSample Sample();

struct MouseSample {
    float x = 0.f;
    float y = 0.f;
    float winW = 0.f;
    float winH = 0.f;
    bool valid = false;
};
// Latest cursor position (window coordinates) as of the call; the virtual remote reads this when the game
// polls, not on a timer.
MouseSample MouseNow();
// Accumulated motion since the last call.
void ConsumeMouseDelta(float& dx, float& dy);

// Main thread, once per frame: relative mouse mode follows the settings while the game is unblocked and focused.
void UpdateWindowState(bool inputBlocked);

enum class CaptureState { Idle, Pending, Done, Cancelled, Cleared };
struct CaptureResult {
    CaptureState state = CaptureState::Idle;
    InputMap::Binding binding;
};
void BeginCapture(InputMap::Profile profile);
// pointerOverUi: ignore mouse clicks that land on menu controls.
CaptureResult PollCapture(bool pointerOverUi);
void CancelCapture();
bool Capturing();

} // namespace AimInput

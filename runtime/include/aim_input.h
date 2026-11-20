#pragma once

// SDL glue for the controls: tracks keyboard/mouse/wheel/focus from the event stream (main thread), samples
// them together with the gamepad for the virtual remote (any thread), manages the relative mouse mode, and
// implements press-to-bind capture for the settings menu.

#include "dual_analog.h"
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
// The part of the window (window coordinates, same units as MouseSample) that shows the game picture: the real
// presented viewport from the renderer when known, else the configured aspect.
DualAnalog::Rect PictureRect(const MouseSample& ms);
// Accumulated motion since the last call.
void ConsumeMouseDelta(float& dx, float& dy);

// Main thread, once per frame: relative mouse mode follows the settings while the game is unblocked and focused.
// Also runs the control-scheme service: watches gamepad / mouse / keyboard activity, decides the scheme in effect
// (Automatic = the device used last, see InputMap::UpdateScheme) and pushes changes to the virtual remote.
void UpdateWindowState(bool inputBlocked);

// Scheme in effect and the device used last (updated by UpdateWindowState; readable from any thread).
InputMap::Scheme EffectiveScheme();
InputMap::ActiveDevice LastDevice();

// Latest automatic switch, for the on-screen note. `serial` grows by one per switch (0 = none yet).
struct SchemeNotice {
    uint32_t serial = 0;
    InputMap::Scheme scheme = InputMap::Scheme::Pointer;
    uint64_t atMs = 0;  // SDL_GetTicks() at the switch
};
SchemeNotice LatestSwitch();

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

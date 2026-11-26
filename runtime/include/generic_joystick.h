#pragma once

// Generic DirectInput / XInput / HIDAPI joystick manager (via SDL3 Joystick API).
// Supports multiple simultaneous joysticks (e.g. HOTAS flight stick + separate throttle)
// regardless of whether SDL has a standard gamepad mapping for them.

#include "input_map.h"

#include <SDL3/SDL_events.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace GenericJoystick {

struct DeviceInfo {
    int slot = 0;               // 0..InputMap::kMaxJoysticks - 1
    uint32_t instanceId = 0;
    std::string name;
    int numButtons = 0;
    int numAxes = 0;
    int numHats = 0;
    bool isGamepad = false;
};

// Call from the SDL event pump on the main thread.
void HandleSdlEvent(const SDL_Event& event);

// Ensures all currently connected SDL joysticks are opened and assigned to slots.
void RefreshDevices();

// Populates raw joystick state for all connected slots (thread-safe).
void SampleAll(std::array<InputMap::RawJoy, InputMap::kMaxJoysticks>& out);

// Snapshot of currently connected joystick devices for the Controls UI.
std::vector<DeviceInfo> ConnectedDevices();

// Press-to-bind capture for generic joysticks (tracks resting axis baselines so
// HOTAS throttles or pedals resting at -1/+1 do not immediately misfire).
void BeginCapture();
bool PollCapture(InputMap::Binding& outBinding);
void CancelCapture();

// True if at least one non-gamepad generic joystick (e.g. HOTAS / flight stick) is connected.
bool HasGenericDevice();

} // namespace GenericJoystick

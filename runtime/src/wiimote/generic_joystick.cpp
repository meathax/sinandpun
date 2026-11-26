#include "generic_joystick.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace GenericJoystick {
namespace {

constexpr float kAxisCaptureDelta = 0.45f;

struct Slot {
    bool occupied = false;
    SDL_JoystickID instanceId = 0;
    SDL_Joystick* joy = nullptr;
    bool ownsHandle = false;
    bool isGamepad = false;
    std::string name;
    int numButtons = 0;
    int numAxes = 0;
    int numHats = 0;
};

struct State {
    std::mutex mutex;
    std::array<Slot, InputMap::kMaxJoysticks> slots{};
    bool capturing = false;
    std::array<InputMap::RawJoy, InputMap::kMaxJoysticks> captureBaseline{};
};

State& G() {
    static State state;
    return state;
}

void CloseSlotLocked(Slot& slot) {
    if (slot.ownsHandle && slot.joy != nullptr) {
        SDL_CloseJoystick(slot.joy);
    }
    slot = Slot{};
}

// Returns a valid SDL_Joystick* for `slot`, re-validating borrowed gamepad handles so we
// never dereference a pointer that Aurora closed on SDL_EVENT_GAMEPAD_REMOVED.
SDL_Joystick* ResolveJoystickLocked(const Slot& slot) {
    if (!slot.occupied) return nullptr;
    if (slot.ownsHandle) return slot.joy;
    SDL_Gamepad* gp = SDL_GetGamepadFromID(slot.instanceId);
    return gp != nullptr ? SDL_GetGamepadJoystick(gp) : nullptr;
}

void PopulateMetadataLocked(Slot& slot, SDL_Joystick* joy) {
    const char* name = joy != nullptr ? SDL_GetJoystickName(joy) : nullptr;
    slot.name = (name != nullptr && *name != '\0') ? name : "Joystick";
    slot.numButtons = joy != nullptr ? std::max(0, SDL_GetNumJoystickButtons(joy)) : 0;
    slot.numAxes = joy != nullptr ? std::max(0, SDL_GetNumJoystickAxes(joy)) : 0;
    slot.numHats = joy != nullptr ? std::max(0, SDL_GetNumJoystickHats(joy)) : 0;
}

void SampleSlotLocked(const Slot& slot, InputMap::RawJoy& dst) {
    dst = InputMap::RawJoy{};
    SDL_Joystick* joy = ResolveJoystickLocked(slot);
    if (joy == nullptr) return;

    dst.connected = true;

    const int nButtons = std::clamp(SDL_GetNumJoystickButtons(joy), 0, static_cast<int>(InputMap::kMaxJoyButtons));
    for (int b = 0; b < nButtons; ++b) {
        dst.buttons[static_cast<size_t>(b)] = SDL_GetJoystickButton(joy, b);
    }

    const int nAxes = std::clamp(SDL_GetNumJoystickAxes(joy), 0, static_cast<int>(InputMap::kMaxJoyAxes));
    for (int a = 0; a < nAxes; ++a) {
        const Sint16 raw = SDL_GetJoystickAxis(joy, a);
        dst.axes[static_cast<size_t>(a)] =
            raw < 0 ? static_cast<float>(raw) / 32768.0f : static_cast<float>(raw) / 32767.0f;
    }

    const int nHats = std::clamp(SDL_GetNumJoystickHats(joy), 0, static_cast<int>(InputMap::kMaxJoyHats));
    for (int h = 0; h < nHats; ++h) {
        const Uint8 hat = SDL_GetJoystickHat(joy, h);
        uint8_t mask = 0;
        if (hat & SDL_HAT_UP)    mask |= (1u << 0);
        if (hat & SDL_HAT_RIGHT) mask |= (1u << 1);
        if (hat & SDL_HAT_DOWN)  mask |= (1u << 2);
        if (hat & SDL_HAT_LEFT)  mask |= (1u << 3);
        dst.hats[static_cast<size_t>(h)] = mask;
    }
}

void SampleAllLocked(const State& g, std::array<InputMap::RawJoy, InputMap::kMaxJoysticks>& out) {
    for (size_t s = 0; s < InputMap::kMaxJoysticks; ++s) {
        SampleSlotLocked(g.slots[s], out[s]);
    }
}

} // namespace

void RefreshDevices() {
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        return;
    }

    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    std::vector<SDL_JoystickID> currentIds;
    if (ids != nullptr && count > 0) {
        currentIds.assign(ids, ids + count);
    }
    if (ids != nullptr) {
        SDL_free(ids);
    }

    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);

    // Prune disconnected slots while preserving existing slot indices.
    for (size_t s = 0; s < InputMap::kMaxJoysticks; ++s) {
        Slot& slot = g.slots[s];
        if (!slot.occupied) continue;
        const bool stillPresent =
            std::find(currentIds.begin(), currentIds.end(), slot.instanceId) != currentIds.end();
        if (!stillPresent) {
            CloseSlotLocked(slot);
            g.captureBaseline[s] = InputMap::RawJoy{};
            continue;
        }
        // Keep metadata fresh (e.g. if a borrowed gamepad handle just became available).
        slot.isGamepad = SDL_IsGamepad(slot.instanceId);
        if (SDL_Joystick* joy = ResolveJoystickLocked(slot)) {
            PopulateMetadataLocked(slot, joy);
        }
    }

    // Collect newly connected joysticks not yet assigned to a slot.
    std::vector<SDL_JoystickID> unassigned;
    unassigned.reserve(currentIds.size());
    for (SDL_JoystickID id : currentIds) {
        bool tracked = false;
        for (const Slot& slot : g.slots) {
            if (slot.occupied && slot.instanceId == id) {
                tracked = true;
                break;
            }
        }
        if (!tracked) {
            unassigned.push_back(id);
        }
    }

    // Prefer non-gamepad generic joysticks (HOTAS, flight sticks, throttles, pedals),
    // then stable ascending instance ID order.
    std::sort(unassigned.begin(), unassigned.end(), [](SDL_JoystickID a, SDL_JoystickID b) {
        const bool aPad = SDL_IsGamepad(a);
        const bool bPad = SDL_IsGamepad(b);
        if (aPad != bPad) return !aPad;
        return a < b;
    });

    for (SDL_JoystickID id : unassigned) {
        size_t freeSlot = InputMap::kMaxJoysticks;
        for (size_t s = 0; s < InputMap::kMaxJoysticks; ++s) {
            if (!g.slots[s].occupied) {
                freeSlot = s;
                break;
            }
        }
        if (freeSlot >= InputMap::kMaxJoysticks) {
            break;
        }

        const bool isGamepad = SDL_IsGamepad(id);
        SDL_Joystick* joy = nullptr;
        bool ownsHandle = false;

        if (SDL_Gamepad* gp = SDL_GetGamepadFromID(id)) {
            joy = SDL_GetGamepadJoystick(gp);
            ownsHandle = false;
        } else {
            joy = SDL_OpenJoystick(id);
            ownsHandle = (joy != nullptr);
        }

        if (joy == nullptr) {
            continue;
        }

        Slot& slot = g.slots[freeSlot];
        slot.occupied = true;
        slot.instanceId = id;
        slot.joy = joy;
        slot.ownsHandle = ownsHandle;
        slot.isGamepad = isGamepad;
        PopulateMetadataLocked(slot, joy);
    }
}

void HandleSdlEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_JOYSTICK_ADDED:
        RefreshDevices();
        break;
    case SDL_EVENT_JOYSTICK_REMOVED: {
        const SDL_JoystickID id = event.jdevice.which;
        State& g = G();
        std::lock_guard<std::mutex> lock(g.mutex);
        for (size_t s = 0; s < InputMap::kMaxJoysticks; ++s) {
            Slot& slot = g.slots[s];
            if (slot.occupied && slot.instanceId == id) {
                CloseSlotLocked(slot);
                g.captureBaseline[s] = InputMap::RawJoy{};
            }
        }
        break;
    }
    default:
        break;
    }
}

void SampleAll(std::array<InputMap::RawJoy, InputMap::kMaxJoysticks>& out) {
    for (auto& j : out) {
        j = InputMap::RawJoy{};
    }
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        return;
    }
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    SampleAllLocked(g, out);
}

std::vector<DeviceInfo> ConnectedDevices() {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    std::vector<DeviceInfo> out;
    out.reserve(InputMap::kMaxJoysticks);
    for (size_t s = 0; s < InputMap::kMaxJoysticks; ++s) {
        const Slot& slot = g.slots[s];
        if (!slot.occupied) continue;
        DeviceInfo info;
        info.slot = static_cast<int>(s);
        info.instanceId = static_cast<uint32_t>(slot.instanceId);
        info.name = slot.name;
        info.numButtons = slot.numButtons;
        info.numAxes = slot.numAxes;
        info.numHats = slot.numHats;
        info.isGamepad = slot.isGamepad;
        out.push_back(std::move(info));
    }
    return out;
}

void BeginCapture() {
    RefreshDevices();
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.capturing = true;
    for (auto& j : g.captureBaseline) {
        j = InputMap::RawJoy{};
    }
    if (SDL_WasInit(SDL_INIT_JOYSTICK)) {
        SampleAllLocked(g, g.captureBaseline);
    }
}

bool PollCapture(InputMap::Binding& outBinding) {
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        return false;
    }
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.capturing) {
        return false;
    }

    std::array<InputMap::RawJoy, InputMap::kMaxJoysticks> cur{};
    SampleAllLocked(g, cur);

    for (size_t s = 0; s < InputMap::kMaxJoysticks; ++s) {
        const Slot& slot = g.slots[s];
        // Standard SDL_Gamepads already open in Aurora bind via AimInput as pad:*;
        // skip them here so lower joystick axis thresholds never steal pad:* bindings.
        if (slot.isGamepad && SDL_GetGamepadFromID(slot.instanceId) != nullptr) {
            continue;
        }
        if (!cur[s].connected) {
            g.captureBaseline[s] = InputMap::RawJoy{};
            continue;
        }
        if (!g.captureBaseline[s].connected) {
            g.captureBaseline[s] = cur[s];
            continue;
        }

        // Newly pressed buttons
        for (size_t b = 0; b < InputMap::kMaxJoyButtons; ++b) {
            const bool down = cur[s].buttons[b];
            const bool wasDown = g.captureBaseline[s].buttons[b];
            g.captureBaseline[s].buttons[b] = down;
            if (down && !wasDown) {
                outBinding = InputMap::JoyButton(static_cast<int>(s), static_cast<int>(b));
                g.capturing = false;
                return true;
            }
        }

        // Newly pressed single-direction hats
        for (size_t h = 0; h < InputMap::kMaxJoyHats; ++h) {
            const uint8_t mask = cur[s].hats[h];
            const uint8_t prevMask = g.captureBaseline[s].hats[h];
            g.captureBaseline[s].hats[h] = mask;
            const uint8_t rising = static_cast<uint8_t>(mask & ~prevMask);
            if (rising != 0 && (mask & (mask - 1)) == 0) {
                for (int d = 0; d < 4; ++d) {
                    if (rising & (1u << d)) {
                        outBinding = InputMap::JoyHat(static_cast<int>(s), static_cast<int>(h), d);
                        g.capturing = false;
                        return true;
                    }
                }
            }
        }

        // Axis movement relative to resting baseline (handles HOTAS throttles/sliders resting at -1/+1)
        for (size_t a = 0; a < InputMap::kMaxJoyAxes; ++a) {
            const float val = cur[s].axes[a];
            const float base = g.captureBaseline[s].axes[a];
            const float delta = val - base;
            if (std::fabs(delta) >= kAxisCaptureDelta) {
                const int dir = delta > 0.f ? 1 : -1;
                outBinding = InputMap::JoyAxis(static_cast<int>(s), static_cast<int>(a), dir);
                g.capturing = false;
                return true;
            }
        }
    }

    return false;
}

void CancelCapture() {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.capturing = false;
}

bool HasGenericDevice() {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    for (const Slot& slot : g.slots) {
        if (slot.occupied && !slot.isGamepad) {
            return true;
        }
    }
    return false;
}

} // namespace GenericJoystick

#include "aim_input.h"

#include "aim_settings.h"
#include "runtime_log.h"
#include "virtual_remote.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

namespace AimInput {
namespace {

constexpr Uint64 kWheelPulseMs = 120;

struct State {
    std::mutex mutex;
    bool focused = true;
    bool relative = false;
    std::bitset<512> keys;
    uint8_t mouseButtons = 0;
    Uint64 wheelUpUntil = 0;
    Uint64 wheelDownUntil = 0;
    MouseSample mouse;
    float dx = 0.f, dy = 0.f;

    bool capturing = false;
    InputMap::Profile captureProfile = InputMap::Profile::Gamepad;
    CaptureResult captureResult;
    InputMap::Binding pendingMouse;
    std::array<bool, 32> padBaseline{};
    std::array<bool, 8> axisReady{};

    // Scheme service. Pulses are presses seen by the event stream since the last service tick, so a tap shorter
    // than a frame still counts.
    std::bitset<512> pulseKeys;
    std::bitset<32> pulsePad;
    uint8_t pulseButtons = 0;
    bool pulseWheel = false;
    float motionPx = 0.f;
    InputMap::RawSample prevPolled;
    bool havePrev = false;
    bool seeded = false;
    InputMap::SchemeArbiter arbiter;
};

State& G() {
    static State state;
    return state;
}

struct Published {
    std::atomic<uint8_t> scheme{static_cast<uint8_t>(InputMap::Scheme::Pointer)};
    std::atomic<uint8_t> device{static_cast<uint8_t>(InputMap::ActiveDevice::None)};
    std::atomic<uint32_t> noticeSerial{0};
    std::atomic<uint8_t> noticeScheme{static_cast<uint8_t>(InputMap::Scheme::Pointer)};
    std::atomic<uint64_t> noticeAt{0};
};

Published& Pub() {
    static Published p;
    return p;
}

InputMap::PadConditioning Conditioning() {
    static std::mutex mutex;
    static uint32_t generation = 0;
    static InputMap::PadConditioning cached;
    std::lock_guard<std::mutex> lock(mutex);
    const uint32_t now = AimSettings::Generation();
    if (generation != now || generation == 0) {
        const DualAnalog::Config c = AimSettings::Get().aim;
        cached = {c.leftSaturation, c.invertMoveY, c.triggerThreshold};
        generation = now;
    }
    return cached;
}

const char* SchemeName(InputMap::Scheme s) { return s == InputMap::Scheme::Classic ? "classic" : "pointer"; }
const char* DeviceName(InputMap::ActiveDevice d) {
    return d == InputMap::ActiveDevice::Gamepad ? "gamepad" : d == InputMap::ActiveDevice::KeyboardMouse ? "keyboard+mouse" : "none";
}

std::string SdlKeyName(int code) {
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
    return name != nullptr ? name : "";
}

int SdlKeyCode(const std::string& name) {
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? 0 : static_cast<int>(code);
}

float AxisValue(SDL_Gamepad* pad, int axis) {
    const int16_t raw = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(axis));
    return raw < 0 ? static_cast<float>(raw) / 32768.0f : static_cast<float>(raw) / 32767.0f;
}

void ClearInputLocked(State& g) {
    g.keys.reset();
    g.mouseButtons = 0;
    g.wheelUpUntil = g.wheelDownUntil = 0;
    g.dx = g.dy = 0.f;
    g.pulseKeys.reset();
    g.pulsePad.reset();
    g.pulseButtons = 0;
    g.pulseWheel = false;
    g.motionPx = 0.f;
    g.havePrev = false;
}

void RefreshWindowSizeLocked(State& g) {
    SDL_Window* window = aurora_get_window();
    int w = 0, h = 0;
    if (window != nullptr && SDL_GetWindowSize(window, &w, &h)) {
        g.mouse.winW = static_cast<float>(w);
        g.mouse.winH = static_cast<float>(h);
    }
}

} // namespace

void Install() {
    InputMap::KeyHooks().name = &SdlKeyName;
    InputMap::KeyHooks().code = &SdlKeyCode;
}

void HandleSdlEvent(const SDL_Event& event) {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        const int code = static_cast<int>(event.key.scancode);
        if (code > 0 && code < 512) {
            g.keys[code] = true;
            if (!event.key.repeat) g.pulseKeys[code] = true;
        }
        if (g.capturing && !event.key.repeat && g.captureProfile == InputMap::Profile::KeyboardMouse &&
            g.captureResult.state == CaptureState::Pending) {
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                g.captureResult.state = CaptureState::Cancelled;
            } else if (event.key.scancode == SDL_SCANCODE_DELETE) {
                g.captureResult = {CaptureState::Cleared, {}};
            } else if (event.key.scancode != SDL_SCANCODE_F10 && event.key.scancode != SDL_SCANCODE_F1) {
                g.captureResult = {CaptureState::Done, InputMap::Key(code)};
            }
        }
        break;
    }
    case SDL_EVENT_KEY_UP: {
        const int code = static_cast<int>(event.key.scancode);
        if (code > 0 && code < 512) g.keys[code] = false;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button >= 1 && event.button.button <= 5) {
            g.mouseButtons |= static_cast<uint8_t>(1u << (event.button.button - 1));
            g.pulseButtons |= static_cast<uint8_t>(1u << (event.button.button - 1));
            if (g.capturing && g.captureProfile == InputMap::Profile::KeyboardMouse) {
                g.pendingMouse = InputMap::MouseButton(event.button.button);
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button >= 1 && event.button.button <= 5) {
            g.mouseButtons &= static_cast<uint8_t>(~(1u << (event.button.button - 1)));
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float y = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
        if (y != 0.f) {
            g.pulseWheel = true;
            (y > 0.f ? g.wheelUpUntil : g.wheelDownUntil) = SDL_GetTicks() + kWheelPulseMs;
            if (g.capturing && g.captureProfile == InputMap::Profile::KeyboardMouse) {
                g.pendingMouse = InputMap::MouseButton(y > 0.f ? InputMap::kMouseWheelUp : InputMap::kMouseWheelDown);
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        g.mouse.x = event.motion.x;
        g.mouse.y = event.motion.y;
        g.mouse.valid = true;
        g.dx += event.motion.xrel;
        g.dy += event.motion.yrel;
        g.motionPx += std::fabs(event.motion.xrel) + std::fabs(event.motion.yrel);
        RefreshWindowSizeLocked(g);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button < 32) g.pulsePad[event.gbutton.button] = true;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        RefreshWindowSizeLocked(g);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g.focused = false;
        ClearInputLocked(g);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        g.focused = true;
        ClearInputLocked(g);
        break;
    default:
        break;
    }
}

bool Focused() {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.focused;
}

InputMap::RawSample Sample() {
    InputMap::RawSample raw;
    State& g = G();
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.focused) {
            raw.keys = g.keys;
            raw.mouseButtons = g.mouseButtons;
            const Uint64 now = SDL_GetTicks();
            raw.wheelUp = now < g.wheelUpUntil;
            raw.wheelDown = now < g.wheelDownUntil;
        }
    }
    if (Focused()) {
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
            for (int i = 0; i < 21; ++i) {
                raw.padButtons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            }
            for (int i = 0; i < 6; ++i) raw.padAxes[i] = AxisValue(pad, i);
            InputMap::ConditionPad(raw, Conditioning());
        }
    }
    return raw;
}

MouseSample MouseNow() {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.mouse;
}

void ConsumeMouseDelta(float& dx, float& dy) {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    dx = g.dx;
    dy = g.dy;
    g.dx = g.dy = 0.f;
}

namespace {

void PublishScheme(const AimSettings::Settings& settings, InputMap::Scheme scheme, const char* why, bool log) {
    Published& pub = Pub();
    if (log) {
        RT_LOGF(RT_TAG_BT, "[input] scheme -> %s (%s, device=%s)\n", SchemeName(scheme), why,
                DeviceName(static_cast<InputMap::ActiveDevice>(pub.device.load())));
    }
    pub.scheme.store(static_cast<uint8_t>(scheme));
    if (settings.scheme != scheme) AimSettings::SetEffectiveScheme(scheme);
    VirtualRemote::SetScheme(scheme);
}

// Runs once per frame on the main thread. Decision logic is the pure InputMap::UpdateScheme. The remote is
// only called after g.mutex is released (the remote's own thread samples input under its lock).
void TickScheme() {
    State& g = G();
    const AimSettings::Settings settings = AimSettings::Get();
    static const bool scripted = std::getenv("SIN_VREMOTE_SCRIPT") != nullptr;
    // A scripted run drives the remote itself: Automatic must not swap the extension under it.
    const InputMap::SchemeMode mode =
        (scripted && settings.schemeMode == InputMap::SchemeMode::Auto) ? InputMap::SchemeMode::Pointer : settings.schemeMode;
    Published& pub = Pub();
    const Uint64 now = SDL_GetTicks();

    const InputMap::RawSample polled = Sample();
    const bool pause = !Focused() || Capturing();

    std::bitset<512> bound;
    for (const auto& slots : settings.bindings.kbm) {
        for (const InputMap::Binding& b : slots) {
            if (b.device == InputMap::Device::Key && b.code > 0 && b.code < 512) bound[b.code] = true;
        }
    }
    InputMap::SenseConfig cfg;
    cfg.stick = std::clamp(std::max(settings.aim.leftDeadzone, settings.aim.radialDeadzone) + 0.2f, 0.45f, 0.85f);

    bool publish = false, log = false;
    InputMap::Scheme scheme = InputMap::Scheme::Pointer;
    const char* why = "";
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        InputMap::RawSample cur = polled;
        cur.keys |= g.pulseKeys;
        cur.padButtons |= g.pulsePad;
        cur.mouseButtons |= g.pulseButtons;
        cur.wheelUp = cur.wheelUp || g.pulseWheel;
        const float motion = g.motionPx;
        g.pulseKeys.reset();
        g.pulsePad.reset();
        g.pulseButtons = 0;
        g.pulseWheel = false;
        g.motionPx = 0.f;
        if (!g.seeded) {
            const bool pad = PADGetSDLGamepadForIndex(0) != nullptr;
            InputMap::SeedScheme(g.arbiter, mode, pad ? InputMap::ActiveDevice::Gamepad : InputMap::ActiveDevice::None);
            g.seeded = true;
            g.prevPolled = polled;
            g.havePrev = true;
            pub.device.store(static_cast<uint8_t>(g.arbiter.device));
            publish = log = true;
            why = "startup";
        } else {
            InputMap::DeviceSense sense;
            if (!pause && g.havePrev) sense = InputMap::Sense(g.prevPolled, cur, motion, bound, cfg);
            g.prevPolled = polled;
            g.havePrev = !pause;
            const InputMap::SchemeUpdate update =
                InputMap::UpdateScheme(g.arbiter, mode, sense, static_cast<int64_t>(now));
            if (update.deviceChanged) pub.device.store(static_cast<uint8_t>(g.arbiter.device));
            if (update.schemeChanged) {
                if (update.deviceChanged && mode == InputMap::SchemeMode::Auto) {
                    pub.noticeScheme.store(static_cast<uint8_t>(g.arbiter.scheme));
                    pub.noticeAt.store(now);
                    pub.noticeSerial.fetch_add(1);
                }
                publish = log = true;
                why = mode == InputMap::SchemeMode::Auto ? "auto" : "setting";
            }
        }
        scheme = g.arbiter.scheme;
    }
    if (publish) PublishScheme(settings, scheme, why, log);
}

} // namespace

InputMap::Scheme EffectiveScheme() { return static_cast<InputMap::Scheme>(Pub().scheme.load()); }
InputMap::ActiveDevice LastDevice() { return static_cast<InputMap::ActiveDevice>(Pub().device.load()); }

SchemeNotice LatestSwitch() {
    Published& pub = Pub();
    SchemeNotice n;
    n.serial = pub.noticeSerial.load();
    n.scheme = static_cast<InputMap::Scheme>(pub.noticeScheme.load());
    n.atMs = pub.noticeAt.load();
    return n;
}

void UpdateWindowState(bool inputBlocked) {
    State& g = G();
    const AimSettings::MouseConfig mouse = AimSettings::Get().mouse;
    SDL_Window* window = aurora_get_window();
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        const bool want = window != nullptr && mouse.enabled && mouse.raw && !inputBlocked && g.focused;
        if (want != g.relative && window != nullptr) {
            SDL_SetWindowRelativeMouseMode(window, want);
            g.relative = want;
            g.dx = g.dy = 0.f;
        }
    }
    TickScheme();
}

void BeginCapture(InputMap::Profile profile) {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.capturing = true;
    g.captureProfile = profile;
    g.captureResult = {CaptureState::Pending, {}};
    g.pendingMouse = {};
    g.padBaseline.fill(false);
    g.axisReady.fill(false);
    if (profile == InputMap::Profile::Gamepad) {
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
            for (int i = 0; i < 21; ++i) {
                g.padBaseline[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            }
            for (int i = 0; i < 6; ++i) g.axisReady[i] = std::fabs(AxisValue(pad, i)) < 0.25f;
        }
    }
}

CaptureResult PollCapture(bool pointerOverUi) {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.capturing) return {};
    if (g.captureResult.state == CaptureState::Pending) {
        if (g.captureProfile == InputMap::Profile::KeyboardMouse) {
            if (g.pendingMouse.Valid()) {
                if (!pointerOverUi) g.captureResult = {CaptureState::Done, g.pendingMouse};
                g.pendingMouse = {};
            }
        } else if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
            for (int i = 0; i < 21 && g.captureResult.state == CaptureState::Pending; ++i) {
                const bool down = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                if (down && !g.padBaseline[i]) g.captureResult = {CaptureState::Done, InputMap::PadButton(i)};
                g.padBaseline[i] = down;
            }
            for (int i = 0; i < 6 && g.captureResult.state == CaptureState::Pending; ++i) {
                const float v = AxisValue(pad, i);
                if (std::fabs(v) < 0.25f) g.axisReady[i] = true;
                if (g.axisReady[i] && std::fabs(v) >= 0.6f) {
                    const int dir = (i >= 4 || v > 0.f) ? 1 : -1;
                    g.captureResult = {CaptureState::Done, InputMap::PadAxis(i, dir)};
                }
            }
        }
    }
    const CaptureResult result = g.captureResult;
    if (result.state != CaptureState::Pending) {
        g.capturing = false;
        g.captureResult = {};
    }
    return result;
}

void CancelCapture() {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    g.capturing = false;
    g.captureResult = {};
}

bool Capturing() {
    State& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.capturing;
}

} // namespace AimInput

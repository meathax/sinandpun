#pragma once

// Pure binding logic (no SDL, no globals): binding names, parsing/formatting, per-profile slot tables, value
// resolution against a raw input sample, conflict detection/swap, and aim-device arbitration.
// Unit-tested in runtime/tests/controls_tests.cpp. Numeric codes mirror SDL3 (gamepad button/axis indices,
// scancodes, mouse button numbers) so the SDL glue can pass them straight through.

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace InputMap {

enum class Device : uint8_t { None, Pad, Key, Mouse };
enum class Profile : uint8_t { Gamepad, KeyboardMouse };

inline constexpr int32_t kPadAxisBase = 1000;   // Pad code >= base: axis (code - base) with direction
inline constexpr int32_t kMouseWheelUp = 100;
inline constexpr int32_t kMouseWheelDown = 101;
inline constexpr size_t kSlots = 3;             // primary, secondary, extra per action and profile

struct Binding {
    Device device = Device::None;
    int32_t code = 0;
    int8_t dir = 0;  // pad axes: -1 / +1

    bool Valid() const { return device != Device::None; }
    bool operator==(const Binding& o) const { return device == o.device && code == o.code && dir == o.dir; }
    bool operator!=(const Binding& o) const { return !(*this == o); }
};

inline Binding PadButton(int code) { return {Device::Pad, code, 0}; }
inline Binding PadAxis(int axis, int dir) { return {Device::Pad, kPadAxisBase + axis, static_cast<int8_t>(dir)}; }
inline Binding Key(int scancode) { return {Device::Key, scancode, 0}; }
inline Binding MouseButton(int button) { return {Device::Mouse, button, 0}; }

inline bool AllowedInProfile(Profile p, Device d) {
    return p == Profile::Gamepad ? d == Device::Pad : (d == Device::Key || d == Device::Mouse);
}

// ---- Names -------------------------------------------------------------------------------------

struct NamedBinding {
    const char* name;
    int32_t code;
    int8_t dir;
};

inline constexpr std::array<NamedBinding, 21> kPadButtonNames = {{
    {"south", 0, 0}, {"east", 1, 0}, {"west", 2, 0}, {"north", 3, 0}, {"back", 4, 0}, {"guide", 5, 0},
    {"start", 6, 0}, {"left_stick", 7, 0}, {"right_stick", 8, 0}, {"left_shoulder", 9, 0},
    {"right_shoulder", 10, 0}, {"dpad_up", 11, 0}, {"dpad_down", 12, 0}, {"dpad_left", 13, 0},
    {"dpad_right", 14, 0}, {"misc1", 15, 0}, {"right_paddle1", 16, 0}, {"left_paddle1", 17, 0},
    {"right_paddle2", 18, 0}, {"left_paddle2", 19, 0}, {"touchpad", 20, 0},
}};

inline constexpr std::array<NamedBinding, 12> kPadAxisNames = {{
    {"left_stick_left", kPadAxisBase + 0, -1}, {"left_stick_right", kPadAxisBase + 0, 1},
    {"left_stick_up", kPadAxisBase + 1, -1}, {"left_stick_down", kPadAxisBase + 1, 1},
    {"right_stick_left", kPadAxisBase + 2, -1}, {"right_stick_right", kPadAxisBase + 2, 1},
    {"right_stick_up", kPadAxisBase + 3, -1}, {"right_stick_down", kPadAxisBase + 3, 1},
    {"left_trigger", kPadAxisBase + 4, 1}, {"right_trigger", kPadAxisBase + 5, 1},
    {"left_trigger_neg", kPadAxisBase + 4, -1}, {"right_trigger_neg", kPadAxisBase + 5, -1},
}};

inline constexpr std::array<NamedBinding, 7> kMouseNames = {{
    {"left", 1, 0}, {"middle", 2, 0}, {"right", 3, 0}, {"x1", 4, 0}, {"x2", 5, 0},
    {"wheel_up", kMouseWheelUp, 0}, {"wheel_down", kMouseWheelDown, 0},
}};

// Keyboard names come from SDL (SDL_GetScancodeName / FromName), installed by the SDL glue. Without hooks keys
// round-trip as "#<scancode>".
struct KeyNameHooks {
    std::string (*name)(int) = nullptr;
    int (*code)(const std::string&) = nullptr;
};
inline KeyNameHooks& KeyHooks() {
    static KeyNameHooks hooks;
    return hooks;
}

// Fallback names (SDL3 scancodes) so defaults and tests work before/without the SDL hooks.
inline constexpr std::array<NamedBinding, 11> kBuiltinKeys = {{
    {"Escape", 41, 0},
    {"Return", 40, 0}, {"Backspace", 42, 0}, {"Tab", 43, 0}, {"Space", 44, 0}, {"Home", 74, 0},
    {"Right", 79, 0}, {"Left", 80, 0}, {"Down", 81, 0}, {"Up", 82, 0}, {"Left Ctrl", 224, 0},
}};

inline std::string BuiltinKeyName(int code) {
    if (code >= 4 && code <= 29) return std::string(1, static_cast<char>('A' + (code - 4)));
    if (code == 225) return "Left Shift";
    for (const auto& n : kBuiltinKeys) if (n.code == code) return n.name;
    return "";
}

inline int BuiltinKeyCode(const std::string& name) {
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return 4 + (name[0] - 'A');
    if (name == "Left Shift") return 225;
    for (const auto& n : kBuiltinKeys) if (name == n.name) return n.code;
    return 0;
}

inline std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return std::string(s.substr(a, b - a));
}

inline std::string Format(const Binding& b) {
    switch (b.device) {
    case Device::Pad:
        for (const auto& n : kPadButtonNames) if (b.code == n.code && b.code < kPadAxisBase) return std::string("pad:") + n.name;
        for (const auto& n : kPadAxisNames) if (b.code == n.code && b.dir == n.dir) return std::string("pad:") + n.name;
        return "";
    case Device::Mouse:
        for (const auto& n : kMouseNames) if (b.code == n.code) return std::string("mouse:") + n.name;
        return "";
    case Device::Key: {
        const KeyNameHooks& h = KeyHooks();
        std::string name = h.name ? h.name(b.code) : std::string();
        if (name.empty()) name = BuiltinKeyName(b.code);
        if (name.empty()) name = "#" + std::to_string(b.code);
        return "key:" + name;
    }
    case Device::None:
        break;
    }
    return "";
}

inline Binding Parse(std::string_view text) {
    const std::string t = Trim(text);
    const size_t colon = t.find(':');
    if (colon == std::string::npos) return {};
    const std::string kind = t.substr(0, colon);
    const std::string name = Trim(t.substr(colon + 1));
    if (kind == "pad") {
        for (const auto& n : kPadButtonNames) if (name == n.name) return {Device::Pad, n.code, 0};
        for (const auto& n : kPadAxisNames) if (name == n.name) return {Device::Pad, n.code, n.dir};
    } else if (kind == "mouse") {
        for (const auto& n : kMouseNames) if (name == n.name) return {Device::Mouse, n.code, 0};
    } else if (kind == "key") {
        if (!name.empty() && name[0] == '#') {
            const int code = std::atoi(name.c_str() + 1);
            if (code > 0 && code < 512) return {Device::Key, code, 0};
            return {};
        }
        const KeyNameHooks& h = KeyHooks();
        int code = BuiltinKeyCode(name);
        if (code == 0 && h.code) code = h.code(name);
        if (code > 0 && code < 512) return {Device::Key, code, 0};
    }
    return {};
}

using Slots = std::array<Binding, kSlots>;

// "a | b | c" -> slots. Entries that do not parse or do not belong to the profile are dropped; duplicates and
// extra entries are ignored.
inline Slots ParseList(std::string_view text, Profile profile) {
    Slots out{};
    size_t used = 0;
    size_t begin = 0;
    while (begin <= text.size() && used < kSlots) {
        size_t end = text.find('|', begin);
        if (end == std::string_view::npos) end = text.size();
        const Binding b = Parse(text.substr(begin, end - begin));
        if (b.Valid() && AllowedInProfile(profile, b.device) &&
            std::find(out.begin(), out.begin() + used, b) == out.begin() + used) {
            out[used++] = b;
        }
        begin = end + 1;
    }
    return out;
}

inline std::string FormatList(const Slots& slots) {
    std::string out;
    for (const Binding& b : slots) {
        const std::string s = Format(b);
        if (s.empty()) continue;
        if (!out.empty()) out += " | ";
        out += s;
    }
    return out;
}

// ---- Resolution --------------------------------------------------------------------------------

struct RawSample {
    std::bitset<32> padButtons;
    std::array<float, 8> padAxes{};
    std::bitset<512> keys;
    uint8_t mouseButtons = 0;  // bit (n - 1) = mouse button n
    bool wheelUp = false;
    bool wheelDown = false;
};

// 0..1. Pad axes resolve analog (the part of the axis in the bound direction); everything else is 0 or 1.
inline float Value(const Binding& b, const RawSample& raw) {
    switch (b.device) {
    case Device::Pad:
        if (b.code >= kPadAxisBase) {
            const int axis = b.code - kPadAxisBase;
            if (axis < 0 || axis >= static_cast<int>(raw.padAxes.size()) || b.dir == 0) return 0.f;
            return std::clamp(raw.padAxes[axis] * static_cast<float>(b.dir), 0.f, 1.f);
        }
        return (b.code >= 0 && b.code < 32 && raw.padButtons[b.code]) ? 1.f : 0.f;
    case Device::Key:
        return (b.code > 0 && b.code < 512 && raw.keys[b.code]) ? 1.f : 0.f;
    case Device::Mouse:
        if (b.code == kMouseWheelUp) return raw.wheelUp ? 1.f : 0.f;
        if (b.code == kMouseWheelDown) return raw.wheelDown ? 1.f : 0.f;
        return (b.code >= 1 && b.code <= 5 && (raw.mouseButtons & (1u << (b.code - 1)))) ? 1.f : 0.f;
    case Device::None:
        break;
    }
    return 0.f;
}

inline float SlotsValue(const Slots& slots, const RawSample& raw) {
    float v = 0.f;
    for (const Binding& b : slots) v = std::max(v, Value(b, raw));
    return v;
}

// ---- Tables, conflicts -------------------------------------------------------------------------

template <size_t N>
struct Table {
    std::array<Slots, N> pad{};
    std::array<Slots, N> kbm{};
    std::array<Slots, N>& For(Profile p) { return p == Profile::Gamepad ? pad : kbm; }
    const std::array<Slots, N>& For(Profile p) const { return p == Profile::Gamepad ? pad : kbm; }
};

template <size_t N>
std::optional<size_t> FindOwner(const std::array<Slots, N>& table, const Binding& b, size_t exceptAction) {
    if (!b.Valid()) return std::nullopt;
    for (size_t a = 0; a < N; ++a) {
        if (a == exceptAction) continue;
        for (const Binding& s : table[a]) if (s == b) return a;
    }
    return std::nullopt;
}

// Puts `b` into (action, slot). A binding already used by another action in the same profile is swapped: that
// action receives the slot's previous binding (possibly empty). Returns the swapped action. A duplicate in the
// same action's other slots is cleared. An invalid `b` just clears the slot.
template <size_t N>
std::optional<size_t> AssignSwap(std::array<Slots, N>& table, size_t action, size_t slot, const Binding& b) {
    if (action >= N || slot >= kSlots) return std::nullopt;
    const Binding previous = table[action][slot];
    std::optional<size_t> swapped;
    if (b.Valid()) {
        if (const auto owner = FindOwner(table, b, action)) {
            for (Binding& s : table[*owner]) {
                if (s == b) s = previous;
            }
            swapped = owner;
        }
        for (size_t i = 0; i < kSlots; ++i) {
            if (i != slot && table[action][i] == b) table[action][i] = {};
        }
    }
    table[action][slot] = b;
    return swapped;
}

// Number of action pairs sharing a binding inside one profile.
template <size_t N>
size_t CountConflicts(const std::array<Slots, N>& table) {
    size_t count = 0;
    for (size_t a = 0; a < N; ++a) {
        for (const Binding& b : table[a]) {
            if (b.Valid() && FindOwner(table, b, a)) ++count;
        }
    }
    return count / 2;
}

// ---- Aim device arbitration --------------------------------------------------------------------

enum class AimSource : uint8_t { Stick, Mouse };

struct Arbiter {
    AimSource active = AimSource::Stick;
    bool prevStick = false;
    bool prevMouse = false;
};

// The device whose motion STARTED most recently drives the pointer. A source that is merely still held (a
// steady stick, a continuously moving mouse) cannot retake control; if both start in the same poll the current
// source stays.
inline AimSource Arbitrate(Arbiter& s, bool stickMoving, bool mouseMoving) {
    const bool stickStarted = stickMoving && !s.prevStick;
    const bool mouseStarted = mouseMoving && !s.prevMouse;
    if (stickStarted && !mouseStarted) s.active = AimSource::Stick;
    else if (mouseStarted && !stickStarted) s.active = AimSource::Mouse;
    else if (stickMoving && !mouseMoving) s.active = AimSource::Stick;
    else if (mouseMoving && !stickMoving) s.active = AimSource::Mouse;
    s.prevStick = stickMoving;
    s.prevMouse = mouseMoving;
    return s.active;
}

// ---- Control scheme selection ------------------------------------------------------------------
// The scheme is the guest-visible controller: Classic Controller emulation (gamepad) or Wii Remote + Nunchuk
// (mouse pointer + keyboard). SchemeMode is the user's setting; Auto follows the device used last.

enum class Scheme : uint8_t { Classic, Pointer };
enum class SchemeMode : uint8_t { Auto, Classic, Pointer };
enum class ActiveDevice : uint8_t { None, Gamepad, KeyboardMouse };

inline constexpr int kSchemeSchemaVersion = 2;
inline constexpr std::array<const char*, 3> kSchemeModeKeys = {"auto", "classic", "pointer"};
inline constexpr std::array<const char*, 3> kSchemeModeLabels = {"Automatic (recommended)",
                                                                  "Legacy Classic Controller",
                                                                  "Always Wii Remote pointer"};

inline std::string SchemeModeKey(SchemeMode m) { return kSchemeModeKeys[static_cast<size_t>(m)]; }

// Tolerant parse (case/space insensitive). Unknown text keeps `fallback`. Schema < 2 wrote the old default
// "pointer" on every save, so it is not a deliberate choice and migrates to Auto; "classic" was an explicit
// opt-in and is kept.
inline SchemeMode ParseSchemeMode(std::string_view text, int schemaVersion, SchemeMode fallback) {
    std::string t = Trim(text);
    for (char& c : t) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (t == "auto") return SchemeMode::Auto;
    if (t == "classic") return SchemeMode::Classic;
    if (t == "pointer") return schemaVersion < kSchemeSchemaVersion ? SchemeMode::Auto : SchemeMode::Pointer;
    return fallback;
}

// Pad axis layout of RawSample: 0 LX, 1 LY, 2 RX, 3 RY, 4 LT, 5 RT.
struct PadConditioning {
    float leftSaturation = 1.0f;     // left stick deflection that counts as full (0.5..1)
    bool invertMoveY = false;        // swap left stick up/down
    float triggerThreshold = 0.5f;   // trigger pull that counts as pressed (0.1..0.9)
};

// Input conditioning applied to the pad part of a sample before bindings resolve. Triggers are remapped so
// the threshold lands on 0.5 (the digital press level) and full pull stays 1; below it they read 0.
inline float ConditionTrigger(float v, float threshold) {
    threshold = std::clamp(threshold, 0.05f, 0.95f);
    if (v < threshold) return 0.f;
    return std::clamp(0.5f + 0.5f * (v - threshold) / (1.f - threshold), 0.f, 1.f);
}

inline void ConditionPad(RawSample& raw, const PadConditioning& c) {
    const float sat = std::clamp(c.leftSaturation, 0.3f, 1.f);
    float x = raw.padAxes[0] / sat, y = raw.padAxes[1] / sat;
    const float m = std::sqrt(x * x + y * y);
    if (m > 1.f) {
        x /= m;
        y /= m;
    }
    raw.padAxes[0] = x;
    raw.padAxes[1] = c.invertMoveY ? -y : y;
    raw.padAxes[4] = ConditionTrigger(raw.padAxes[4], c.triggerThreshold);
    raw.padAxes[5] = ConditionTrigger(raw.padAxes[5], c.triggerThreshold);
}

// What the devices did during one poll. "Input" = deliberate activity this poll (a fresh press, a stick or
// trigger held past the threshold, mouse motion above the noise floor, a wheel tick). "Held" = a button/key
// is down or a stick/trigger is deflected right now.
struct DeviceSense {
    bool padInput = false, kbmInput = false;
    bool padHeld = false, kbmHeld = false;
};

struct SenseConfig {
    float stick = 0.5f;      // radial stick deflection that counts as deliberate (above any drift/deadzone)
    float trigger = 0.5f;
    float mousePx = 4.0f;    // pointer travel per poll that counts as a deliberate move (above sensor noise)
};

// `prev` is the previous poll WITHOUT event pulses, `cur` the current poll merged with pulses (quick taps
// between polls). `boundKeys` limits keyboard activity to keys that do something in the game.
inline DeviceSense Sense(const RawSample& prev, const RawSample& cur, float mouseMotionPx,
                         const std::bitset<512>& boundKeys, const SenseConfig& cfg) {
    DeviceSense s;
    const std::bitset<32> padEdge = cur.padButtons & ~prev.padButtons;
    s.padHeld = cur.padButtons.any();
    s.padInput = padEdge.any();
    auto radial = [&](int a, int b) { return std::sqrt(cur.padAxes[a] * cur.padAxes[a] + cur.padAxes[b] * cur.padAxes[b]); };
    const bool stick = radial(0, 1) >= cfg.stick || radial(2, 3) >= cfg.stick;
    const bool trig = cur.padAxes[4] >= cfg.trigger || cur.padAxes[5] >= cfg.trigger;
    if (stick || trig) s.padHeld = s.padInput = true;

    const std::bitset<512> keysDown = cur.keys & boundKeys;
    const std::bitset<512> keyEdge = keysDown & ~(prev.keys & boundKeys);
    const uint8_t buttonEdge = static_cast<uint8_t>(cur.mouseButtons & ~prev.mouseButtons);
    s.kbmHeld = keysDown.any() || cur.mouseButtons != 0;
    s.kbmInput = keyEdge.any() || buttonEdge != 0 || cur.wheelUp || cur.wheelDown || mouseMotionPx >= cfg.mousePx;
    return s;
}

struct SchemeArbiter {
    ActiveDevice device = ActiveDevice::None;
    Scheme scheme = Scheme::Pointer;
    int64_t lastSwitchMs = -1000000;
};

struct SchemeUpdate {
    bool deviceChanged = false;
    bool schemeChanged = false;
};

inline constexpr int64_t kSchemeDwellMs = 400;

inline Scheme EffectiveScheme(SchemeMode mode, const SchemeArbiter&) {
    return mode == SchemeMode::Classic ? Scheme::Classic : Scheme::Pointer;
}

// Seeds the device without a switch (startup, mode change): no dwell consumed, no event.
inline void SeedScheme(SchemeArbiter& a, SchemeMode mode, ActiveDevice device) {
    a.device = device;
    a.scheme = EffectiveScheme(mode, a);
}

// The last deliberate device wins, with hysteresis:
//  * both devices active in the same poll: nothing changes;
//  * a device that is still holding a button/key/stick keeps control;
//  * after a switch the other device must wait kSchemeDwellMs before it can take over again;
//  * the very first input ever (device None) is accepted at once.
// A mode other than Auto pins the scheme but the active device is still tracked for the UI.
inline SchemeUpdate UpdateScheme(SchemeArbiter& a, SchemeMode mode, const DeviceSense& s, int64_t nowMs,
                                 int64_t dwellMs = kSchemeDwellMs) {
    SchemeUpdate out;
    ActiveDevice candidate = ActiveDevice::None;
    if (s.padInput && !s.kbmInput) candidate = ActiveDevice::Gamepad;
    else if (s.kbmInput && !s.padInput) candidate = ActiveDevice::KeyboardMouse;
    if (candidate != ActiveDevice::None && candidate != a.device) {
        const bool currentHeld = a.device == ActiveDevice::Gamepad ? s.padHeld
                               : a.device == ActiveDevice::KeyboardMouse ? s.kbmHeld : false;
        const bool first = a.device == ActiveDevice::None;
        if (first || (!currentHeld && nowMs - a.lastSwitchMs >= dwellMs)) {
            a.device = candidate;
            a.lastSwitchMs = nowMs;
            out.deviceChanged = true;
        }
    }
    const Scheme next = EffectiveScheme(mode, a);
    out.schemeChanged = next != a.scheme;
    a.scheme = next;
    return out;
}

} // namespace InputMap

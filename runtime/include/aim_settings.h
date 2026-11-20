#pragma once

// Controls state shared between the F10 settings menu and the virtual Wii Remote: aiming tunables, the
// controller scheme, mouse options and the per-profile action bindings. Persisted to Config.toml:
//   [input]                version = 1, scheme = "classic" | "pointer"
//   [input.gamepad]        <action> = "pad:south | pad:right_trigger"        (up to 3 bindings)
//   [input.keyboard_mouse] <action> = "key:Space | mouse:x1 | mouse:wheel_up"
//   [aim]                  right-stick / pointer tunables (see Save())
//   [mouse]                mouse-as-pointer options
// Loading is tolerant: missing/unknown keys keep defaults, bad values are clamped. Changes apply live through
// Generation().

#include "dual_analog.h"
#include "input_map.h"
#include "runtime_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace AimSettings {

inline constexpr int kSchemaVersion = 1;

// Action names follow Sin & Punishment: Star Successor (manual + StrategyWiki control tables). The Classic
// Controller / Wii Remote+Nunchuk wire mapping is in virtual_remote.cpp.
enum class Action : size_t {
    Fire,        // Classic R / Wiimote B: hold = shoot, tap = sword
    LockOn,      // Classic ZR / Wiimote A: tap = lock on, hold + release = charge shot
    Jump,        // Classic L / Nunchuk C
    Evade,       // Classic ZL / Nunchuk Z
    Confirm,     // Classic A (menus)
    Cancel,      // Classic B (menus)
    Pause,       // + button
    Minus,       // - button
    Home,        // HOME button
    ButtonX,     // Classic X / Wiimote 1
    ButtonY,     // Classic Y / Wiimote 2
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    MoveLeft,    // left stick
    MoveRight,
    MoveUp,
    MoveDown,
    AimLeft,     // right stick
    AimRight,
    AimUp,
    AimDown,
    Precision,   // port extra: slow the reticle while held
    Recenter,    // port extra: recentre the pointer
    Count,
};

constexpr size_t kActionCount = static_cast<size_t>(Action::Count);

struct ActionInfo {
    const char* key;
    const char* label;
    const char* group;
    const char* detail;
    const char* padDefault;
    const char* kbmDefault;
};

inline constexpr std::array<ActionInfo, kActionCount> kActions = {{
    {"fire", "Shoot / Sword", "Combat", "Hold to shoot, tap to slash. Classic R / Wii Remote B.",
     "pad:right_shoulder", "key:J | mouse:left"},
    {"lock_on", "Lock-on / Charge shot", "Combat",
     "Tap to lock on; hold, then release for the charge shot. Classic ZR / Wii Remote A.",
     "pad:right_trigger", "key:K | mouse:right"},
    {"jump", "Jump", "Combat", "Classic L / Nunchuk C.", "pad:left_shoulder",
     "key:Space | mouse:x1 | mouse:wheel_up"},
    {"evade", "Evade", "Combat", "Classic ZL / Nunchuk Z.", "pad:left_trigger",
     "key:Left Shift | mouse:x2 | mouse:wheel_down"},
    {"confirm", "Menu: confirm", "Menus", "Classic A.", "pad:east", "key:Return"},
    {"cancel", "Menu: cancel", "Menus", "Classic B.", "pad:south", "key:Backspace"},
    {"pause", "Pause / skip cut scene (+)", "Menus", "+ button.", "pad:start", "key:P"},
    {"minus", "- button", "Menus", "No in-game action known.", "pad:back", "key:Tab"},
    {"home", "HOME button", "Menus", "Opens the Wii HOME menu.", "pad:guide", "key:Home"},
    {"button_x", "X button", "Menus", "Classic X / Wii Remote 1. No in-game action known.", "pad:north", "key:Q"},
    {"button_y", "Y button", "Menus", "Classic Y / Wii Remote 2. No in-game action known.", "pad:west", "key:E"},
    {"dpad_up", "Control Pad up", "Menus", "Menu navigation.", "pad:dpad_up", ""},
    {"dpad_down", "Control Pad down", "Menus", "Menu navigation.", "pad:dpad_down", ""},
    {"dpad_left", "Control Pad left", "Menus", "Menu navigation.", "pad:dpad_left", ""},
    {"dpad_right", "Control Pad right", "Menus", "Menu navigation.", "pad:dpad_right", ""},
    {"move_left", "Move left", "Movement", "Left stick / Nunchuk stick.", "pad:left_stick_left", "key:A"},
    {"move_right", "Move right", "Movement", "", "pad:left_stick_right", "key:D"},
    {"move_up", "Move up / hover", "Movement", "", "pad:left_stick_up", "key:W"},
    {"move_down", "Move down", "Movement", "", "pad:left_stick_down", "key:S"},
    {"aim_left", "Aim left", "Aiming", "Right stick moves the reticle.", "pad:right_stick_left", "key:Left"},
    {"aim_right", "Aim right", "Aiming", "", "pad:right_stick_right", "key:Right"},
    {"aim_up", "Aim up", "Aiming", "", "pad:right_stick_up", "key:Up"},
    {"aim_down", "Aim down", "Aiming", "", "pad:right_stick_down", "key:Down"},
    {"precision", "Precision aim (hold)", "Aiming", "Slows the reticle. Port extra, not a Wii control.",
     "pad:right_stick", "key:Left Ctrl"},
    {"recenter", "Recenter pointer", "Aiming", "Port extra, not a Wii control.", "pad:left_stick",
     "key:R | mouse:middle"},
}};

enum class Scheme : uint8_t { Classic, Pointer };
inline constexpr std::array<const char*, 2> kSchemeKeys = {"classic", "pointer"};
inline constexpr std::array<const char*, 2> kSchemeLabels = {"Classic Controller (right stick aims)",
                                                              "Wii Remote + Nunchuk (pointer)"};

struct MouseConfig {
    bool enabled = true;           // the mouse may drive the aim
    bool raw = false;              // relative/captured mode instead of absolute window position
    float sensitivity = 0.6f;      // relative: camera px per mouse count
    float smoothingMs = 0.0f;      // absolute: 1-pole smoothing, 0 = latest sample
    float rangeX = 1.0f;           // absolute: calibration of the usable camera span
    float rangeY = 1.0f;
    float stickSpeed = 1500.0f;    // Classic scheme: mouse counts/s that equal full stick deflection
};

struct Settings {
    Scheme scheme = Scheme::Pointer;  // Pointer is the verified path; Classic is opt-in (see docs/STATUS.md)
    DualAnalog::Config aim;
    MouseConfig mouse;
    InputMap::Table<kActionCount> bindings;
};

inline Settings DefaultBindings(Settings s = {}) {
    for (size_t i = 0; i < kActionCount; ++i) {
        s.bindings.pad[i] = InputMap::ParseList(kActions[i].padDefault, InputMap::Profile::Gamepad);
        s.bindings.kbm[i] = InputMap::ParseList(kActions[i].kbmDefault, InputMap::Profile::KeyboardMouse);
    }
    return s;
}

inline void Sanitize(Settings& s) {
    DualAnalog::Sanitize(s.aim);
    MouseConfig& m = s.mouse;
    auto fin = [](float v, float d) { return std::isfinite(v) ? v : d; };
    m.sensitivity = std::clamp(fin(m.sensitivity, 0.6f), 0.1f, 4.0f);
    m.smoothingMs = std::clamp(fin(m.smoothingMs, 0.0f), 0.0f, 150.0f);
    m.rangeX = std::clamp(fin(m.rangeX, 1.0f), 0.5f, 1.5f);
    m.rangeY = std::clamp(fin(m.rangeY, 1.0f), 0.5f, 1.5f);
    m.stickSpeed = std::clamp(fin(m.stickSpeed, 1500.0f), 300.0f, 6000.0f);
}

namespace detail {

struct State {
    std::mutex mutex;
    bool loaded = false;
    Settings settings;
    std::atomic<uint32_t> generation{1};
    State() { settings = DefaultBindings(); }
};

inline State& Shared() {
    static State state;
    return state;
}

template <typename T, typename... Path>
std::optional<T> FindValue(const toml::value& d, const Path&... path) {
    try {
        return toml::find<T>(d, path...);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

template <typename... Path>
std::optional<float> FindNumber(const toml::value& d, const Path&... path) {
    if (const auto f = FindValue<double>(d, path...)) return static_cast<float>(*f);
    if (const auto i = FindValue<int64_t>(d, path...)) return static_cast<float>(*i);
    return std::nullopt;
}

template <size_t N>
size_t KeyIndex(const std::array<const char*, N>& keys, const std::string& value, size_t fallback) {
    for (size_t i = 0; i < N; ++i) if (value == keys[i]) return i;
    return fallback;
}

inline void LoadFromConfig(Settings& s) {
    std::ifstream file(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
    if (!file) return;
    toml::value d;
    try {
        d = toml::parse(file, "Config.toml");
    } catch (const std::exception&) {
        return;
    }
    DualAnalog::Config& c = s.aim;
    auto num = [&](const char* key, float& out) {
        if (const auto v = FindNumber(d, "aim", key)) out = *v;
    };
    auto flag = [&](const char* section, const char* key, bool& out) {
        if (const auto v = FindValue<bool>(d, section, key)) out = *v;
    };
    if (const auto v = FindValue<std::string>(d, "input", "scheme")) {
        s.scheme = static_cast<Scheme>(KeyIndex(kSchemeKeys, *v, static_cast<size_t>(s.scheme)));
    }
    if (const auto v = FindValue<std::string>(d, "aim", "deadzone_mode")) {
        c.deadzoneMode = static_cast<DualAnalog::DeadzoneMode>(
            KeyIndex(DualAnalog::kDeadzoneModeKeys, *v, static_cast<size_t>(c.deadzoneMode)));
    }
    if (const auto v = FindValue<std::string>(d, "aim", "curve")) {
        c.curve = static_cast<DualAnalog::Curve>(KeyIndex(DualAnalog::kCurveKeys, *v, static_cast<size_t>(c.curve)));
    }
    if (const auto v = FindValue<std::string>(d, "aim", "pointer_mode")) c.absoluteMode = *v == "absolute";
    num("right_deadzone", c.radialDeadzone);
    num("right_saturation", c.outerSaturation);
    num("curve_exponent", c.curveExponent);
    num("dual_zone_split", c.dualZoneSplit);
    num("dual_zone_output", c.dualZoneOutput);
    num("sensitivity_x", c.sensitivityX);
    num("sensitivity_y", c.sensitivityY);
    flag("aim", "invert_y", c.invertY);
    num("smoothing_ms", c.smoothingMs);
    num("precision_scale", c.precisionScale);
    num("max_speed", c.maxSpeedX);
    c.maxSpeedY = c.maxSpeedX * (700.0f / 900.0f);
    num("speed_cap", c.speedCap);
    if (const auto v = FindNumber(d, "aim", "ramp_ms")) c.rampSeconds = *v * 0.001f;
    num("edge_boost", c.edgeBoost);
    if (const auto v = FindNumber(d, "aim", "edge_accel_ms")) c.edgeAccelSeconds = *v * 0.001f;
    flag("aim", "recenter_on_release", c.recenterOnRelease);
    if (const auto v = FindNumber(d, "aim", "recenter_ms")) c.recenterSeconds = *v * 0.001f;
    num("left_deadzone", c.leftDeadzone);

    MouseConfig& m = s.mouse;
    flag("mouse", "enabled", m.enabled);
    flag("mouse", "raw", m.raw);
    if (const auto v = FindNumber(d, "mouse", "sensitivity")) m.sensitivity = *v;
    if (const auto v = FindNumber(d, "mouse", "smoothing_ms")) m.smoothingMs = *v;
    if (const auto v = FindNumber(d, "mouse", "range_x")) m.rangeX = *v;
    if (const auto v = FindNumber(d, "mouse", "range_y")) m.rangeY = *v;
    if (const auto v = FindNumber(d, "mouse", "stick_speed")) m.stickSpeed = *v;

    // An explicit key (even an empty string) replaces the default for that action/profile.
    for (size_t i = 0; i < kActionCount; ++i) {
        if (const auto v = FindValue<std::string>(d, "input", "gamepad", kActions[i].key)) {
            s.bindings.pad[i] = InputMap::ParseList(*v, InputMap::Profile::Gamepad);
        }
        if (const auto v = FindValue<std::string>(d, "input", "keyboard_mouse", kActions[i].key)) {
            s.bindings.kbm[i] = InputMap::ParseList(*v, InputMap::Profile::KeyboardMouse);
        }
    }
}

inline std::string Float(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", value);
    return buffer;
}

} // namespace detail

// Key names need SDL; the glue (aim_input.cpp) installs the hooks before the first Get().
inline Settings Get() {
    detail::State& state = detail::Shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.loaded) {
        state.loaded = true;
        detail::LoadFromConfig(state.settings);
        Sanitize(state.settings);
    }
    return state.settings;
}

inline uint32_t Generation() { return detail::Shared().generation.load(std::memory_order_acquire); }

inline void Set(Settings settings) {
    Get();
    Sanitize(settings);
    detail::State& state = detail::Shared();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.settings = std::move(settings);
    }
    state.generation.fetch_add(1, std::memory_order_release);
}

inline void ResetAimToDefaults() {
    Settings s = Get();
    s.aim = DualAnalog::Config{};
    s.mouse = MouseConfig{};
    Set(s);
}

inline void ResetBindingsToDefaults() { Set(DefaultBindings(Get())); }

inline void ResetAllToDefaults() { Set(DefaultBindings(Settings{})); }

// Writes every key in one pass so a slider release costs a single file rewrite.
inline bool Save() {
    const Settings s = Get();
    const DualAnalog::Config& c = s.aim;
    const MouseConfig& m = s.mouse;
    const auto path = RuntimeConfigFile::ResolveConfigPath();
    std::ostringstream existing;
    if (std::ifstream input(path, std::ios::binary); input) {
        existing << input.rdbuf();
    }
    std::string text = existing.str();
    bool ok = true;
    auto put = [&](const char* section, std::string_view key, const std::string& value) {
        if (const auto updated = RuntimeConfigFile::UpdateSettingText(text, section, key, value)) {
            text = *updated;
        } else {
            ok = false;
        }
    };
    using detail::Float;
    auto str = [](const char* v) { return RuntimeConfigFile::FormatString(v); };
    auto boolean = [](bool v) { return std::string(v ? "true" : "false"); };
    put("input", "version", std::to_string(kSchemaVersion));
    put("input", "scheme", str(kSchemeKeys[static_cast<size_t>(s.scheme)]));
    put("aim", "deadzone_mode", str(DualAnalog::kDeadzoneModeKeys[static_cast<size_t>(c.deadzoneMode)]));
    put("aim", "right_deadzone", Float(c.radialDeadzone));
    put("aim", "right_saturation", Float(c.outerSaturation));
    put("aim", "curve", str(DualAnalog::kCurveKeys[static_cast<size_t>(c.curve)]));
    put("aim", "curve_exponent", Float(c.curveExponent));
    put("aim", "dual_zone_split", Float(c.dualZoneSplit));
    put("aim", "dual_zone_output", Float(c.dualZoneOutput));
    put("aim", "sensitivity_x", Float(c.sensitivityX));
    put("aim", "sensitivity_y", Float(c.sensitivityY));
    put("aim", "invert_y", boolean(c.invertY));
    put("aim", "smoothing_ms", Float(c.smoothingMs));
    put("aim", "precision_scale", Float(c.precisionScale));
    put("aim", "pointer_mode", str(c.absoluteMode ? "absolute" : "velocity"));
    put("aim", "max_speed", Float(c.maxSpeedX));
    put("aim", "speed_cap", Float(c.speedCap));
    put("aim", "ramp_ms", Float(c.rampSeconds * 1000.0f));
    put("aim", "edge_boost", Float(c.edgeBoost));
    put("aim", "edge_accel_ms", Float(c.edgeAccelSeconds * 1000.0f));
    put("aim", "recenter_on_release", boolean(c.recenterOnRelease));
    put("aim", "recenter_ms", Float(c.recenterSeconds * 1000.0f));
    put("aim", "left_deadzone", Float(c.leftDeadzone));
    put("mouse", "enabled", boolean(m.enabled));
    put("mouse", "raw", boolean(m.raw));
    put("mouse", "sensitivity", Float(m.sensitivity));
    put("mouse", "smoothing_ms", Float(m.smoothingMs));
    put("mouse", "range_x", Float(m.rangeX));
    put("mouse", "range_y", Float(m.rangeY));
    put("mouse", "stick_speed", Float(m.stickSpeed));
    for (size_t i = 0; i < kActionCount; ++i) {
        put("input.gamepad", kActions[i].key, RuntimeConfigFile::FormatString(InputMap::FormatList(s.bindings.pad[i])));
        put("input.keyboard_mouse", kActions[i].key,
            RuntimeConfigFile::FormatString(InputMap::FormatList(s.bindings.kbm[i])));
    }
    if (!ok) {
        std::cerr << "[runtime-config] Not updating invalid " << RuntimeConfigFile::PathToUtf8(path) << std::endl;
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "[runtime-config] Unable to write " << RuntimeConfigFile::PathToUtf8(path) << std::endl;
        return false;
    }
    output << text;
    return static_cast<bool>(output);
}

} // namespace AimSettings

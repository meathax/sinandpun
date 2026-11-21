#pragma once

// Controls state shared between the F10 settings menu and the virtual Wii Remote: aiming tunables, the
// controller scheme, mouse options and the per-profile action bindings. Persisted to Config.toml:
//   [input]                version = 3, scheme = "auto" | "classic" | "pointer", switch_toast = true
//                          (auto: the device used last picks the scheme live; files written before version 2
//                          with scheme = "pointer" - the old default - are read as "auto")
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

inline constexpr int kSchemaVersion = 3;

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
     "pad:right_trigger", "mouse:left | key:J"},
    {"lock_on", "Lock-on / Charge shot", "Combat",
     "Tap to lock on; hold, then release for the charge shot. Classic ZR / Wii Remote A.",
     "pad:left_trigger", "mouse:right | key:K"},
    {"jump", "Jump", "Combat", "Classic L / Nunchuk C.", "pad:left_shoulder",
     "key:Space | mouse:x1"},
    {"evade", "Evade", "Combat", "Classic ZL / Nunchuk Z.", "pad:right_shoulder",
     "key:Left Shift | mouse:x2"},
    {"confirm", "Menu: confirm", "Menus", "Confirm in game menus. Options UI keeps A / Enter as recovery controls.", "pad:south", "key:Return"},
    {"cancel", "Menu: cancel", "Menus", "Back in game menus. Options UI keeps B / Escape as recovery controls.", "pad:east", "key:Backspace"},
    {"pause", "Pause / skip cut scene (+)", "Menus", "+ button.", "pad:start", "key:Escape | key:P"},
    {"minus", "Switch character (-)", "Menus", "Switch characters once unlocked.", "pad:back", "key:Tab"},
    {"home", "HOME button", "Menus", "Opens the Wii HOME menu.", "pad:guide", "key:Home"},
    {"button_x", "Extra button 1", "Menus", "Classic X / Wii Remote 1.", "pad:west", "key:Q"},
    {"button_y", "Extra button 2", "Menus", "Classic Y / Wii Remote 2.", "pad:north", "key:E"},
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

using Scheme = InputMap::Scheme;
using SchemeMode = InputMap::SchemeMode;
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
    SchemeMode schemeMode = SchemeMode::Auto;
    // Scheme the virtual remote starts with: the pinned scheme, or Pointer (the verified path) in Auto until the
    // first input decides. The input service keeps it equal to the scheme in effect so a remote created late
    // starts right; live changes go through VirtualRemote::SetScheme.
    Scheme scheme = Scheme::Pointer;
    bool switchToast = true;          // brief on-screen note when the scheme switches automatically
    bool playStationPrompts = false;  // Xbox One by default; independent of the input scheme
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

inline Settings LegacyBindings() {
    Settings s = DefaultBindings();
    const std::pair<Action,const char*> pad[] = {
        {Action::Fire,"pad:right_shoulder"},{Action::LockOn,"pad:right_trigger"},
        {Action::Evade,"pad:left_trigger"},{Action::Confirm,"pad:east"},{Action::Cancel,"pad:south"},
        {Action::ButtonX,"pad:north"},{Action::ButtonY,"pad:west"}};
    for (const auto& [a,v] : pad) s.bindings.pad[static_cast<size_t>(a)] = InputMap::ParseList(v,InputMap::Profile::Gamepad);
    const std::pair<Action,const char*> kbm[] = {
        {Action::Fire,"key:J | mouse:left"},{Action::LockOn,"key:K | mouse:right"},
        {Action::Jump,"key:Space | mouse:x1 | mouse:wheel_up"},
        {Action::Evade,"key:Left Shift | mouse:x2 | mouse:wheel_down"},{Action::Pause,"key:P"}};
    for (const auto& [a,v] : kbm) s.bindings.kbm[static_cast<size_t>(a)] = InputMap::ParseList(v,InputMap::Profile::KeyboardMouse);
    return s;
}

inline float ActionValue(const Settings& s, Action a, const InputMap::RawSample& raw) {
    const size_t i = static_cast<size_t>(a);
    return std::max(InputMap::SlotsValue(s.bindings.pad[i],raw),InputMap::SlotsValue(s.bindings.kbm[i],raw));
}

inline bool KeyBound(const Settings& s, int code) {
    for (const auto& slots : s.bindings.kbm)
        for (const auto& b : slots)
            if (b.device == InputMap::Device::Key && b.code == code) return true;
    return false;
}

inline void Sanitize(Settings& s) {
    if (s.schemeMode == SchemeMode::Classic) s.scheme = Scheme::Classic;
    else if (s.schemeMode == SchemeMode::Pointer) s.scheme = Scheme::Pointer;
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

// Test harness overrides (tools/aim_test.ps1), applied after Config.toml and never saved:
//   SIN_INPUT_SCHEME=classic|pointer|auto
//   SIN_AIM_OVERRIDE="invert_y=1,sensitivity_x=1.5,right_deadzone=0.2,pointer_mode=absolute,curve=power,..."
// Keys are the [aim] keys of Config.toml (numbers, booleans, curve, deadzone_mode, pointer_mode).
inline void ApplyTestOverrides(Settings& s) {
    if (const char* e = std::getenv("SIN_INPUT_SCHEME"); e && *e) {
        s.schemeMode = InputMap::ParseSchemeMode(e, kSchemaVersion, s.schemeMode);
    }
    const char* o = std::getenv("SIN_AIM_OVERRIDE");
    if (!o || !*o) return;
    DualAnalog::Config& c = s.aim;
    std::string text(o);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(',', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        const float f = static_cast<float>(std::atof(v.c_str()));
        const bool b = v == "1" || v == "true";
        if (k == "right_deadzone") c.radialDeadzone = f;
        else if (k == "right_saturation") c.outerSaturation = f;
        else if (k == "curve_exponent") c.curveExponent = f;
        else if (k == "sensitivity_x") c.sensitivityX = f;
        else if (k == "sensitivity_y") c.sensitivityY = f;
        else if (k == "invert_y") c.invertY = b;
        else if (k == "smoothing_ms") c.smoothingMs = f;
        else if (k == "precision_scale") c.precisionScale = f;
        else if (k == "max_speed") { c.maxSpeedX = f; c.maxSpeedY = f * (700.0f / 900.0f); }
        else if (k == "speed_cap") c.speedCap = f;
        else if (k == "ramp_ms") c.rampSeconds = f * 0.001f;
        else if (k == "edge_boost") c.edgeBoost = f;
        else if (k == "recenter_on_release") c.recenterOnRelease = b;
        else if (k == "left_deadzone") c.leftDeadzone = f;
        else if (k == "left_saturation") c.leftSaturation = f;
        else if (k == "invert_move_y") c.invertMoveY = b;
        else if (k == "pointer_mode") c.absoluteMode = v == "absolute";
        else if (k == "curve") {
            for (size_t i = 0; i < DualAnalog::kCurveKeys.size(); ++i) {
                if (v == DualAnalog::kCurveKeys[i]) c.curve = static_cast<DualAnalog::Curve>(i);
            }
        } else if (k == "deadzone_mode") {
            for (size_t i = 0; i < DualAnalog::kDeadzoneModeKeys.size(); ++i) {
                if (v == DualAnalog::kDeadzoneModeKeys[i]) c.deadzoneMode = static_cast<DualAnalog::DeadzoneMode>(i);
            }
        }
    }
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
    int schemaVersion = 1;
    if (const auto v = FindValue<int64_t>(d, "input", "version")) schemaVersion = static_cast<int>(*v);
    if (schemaVersion < 3) s.bindings = LegacyBindings().bindings;
    if (const auto v = FindValue<std::string>(d, "input", "scheme")) {
        s.schemeMode = InputMap::ParseSchemeMode(*v, schemaVersion, s.schemeMode);
    }
    flag("input", "switch_toast", s.switchToast);
    if (const auto v = FindValue<std::string>(d, "input", "button_prompts")) {
        s.playStationPrompts = *v == "playstation";
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
    num("left_saturation", c.leftSaturation);
    num("trigger_threshold", c.triggerThreshold);
    flag("aim", "invert_move_y", c.invertMoveY);

    ApplyTestOverrides(s);

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
    if (schemaVersion < 3) {
        const auto legacy = LegacyBindings();
        const auto modern = DefaultBindings();
        // Upgrade only an untouched profile as a unit. Individual substitutions
        // could collide with a custom binding elsewhere in that profile.
        if (s.bindings.pad == legacy.bindings.pad) {
            s.bindings.pad = modern.bindings.pad;
            if (s.schemeMode == SchemeMode::Pointer) s.schemeMode = SchemeMode::Auto;
        }
        if (s.bindings.kbm == legacy.bindings.kbm) s.bindings.kbm = modern.bindings.kbm;
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

// Called by the input service when the scheme in effect changes; no generation bump (nothing to re-read) and
// never persisted. Only meaningful in Auto: pinned modes are re-synced by Sanitize.
inline void SetEffectiveScheme(Scheme scheme) {
    Get();
    detail::State& state = detail::Shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.settings.scheme = scheme;
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
    put("input", "scheme", str(InputMap::kSchemeModeKeys[static_cast<size_t>(s.schemeMode)]));
    put("input", "switch_toast", boolean(s.switchToast));
    put("input", "button_prompts", str(s.playStationPrompts ? "playstation" : "xbox_one"));
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
    put("aim", "left_saturation", Float(c.leftSaturation));
    put("aim", "invert_move_y", boolean(c.invertMoveY));
    put("aim", "trigger_threshold", Float(c.triggerThreshold));
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

// One-time, backed-up upgrade. Customized profiles remain intact; untouched
// legacy profiles adopt the modern defaults. Never overwrite an earlier backup.
inline bool UpgradeLayoutConfig() {
    Get();
    const auto path=RuntimeConfigFile::ResolveConfigPath();
    std::ifstream input(path,std::ios::binary);
    if (input) {
        try {
            const auto d=toml::parse(input,"Config.toml");
            if (const auto v=detail::FindValue<int64_t>(d,"input","version"); v && *v>=kSchemaVersion) return true;
        } catch (...) { return false; }
        input.close();
        auto backup=path;backup += ".input-v2.bak";
        std::error_code ec;
        if (!std::filesystem::exists(backup,ec)) {
            std::filesystem::copy_file(path,backup,std::filesystem::copy_options::none,ec);
            if (ec) return false;
        }
    }
    return Save();
}

} // namespace AimSettings

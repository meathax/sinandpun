#pragma once

// Controls state shared between the F10 settings menu and the virtual Wii Remote: aiming tunables, the
// controller scheme, mouse options and the per-profile action bindings. Persisted to Config.toml:
//   [input]                version = 6, scheme = "auto" | "classic" | "pointer"
//                          (auto: both devices drive the Wii pointer; files written before version 2
//                          with scheme = "pointer" - the old default - are read as "auto")
//   [input.gamepad]        <action> = "pad:south | pad:right_trigger"        (up to 3 bindings)
//   [input.keyboard_mouse] <action> = "key:Space | mouse:x1 | mouse:wheel_up"
//   [aim]                  right-stick / pointer tunables (see Save(); schema 6 replaced the speed model)
//   [mouse]                mouse-as-pointer options
// Loading is tolerant: missing/unknown keys keep defaults, bad values are clamped. Changes apply live through
// Generation().

#include "dual_analog.h"
#include "gyro_aim.h"
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

inline constexpr int kSchemaVersion = 6;

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
    GyroModifier, // optional gyro activation button
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
    {"fire", "Shoot / slash", "Combat", "Hold to shoot, tap to slash. Wii: B.", "pad:right_trigger", "mouse:left | key:J"},
    {"lock_on", "Lock on / charge shot", "Combat", "Tap to lock on. Hold, then release, for a charge shot. Wii: A.",
     "pad:left_trigger", "mouse:right | key:K"},
    {"jump", "Jump", "Combat", "Wii: Nunchuk C.", "pad:right_shoulder", "key:Space | mouse:x1"},
    {"evade", "Evade", "Combat", "Dodge in the direction you are moving. Wii: Nunchuk Z.", "pad:left_shoulder",
     "key:Left Shift | mouse:x2"},
    {"confirm", "Menu confirm", "Menus", "Wii: A.", "pad:south", "key:Return"},
    {"cancel", "Menu back", "Menus", "Wii: B.", "pad:east", "key:Backspace"},
    {"pause", "Pause / skip scene", "Menus", "Wii: +.", "pad:start", "key:Escape | key:P"},
    {"minus", "Switch character", "Menus", "Once unlocked. Wii: -.", "pad:back", "key:Tab"},
    {"home", "HOME menu", "Menus", "Wii: HOME.", "pad:guide", "key:Home"},
    {"button_x", "Wii Remote 1", "Menus", "Not used by the game during play.", "pad:west", "key:Q"},
    {"button_y", "Wii Remote 2", "Menus", "Not used by the game during play.", "pad:north", "key:E"},
    {"dpad_up", "Menu up", "Menus", "Wii: Control Pad.", "pad:dpad_up", ""},
    {"dpad_down", "Menu down", "Menus", "", "pad:dpad_down", ""},
    {"dpad_left", "Menu left", "Menus", "", "pad:dpad_left", ""},
    {"dpad_right", "Menu right", "Menus", "", "pad:dpad_right", ""},
    {"move_left", "Move left", "Movement", "Wii: Nunchuk stick.", "pad:left_stick_left", "key:A"},
    {"move_right", "Move right", "Movement", "", "pad:left_stick_right", "key:D"},
    {"move_up", "Move up / hover", "Movement", "", "pad:left_stick_up", "key:W"},
    {"move_down", "Move down", "Movement", "", "pad:left_stick_down", "key:S"},
    {"aim_left", "Aim left", "Aiming", "Moves the reticle. Wii: pointer.", "pad:right_stick_left", "key:Left"},
    {"aim_right", "Aim right", "Aiming", "", "pad:right_stick_right", "key:Right"},
    {"aim_up", "Aim up", "Aiming", "", "pad:right_stick_up", "key:Up"},
    {"aim_down", "Aim down", "Aiming", "", "pad:right_stick_down", "key:Down"},
    {"gyro_modifier", "Gyro button", "Aiming", "Only used when the Gyro button option is set to hold.",
     "pad:right_stick", "key:Left Ctrl"},
    {"recenter", "Recenter reticle", "Aiming", "Snaps the reticle to the middle of the screen.", "pad:left_stick",
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

enum class ButtonPromptStyle : uint8_t {
    Xbox = 0,
    PlayStation = 1,
    Keyboard = 2,
    Wii = 3,
};

inline constexpr std::array<const char*, 4> kButtonPromptKeys = {"xbox_one", "playstation", "keyboard", "wii"};
inline constexpr std::array<const char*, 4> kButtonPromptLabels = {"Xbox", "PlayStation", "Keyboard", "Original Wii"};

inline ButtonPromptStyle ParseButtonPromptStyle(std::string_view text) {
    std::string t = InputMap::Trim(text);
    for (char& c : t) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (t == "playstation" || t == "ps") return ButtonPromptStyle::PlayStation;
    if (t == "keyboard" || t == "kbm" || t == "m+kb") return ButtonPromptStyle::Keyboard;
    if (t == "wii" || t == "original_wii" || t == "original") return ButtonPromptStyle::Wii;
    return ButtonPromptStyle::Xbox;
}

struct Settings {
    SchemeMode schemeMode = SchemeMode::Auto;
    // The input service keeps the initial extension equal to the scheme in effect so a remote created late
    // starts right; live changes go through VirtualRemote::SetScheme.
    Scheme scheme = Scheme::Pointer;
    ButtonPromptStyle buttonPromptStyle = ButtonPromptStyle::Xbox;
    DualAnalog::Config aim = DualAnalog::ModernAim();
    MouseConfig mouse;
    GyroAim::Config gyro;
    InputMap::Table<kActionCount> bindings;

    bool IsPlayStationPrompts() const { return buttonPromptStyle == ButtonPromptStyle::PlayStation; }
    bool IsKeyboardPrompts() const { return buttonPromptStyle == ButtonPromptStyle::Keyboard; }
    bool IsWiiPrompts() const { return buttonPromptStyle == ButtonPromptStyle::Wii; }
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
        {Action::Jump,"pad:left_shoulder"},{Action::Fire,"pad:right_shoulder"},{Action::LockOn,"pad:right_trigger"},
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
    else if (s.schemeMode == SchemeMode::Pointer || s.schemeMode == SchemeMode::KeyboardMouse) s.scheme = Scheme::Pointer;
    if (s.schemeMode == SchemeMode::KeyboardMouse) s.mouse.enabled = true;
    if (s.gyro.enabled) s.scheme = Scheme::Pointer;
    if (static_cast<uint8_t>(s.buttonPromptStyle) >= kButtonPromptKeys.size()) {
        s.buttonPromptStyle = ButtonPromptStyle::Xbox;
    }
    s.gyro.activation = std::clamp(s.gyro.activation, 0, 2);
    s.gyro.space = std::clamp(s.gyro.space, 0, 2);
    for (float* value : {&s.gyro.sensitivityX, &s.gyro.sensitivityY})
        *value = std::isfinite(*value) ? std::clamp(*value, 1.f, 50.f) : GyroAim::kDefaultSensitivity;
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
        else if (k == "sensitivity") c.sensitivity = f;
        else if (k == "vertical_ratio") c.verticalRatio = f;
        else if (k == "invert_y") c.invertY = b;
        else if (k == "left_deadzone") c.leftDeadzone = f;
        else if (k == "left_saturation") c.leftSaturation = f;
        else if (k == "invert_move_y") c.invertMoveY = b;
        else if (k == "trigger_threshold") c.triggerThreshold = f;
    }
}

// Aim keys written by schema 6. Schema 5 and older used a different speed model (curve presets, ramp, edge boost,
// separate X/Y sensitivity, absolute mode); those keys are still accepted in the file and ignored. On upgrade the
// stick tuning starts from the new defaults, keeping only settings whose meaning did not change.
inline void LoadAim(const toml::value& d, int schemaVersion, DualAnalog::Config& c) {
    auto num = [&](const char* key, float& out) {
        if (const auto v = FindNumber(d, "aim", key)) out = *v;
    };
    auto flag = [&](const char* key, bool& out) {
        if (const auto v = FindValue<bool>(d, "aim", key)) out = *v;
    };
    const DualAnalog::Config defaults;
    flag("invert_y", c.invertY);
    num("left_deadzone", c.leftDeadzone);
    num("left_saturation", c.leftSaturation);
    flag("invert_move_y", c.invertMoveY);
    if (schemaVersion >= 6) {
        num("right_deadzone", c.radialDeadzone);
        num("right_saturation", c.outerSaturation);
        num("curve_exponent", c.curveExponent);
        num("sensitivity", c.sensitivity);
        num("vertical_ratio", c.verticalRatio);
        num("trigger_threshold", c.triggerThreshold);
        return;
    }
    // Keep a deadzone or trigger point the player changed away from the old defaults (0.12 / 0.5).
    float oldDeadzone = 0.12f, oldTrigger = 0.5f;
    num("right_deadzone", oldDeadzone);
    num("trigger_threshold", oldTrigger);
    c.radialDeadzone = std::fabs(oldDeadzone - 0.12f) < 1e-3f ? defaults.radialDeadzone : oldDeadzone;
    c.triggerThreshold = std::fabs(oldTrigger - 0.5f) < 1e-3f ? defaults.triggerThreshold : oldTrigger;
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
    auto flag = [&](const char* section, const char* key, bool& out) {
        if (const auto v = FindValue<bool>(d, section, key)) out = *v;
    };
    int schemaVersion = 1;
    if (const auto v = FindValue<int64_t>(d, "input", "version")) schemaVersion = static_cast<int>(*v);
    if (schemaVersion < 3) s.bindings = LegacyBindings().bindings;
    if (const auto v = FindValue<std::string>(d, "input", "scheme")) {
        s.schemeMode = InputMap::ParseSchemeMode(*v, schemaVersion, s.schemeMode);
    }
    if (const auto v = FindValue<std::string>(d, "input", "button_prompts")) {
        s.buttonPromptStyle = ParseButtonPromptStyle(*v);
    }
    LoadAim(d, schemaVersion, s.aim);
    ApplyTestOverrides(s);

    flag("gyro", "enabled", s.gyro.enabled);
    flag("gyro", "invert_x", s.gyro.invertX);
    flag("gyro", "invert_y", s.gyro.invertY);
    if (const auto v = FindValue<int>(d, "gyro", "activation")) s.gyro.activation = *v;
    if (const auto v = FindValue<int>(d, "gyro", "space")) s.gyro.space = *v;
    if (const auto v = FindNumber(d, "gyro", "sensitivity_x")) s.gyro.sensitivityX = *v;
    if (const auto v = FindNumber(d, "gyro", "sensitivity_y")) s.gyro.sensitivityY = *v;
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
        const char* key = schemaVersion < 5 && i == static_cast<size_t>(Action::GyroModifier)
            ? "precision" : kActions[i].key;
        if (const auto v = FindValue<std::string>(d, "input", "gamepad", key)) {
            s.bindings.pad[i] = InputMap::ParseList(*v, InputMap::Profile::Gamepad);
        }
        if (const auto v = FindValue<std::string>(d, "input", "keyboard_mouse", key)) {
            s.bindings.kbm[i] = InputMap::ParseList(*v, InputMap::Profile::KeyboardMouse);
        }
    }
    if (schemaVersion >= 3 && schemaVersion < 5) {
        const auto modern = DefaultBindings();
        auto& jump = s.bindings.pad[static_cast<size_t>(Action::Jump)];
        auto& evade = s.bindings.pad[static_cast<size_t>(Action::Evade)];
        if (jump == modern.bindings.pad[static_cast<size_t>(Action::Evade)] &&
            evade == modern.bindings.pad[static_cast<size_t>(Action::Jump)])
            std::swap(jump, evade);
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
    s.aim = DualAnalog::ModernAim();
    s.mouse = MouseConfig{};
    s.gyro = GyroAim::Config{};
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
    put("input", "button_prompts", str(kButtonPromptKeys[static_cast<size_t>(s.buttonPromptStyle)]));
    put("aim", "right_deadzone", Float(c.radialDeadzone));
    put("aim", "right_saturation", Float(c.outerSaturation));
    put("aim", "curve_exponent", Float(c.curveExponent));
    put("aim", "sensitivity", Float(c.sensitivity));
    put("aim", "vertical_ratio", Float(c.verticalRatio));
    put("aim", "invert_y", boolean(c.invertY));
    put("aim", "left_deadzone", Float(c.leftDeadzone));
    put("aim", "left_saturation", Float(c.leftSaturation));
    put("aim", "invert_move_y", boolean(c.invertMoveY));
    put("aim", "trigger_threshold", Float(c.triggerThreshold));
    put("gyro", "enabled", boolean(s.gyro.enabled));
    put("gyro", "activation", std::to_string(s.gyro.activation));
    put("gyro", "space", std::to_string(s.gyro.space));
    put("gyro", "sensitivity_x", Float(s.gyro.sensitivityX));
    put("gyro", "sensitivity_y", Float(s.gyro.sensitivityY));
    put("gyro", "invert_x", boolean(s.gyro.invertX));
    put("gyro", "invert_y", boolean(s.gyro.invertY));
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
        int64_t oldVersion = 0;
        try {
            const auto d=toml::parse(input,"Config.toml");
            if (const auto v=detail::FindValue<int64_t>(d,"input","version")) oldVersion = *v;
            if (oldVersion>=kSchemaVersion) return true;
        } catch (...) { return false; }
        input.close();
        // Schema <= 2 files keep the historical backup name; later upgrades get one backup per old version.
        auto backup=path;
        backup += oldVersion <= 2 ? std::string(".input-v2.bak") : ".input-v" + std::to_string(oldVersion) + ".bak";
        std::error_code ec;
        if (!std::filesystem::exists(backup,ec)) {
            std::filesystem::copy_file(path,backup,std::filesystem::copy_options::none,ec);
            if (ec) return false;
        }
    }
    return Save();
}

} // namespace AimSettings

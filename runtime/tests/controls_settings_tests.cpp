// Config round-trip and default-binding completeness against the real aim_settings.h (needs toml11).
// Standalone, portable mode keeps the config inside a scratch folder next to the executable:
//   mkdir build/ct; echo > build/ct/portable.txt
//   clang++ -std=c++20 -I runtime/include -I runtime/third_party/toml11 runtime/tests/controls_settings_tests.cpp -o build/ct/t.exe -lole32 -luuid -lshell32
//   build/ct/t.exe

#include "aim_settings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static void WriteConfig(const std::string& text) {
    const auto path = RuntimeConfigFile::ResolveConfigPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc) << text;
}

static AimSettings::Settings Load() {
    AimSettings::Settings s = AimSettings::DefaultBindings();
    AimSettings::detail::LoadFromConfig(s);
    AimSettings::Sanitize(s);
    return s;
}

static void TestDefaults() {
    using namespace AimSettings;
    const Settings s = DefaultBindings();
    CHECK(s.schemeMode == SchemeMode::Auto);
    CHECK(s.switchToast);
    const auto stick = DualAnalog::StickPointerConfig(s.aim, false);
    CHECK(stick.cameraMinX == 287.f && stick.cameraMaxX == 736.f);
    CHECK(s.aim.radialDeadzone == 0.10f && s.aim.outerSaturation == 0.92f && s.aim.curveExponent == 2.2f);
    CHECK(s.aim.sensitivity == 1.f && s.aim.verticalRatio == 1.f && !s.aim.invertY);
    CHECK(stick.maxSpeedX == DualAnalog::kFullTiltSpeed);
    CHECK(s.aim.triggerThreshold == 0.35f);
    CHECK(!s.gyro.enabled && s.gyro.sensitivityX == GyroAim::kDefaultSensitivity);
    CHECK(!s.playStationPrompts);
    for (size_t i = 0; i < kActionCount; ++i) {
        // Every action has a gamepad default; keyboard defaults may be empty only for the d-pad actions.
        CHECK(s.bindings.pad[i][0].Valid());
        const bool dpad = i >= static_cast<size_t>(Action::DpadUp) && i <= static_cast<size_t>(Action::DpadRight);
        CHECK(dpad || s.bindings.kbm[i][0].Valid());
        CHECK(std::string(kActions[i].kbmDefault).empty() == !s.bindings.kbm[i][0].Valid());
    }
    CHECK(InputMap::CountConflicts(s.bindings.pad) == 0);
    CHECK(InputMap::CountConflicts(s.bindings.kbm) == 0);
    // Game-facing actions sit where the manual says (Classic R/ZR/L/ZL, Nunchuk C/Z).
    auto pad = [&](Action a) { return InputMap::FormatList(s.bindings.pad[static_cast<size_t>(a)]); };
    CHECK(pad(Action::Fire) == "pad:right_trigger");
    CHECK(pad(Action::LockOn) == "pad:left_trigger");
    CHECK(pad(Action::Jump) == "pad:right_shoulder");
    CHECK(pad(Action::Evade) == "pad:left_shoulder");
    CHECK(pad(Action::Confirm) == "pad:south" && pad(Action::Cancel) == "pad:east");
    CHECK(pad(Action::Pause) == "pad:start" && pad(Action::Minus) == "pad:back" && pad(Action::Home) == "pad:guide");
    auto kbm = [&](Action a) { return InputMap::FormatList(s.bindings.kbm[static_cast<size_t>(a)]); };
    CHECK(kbm(Action::Fire).find("mouse:left") != std::string::npos);
    CHECK(kbm(Action::LockOn).find("mouse:right") != std::string::npos);
    CHECK(kbm(Action::MoveUp) == "key:W" && kbm(Action::MoveLeft) == "key:A");
}

static void TestRoundTrip() {
    using namespace AimSettings;
    std::filesystem::remove(RuntimeConfigFile::ResolveConfigPath());
    Settings s = DefaultBindings();
    s.schemeMode = SchemeMode::Auto;
    s.switchToast = false;
    s.playStationPrompts = true;
    s.aim.leftDeadzone = 0.2f;
    s.aim.leftSaturation = 0.8f;
    s.aim.invertMoveY = true;
    s.aim.triggerThreshold = 0.3f;
    s.aim.sensitivity = 1.5f;
    s.aim.curveExponent = 3.f;
    s.aim.verticalRatio = 0.8f;
    s.aim.radialDeadzone = 0.2f;
    s.aim.invertY = true;
    s.mouse.raw = true;
    s.mouse.sensitivity = 1.25f;
    InputMap::AssignSwap(s.bindings.kbm, static_cast<size_t>(Action::Jump), 0, InputMap::Key(30));
    Set(s);
    CHECK(Save());
    const Settings r = Load();
    CHECK(r.schemeMode == SchemeMode::Auto);
    CHECK(!r.switchToast);
    CHECK(r.playStationPrompts);
    CHECK(std::fabs(r.aim.leftDeadzone - 0.2f) < 1e-3f && std::fabs(r.aim.leftSaturation - 0.8f) < 1e-3f);
    CHECK(r.aim.invertMoveY && std::fabs(r.aim.triggerThreshold - 0.3f) < 1e-3f);
    CHECK(r.mouse.raw && std::fabs(r.mouse.sensitivity - 1.25f) < 1e-3f);
    CHECK(std::fabs(r.aim.sensitivity - 1.5f) < 1e-3f && std::fabs(r.aim.curveExponent - 3.f) < 1e-3f);
    CHECK(std::fabs(r.aim.verticalRatio - 0.8f) < 1e-3f && std::fabs(r.aim.radialDeadzone - 0.2f) < 1e-3f);
    CHECK(r.aim.invertY);
    CHECK(r.bindings.kbm[static_cast<size_t>(Action::Jump)][0] == InputMap::Key(30));
    // Every mode survives a save.
    for (SchemeMode m : {SchemeMode::Classic, SchemeMode::Pointer, SchemeMode::Auto}) {
        Settings x = Get();
        x.schemeMode = m;
        Set(x);
        CHECK(Save());
        CHECK(Load().schemeMode == m);
        CHECK(Get().scheme == (m == SchemeMode::Classic ? Scheme::Classic : m == SchemeMode::Pointer ? Scheme::Pointer : Get().scheme));
    }
}

static void TestLegacyAndTolerance() {
    using namespace AimSettings;
    WriteConfig("[input]\nversion = 1\nscheme = \"pointer\"\n");
    CHECK(Load().schemeMode == SchemeMode::Auto);  // old default migrates
    WriteConfig("[input]\nversion = 1\nscheme = \"classic\"\n");
    CHECK(Load().schemeMode == SchemeMode::Classic);
    WriteConfig("[input]\nversion = 3\nscheme = \"pointer\"\n");
    CHECK(Load().schemeMode == SchemeMode::Pointer);
    WriteConfig("[input]\nversion = 2\nscheme = \"auto\"\n");
    CHECK(Load().schemeMode == SchemeMode::Auto);
    WriteConfig("[input]\nbutton_prompts = \"unknown\"\n");
    CHECK(!Load().playStationPrompts);
    WriteConfig("[input]\nbutton_prompts = \"xbox_one\"\n");
    CHECK(!Load().playStationPrompts);
    WriteConfig("[input]\nscheme = 7\n[aim]\nleft_saturation = 40\ntrigger_threshold = -2\n");
    const Settings s = Load();
    CHECK(s.schemeMode == SchemeMode::Auto);
    CHECK(s.aim.leftSaturation == 1.0f && s.aim.triggerThreshold == 0.1f);
    WriteConfig("this is not toml [[[");
    CHECK(Load().schemeMode == SchemeMode::Auto);
}

int main() {
    TestDefaults();
    TestRoundTrip();
    TestLegacyAndTolerance();
    WriteConfig("[input]\nversion = 2\nscheme = \"pointer\"\n");
    const auto upgraded=Load();
    CHECK(upgraded.schemeMode==AimSettings::SchemeMode::Auto);
    CHECK(upgraded.bindings.pad==AimSettings::DefaultBindings().bindings.pad);
    CHECK(upgraded.bindings.kbm==AimSettings::DefaultBindings().bindings.kbm);
    WriteConfig("[input]\nversion = 2\nscheme = \"pointer\"\n[input.gamepad]\nfire = \"pad:north\"\n");
    const auto custom=Load();
    CHECK(custom.schemeMode==AimSettings::SchemeMode::Pointer);
    CHECK(InputMap::FormatList(custom.bindings.pad[0])=="pad:north");
    CHECK(custom.bindings.pad[1]==AimSettings::LegacyBindings().bindings.pad[1]);
    CHECK(custom.bindings.kbm==AimSettings::DefaultBindings().bindings.kbm);
    WriteConfig("[input]\nversion = 3\nscheme = \"auto\"\n[aim]\ncurve = \"linear\"\n"
                "max_speed = 900.0\nspeed_cap = 1600.0\nramp_ms = 120.0\ndual_zone_split = 0.5\n");
    CHECK(Load().aim.sensitivity == 1.f && Load().aim.curveExponent == 2.2f);
    // Schema 5 (old speed model): untouched tuning adopts the new defaults; invert and changed deadzones survive;
    // the removed keys are accepted without error.
    WriteConfig("[input]\nversion = 5\nscheme = \"auto\"\n[aim]\ndeadzone_mode = \"scaled_radial\"\n"
                "right_deadzone = 0.120\ncurve = \"s_curve\"\ncurve_exponent = 1.600\nsensitivity_x = 0.800\n"
                "sensitivity_y = 0.800\nmax_speed = 1100.000\nspeed_cap = 8000.000\nramp_ms = 0.000\n"
                "edge_boost = 1.000\npointer_mode = \"velocity\"\ninvert_y = true\ntrigger_threshold = 0.500\n"
                "left_deadzone = 0.180\n");
    {
        const auto old5 = Load();
        CHECK(old5.aim.radialDeadzone == 0.10f && old5.aim.triggerThreshold == 0.35f);
        CHECK(old5.aim.sensitivity == 1.f && old5.aim.curveExponent == 2.2f && old5.aim.invertY);
        CHECK(std::fabs(old5.aim.leftDeadzone - 0.18f) < 1e-4f);
    }
    WriteConfig("[input]\nversion = 5\n[aim]\nright_deadzone = 0.250\ntrigger_threshold = 0.200\n");
    {
        const auto custom5 = Load();
        CHECK(std::fabs(custom5.aim.radialDeadzone - 0.25f) < 1e-4f);
        CHECK(std::fabs(custom5.aim.triggerThreshold - 0.2f) < 1e-4f);
    }
    auto stored=AimSettings::DefaultBindings();
    stored.aim.sensitivity = 2.25f;
    stored.aim.curveExponent = 1.5f;
    stored.gyro.enabled = true; stored.gyro.activation = 2; stored.gyro.space = 2;
    stored.gyro.sensitivityX = 17.5f; stored.gyro.sensitivityY = 8.5f;
    stored.gyro.invertX = true; stored.gyro.invertY = true;
    stored.bindings.pad[0]={InputMap::PadButton(2),InputMap::PadAxis(5,1),InputMap::PadButton(10)};
    stored.bindings.kbm[0]={InputMap::Key(41),InputMap::MouseButton(5),InputMap::MouseButton(InputMap::kMouseWheelUp)};
    stored.bindings.pad[1]={};stored.bindings.kbm[1]={};
    AimSettings::Set(stored);CHECK(AimSettings::Save());
    const auto remapped=Load();
    CHECK(remapped.aim.sensitivity == 2.25f && remapped.aim.curveExponent == 1.5f);
    CHECK(remapped.gyro.enabled && remapped.gyro.activation == 2 && remapped.gyro.space == 2);
    CHECK(remapped.gyro.sensitivityX == 17.5f && remapped.gyro.sensitivityY == 8.5f);
    CHECK(remapped.gyro.invertX && remapped.gyro.invertY);
    CHECK(remapped.bindings.pad==stored.bindings.pad);
    CHECK(remapped.bindings.kbm==stored.bindings.kbm);
    std::filesystem::remove(RuntimeConfigFile::ResolveConfigPath());
    if (g_failures == 0) std::puts("controls_settings_tests: all passed");
    return g_failures == 0 ? 0 : 1;
}

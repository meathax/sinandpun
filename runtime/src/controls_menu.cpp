#include "controls_menu.h"

#include "aim_input.h"
#include "aim_settings.h"
#include "dual_analog.h"
#include "generic_joystick.h"
#include "input_map.h"
#include "runtime_config.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>

namespace controls_menu {
namespace {

using AimSettings::Action;
using AimSettings::kActionCount;
using game_ui::kChanged;
using game_ui::kCommit;
using game_ui::ListView;
using Clock = std::chrono::steady_clock;

constexpr std::array<const char*, kPageCount> kPageNames = {"Gamepad", "Gyro", "Buttons", "Mouse"};

struct Edit {
    bool dirty = false;  // push to the live settings
    bool save = false;   // write Config.toml
    void Take(game_ui::Change change) {
        if (change & kChanged) dirty = true;
        if (change & kCommit) save = true;
    }
};

// ---- Live preview ------------------------------------------------------------------------------------

struct Preview {
    DualAnalog::PointerState pointer;
    InputMap::Arbiter arbiter;
    float lastMouseX = 0.f, lastMouseY = 0.f;
    bool haveMouse = false;
    float rawX = 0.f, rawY = 0.f;
} g_preview;

void AimVector(const AimSettings::Settings& s, const InputMap::RawSample& raw, float& x, float& y) {
    x = AimSettings::ActionValue(s, Action::AimRight, raw) - AimSettings::ActionValue(s, Action::AimLeft, raw);
    y = AimSettings::ActionValue(s, Action::AimDown, raw) - AimSettings::ActionValue(s, Action::AimUp, raw);
    const float m = std::sqrt(x * x + y * y);
    if (m > 1.f) {
        x /= m;
        y /= m;
    }
}

// Mirrors the virtual remote's arbitration with the same pure functions; the real remote is input-blocked
// while the menu is open, so the preview runs its own copy.
InputMap::AimSource UpdatePreview(const AimSettings::Settings& s) {
    static int lastFrame = -1;
    const int frame = ImGui::GetFrameCount();
    if (frame == lastFrame) return g_preview.arbiter.active;
    lastFrame = frame;
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.05f);
    const InputMap::RawSample raw = AimInput::Sample();
    AimVector(s, raw, g_preview.rawX, g_preview.rawY);
    const AimInput::MouseSample ms = AimInput::MouseNow();
    float mdx = 0.f, mdy = 0.f;
    if (ms.valid && g_preview.haveMouse) {
        mdx = ms.x - g_preview.lastMouseX;
        mdy = ms.y - g_preview.lastMouseY;
    }
    g_preview.lastMouseX = ms.x;
    g_preview.lastMouseY = ms.y;
    g_preview.haveMouse = ms.valid;
    const bool mouseMoving = s.mouse.enabled && (std::fabs(mdx) + std::fabs(mdy)) > 0.f;
    const bool stickMoving = DualAnalog::StickActive(s.aim, g_preview.rawX, g_preview.rawY);
    const InputMap::AimSource source = InputMap::Arbitrate(g_preview.arbiter, stickMoving, mouseMoving);
    if (source == InputMap::AimSource::Mouse && s.mouse.enabled) {
        if (s.mouse.raw) {
            DualAnalog::MouseRelativeStep(g_preview.pointer, s.aim, mdx, mdy, s.mouse.sensitivity);
        } else if (ms.valid) {
            const DualAnalog::Rect rect = AimInput::PictureRect(ms);
            float u = 0.5f, v = 0.5f, tx = g_preview.pointer.x, ty = g_preview.pointer.y;
            DualAnalog::WindowToNormalized(rect, ms.x, ms.y, u, v);
            DualAnalog::NormalizedToCamera(s.aim, u, v, s.mouse.rangeX, s.mouse.rangeY, tx, ty);
            g_preview.pointer.x = DualAnalog::SmoothToward(g_preview.pointer.x, tx, dt, s.mouse.smoothingMs);
            g_preview.pointer.y = DualAnalog::SmoothToward(g_preview.pointer.y, ty, dt, s.mouse.smoothingMs);
        }
    } else {
        const auto config = DualAnalog::StickPointerConfig(s.aim, RuntimeConfigFile::WiiSensorBarAbove());
        DualAnalog::Advance(g_preview.pointer, config, -g_preview.rawX, g_preview.rawY, dt);
    }
    return g_preview.arbiter.active;
}

// Live raw input: both sticks, both triggers (after the trigger threshold), mouse position and buttons.
void DrawDeviceMonitor(ListView& v) {
    v.Block(150.0f, [&](ImDrawList* dl, ImVec2 mn, ImVec2 mx) {
        const float S = game_ui::Scale();
        const float size = 116.0f * S;
        const float gap = 22.0f * S;
        const ImU32 bg = IM_COL32(8, 30, 62, 150);
        const ImU32 rim = IM_COL32(255, 255, 255, 110);
        const float top = mn.y + 2.0f * S;
        float x = mn.x + 4.0f * S;
        const InputMap::RawSample raw = AimInput::Sample();
        const AimSettings::Settings s = AimSettings::Get();

        auto stick = [&](const char* label, float ax, float ay, float deadzone) {
            const ImVec2 boxMin(x, top), boxMax(x + size, top + size);
            dl->AddRectFilled(boxMin, boxMax, bg, 14.0f * S);
            dl->AddRect(boxMin, boxMax, rim, 14.0f * S, 0, 1.5f);
            const ImVec2 c(x + size * 0.5f, top + size * 0.5f);
            const float r = size * 0.5f - 8.0f * S;
            dl->AddCircle(c, r, IM_COL32(255, 255, 255, 70), 48);
            dl->AddCircle(c, r * deadzone, IM_COL32(255, 168, 80, 230), 32, 1.5f);
            dl->AddCircleFilled(ImVec2(c.x + ax * r, c.y + ay * r), 5.0f * S, IM_COL32(110, 255, 120, 255));
            game_ui::Text(dl, 20.0f, ImVec2(x + 6.0f * S, boxMax.y + 2.0f * S), game_ui::kTextDim, label, game_ui::Outline::Thin);
            x += size + gap;
        };
        stick("Left stick", raw.padAxes[0], raw.padAxes[1], s.aim.leftDeadzone);
        stick("Right stick", raw.padAxes[2], raw.padAxes[3], s.aim.radialDeadzone);

        // Triggers: bars filled up to the pull, a tick at the press level (0.5 after conditioning).
        const float barW = 26.0f * S;
        for (int i = 0; i < 2; ++i) {
            const ImVec2 b0(x + i * (barW + 10.0f * S), top), b1(b0.x + barW, top + size);
            dl->AddRectFilled(b0, b1, bg, 8.0f * S);
            const float value = raw.padAxes[4 + i];
            dl->AddRectFilled(ImVec2(b0.x, b1.y - size * value), b1, value >= 0.5f ? IM_COL32(110, 255, 120, 255) : IM_COL32(110, 200, 255, 220), 8.0f * S);
            dl->AddRect(b0, b1, rim, 8.0f * S, 0, 1.5f);
            dl->AddLine(ImVec2(b0.x, b1.y - size * 0.5f), ImVec2(b1.x, b1.y - size * 0.5f), IM_COL32(255, 168, 80, 230), 1.5f);
        }
        game_ui::Text(dl, 20.0f, ImVec2(x, top + size + 2.0f * S), game_ui::kTextDim, "LT   RT", game_ui::Outline::Thin);
        x += barW * 2 + 10.0f * S + gap;

        // Mouse: cursor inside the window and the three main buttons.
        const AimInput::MouseSample ms = AimInput::MouseNow();
        const float boxW = size * 1.35f;
        const ImVec2 pMin(x, top), pMax(x + boxW, top + size);
        dl->AddRectFilled(pMin, pMax, IM_COL32(6, 20, 44, 190), 14.0f * S);
        dl->AddRect(pMin, pMax, rim, 14.0f * S, 0, 1.5f);
        if (ms.valid && ms.winW > 0.f && ms.winH > 0.f) {
            const float u = std::clamp(ms.x / ms.winW, 0.f, 1.f), w = std::clamp(ms.y / ms.winH, 0.f, 1.f);
            dl->AddCircleFilled(ImVec2(pMin.x + 8.0f * S + u * (boxW - 16.0f * S), pMin.y + 8.0f * S + w * (size - 16.0f * S)),
                                5.0f * S, IM_COL32(255, 96, 96, 255));
        }
        for (int i = 0; i < 3; ++i) {
            const bool down = (raw.mouseButtons & (1u << i)) != 0;
            dl->AddCircleFilled(ImVec2(pMin.x + (boxW * 0.25f) * (i + 1), pMax.y - 12.0f * S), 6.0f * S,
                                down ? IM_COL32(110, 255, 120, 255) : IM_COL32(255, 255, 255, 60), 12);
        }
        game_ui::Text(dl, 20.0f, ImVec2(pMin.x + 6.0f * S, pMax.y + 2.0f * S), game_ui::kTextDim, "Mouse", game_ui::Outline::Thin);
        x += boxW + gap;

        const size_t keys = raw.keys.count();
        const std::string held = "Keys held: " + std::to_string(keys);
        if (x + 150.0f * S < mx.x) {
            game_ui::Text(dl, 20.0f, ImVec2(x, top + 8.0f * S), game_ui::kTextNormal, held.c_str(), game_ui::Outline::Thin);
        }
    });
}

void DrawPreview(ListView& v, const AimSettings::Settings& s) {
    const DualAnalog::Config c = s.aim;
    v.Block(150.0f, [&](ImDrawList* dl, ImVec2 mn, ImVec2 mx) {
        const float S = game_ui::Scale();
        const float size = 116.0f * S;
        const float gap = 22.0f * S;
        const ImU32 bg = IM_COL32(8, 30, 62, 150);
        const ImU32 rim = IM_COL32(255, 255, 255, 110);
        const float top = mn.y + 2.0f * S;
        float x = mn.x + 4.0f * S;

        // Stick: deadzone and saturation rings, raw stick (white), shaped output (green).
        const ImVec2 boxMin(x, top);
        const ImVec2 boxMax(x + size, top + size);
        dl->AddRectFilled(boxMin, boxMax, bg, 14.0f * S);
        dl->AddRect(boxMin, boxMax, rim, 14.0f * S, 0, 1.5f);
        const ImVec2 centre(x + size * 0.5f, top + size * 0.5f);
        const float r = size * 0.5f - 8.0f * S;
        dl->AddCircle(centre, r, IM_COL32(255, 255, 255, 70), 48);
        dl->AddCircle(centre, r * c.radialDeadzone, IM_COL32(255, 168, 80, 230), 32, 1.5f);
        dl->AddCircle(centre, r * c.outerSaturation, IM_COL32(110, 200, 255, 230), 48, 1.5f);
        float ox = 0.f, oy = 0.f;
        DualAnalog::ApplyDeadzoneAndCurve(c, g_preview.rawX, g_preview.rawY, ox, oy);
        if (c.invertY) oy = -oy;
        dl->AddCircleFilled(ImVec2(centre.x + g_preview.rawX * r, centre.y + g_preview.rawY * r), 4.0f * S, IM_COL32(255, 255, 255, 255));
        dl->AddCircleFilled(ImVec2(centre.x + ox * r, centre.y + oy * r), 5.0f * S, IM_COL32(110, 255, 120, 255));
        game_ui::Text(dl, 20.0f, ImVec2(x + 6.0f * S, boxMax.y + 2.0f * S), game_ui::kTextDim, "Stick", game_ui::Outline::Thin);
        x += size + gap;

        // Pointer box (the picture area): where the reticle ends up.
        const float boxW = size * 1.35f;
        const ImVec2 pMin(x, top);
        const ImVec2 pMax(x + boxW, top + size);
        dl->AddRectFilled(pMin, pMax, IM_COL32(6, 20, 44, 190), 14.0f * S);
        dl->AddRect(pMin, pMax, rim, 14.0f * S, 0, 1.5f);
        const bool mouse = g_preview.arbiter.active == InputMap::AimSource::Mouse && s.mouse.enabled;
        const auto area = mouse ? c : DualAnalog::StickPointerConfig(c, RuntimeConfigFile::WiiSensorBarAbove());
        const float hx = (area.cameraMaxX - area.cameraMinX) * 0.5f * (mouse ? s.mouse.rangeX : 1.f);
        const float hy = (area.cameraMaxY - area.cameraMinY) * 0.5f * (mouse ? s.mouse.rangeY : 1.f);
        const float u = std::clamp(0.5f - (g_preview.pointer.x - DualAnalog::CenterX(area)) / (2.f * hx), 0.f, 1.f);
        const float vv = std::clamp(0.5f + (g_preview.pointer.y - DualAnalog::CenterY(area)) / (2.f * hy), 0.f, 1.f);
        const ImVec2 p(pMin.x + 10.0f * S + u * (boxW - 20.0f * S), pMin.y + 10.0f * S + vv * (size - 20.0f * S));
        const float arm = 9.0f * S;
        dl->AddCircle(p, arm * 0.8f, IM_COL32(255, 96, 96, 255), 20, 2.0f);
        dl->AddLine(ImVec2(p.x - arm * 1.5f, p.y), ImVec2(p.x + arm * 1.5f, p.y), IM_COL32(255, 96, 96, 255), 1.5f);
        dl->AddLine(ImVec2(p.x, p.y - arm * 1.5f), ImVec2(p.x, p.y + arm * 1.5f), IM_COL32(255, 96, 96, 255), 1.5f);
        game_ui::Text(dl, 20.0f, ImVec2(pMin.x + 6.0f * S, pMax.y + 2.0f * S), game_ui::kTextDim, "Reticle", game_ui::Outline::Thin);
        x += boxW + gap;

        // Response curve with the current deflection marked.
        const ImVec2 cMin(x, top);
        const ImVec2 cMax(x + size, top + size);
        dl->AddRectFilled(cMin, cMax, bg, 14.0f * S);
        dl->AddRect(cMin, cMax, rim, 14.0f * S, 0, 1.5f);
        const float inset = 10.0f * S;
        const float pw = size - inset * 2.0f;
        ImVec2 previous(cMin.x + inset, cMax.y - inset);
        for (int i = 1; i <= 64; ++i) {
            const float in = static_cast<float>(i) / 64.0f;
            const float out = DualAnalog::EvalCurve(c, DualAnalog::RemapMagnitude(c, in));
            const ImVec2 next(cMin.x + inset + in * pw, cMax.y - inset - out * pw);
            dl->AddLine(previous, next, IM_COL32(255, 226, 90, 255), 2.0f);
            previous = next;
        }
        const float mag = std::min(1.0f, std::sqrt(g_preview.rawX * g_preview.rawX + g_preview.rawY * g_preview.rawY));
        const float markY = DualAnalog::EvalCurve(c, DualAnalog::RemapMagnitude(c, mag));
        dl->AddCircleFilled(ImVec2(cMin.x + inset + mag * pw, cMax.y - inset - markY * pw), 5.0f * S, IM_COL32(110, 255, 120, 255));
        game_ui::Text(dl, 20.0f, ImVec2(cMin.x + 6.0f * S, cMax.y + 2.0f * S), game_ui::kTextDim, "Response", game_ui::Outline::Thin);
        x += size + gap;

        // Legend
        const float lx = x;
        const char* lines[] = {"White: raw stick", "Green: shaped", "Orange: deadzone", "Blue: full deflection"};
        const ImU32 colours[] = {IM_COL32(255, 255, 255, 255), IM_COL32(110, 255, 120, 255), IM_COL32(255, 168, 80, 255),
                                 IM_COL32(110, 200, 255, 255)};
        float ly = top + 8.0f * S;
        for (int i = 0; i < 4; ++i) {
            if (lx + 150.0f * S > mx.x) break;
            dl->AddCircleFilled(ImVec2(lx + 6.0f * S, ly + 10.0f * S), 5.0f * S, colours[i], 12);
            game_ui::Text(dl, 20.0f, ImVec2(lx + 20.0f * S, ly), game_ui::kTextNormal, lines[i], game_ui::Outline::Thin);
            ly += 30.0f * S;
        }
    });
}

// Position-based pad names: the Classic Controller's A / B sit in the east / south positions, which are the
// Xbox B / A buttons, so letters would mislead.
std::string FriendlyName(const InputMap::Binding& b) {
    if (b.device == InputMap::Device::Joy) {
        const int dev = InputMap::JoyDevice(b);
        const int local = InputMap::JoyLocalCode(b);
        if (dev < 0 || dev >= static_cast<int>(InputMap::kMaxJoysticks)) return "-";
        const std::string prefix = "Joy " + std::to_string(dev + 1) + " ";
        if (local >= 0 && local < static_cast<int>(InputMap::kMaxJoyButtons)) {
            return prefix + "Btn " + std::to_string(local + 1);
        }
        if (local >= InputMap::kJoyAxisOffset &&
            local < InputMap::kJoyAxisOffset + static_cast<int>(InputMap::kMaxJoyAxes) && b.dir != 0) {
            return prefix + "Axis " + std::to_string(local - InputMap::kJoyAxisOffset + 1) +
                   (b.dir > 0 ? " +" : " -");
        }
        if (local >= InputMap::kJoyHatOffset &&
            local < InputMap::kJoyHatOffset + static_cast<int>(InputMap::kMaxJoyHats * 4)) {
            static constexpr std::array<const char*, 4> kHatDirs = {"Up", "Right", "Down", "Left"};
            const int hat = (local - InputMap::kJoyHatOffset) / 4;
            const int dirIdx = (local - InputMap::kJoyHatOffset) % 4;
            return prefix + "Hat " + std::to_string(hat + 1) + " " + kHatDirs[static_cast<size_t>(dirIdx)];
        }
        return "-";
    }
    const std::string name = InputMap::Format(b);
    if (name.empty()) return "-";
    const size_t colon = name.find(':');
    if (colon == std::string::npos) return "-";
    const std::string kind = name.substr(0, colon);
    std::string rest = name.substr(colon + 1);
    if (rest.empty()) return "-";
    if (kind == "pad") {
        static const std::pair<const char*, const char*> kPadNames[] = {
            {"south", "A / Cross"}, {"east", "B / Circle"}, {"west", "X / Square"}, {"north", "Y / Triangle"},
            {"left_shoulder", "LB / L1"}, {"right_shoulder", "RB / R1"}, {"left_trigger", "LT / L2"},
            {"right_trigger", "RT / R2"}, {"start", "Start"}, {"back", "Back"}, {"guide", "Guide"},
            {"left_stick", "L3 (click)"}, {"right_stick", "R3 (click)"}, {"dpad_up", "D-pad up"},
            {"dpad_down", "D-pad down"}, {"dpad_left", "D-pad left"}, {"dpad_right", "D-pad right"},
        };
        for (const auto& entry : kPadNames) {
            if (rest == entry.first) return entry.second;
        }
    } else if (kind == "mouse") {
        static const std::pair<const char*, const char*> kMouseLabels[] = {
            {"left", "Left Click"}, {"right", "Right Click"}, {"middle", "Middle Click"},
            {"x1", "Mouse 4"}, {"x2", "Mouse 5"},
            {"wheel_up", "Wheel Up"}, {"wheel_down", "Wheel Down"},
        };
        for (const auto& entry : kMouseLabels) {
            if (rest == entry.first) return entry.second;
        }
    }
    std::replace(rest.begin(), rest.end(), '_', ' ');
    if (!rest.empty()) rest[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(rest[0])));
    return kind == "mouse" ? "Mouse " + rest : rest;
}
struct CaptureUi {
    bool open = false;
    size_t action = 0;
    size_t slot = 0;
    InputMap::Profile profile = InputMap::Profile::Gamepad;
    Clock::time_point deadline{};
    game_ui::ModalState modal;
} g_capture;

std::string g_status;
int g_profileIndex = 0;
bool g_profileManual = false;
bool g_showKbmBindings = false;

void ApplyCapture(const InputMap::Binding& binding) {
    AimSettings::Settings s = AimSettings::Get();
    auto& table = s.bindings.For(g_capture.profile);
    const auto swapped = InputMap::AssignSwap(table, g_capture.action, g_capture.slot, binding);
    g_status = swapped ? std::string("Swapped with \"") + AimSettings::kActions[*swapped].label + "\"" : std::string();
    AimSettings::Set(s);
    AimSettings::Save();
}

void BuildBindings(ListView& v, AimSettings::Settings& s, Edit& e, InputMap::Profile profile) {
    const bool pad = profile == InputMap::Profile::Gamepad;
    v.Section(pad ? "Gamepad / Joystick bindings" : "Keyboard and mouse bindings");
    v.Note("Up to three bindings per action. Confirm or click rebinds the selected slot. Click the left stick, press Delete or right-click to clear. Conflicting bindings are swapped. X applies the page.");
    auto& table = s.bindings.For(profile);
    if (const size_t conflicts = InputMap::CountConflicts(table)) {
        v.Info("Conflicts", std::to_string(conflicts) + " duplicate binding(s) in this profile", game_ui::kWarn);
    }
    if (!g_status.empty()) v.Info("Last change", g_status, game_ui::kWarn);

    const char* group = "";
    for (size_t a = 0; a < kActionCount; ++a) {
        const AimSettings::ActionInfo& info = AimSettings::kActions[a];
        if (std::string(info.group) != group) {
            group = info.group;
            v.Section(group);
        }
        std::array<std::string, 3> names;
        for (size_t slot = 0; slot < InputMap::kSlots; ++slot) names[slot] = FriendlyName(table[a][slot]);
        int cleared = -1;
        const int fired = v.Bindings(info.label, names, info.detail[0] != '\0' ? info.detail : "Confirm rebinds the slot; left-stick click clears it.", &cleared);
        if (fired >= 0) {
            g_capture.open = true;
            g_capture.action = a;
            g_capture.slot = static_cast<size_t>(fired);
            g_capture.profile = profile;
            g_capture.deadline = Clock::now() + std::chrono::seconds(8);
            g_capture.modal = game_ui::ModalState{};
            AimInput::BeginCapture(profile);
        }
        if (cleared >= 0) {
            InputMap::AssignSwap(table, a, static_cast<size_t>(cleared), InputMap::Binding{});
            g_status.clear();
            e.dirty = e.save = true;
        }
    }
    v.Section("Reset");
    if (v.Action(pad ? "Reset gamepad / joystick bindings" : "Reset keyboard and mouse bindings", "Reset...",
                 "Restores every binding of this device to its default.")) {
        game_ui::RequestConfirm("Reset bindings", "Restore the default bindings for this device?", "Reset", [profile] {
            AimSettings::Settings cur = AimSettings::Get();
            const AimSettings::Settings defaults = AimSettings::DefaultBindings();
            cur.bindings.For(profile) = defaults.bindings.For(profile);
            g_status.clear();
            AimSettings::Set(cur);
            AimSettings::Save();
        });
    }
}

// ---- Pages -------------------------------------------------------------------------------------------

// Advanced tuning rows stay hidden until asked for (per session, not saved).
bool g_showAdvanced = false;

void AdvancedToggle(ListView& v) {
    v.Toggle("Show advanced tuning", &g_showAdvanced, false, "Reveals fine-tuning options. Most players never need them.");
}

void ControlSchemeChoice(ListView& v, AimSettings::Settings& s, Edit& e) {
    int style = s.schemeMode == InputMap::SchemeMode::KeyboardMouse ? 1
              : s.schemeMode == InputMap::SchemeMode::Classic ? 2 : 0;
    static const char* const kStyles[] = {"Automatic (Wii pointer)", "M+KB", "Classic Controller"};
    if (auto r = v.Choice("Control scheme", &style, kStyles, 3, 0,
                          "Automatic (Wii pointer): the right stick and mouse move the reticle freely (recommended). "
                          "M+KB: locks controls to mouse and keyboard. "
                          "Classic Controller: the game's original stick aiming.")) {
        if (style == 1) {
            s.schemeMode = InputMap::SchemeMode::KeyboardMouse;
            s.mouse.enabled = true;
            if (s.buttonPromptStyle == AimSettings::ButtonPromptStyle::Xbox ||
                s.buttonPromptStyle == AimSettings::ButtonPromptStyle::PlayStation) {
                s.buttonPromptStyle = AimSettings::ButtonPromptStyle::Keyboard;
            }
        } else if (style == 2) {
            s.schemeMode = InputMap::SchemeMode::Classic;
        } else {
            s.schemeMode = InputMap::SchemeMode::Auto;
        }
        e.Take(r);
    }
}

void ButtonPromptChoice(ListView& v, AimSettings::Settings& s, Edit& e) {
    int prompts = static_cast<int>(s.buttonPromptStyle);
    if (auto r = v.Choice("Button icons", &prompts, AimSettings::kButtonPromptLabels.data(),
                          static_cast<int>(AimSettings::kButtonPromptLabels.size()), 0,
                          "Icons used in game prompts and menu hints. Keyboard reflects your remapped keys; "
                          "Original Wii keeps the game's original Wii button prompts.")) {
        s.buttonPromptStyle = static_cast<AimSettings::ButtonPromptStyle>(prompts);
        e.Take(r);
    }
}

void BuildGamepadPage(ListView& v, AimSettings::Settings& s, Edit& e) {
    DualAnalog::Config& c = s.aim;
    const DualAnalog::Config d;
    UpdatePreview(s);
    v.Section("Aiming");
    ControlSchemeChoice(v, s, e);
    e.Take(v.Slider("Aim sensitivity", &c.sensitivity, 0.2f, 3.0f, 0.05f, "%.2fx", d.sensitivity,
                    "Reticle speed. At 1.00 full tilt crosses the screen in about a third of a second."));
    e.Take(v.Toggle("Invert vertical aim", &c.invertY, d.invertY, "Push up to aim down."));
    DrawPreview(v, s);
    AdvancedToggle(v);
    if (g_showAdvanced) {
        const uint32_t in = game_ui::kRowIndent;
        e.Take(v.Slider("Aim deadzone", &c.radialDeadzone, 0.0f, 0.4f, 0.01f, "%.2f", d.radialDeadzone,
                        "Right stick travel ignored around the centre. Raise it if the reticle drifts.", in));
        e.Take(v.Slider("Aim acceleration", &c.curveExponent, 1.0f, 4.0f, 0.1f, "%.1f", d.curveExponent,
                        "1 = linear. Higher = finer control near the centre; full-tilt speed stays the same.", in));
        e.Take(v.Slider("Vertical aim speed", &c.verticalRatio, 0.5f, 2.0f, 0.05f, "%.2fx", d.verticalRatio,
                        "Vertical speed relative to horizontal.", in));
        e.Take(v.Slider("Full-tilt point", &c.outerSaturation, 0.6f, 1.0f, 0.01f, "%.2f", d.outerSaturation,
                        "Stick travel that counts as full tilt. Lower it if your stick never reaches top speed.", in));
        e.Take(v.Slider("Move deadzone", &c.leftDeadzone, 0.0f, 0.4f, 0.01f, "%.2f", d.leftDeadzone,
                        "Left stick travel ignored around the centre. Raise it if your character drifts.", in));
        e.Take(v.Slider("Trigger press point", &c.triggerThreshold, 0.1f, 0.9f, 0.01f, "%.2f", d.triggerThreshold,
                        "How far LT / RT must be pulled to count as pressed.", in));
    }

    v.Section("Gamepad");
    ButtonPromptChoice(v, s, e);
    if (v.Action("Recommended layout", "Restore",
                 "RT shoot, LT lock on, RB jump, LB evade, left stick move, right stick aim. Keyboard and mouse keep theirs.")) {
        game_ui::RequestConfirm("Recommended layout", "Replace your gamepad buttons with the recommended layout?", "Restore", [] {
            auto cur = AimSettings::Get();
            cur.bindings.pad = AimSettings::DefaultBindings().bindings.pad;
            cur.schemeMode = AimSettings::SchemeMode::Auto;
            AimSettings::Set(cur);
            AimSettings::Save();
        });
    }
    v.Note("Left stick moves, right stick aims. RT shoots (tap to slash), LT locks on (hold for a charge shot), "
           "RB jumps, LB evades, Start pauses, Back switches character, L3 recenters the reticle.");
}

void BuildGyroPage(ListView& v, AimSettings::Settings& s, Edit& e) {
    GyroAim::Config& g = s.gyro;
    const GyroAim::Config d;
    UpdatePreview(s);
    e.Take(v.Toggle("Gyro aiming", &g.enabled, d.enabled,
                    "Tilt the controller to aim. Needs a controller with motion sensors (DualSense, DualShock 4, Switch)."));
    v.Info("Status", AimInput::GyroStatus(), g.enabled ? game_ui::kGood : game_ui::kTextDim);
    float sensitivity = g.sensitivityX;
    if (auto r = v.Slider("Gyro sensitivity", &sensitivity, 1.f, 50.f, 0.5f, "%.1f", d.sensitivityX,
                          "Reticle movement per degree of rotation. 12 = full screen width in about 37 degrees.")) {
        g.sensitivityX = g.sensitivityY = sensitivity;
        e.Take(r);
    }
    static const char* const kButton[] = {"Not used", "Hold to aim", "Hold to pause"};
    e.Take(v.Choice("Gyro button", &g.activation, kButton, 3, d.activation,
                    "Hold to pause lets you lift and re-center the controller (ratchet). The button is R3 by default."));
    e.Take(v.Toggle("Invert vertical gyro", &g.invertY, d.invertY, "Tilt up to aim down."));
    if (v.Action("Calibrate", "Calibrate", "Rest the controller on a flat surface for a few seconds. Fixes drift.")) {
        AimInput::CalibrateGyro();
    }
    DrawPreview(v, s);
    AdvancedToggle(v);
    if (g_showAdvanced) {
        const uint32_t in = game_ui::kRowIndent;
        static const char* const kSpaces[] = {"Player space", "Yaw (turn)", "Roll (lean)"};
        e.Take(v.Choice("Gyro axes", &g.space, kSpaces, 3, d.space,
                        "Player space works however you hold the controller. Yaw and Roll use one controller axis.", in));
        e.Take(v.Toggle("Invert horizontal gyro", &g.invertX, d.invertX, "Turn right to aim left.", in));
    }
    v.Note("The right stick still works for big moves while gyro adds fine aim. Gyro uses the Wii pointer aiming style.");
}

void BuildButtonsPage(ListView& v, AimSettings::Settings& s, Edit& e) {
    if (!g_profileManual) {
        g_profileIndex = s.schemeMode == InputMap::SchemeMode::KeyboardMouse ? 1 : 0;
    }
    static const char* const kDevices[] = {"Gamepad / Joystick (HOTAS)", "Keyboard & mouse"};
    if (v.Choice("Device", &g_profileIndex, kDevices, 2, 0, "Which device's buttons to change.")) {
        g_profileManual = true;
    }
    v.Section("Live input");
    DrawDeviceMonitor(v);
    const auto joyDevices = GenericJoystick::ConnectedDevices();
    if (!joyDevices.empty()) {
        const InputMap::RawSample raw = AimInput::Sample();
        v.Section("Connected joysticks / HOTAS");
        for (const auto& dev : joyDevices) {
            bool active = false;
            if (dev.slot >= 0 && dev.slot < static_cast<int>(InputMap::kMaxJoysticks)) {
                const auto& rj = raw.joys[static_cast<size_t>(dev.slot)];
                if (rj.connected) {
                    active = rj.buttons.any();
                    for (uint8_t h : rj.hats) if (h != 0) active = true;
                    for (float a : rj.axes) if (std::fabs(a) >= 0.35f) active = true;
                }
            }
            const std::string label = "Joy " + std::to_string(dev.slot + 1);
            std::string info = dev.name + " (" + std::to_string(dev.numButtons) + " btn, " +
                               std::to_string(dev.numAxes) + " axis, " + std::to_string(dev.numHats) + " hat)";
            if (active) info += "  [ACTIVE]";
            v.Info(label.c_str(), info, active ? game_ui::kGood : game_ui::kTextNormal);
        }
    }
    BuildBindings(v, s, e, g_profileIndex == 1 ? InputMap::Profile::KeyboardMouse : InputMap::Profile::Gamepad);
}

void BuildMousePage(ListView& v, AimSettings::Settings& s, Edit& e) {
    AimSettings::MouseConfig& m = s.mouse;
    const AimSettings::MouseConfig d{};
    UpdatePreview(s);
    v.Section("Mouse & keyboard");
    ControlSchemeChoice(v, s, e);
    ButtonPromptChoice(v, s, e);
    e.Take(v.Toggle("Mouse aiming", &m.enabled, d.enabled, "The mouse moves the reticle. Whichever device moved last wins."));
    int mode = m.raw ? 1 : 0;
    static const char* const kModes[] = {"Follow cursor", "Captured"};
    if (auto r = v.Choice("Mouse mode", &mode, kModes, 2, 0,
                          "Follow cursor: the reticle sits under the mouse cursor. Captured: the cursor is hidden and mouse motion moves the reticle.")) {
        m.raw = mode == 1;
        e.Take(r);
    }
    if (m.raw) {
        e.Take(v.Slider("Mouse sensitivity", &m.sensitivity, 0.1f, 4.0f, 0.05f, "%.2f", d.sensitivity,
                        "Reticle movement per mouse count.", game_ui::kRowIndent));
    }
    DrawPreview(v, s);
    AdvancedToggle(v);
    if (g_showAdvanced) {
        const uint32_t in = game_ui::kRowIndent;
        if (!m.raw) {
            e.Take(v.Slider("Cursor smoothing", &m.smoothingMs, 0.0f, 150.0f, 5.0f, "%.0f ms", d.smoothingMs,
                            "0 follows the cursor exactly.", in));
            e.Take(v.Slider("Horizontal reach", &m.rangeX, 0.5f, 1.5f, 0.01f, "%.2f", d.rangeX,
                            "Adjust if the reticle does not line up with the cursor.", in));
            e.Take(v.Slider("Vertical reach", &m.rangeY, 0.5f, 1.5f, 0.01f, "%.2f", d.rangeY,
                            "Adjust if the reticle does not line up with the cursor.", in));
        }
        e.Take(v.Slider("Mouse speed (Classic style)", &m.stickSpeed, 300.0f, 6000.0f, 100.0f, "%.0f", d.stickSpeed,
                        "With the Classic Controller aiming style the mouse acts as the right stick: mouse speed that equals full tilt.", in));
    }
    v.Note("Menus: left click selects, Esc or Backspace goes back. Combat: WASD moves, left click shoots, "
           "right click locks on, Space jumps, Shift evades, R recenters. Alt+Enter toggles fullscreen, F10 opens this menu.");
    v.Section("Remapping");
    v.Toggle("Show keyboard & mouse bindings", &g_showKbmBindings, false,
             "Remap every keyboard key and mouse button directly here (also available on the Buttons page).");
    if (g_showKbmBindings) {
        BuildBindings(v, s, e, InputMap::Profile::KeyboardMouse);
    }
}

} // namespace

const char* PageName(int page) { return page >= 0 && page < kPageCount ? kPageNames[static_cast<size_t>(page)] : ""; }

void BuildPage(ListView& view, int page) {
    AimSettings::Settings s = AimSettings::Get();
    Edit e;
    switch (page) {
    case 0: BuildGamepadPage(view, s, e); break;
    case 1: BuildGyroPage(view, s, e); break;
    case 2: BuildButtonsPage(view, s, e); break;
    default: BuildMousePage(view, s, e); break;
    }
    if (e.dirty) AimSettings::Set(s);
    if (e.save) AimSettings::Save();
}

// Brief note at the bottom centre when Automatic switches scheme. Called once per frame from the overlay.
void DrawHud() {
    static uint32_t seen = AimInput::LatestSwitch().serial;
    static Clock::time_point shown{};
    static bool gamepad = false;
    static bool active = false;
    const AimInput::SchemeNotice n = AimInput::LatestSwitch();
    if (n.serial != seen) {
        seen = n.serial;
        if (AimSettings::Get().switchToast) {
            active = true;
            shown = Clock::now();
            gamepad = AimInput::LastDevice() == InputMap::ActiveDevice::Gamepad;
        }
    }
    if (!active) return;
    constexpr float kHold = 2.0f, kFadeIn = 0.15f, kFadeOut = 0.5f;
    const float age = std::chrono::duration<float>(Clock::now() - shown).count();
    if (age >= kHold + kFadeOut) {
        active = false;
        return;
    }
    const float alpha = age < kFadeIn ? age / kFadeIn : age < kHold ? 1.0f : 1.0f - (age - kHold) / kFadeOut;
    const char* text = gamepad ? "Gamepad pointer active" : "Mouse and keyboard active";
    const ImGuiIO& io = ImGui::GetIO();
    const float S = game_ui::Scale();
    const float w = game_ui::TextSize(22.0f, text).x + 24.0f * S;
    const ImVec2 anchor(io.DisplaySize.x * 0.5f + w * 0.5f, io.DisplaySize.y - 90.0f * S);
    game_ui::DrawHudBox(ImGui::GetForegroundDrawList(), anchor, {{text, game_ui::kWhite}}, alpha);
}

void DrawModals(game_ui::Frame& input) {
    if (!g_capture.open) return;
    const AimSettings::ActionInfo& info = AimSettings::kActions[g_capture.action];
    const bool pad = g_capture.profile == InputMap::Profile::Gamepad;
    const float remaining = std::chrono::duration<float>(g_capture.deadline - Clock::now()).count();
    game_ui::ModalSpec spec;
    spec.title = std::string("Bind: ") + info.label;
    spec.lines = {pad ? "Press a gamepad/joystick button, pull a trigger, move an axis or press a POV hat." : "Press a key, click a mouse button or scroll the wheel.",
                  pad ? "Release everything first. Use the dialog buttons to clear or cancel." : "Escape and Delete can be bound. F10 is reserved. Use Clear / Cancel below or wait for the timeout."};
    spec.buttons = {"Clear binding", "Cancel"};
    spec.progress = std::clamp(remaining / 8.0f, 0.0f, 1.0f);
    spec.mouseOnly = true;
    spec.hint = "Cancels in " + std::to_string(std::max(0, static_cast<int>(std::ceil(remaining)))) + " s";
    const int result = game_ui::DrawModal(g_capture.modal, input, spec);
    InputMap::Binding none;
    if (result == 0) {
        ApplyCapture(none);
        g_capture.open = false;
        AimInput::CancelCapture();
    } else if (result == 1 || remaining <= 0.0f) {
        g_capture.open = false;
        AimInput::CancelCapture();
    } else {
        const AimInput::CaptureResult captured = AimInput::PollCapture(game_ui::ModalHot());
        if (captured.state == AimInput::CaptureState::Done) {
            ApplyCapture(captured.binding);
            g_capture.open = false;
        } else if (captured.state == AimInput::CaptureState::Cleared) {
            ApplyCapture(none);
            g_capture.open = false;
        } else if (captured.state == AimInput::CaptureState::Cancelled) {
            g_capture.open = false;
        }
    }
    if (!g_capture.open) {
        g_capture.modal = game_ui::ModalState{};
        game_ui::Suppress(0.2f);
    }
}


bool Capturing() { return g_capture.open || AimInput::Capturing(); }

void ResetPage(int page) {
    AimSettings::Settings s = AimSettings::Get();
    switch (page) {
    case 0:
        s.aim = DualAnalog::ModernAim();
        s.schemeMode = InputMap::SchemeMode::Auto;
        s.buttonPromptStyle = AimSettings::ButtonPromptStyle::Xbox;
        break;
    case 1:
        // While gyro stays enabled, Sanitize also keeps the aiming style on the Wii pointer.
        s.gyro = GyroAim::Config{};
        break;
    case 2:
        s.bindings.For(g_profileIndex == 1 ? InputMap::Profile::KeyboardMouse : InputMap::Profile::Gamepad) =
            AimSettings::DefaultBindings().bindings.For(g_profileIndex == 1 ? InputMap::Profile::KeyboardMouse
                                                                              : InputMap::Profile::Gamepad);
        g_status.clear();
        break;
    default:
        s.mouse = AimSettings::MouseConfig{};
        break;
    }
    AimSettings::Set(s);
    AimSettings::Save();
}

void ResetAll() {
    g_status.clear();
    AimSettings::ResetAllToDefaults();
    AimSettings::Save();
}

} // namespace controls_menu

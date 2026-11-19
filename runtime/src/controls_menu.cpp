#include "controls_menu.h"

#include "aim_input.h"
#include "aim_settings.h"
#include "dual_analog.h"
#include "input_map.h"
#include "runtime_config.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace controls_menu {
namespace {

using AimSettings::Action;
using AimSettings::kActionCount;
using Clock = std::chrono::steady_clock;

struct Edit {
    bool dirty = false;  // push to the live settings
    bool save = false;   // write Config.toml
};

void Tip(const char* help) {
    if (help != nullptr && *help != '\0' && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
}

bool Slider(Edit& e, const char* label, float* v, float lo, float hi, const char* fmt, const char* help) {
    ImGui::SetNextItemWidth(220.0f);
    const bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (changed) e.dirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) e.save = true;
    Tip(help);
    return changed;
}

bool Check(Edit& e, const char* label, bool* v, const char* help) {
    const bool changed = ImGui::Checkbox(label, v);
    if (changed) e.dirty = e.save = true;
    Tip(help);
    return changed;
}

template <size_t N>
bool Combo(Edit& e, const char* label, int* index, const std::array<const char*, N>& items, const char* help) {
    ImGui::SetNextItemWidth(260.0f);
    const bool changed = ImGui::Combo(label, index, items.data(), static_cast<int>(N));
    if (changed) e.dirty = e.save = true;
    Tip(help);
    return changed;
}

// Two presses within three seconds: a cheap confirmation that works with every input device.
bool ConfirmButton(const char* label, const char* id) {
    static std::string armedId;
    static Clock::time_point armedAt{};
    const bool armed = armedId == id && Clock::now() - armedAt < std::chrono::seconds(3);
    const std::string text = armed ? std::string("Really reset? Press again##") + id : std::string(label) + "##" + id;
    if (ImGui::Button(text.c_str())) {
        if (armed) {
            armedId.clear();
            return true;
        }
        armedId = id;
        armedAt = Clock::now();
    }
    return false;
}

// ---- Live preview ------------------------------------------------------------------------------

struct Preview {
    DualAnalog::PointerState pointer;
    DualAnalog::StickFilter filter;
    InputMap::Arbiter arbiter;
    float lastMouseX = 0.f, lastMouseY = 0.f;
    bool haveMouse = false;
    float mouseU = 0.5f, mouseV = 0.5f;
    float rawX = 0.f, rawY = 0.f;
} g_preview;

float ActionValue(const AimSettings::Settings& s, Action a, const InputMap::RawSample& raw) {
    const size_t i = static_cast<size_t>(a);
    return std::max(InputMap::SlotsValue(s.bindings.pad[i], raw), InputMap::SlotsValue(s.bindings.kbm[i], raw));
}

void AimVector(const AimSettings::Settings& s, const InputMap::RawSample& raw, float& x, float& y) {
    x = ActionValue(s, Action::AimRight, raw) - ActionValue(s, Action::AimLeft, raw);
    y = ActionValue(s, Action::AimDown, raw) - ActionValue(s, Action::AimUp, raw);
    const float m = std::sqrt(x * x + y * y);
    if (m > 1.f) {
        x /= m;
        y /= m;
    }
}

// Mirrors the virtual remote's arbitration with the same pure functions; the real remote is input-blocked
// while the menu is open, so the preview runs its own copy.
InputMap::AimSource UpdatePreview(const AimSettings::Settings& s, bool precision) {
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
            const DualAnalog::Rect rect = DualAnalog::ContentRect(
                ms.winW, ms.winH, RuntimeConfigFile::ForceAspectEnabled(),
                RuntimeConfigFile::WidescreenEnabled() ? 16.0f / 9.0f : 4.0f / 3.0f);
            float u = 0.5f, v = 0.5f, tx = g_preview.pointer.x, ty = g_preview.pointer.y;
            DualAnalog::WindowToNormalized(rect, ms.x, ms.y, u, v);
            DualAnalog::NormalizedToCamera(s.aim, u, v, s.mouse.rangeX, s.mouse.rangeY, tx, ty);
            g_preview.pointer.x = DualAnalog::SmoothToward(g_preview.pointer.x, tx, dt, s.mouse.smoothingMs);
            g_preview.pointer.y = DualAnalog::SmoothToward(g_preview.pointer.y, ty, dt, s.mouse.smoothingMs);
        }
    } else {
        DualAnalog::Advance(g_preview.pointer, s.aim, -g_preview.rawX, g_preview.rawY, dt, precision);
    }
    return g_preview.arbiter.active;
}

void DrawAimDevice(InputMap::AimSource source, bool mouseEnabled) {
    const bool mouse = source == InputMap::AimSource::Mouse && mouseEnabled;
    ImGui::TextUnformatted("Active aim device:");
    ImGui::SameLine();
    ImGui::TextColored(mouse ? ImVec4(0.4f, 0.8f, 1.0f, 1.0f) : ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s",
                       mouse ? "Mouse" : "Right stick / aim keys");
    Tip("Whichever device started moving last drives the aim. Move the other one to take over.");
}

void DrawPreview(const AimSettings::Settings& s) {
    const DualAnalog::Config& c = s.aim;
    const float size = 120.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Stick box: deadzone and saturation rings, raw stick (white), shaped output (green).
    const ImVec2 centre(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float r = size * 0.5f - 2.0f;
    dl->AddRect(origin, ImVec2(origin.x + size, origin.y + size), IM_COL32(120, 120, 120, 255));
    dl->AddCircle(centre, r, IM_COL32(90, 90, 90, 255), 48);
    dl->AddCircle(centre, r * c.radialDeadzone, IM_COL32(200, 120, 60, 255), 32);
    dl->AddCircle(centre, r * c.outerSaturation, IM_COL32(60, 160, 200, 255), 48);
    float ox = 0.f, oy = 0.f;
    DualAnalog::ApplyDeadzoneAndCurve(c, g_preview.rawX, g_preview.rawY, ox, oy);
    if (c.invertY) oy = -oy;
    dl->AddCircleFilled(ImVec2(centre.x + g_preview.rawX * r, centre.y + g_preview.rawY * r), 3.0f,
                        IM_COL32(255, 255, 255, 255));
    dl->AddCircleFilled(ImVec2(centre.x + ox * r, centre.y + oy * r), 4.0f, IM_COL32(80, 255, 80, 255));

    // Pointer box (the picture area): where the reticle ends up.
    const ImVec2 box(origin.x + size + 16.0f, origin.y);
    const float boxW = 160.0f, boxH = 120.0f;
    dl->AddRectFilled(box, ImVec2(box.x + boxW, box.y + boxH), IM_COL32(20, 20, 30, 255));
    dl->AddRect(box, ImVec2(box.x + boxW, box.y + boxH), IM_COL32(120, 120, 120, 255));
    const float hx = (c.cameraMaxX - c.cameraMinX) * 0.5f * s.mouse.rangeX;
    const float hy = (c.cameraMaxY - c.cameraMinY) * 0.5f * s.mouse.rangeY;
    const float u = std::clamp(0.5f - (g_preview.pointer.x - DualAnalog::CenterX(c)) / (2.0f * hx), 0.f, 1.f);
    const float v = std::clamp(0.5f + (g_preview.pointer.y - DualAnalog::CenterY(c)) / (2.0f * hy), 0.f, 1.f);
    const ImVec2 p(box.x + u * boxW, box.y + v * boxH);
    dl->AddLine(ImVec2(p.x - 6, p.y), ImVec2(p.x + 6, p.y), IM_COL32(255, 80, 80, 255));
    dl->AddLine(ImVec2(p.x, p.y - 6), ImVec2(p.x, p.y + 6), IM_COL32(255, 80, 80, 255));

    // Response curve plot with the current deflection marked.
    const ImVec2 plot(box.x + boxW + 16.0f, origin.y);
    const float plotW = 120.0f, plotH = 120.0f;
    dl->AddRect(plot, ImVec2(plot.x + plotW, plot.y + plotH), IM_COL32(120, 120, 120, 255));
    ImVec2 previous(plot.x, plot.y + plotH);
    for (int i = 1; i <= 64; ++i) {
        const float in = static_cast<float>(i) / 64.0f;
        const float out = DualAnalog::EvalCurve(c, DualAnalog::RemapMagnitude(c, in));
        const ImVec2 next(plot.x + in * plotW, plot.y + plotH - out * plotH);
        dl->AddLine(previous, next, IM_COL32(255, 220, 80, 255), 1.5f);
        previous = next;
    }
    const float mag = std::min(1.0f, std::sqrt(g_preview.rawX * g_preview.rawX + g_preview.rawY * g_preview.rawY));
    const float markY = DualAnalog::EvalCurve(c, DualAnalog::RemapMagnitude(c, mag));
    dl->AddCircleFilled(ImVec2(plot.x + mag * plotW, plot.y + plotH - markY * plotH), 3.0f, IM_COL32(80, 255, 80, 255));

    ImGui::Dummy(ImVec2(size + boxW + plotW + 32.0f, size));
    ImGui::TextDisabled("Stick (raw white, shaped green)   Reticle position   Response curve");
}

// ---- Pages -------------------------------------------------------------------------------------

void DrawAiming(AimSettings::Settings& s, Edit& e) {
    DualAnalog::Config& c = s.aim;
    const InputMap::AimSource source = UpdatePreview(s, false);
    DrawAimDevice(source, s.mouse.enabled);
    DrawPreview(s);

    ImGui::SeparatorText("Right stick");
    int dz = static_cast<int>(c.deadzoneMode);
    if (Combo(e, "Deadzone type", &dz, DualAnalog::kDeadzoneModeLabels,
              "Scaled radial: no jump at the edge (recommended).\nRadial: hard cut.\nAxial: each axis on its own.")) {
        c.deadzoneMode = static_cast<DualAnalog::DeadzoneMode>(dz);
    }
    Slider(e, "Inner deadzone", &c.radialDeadzone, 0.0f, 0.5f, "%.2f", "Stick travel ignored around the centre. Default 0.12.");
    Slider(e, "Outer saturation", &c.outerSaturation, 0.5f, 1.0f, "%.2f",
           "Deflection that counts as full. Lower it if your stick never reaches 1. Default 0.95.");
    int curve = static_cast<int>(c.curve);
    if (Combo(e, "Response curve", &curve, DualAnalog::kCurveLabels, "How deflection turns into speed. Default Linear.")) {
        c.curve = static_cast<DualAnalog::Curve>(curve);
    }
    if (c.curve == DualAnalog::Curve::Power) {
        Slider(e, "Exponent", &c.curveExponent, 0.3f, 4.0f, "%.2f", "Below 1 = faster near the centre, above 1 = finer control. Default 1.6.");
    } else if (c.curve == DualAnalog::Curve::DualZone) {
        Slider(e, "Precision zone size", &c.dualZoneSplit, 0.1f, 0.9f, "%.2f", "Deflection where the precision zone ends. Default 0.50.");
        Slider(e, "Precision zone output", &c.dualZoneOutput, 0.05f, 0.9f, "%.2f", "Output reached at the end of the zone. Default 0.25.");
    }
    Slider(e, "Sensitivity X", &c.sensitivityX, 0.1f, 4.0f, "%.2f", "Horizontal gain. Default 1.00.");
    Slider(e, "Sensitivity Y", &c.sensitivityY, 0.1f, 4.0f, "%.2f", "Vertical gain. Default 1.00.");
    Check(e, "Invert Y", &c.invertY, "Push up to aim down.");
    Slider(e, "Smoothing", &c.smoothingMs, 0.0f, 150.0f, "%.0f ms", "Low-pass filter on the stick. 0 = off.");
    Slider(e, "Precision scale", &c.precisionScale, 0.1f, 1.0f, "%.2f", "Speed factor while the Precision control is held. Default 0.35.");

    ImGui::SeparatorText("Pointer (Wii Remote scheme)");
    int mode = c.absoluteMode ? 1 : 0;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("Stick pointer mode", &mode, "Velocity (like a mouse)\0Absolute (stick = position)\0")) {
        c.absoluteMode = mode == 1;
        e.dirty = e.save = true;
    }
    Tip("Velocity: deflection sets speed, release holds the position. Absolute: stick position is pointer position.");
    float maxSpeed = c.maxSpeedX;
    if (Slider(e, "Max speed", &maxSpeed, 100.0f, 3000.0f, "%.0f px/s", "Camera pixels per second at full deflection. Default 900.")) {
        c.maxSpeedX = maxSpeed;
        c.maxSpeedY = maxSpeed * (700.0f / 900.0f);
    }
    Slider(e, "Speed cap", &c.speedCap, 200.0f, 4000.0f, "%.0f px/s", "Upper limit after ramp-up and edge boost. Default 1600.");
    float rampMs = c.rampSeconds * 1000.0f;
    if (Slider(e, "Ramp-up time", &rampMs, 0.0f, 500.0f, "%.0f ms", "Slow start after leaving the deadzone. Default 120.")) {
        c.rampSeconds = rampMs * 0.001f;
    }
    Slider(e, "Edge boost", &c.edgeBoost, 1.0f, 3.0f, "x%.2f", "Extra speed while the stick is held at the edge. 1 = off.");
    float edgeMs = c.edgeAccelSeconds * 1000.0f;
    if (Slider(e, "Edge boost time", &edgeMs, 50.0f, 2000.0f, "%.0f ms", "How quickly the boost builds. Default 400.")) {
        c.edgeAccelSeconds = edgeMs * 0.001f;
    }
    Check(e, "Recenter on release", &c.recenterOnRelease, "Spring the pointer back to the centre when the stick is released.");
    if (c.recenterOnRelease) {
        float springMs = c.recenterSeconds * 1000.0f;
        if (Slider(e, "Recenter time", &springMs, 30.0f, 2000.0f, "%.0f ms", "Default 250.")) {
            c.recenterSeconds = springMs * 0.001f;
        }
    }

    ImGui::SeparatorText("Left stick");
    Slider(e, "Move deadzone", &c.leftDeadzone, 0.0f, 0.5f, "%.2f", "Default 0.12.");
}

std::string FriendlyName(const InputMap::Binding& b) {
    std::string name = InputMap::Format(b);
    if (name.empty()) return "-";
    const size_t colon = name.find(':');
    std::string kind = name.substr(0, colon);
    std::string rest = name.substr(colon + 1);
    std::replace(rest.begin(), rest.end(), '_', ' ');
    kind = kind == "pad" ? "Pad" : kind == "mouse" ? "Mouse" : "Key";
    return kind + " " + rest;
}

struct CaptureUi {
    bool open = false;
    bool openPopup = false;
    size_t action = 0;
    size_t slot = 0;
    InputMap::Profile profile = InputMap::Profile::Gamepad;
    Clock::time_point deadline{};
} g_capture;

std::string g_status;

void ApplyCapture(AimSettings::Settings& s, Edit& e, const InputMap::Binding& binding) {
    auto& table = s.bindings.For(g_capture.profile);
    const auto swapped = InputMap::AssignSwap(table, g_capture.action, g_capture.slot, binding);
    g_status = swapped ? std::string("Swapped with \"") + AimSettings::kActions[*swapped].label + "\"" : std::string();
    e.dirty = e.save = true;
}

void DrawCapturePopup(AimSettings::Settings& s, Edit& e) {
    if (g_capture.openPopup) {
        ImGui::OpenPopup("Bind input");
        g_capture.openPopup = false;
    }
    bool open = g_capture.open;
    if (!ImGui::BeginPopupModal("Bind input", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_capture.open) {
            g_capture.open = false;
            AimInput::CancelCapture();
        }
        return;
    }
    const AimSettings::ActionInfo& info = AimSettings::kActions[g_capture.action];
    const bool pad = g_capture.profile == InputMap::Profile::Gamepad;
    ImGui::Text("Bind: %s", info.label);
    ImGui::TextUnformatted(pad ? "Press a gamepad button, pull a trigger or push a stick."
                               : "Press a key, click a mouse button or scroll the wheel.");
    ImGui::TextUnformatted(pad ? "Release everything first." : "Esc cancels, Delete clears.");
    const float remaining = std::chrono::duration<float>(g_capture.deadline - Clock::now()).count();
    ImGui::Text("Cancels in %d s", std::max(0, static_cast<int>(std::ceil(remaining))));
    const bool clear = ImGui::Button("Clear binding");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    const bool overUi = ImGui::IsAnyItemHovered();
    InputMap::Binding none;
    if (g_capture.open) {
        if (clear) {
            ApplyCapture(s, e, none);
            g_capture.open = false;
            AimInput::CancelCapture();
        } else if (cancel || !open || remaining <= 0.0f) {
            g_capture.open = false;
            AimInput::CancelCapture();
        } else {
            const AimInput::CaptureResult result = AimInput::PollCapture(overUi);
            if (result.state == AimInput::CaptureState::Done) {
                ApplyCapture(s, e, result.binding);
                g_capture.open = false;
            } else if (result.state == AimInput::CaptureState::Cleared) {
                ApplyCapture(s, e, none);
                g_capture.open = false;
            } else if (result.state == AimInput::CaptureState::Cancelled) {
                g_capture.open = false;
            }
        }
    }
    if (!g_capture.open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawMapping(AimSettings::Settings& s, Edit& e) {
    static int profileIndex = 0;
    ImGui::TextUnformatted("Profile:");
    ImGui::SameLine();
    ImGui::RadioButton("Gamepad", &profileIndex, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Keyboard and mouse", &profileIndex, 1);
    const InputMap::Profile profile = profileIndex == 0 ? InputMap::Profile::Gamepad : InputMap::Profile::KeyboardMouse;
    ImGui::TextDisabled("Click a slot to rebind, right-click to clear. Up to three bindings per action.");
    ImGui::TextDisabled("Names follow Sin & Punishment's Classic Controller layout; the Wii Remote scheme maps them to B/A/C/Z.");

    auto& table = s.bindings.For(profile);
    if (const size_t conflicts = InputMap::CountConflicts(table)) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%zu duplicate binding(s) in this profile.", conflicts);
    }
    if (!g_status.empty()) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", g_status.c_str());

    if (ImGui::BeginTable("##bindings", 1 + static_cast<int>(InputMap::kSlots),
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        const char* group = "";
        for (size_t a = 0; a < kActionCount; ++a) {
            const AimSettings::ActionInfo& info = AimSettings::kActions[a];
            if (std::string(info.group) != group) {
                group = info.group;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", group);
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(info.label);
            Tip(info.detail);
            for (size_t slot = 0; slot < InputMap::kSlots; ++slot) {
                ImGui::TableSetColumnIndex(1 + static_cast<int>(slot));
                ImGui::PushID(static_cast<int>(a * 8 + slot));
                const std::string label = FriendlyName(table[a][slot]) + "###slot";
                if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
                    g_capture = {true, true, a, slot, profile, Clock::now() + std::chrono::seconds(8)};
                    AimInput::BeginCapture(profile);
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    InputMap::AssignSwap(table, a, slot, InputMap::Binding{});
                    g_status.clear();
                    e.dirty = e.save = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    DrawCapturePopup(s, e);

    if (ConfirmButton("Reset this profile to defaults", "resetprofile")) {
        const AimSettings::Settings defaults = AimSettings::DefaultBindings();
        s.bindings.For(profile) = defaults.bindings.For(profile);
        g_status.clear();
        e.dirty = e.save = true;
    }
}

void DrawMouseKeyboard(AimSettings::Settings& s, Edit& e) {
    AimSettings::MouseConfig& m = s.mouse;
    const InputMap::AimSource source = UpdatePreview(s, false);
    DrawAimDevice(source, m.enabled);
    Check(e, "Mouse aims", &m.enabled, "Let the mouse drive the reticle. The last device to move wins.");
    int mode = m.raw ? 1 : 0;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("Mouse mode", &mode, "Absolute (cursor position)\0Relative (captured, raw motion)\0")) {
        m.raw = mode == 1;
        e.dirty = e.save = true;
    }
    Tip("Absolute: the reticle sits under the OS cursor (Wii Remote scheme).\n"
        "Relative: the cursor is captured and hidden; motion moves the reticle.");
    if (m.raw) {
        Slider(e, "Sensitivity", &m.sensitivity, 0.1f, 4.0f, "%.2f", "Camera pixels per mouse count. Default 0.60.");
    } else {
        Slider(e, "Smoothing", &m.smoothingMs, 0.0f, 150.0f, "%.0f ms", "0 applies the latest mouse sample directly.");
        Slider(e, "Calibration X", &m.rangeX, 0.5f, 1.5f, "%.2f", "Scales horizontal reach so the reticle lines up with the cursor. Default 1.00.");
        Slider(e, "Calibration Y", &m.rangeY, 0.5f, 1.5f, "%.2f", "Scales vertical reach. Default 1.00.");
    }
    Slider(e, "Classic scheme stick speed", &m.stickSpeed, 300.0f, 6000.0f, "%.0f counts/s",
           "In the Classic scheme the mouse emulates the right stick: this speed equals full deflection.");
    DrawPreview(s);
    ImGui::SeparatorText("Fixed keys");
    ImGui::TextDisabled("F10 / F1: open or close this menu    Esc: back / close    Alt+Enter: fullscreen");
    ImGui::TextDisabled("Gamepad: Back + Start opens or closes this menu.");
    ImGui::TextDisabled("Rebind actions on the Mapping page (profile: Keyboard and mouse).");
    if (ConfirmButton("Reset mouse options", "resetmouse")) {
        m = AimSettings::MouseConfig{};
        e.dirty = e.save = true;
    }
}

} // namespace

bool Capturing() { return g_capture.open || AimInput::Capturing(); }

bool ResetConfirmButton(const char* label, const char* id) { return ConfirmButton(label, id); }

void ResetAll() {
    AimSettings::ResetAllToDefaults();
    AimSettings::Save();
}

void Draw(void (*drawGamepadPage)()) {
    AimSettings::Settings s = AimSettings::Get();
    Edit e;
    int scheme = static_cast<int>(s.scheme);
    if (Combo(e, "Controller scheme (restart required)", &scheme, AimSettings::kSchemeLabels,
              "Classic Controller: the game reads the right stick and moves its own reticle.\n"
              "Wii Remote + Nunchuk: the port synthesizes the pointer (best for mouse aiming).\n"
              "Takes effect the next time the game starts.")) {
        s.scheme = static_cast<AimSettings::Scheme>(scheme);
    }
    ImGui::Spacing();
    if (ImGui::BeginTabBar("##controlsTabs")) {
        if (ImGui::BeginTabItem("Aiming")) {
            DrawAiming(s, e);
            if (ConfirmButton("Reset aiming to defaults", "resetaim")) {
                s.aim = DualAnalog::Config{};
                e.dirty = e.save = true;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Mapping")) {
            DrawMapping(s, e);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Mouse & Keyboard")) {
            DrawMouseKeyboard(s, e);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gamepad")) {
            if (drawGamepadPage != nullptr) drawGamepadPage();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (e.dirty) AimSettings::Set(s);
    if (e.save) AimSettings::Save();
}

} // namespace controls_menu

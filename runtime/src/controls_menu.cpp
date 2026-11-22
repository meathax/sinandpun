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

constexpr std::array<const char*, kPageCount> kPageNames = {"Overview", "Gamepad", "Keyboard & Mouse", "Aiming"};

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

void DrawAimDevice(ListView& v, InputMap::AimSource source, bool mouseEnabled) {
    const bool mouse = source == InputMap::AimSource::Mouse && mouseEnabled;
    v.Info("Active aim device", mouse ? "Mouse" : "Right stick / aim keys", mouse ? IM_COL32(150, 220, 255, 255) : game_ui::kGood);
}

const char* SchemeText(InputMap::Scheme s) {
    return s == InputMap::Scheme::Classic ? "Classic Controller" : "Wii Remote + Nunchuk";
}

// Active device and the scheme in effect (the input service in aim_input.cpp owns the decision).
void DrawActiveDevice(ListView& v, const AimSettings::Settings& s) {
    const InputMap::ActiveDevice device = AimInput::LastDevice();
    const char* name = device == InputMap::ActiveDevice::Gamepad          ? "Gamepad"
                       : device == InputMap::ActiveDevice::KeyboardMouse ? "Mouse and keyboard"
                                                                         : "Waiting for input";
    v.Info("Active device", name, device == InputMap::ActiveDevice::None ? game_ui::kTextDim : game_ui::kGood);
    const std::string scheme = std::string(SchemeText(AimInput::EffectiveScheme())) +
                               (s.schemeMode == InputMap::SchemeMode::Auto ? "  (automatic)" : "  (fixed)");
    v.Info("Scheme in effect", scheme, IM_COL32(150, 220, 255, 255));
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

// ---- Pages -------------------------------------------------------------------------------------------

void BuildStickShaping(ListView& v, AimSettings::Settings& s, Edit& e) {
    DualAnalog::Config& c = s.aim;
    const auto d = DualAnalog::ModernAim();
    v.Section("Left stick (move)");
    e.Take(v.Slider("Inner deadzone", &c.leftDeadzone, 0.0f, 0.5f, 0.01f, "%.2f", d.leftDeadzone,
                    "Left stick travel ignored around the centre. Raise it if the character drifts."));
    e.Take(v.Slider("Outer saturation", &c.leftSaturation, 0.5f, 1.0f, 0.01f, "%.2f", d.leftSaturation,
                    "Deflection that counts as full. Lower it if your stick never quite reaches the edge."));
    e.Take(v.Toggle("Invert Y (move)", &c.invertMoveY, d.invertMoveY, "Swap up and down on the left stick."));

    v.Section("Right stick (aim)");
    for (const auto& preset : DualAnalog::kSweepPresets) {
        if (v.Action(preset.name, "Apply", preset.peak < 3000.f ? "Wider fine-control zone for small targets." :
                    preset.peak > 4000.f ? "Earlier acceleration for rapid target changes." :
                    "Fine tracking near centre and a fast sweep at the rim.")) {
            DualAnalog::ApplySweepPreset(c, preset);
            s.schemeMode = AimSettings::SchemeMode::Auto;
            e.dirty = e.save = true;
        }
    }
    if (c.curve == DualAnalog::Curve::PrecisionSweep) {
        e.Take(v.Slider("Tracking speed", &c.trackingSpeed, 50.f, 1200.f, 10.f, "%.0f", 420.f,
                        "Gain for small corrections inside the sweep threshold."));
        e.Take(v.Slider("Sweep threshold", &c.dualZoneSplit, 0.1f, 0.9f, 0.01f, "%.2f", 0.52f,
                        "Fraction of usable stick travel before the fast sweep blends in."));
    }
    float maxSpeed = c.maxSpeedX;
    if (auto r = v.Slider("Full-stick speed", &maxSpeed, 100.0f, 8000.0f, 50.0f, "%.0f px/s", d.maxSpeedX,
                          "Camera pixels per second at full deflection.")) {
        c.maxSpeedX = maxSpeed;
        c.maxSpeedY = maxSpeed * (700.0f / 900.0f);
        e.Take(r);
    }
    int dz = static_cast<int>(c.deadzoneMode);
    if (auto r = v.Choice("Deadzone type", &dz, DualAnalog::kDeadzoneModeLabels.data(), DualAnalog::kDeadzoneModeCount,
                          static_cast<int>(d.deadzoneMode),
                          "Scaled radial: no jump at the edge. Radial: hard cut. Axial: each axis on its own.", 0, "Scaled radial")) {
        c.deadzoneMode = static_cast<DualAnalog::DeadzoneMode>(dz);
        e.Take(r);
    }
    e.Take(v.Slider("Inner deadzone", &c.radialDeadzone, 0.0f, 0.5f, 0.01f, "%.2f", d.radialDeadzone,
                    "Stick travel ignored around the centre."));
    e.Take(v.Slider("Outer saturation", &c.outerSaturation, 0.5f, 1.0f, 0.01f, "%.2f", d.outerSaturation,
                    "Deflection that counts as full. Lower it if your stick never reaches 1."));
    int curve = static_cast<int>(c.curve);
    if (auto r = v.Choice("Response curve", &curve, DualAnalog::kCurveLabels.data(), DualAnalog::kCurveCount,
                          static_cast<int>(d.curve), "How deflection turns into speed.")) {
        c.curve = static_cast<DualAnalog::Curve>(curve);
        e.Take(r);
    }
    if (c.curve == DualAnalog::Curve::Power) {
        e.Take(v.Slider("Exponent", &c.curveExponent, 0.3f, 4.0f, 0.05f, "%.2f", d.curveExponent,
                        "Below 1 = faster near the centre, above 1 = finer control.", game_ui::kRowIndent));
    } else if (c.curve == DualAnalog::Curve::DualZone) {
        e.Take(v.Slider("Precision zone size", &c.dualZoneSplit, 0.1f, 0.9f, 0.01f, "%.2f", d.dualZoneSplit,
                        "Deflection where the precision zone ends.", game_ui::kRowIndent));
        e.Take(v.Slider("Precision zone output", &c.dualZoneOutput, 0.05f, 0.9f, 0.01f, "%.2f", d.dualZoneOutput,
                        "Output reached at the end of the zone.", game_ui::kRowIndent));
    }
    e.Take(v.Slider("Aim sensitivity X", &c.sensitivityX, 0.1f, 4.0f, 0.05f, "%.2f", d.sensitivityX, "Horizontal gain."));
    e.Take(v.Slider("Aim sensitivity Y", &c.sensitivityY, 0.1f, 4.0f, 0.05f, "%.2f", d.sensitivityY, "Vertical gain."));
    e.Take(v.Toggle("Invert Y (aim)", &c.invertY, d.invertY, "Push up to aim down."));
    e.Take(v.Slider("Smoothing", &c.smoothingMs, 0.0f, 150.0f, 5.0f, "%.0f ms", d.smoothingMs, "Low-pass filter on the stick. 0 = off."));

    v.Section("Triggers");
    e.Take(v.Slider("Trigger threshold", &c.triggerThreshold, 0.1f, 0.9f, 0.01f, "%.2f", d.triggerThreshold,
                    "How far an analog trigger (LT / RT) is pulled before it counts as pressed. Lower = lighter pull."));
}

void BuildPointerAiming(ListView& v, AimSettings::Settings& s, Edit& e) {
    v.Section("Gyro aiming");
    e.Take(v.Toggle("Enable gyro", &s.gyro.enabled, false,
        "Uses Wii pointer aiming with the right stick. Requires an SDL gyro + accelerometer controller."));
    static const char* const activation[] = {"Always on", "Hold button to aim", "Hold button to pause"};
    static const char* const spaces[] = {"Player space", "Local yaw", "Local roll"};
    e.Take(v.Choice("Gyro activation", &s.gyro.activation, activation, 3, 0,
        "The gyro activation button is remappable under bindings (default R3)."));
    e.Take(v.Choice("Gyro axes", &s.gyro.space, spaces, 3, 0,
        "Player space compensates for how you tilt the controller."));
    e.Take(v.Slider("Gyro sensitivity X", &s.gyro.sensitivityX, 1.f, 50.f, 0.5f, "%.1f", 12.f,
        "Pointer pixels per degree of controller rotation."));
    e.Take(v.Slider("Gyro sensitivity Y", &s.gyro.sensitivityY, 1.f, 50.f, 0.5f, "%.1f", 12.f,
        "Vertical pointer pixels per degree."));
    e.Take(v.Toggle("Invert gyro X", &s.gyro.invertX, false, "Reverse horizontal motion."));
    e.Take(v.Toggle("Invert gyro Y", &s.gyro.invertY, false, "Reverse vertical motion."));
    if (v.Action("Calibrate gyro", "Calibrate", AimInput::GyroStatus())) AimInput::CalibrateGyro();

    DualAnalog::Config& c = s.aim;
    const auto d = DualAnalog::ModernAim();
    v.Section("Stick pointer (Wii Remote scheme)");
    int mode = c.absoluteMode ? 1 : 0;
    static const char* const kModes[] = {"Velocity", "Absolute"};
    if (auto r = v.Choice("Stick pointer mode", &mode, kModes, 2, 0,
                          "Velocity: deflection sets speed, release holds the position, like a mouse. Absolute: stick position is pointer position.")) {
        c.absoluteMode = mode == 1;
        e.Take(r);
    }
    e.Take(v.Slider("Speed cap", &c.speedCap, 200.0f, 8000.0f, 50.0f, "%.0f px/s", d.speedCap, "Upper limit after ramp-up and edge boost."));
    float rampMs = c.rampSeconds * 1000.0f;
    if (auto r = v.Slider("Ramp-up time", &rampMs, 0.0f, 500.0f, 10.0f, "%.0f ms", d.rampSeconds * 1000.0f,
                          "Slow start after leaving the deadzone.")) {
        c.rampSeconds = rampMs * 0.001f;
        e.Take(r);
    }
    e.Take(v.Slider("Edge boost", &c.edgeBoost, 1.0f, 3.0f, 0.05f, "x%.2f", d.edgeBoost, "Extra speed while the stick is held at the edge. 1 = off."));
    float edgeMs = c.edgeAccelSeconds * 1000.0f;
    if (auto r = v.Slider("Edge boost time", &edgeMs, 50.0f, 2000.0f, 25.0f, "%.0f ms", d.edgeAccelSeconds * 1000.0f,
                          "How quickly the boost builds.")) {
        c.edgeAccelSeconds = edgeMs * 0.001f;
        e.Take(r);
    }
    e.Take(v.Toggle("Recenter on release", &c.recenterOnRelease, d.recenterOnRelease,
                    "Spring the pointer back to the centre when the stick is released."));
    if (c.recenterOnRelease) {
        float springMs = c.recenterSeconds * 1000.0f;
        if (auto r = v.Slider("Recenter time", &springMs, 30.0f, 2000.0f, 10.0f, "%.0f ms", d.recenterSeconds * 1000.0f,
                              "How fast the pointer springs back.", game_ui::kRowIndent)) {
            c.recenterSeconds = springMs * 0.001f;
            e.Take(r);
        }
    }
}

// Position-based pad names: the Classic Controller's A / B sit in the east / south positions, which are the
// Xbox B / A buttons, so letters would mislead.
std::string FriendlyName(const InputMap::Binding& b) {
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
    v.Section(pad ? "Gamepad buttons" : "Keyboard and mouse bindings");
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
    if (v.Action(pad ? "Reset gamepad bindings" : "Reset keyboard and mouse bindings", "Reset...",
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

void BuildOverview(ListView& v, AimSettings::Settings& s, Edit& e) {
    v.Section("Button prompts");
    int prompts = s.playStationPrompts ? 1 : 0;
    static const char* const styles[] = {"Xbox One", "PlayStation"};
    if (auto r = v.Choice("Button prompts", &prompts, styles, 2, 0,
                          "Changes in-game button icons and menu hints immediately. Uses your gamepad bindings.")) {
        s.playStationPrompts = prompts == 1;
        e.Take(r);
    }
    v.Section("Control scheme");
    int mode = static_cast<int>(s.schemeMode);
    if (auto r = v.Choice("Control scheme", &mode, InputMap::kSchemeModeLabels.data(),
                          static_cast<int>(InputMap::kSchemeModeLabels.size()), 0,
                          "Automatic uses the Wii pointer for modern right-stick and mouse aiming. Choose Legacy Classic Controller for original stick behaviour.",
                          0, "Automatic")) {
        s.schemeMode = static_cast<InputMap::SchemeMode>(mode);
        e.Take(r);
    }
    DrawActiveDevice(v, s);
    e.Take(v.Toggle("Show a note when the scheme switches", &s.switchToast, true,
                    "Brief on-screen message when Automatic changes between gamepad and mouse/keyboard."));
    if (v.Action("Modern Xbox layout", "Use recommended layout",
                 "RT shoots, LT locks/charges, LB evades, RB jumps. A confirms and B goes back. Restores gamepad bindings and selects Automatic.")) {
        game_ui::RequestConfirm("Modern Xbox layout", "Replace gamepad bindings with the recommended layout? Keyboard and mouse bindings are kept.", "Use layout", [] {
            auto cur = AimSettings::Get();
            cur.bindings.pad = AimSettings::DefaultBindings().bindings.pad;
            cur.schemeMode = AimSettings::SchemeMode::Auto;
            AimSettings::Set(cur);
            AimSettings::Save();
        });
    }
    v.Note("Recommended gamepad: RT shoots, LT locks/charges, LB evades, RB jumps. Left stick moves; right stick aims. A confirms, B goes back.\n"
           "Mouse and keyboard: WASD moves, the mouse aims, left click = shoot (B), right click = lock-on (A), Space = jump (C), Shift = evade (Z).");
    v.Section("Live input");
    DrawDeviceMonitor(v);
    v.Section("Pages");
    v.Note("Gamepad: sticks, deadzones, triggers and button bindings.\nKeyboard & Mouse: mouse aiming and key bindings.\nAiming: how the stick moves the Wii Remote pointer.");
}

void BuildGamepad(ListView& v, AimSettings::Settings& s, Edit& e) {
    UpdatePreview(s);
    v.Section("Live preview");
    DrawActiveDevice(v, s);
    DrawPreview(v, s);
    BuildStickShaping(v, s, e);
    BuildBindings(v, s, e, InputMap::Profile::Gamepad);
}

void BuildMouseKeyboard(ListView& v, AimSettings::Settings& s, Edit& e) {
    AimSettings::MouseConfig& m = s.mouse;
    const AimSettings::MouseConfig d{};
    const InputMap::AimSource source = UpdatePreview(s);
    v.Section("Mouse aiming");
    DrawActiveDevice(v, s);
    DrawAimDevice(v, source, m.enabled);
    e.Take(v.Toggle("Mouse aims", &m.enabled, d.enabled, "Let the mouse drive the reticle. The last device to move wins."));
    int mode = m.raw ? 1 : 0;
    static const char* const kModes[] = {"Absolute", "Relative"};
    if (auto r = v.Choice("Mouse mode", &mode, kModes, 2, 0,
                          "Absolute: the reticle sits under the OS cursor (Wii Remote scheme). Relative: the cursor is captured and hidden; motion moves the reticle.")) {
        m.raw = mode == 1;
        e.Take(r);
    }
    if (m.raw) {
        e.Take(v.Slider("Sensitivity", &m.sensitivity, 0.1f, 4.0f, 0.05f, "%.2f", d.sensitivity, "Camera pixels per mouse count.", game_ui::kRowIndent));
    } else {
        e.Take(v.Slider("Smoothing", &m.smoothingMs, 0.0f, 150.0f, 5.0f, "%.0f ms", d.smoothingMs, "0 applies the latest mouse sample directly.", game_ui::kRowIndent));
        e.Take(v.Slider("Calibration X", &m.rangeX, 0.5f, 1.5f, 0.01f, "%.2f", d.rangeX,
                        "Scales horizontal reach so the reticle lines up with the cursor.", game_ui::kRowIndent));
        e.Take(v.Slider("Calibration Y", &m.rangeY, 0.5f, 1.5f, 0.01f, "%.2f", d.rangeY, "Scales vertical reach.", game_ui::kRowIndent));
    }
    e.Take(v.Slider("Classic scheme stick speed", &m.stickSpeed, 300.0f, 6000.0f, 100.0f, "%.0f/s", d.stickSpeed,
                    "In the Classic Controller scheme the mouse emulates the right stick: this speed equals full deflection."));
    v.Section("Preview");
    DrawPreview(v, s);
    BuildBindings(v, s, e, InputMap::Profile::KeyboardMouse);
    v.Section("Fixed keys");
    v.Note("Esc: back or close the menu     Alt+Enter: fullscreen     Q / E or Tab: switch tabs     F10: open this menu");
}

} // namespace

const char* PageName(int page) { return page >= 0 && page < kPageCount ? kPageNames[static_cast<size_t>(page)] : ""; }

void BuildPage(ListView& view, int page) {
    AimSettings::Settings s = AimSettings::Get();
    Edit e;
    switch (page) {
    case 0: BuildOverview(view, s, e); break;
    case 1: BuildGamepad(view, s, e); break;
    case 2: BuildMouseKeyboard(view, s, e); break;
    default: {
        UpdatePreview(s);
        view.Section("Live preview");
        DrawActiveDevice(view, s);
        DrawPreview(view, s);
        BuildPointerAiming(view, s, e);
        break;
    }
    }
    if (e.dirty) AimSettings::Set(s);
    if (e.save) AimSettings::Save();
}

// Devices page: the scheme itself is chosen on the Overview page; this just reports it.
void BuildSchemeRow(ListView& view) {
    const AimSettings::Settings s = AimSettings::Get();
    DrawActiveDevice(view, s);
    view.Note("Choose Automatic or a fixed control scheme on the Overview page.");
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
    spec.lines = {pad ? "Press a gamepad button, pull a trigger or push a stick." : "Press a key, click a mouse button or scroll the wheel.",
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

// The scheme now switches live, so no control option needs a restart any more.
bool RestartPending() { return false; }

bool Capturing() { return g_capture.open || AimInput::Capturing(); }

void ResetPage(int page) {
    AimSettings::Settings s = AimSettings::Get();
    const auto fresh = DualAnalog::ModernAim();
    const DualAnalog::Config current = s.aim;
    auto pointerFields = [](DualAnalog::Config& to, const DualAnalog::Config& from) {
        to.absoluteMode = from.absoluteMode;
        to.speedCap = from.speedCap;
        to.rampSeconds = from.rampSeconds;
        to.edgeBoost = from.edgeBoost;
        to.edgeAccelSeconds = from.edgeAccelSeconds;
        to.recenterOnRelease = from.recenterOnRelease;
        to.recenterSeconds = from.recenterSeconds;
    };
    switch (page) {
    case 0:
        s.schemeMode = InputMap::SchemeMode::Auto;
        s.switchToast = true;
        s.playStationPrompts = false;
        break;
    case 1:
        s.aim = fresh;
        pointerFields(s.aim, current);  // the stick pointer belongs to the Aiming page
        s.bindings.pad = AimSettings::DefaultBindings().bindings.pad;
        g_status.clear();
        break;
    case 2:
        s.mouse = AimSettings::MouseConfig{};
        s.bindings.kbm = AimSettings::DefaultBindings().bindings.kbm;
        g_status.clear();
        break;
    default:
        pointerFields(s.aim, fresh);
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

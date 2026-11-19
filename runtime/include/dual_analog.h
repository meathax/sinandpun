#pragma once

// Dual-analog aim synthesis: the core math for Xbox-style controllers.
//
// The virtual remote presents a synthetic IR-camera view of a sensor bar to
// the guest's own WPAD/KPAD stack, so every pointer behavior (smoothing,
// re-centering, bounding) is computed by the game exactly as on hardware.
// This module owns the feel: how the right stick moves that synthetic view.
//
// Scheme: the right stick is a VELOCITY control on the pointer (like a
// mouse), not an absolute joystick-to-position map. Deflection sets speed
// through a response curve; release keeps the pointer where it is. This is
// the scheme that feels right for aiming and it is what the controller setup
// UI tunes.

#include <cmath>
#include <cstdint>

namespace DualAnalog {

// All units: stick in [-1, 1] (SDL axes, deadzone not yet applied);
// camera in IR pixel space; time in seconds.
struct Config {
    // Right stick
    float radialDeadzone = 0.12f;      // fraction of full deflection
    float responseGamma = 0.65f;        // <1 = faster center, >1 = precision
    float maxSpeedX = 900.0f;           // camera px/s at full deflection
    float maxSpeedY = 700.0f;
    // Synthetic sensor bar geometry, in IR camera pixels (1024x768 space).
    // Two dots symmetric about the point the pointer should report.
    float dotSeparation = 72.0f;
    // Bounds of the synthetic view; the pointer saturates at the edges so
    // KPAD never sees the pair leave the camera.
    float cameraMinX = 60.f, cameraMaxX = 964.f;
    float cameraMinY = 60.f, cameraMaxY = 708.f;
};

struct PointerState {
    float x = 512.f;
    float y = 384.f;
    bool visible = false;  // dots leave the camera when the stick is idle
};

// Applies the radial deadzone with edge remap so full deflection stays full.
inline void ApplyDeadzone(float& ax, float& ay, float deadzone) {
    const float magnitude = std::sqrt(ax * ax + ay * ay);
    if (magnitude <= deadzone) {
        ax = 0.f;
        ay = 0.f;
        return;
    }
    const float scaled = (magnitude - deadzone) / (1.f - deadzone);
    const float normalized = scaled / magnitude;
    ax *= normalized;
    ay *= normalized;
}

// Response curve: deflection magnitude through gamma, direction preserved.
inline void ApplyResponseCurve(float& ax, float& ay, float gamma) {
    const float magnitude = std::sqrt(ax * ax + ay * ay);
    if (magnitude < 1e-6f) {
        return;
    }
    const float curved = std::pow(magnitude, gamma);
    const float scale = curved / magnitude;
    ax *= scale;
    ay *= scale;
}

// One integration step. dt in seconds; clamped for stability.
inline void Advance(PointerState& state, const Config& cfg,
                    float rawStickX, float rawStickY, float dt) {
    float ax = rawStickX;
    float ay = rawStickY;
    ApplyDeadzone(ax, ay, cfg.radialDeadzone);
    state.visible = (ax != 0.f) || (ay != 0.f);
    if (!state.visible) {
        return;  // pointer holds position when the stick is released
    }
    ApplyResponseCurve(ax, ay, cfg.responseGamma);
    if (dt > 0.05f) {
        dt = 0.05f;
    }
    state.x += ax * cfg.maxSpeedX * dt;
    state.y += ay * cfg.maxSpeedY * dt;
    if (state.x < cfg.cameraMinX) state.x = cfg.cameraMinX;
    if (state.x > cfg.cameraMaxX) state.x = cfg.cameraMaxX;
    if (state.y < cfg.cameraMinY) state.y = cfg.cameraMinY;
    if (state.y > cfg.cameraMaxY) state.y = cfg.cameraMaxY;
}

// The two synthetic sensor-bar dots for the camera report: symmetric about
// the pointer, horizontal pair like a real bar above/below the TV. KPAD
// computes their midpoint into the on-screen pointer.
inline void EmitDots(const PointerState& state, const Config& cfg,
                     uint16_t& dot0X, uint16_t& dot0Y,
                     uint16_t& dot1X, uint16_t& dot1Y) {
    const float half = cfg.dotSeparation * 0.5f;
    dot0X = static_cast<uint16_t>(state.x - half);
    dot0Y = static_cast<uint16_t>(state.y);
    dot1X = static_cast<uint16_t>(state.x + half);
    dot1Y = static_cast<uint16_t>(state.y);
}

// Left stick -> Nunchuk joystick (classic 8-bit axes, 128 centered).
struct NunchukState {
    uint8_t x = 128;
    uint8_t y = 128;
};

inline NunchukState NunchukFromStick(float rawStickX, float rawStickY,
                                     float deadzone = 0.12f) {
    NunchukState out;
    ApplyDeadzone(rawStickX, rawStickY, deadzone);
    // SDL stick +Y is up; Nunchuk +Y is up too, so no inversion here.
    out.x = static_cast<uint8_t>(128 + rawStickX * 127.f);
    out.y = static_cast<uint8_t>(128 + rawStickY * 127.f);
    return out;
}

} // namespace DualAnalog

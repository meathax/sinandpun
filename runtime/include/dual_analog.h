#pragma once

// Dual-analog aim maths: pure functions only (no SDL, no globals) so they can be unit-tested
// (runtime/tests/controls_tests.cpp).
//
// Two consumers share the same stick shaping (deadzone, response curve, sensitivity, smoothing):
//  * Classic Controller scheme: the shaped right stick is reported as the Classic right stick and the
//    game moves its own reticle.
//  * Wii Remote pointer scheme: the virtual remote presents a synthetic IR sensor-bar view to the
//    guest's own WPAD/KPAD stack; Advance() moves that view like a mouse (velocity) or maps the stick
//    to a position (absolute), and the mouse can place the pointer directly.
//
// Stick convention everywhere in this header: +x right, +y DOWN (SDL axes), range [-1, 1].

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace DualAnalog {

// Right-stick aim model (docs/STICK-AIM.md):
//  1. Radial deadzone with smooth rescale: |stick| below `radialDeadzone` is ignored and the travel up to
//     `outerSaturation` is rescaled to 0..1. There is no jump at the deadzone edge, full speed is reachable on worn
//     sticks and square gates, and the direction is always kept (no axial snapping).
//  2. Response curve f(m) = kLinearShare * m + (1 - kLinearShare) * m^curveExponent. The linear share keeps
//     small deflections responsive just past the deadzone. The power term gives fine precision near the centre
//     and fast traversal at full tilt. f(0) = 0, f(1) = 1, monotonic.
//  3. Velocity pointer: the curve output sets reticle speed. There is no ramp-up, smoothing or edge boost, so
//     speed follows the stick immediately. Speed is integrated over real time (the virtual remote reports every
//     10 ms) and is the same at any presentation frame rate.
//  4. Vertical speed is scaled so equal deflection moves the reticle the same number of screen pixels on both
//     axes (the guest reticle layer is 810x456 over a near-square IR camera span).
//
// Player-facing: sensitivity, invert Y. Advanced: deadzone, acceleration (curve exponent), vertical ratio.
// Config.toml keys of the older knobs (curve, ramp, boost, ...) are still read without error and ignored.
inline constexpr float kLinearShare = 0.15f;
inline constexpr float kFullTiltSpeed = 1300.f;         // camera px/s at full tilt, sensitivity 1: ~0.35 s edge to edge
inline constexpr float kScreenAspectY = 810.f / 456.f;  // vertical camera px per horizontal one for equal screen motion

// All units: stick in [-1, 1] (deadzone not yet applied); camera in IR pixel space; time in seconds.
struct Config {
    // Right stick
    float radialDeadzone = 0.10f;      // inner deadzone
    float outerSaturation = 0.92f;     // deflection at which output reaches 1
    float curveExponent = 2.2f;        // acceleration: 1 = linear, higher = finer centre, same full-tilt speed
    float sensitivity = 1.0f;          // multiplies the full-tilt speed on both axes
    float verticalRatio = 1.0f;        // vertical speed relative to horizontal, in screen pixels
    bool invertY = false;
    // Synthetic sensor bar geometry, in IR camera pixels (1024x768 space).
    float dotSeparation = 72.0f;
    float cameraMinX = 60.f, cameraMaxX = 964.f;
    float cameraMinY = 60.f, cameraMaxY = 708.f;
    float maxSpeedX = kFullTiltSpeed;  // derived by StickPointerConfig: camera px/s at full tilt
    float maxSpeedY = kFullTiltSpeed * kScreenAspectY;
    // Left stick and triggers
    float leftDeadzone = 0.12f;
    float leftSaturation = 1.0f;       // Config.toml only: deflection that counts as full on the left stick
    bool invertMoveY = false;          // Config.toml only: swap left stick up/down
    float triggerThreshold = 0.35f;    // analog trigger pull that counts as pressed
};

inline Config ModernAim() { return Config{}; }

// With the virtual 72-pixel bar, the guest reports ~0.00443 KPAD units per
// camera pixel. TV bar placement offsets the vertical origin by ~103 pixels.
// Keep a stick sweep inside the visible -1..1 playfield, even at its corners.
inline Config StickPointerConfig(Config c, bool barAbove) {
    c.cameraMinX = 287.f;
    c.cameraMaxX = 736.f;
    const float centerY = barAbove ? 487.f : 281.f;
    c.cameraMinY = centerY - 224.f;
    c.cameraMaxY = centerY + 224.f;
    // GunSight draws in an 810x456 layer. Equal screen-pixel motion on
    // both axes needs more vertical camera travel per unit of stick input.
    c.maxSpeedX = kFullTiltSpeed * c.sensitivity;
    c.maxSpeedY = c.maxSpeedX * kScreenAspectY * c.verticalRatio;
    return c;
}

inline void Sanitize(Config& c) {
    auto fin = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    c.radialDeadzone = std::clamp(fin(c.radialDeadzone, 0.10f), 0.0f, 0.5f);
    c.outerSaturation = std::clamp(fin(c.outerSaturation, 0.92f), c.radialDeadzone + 0.1f, 1.0f);
    c.curveExponent = std::clamp(fin(c.curveExponent, 2.2f), 1.0f, 4.0f);
    c.sensitivity = std::clamp(fin(c.sensitivity, 1.0f), 0.2f, 3.0f);
    c.verticalRatio = std::clamp(fin(c.verticalRatio, 1.0f), 0.5f, 2.0f);
    c.leftDeadzone = std::clamp(fin(c.leftDeadzone, 0.12f), 0.0f, 0.5f);
    c.leftSaturation = std::clamp(fin(c.leftSaturation, 1.0f), 0.5f, 1.0f);
    c.triggerThreshold = std::clamp(fin(c.triggerThreshold, 0.35f), 0.1f, 0.9f);
}

// ---- Stick shaping -----------------------------------------------------------------------------

// Response curve on a normalised magnitude m in [0, 1]. f(0) = 0, f(1) = 1, strictly increasing.
inline float EvalCurve(const Config& cfg, float m) {
    m = std::clamp(m, 0.0f, 1.0f);
    return kLinearShare * m + (1.0f - kLinearShare) * std::pow(m, cfg.curveExponent);
}

// Deadzone remap of the radial magnitude: raw -> [0, 1] before the curve, continuous at the inner edge.
inline float RemapMagnitude(const Config& cfg, float m) {
    const float dz = cfg.radialDeadzone;
    const float sat = std::max(cfg.outerSaturation, dz + 0.05f);
    if (m <= dz) return 0.0f;
    return std::clamp((m - dz) / (sat - dz), 0.0f, 1.0f);
}

// Radial deadzone only (no curve): direction kept, magnitude rescaled to [0, 1].
inline void ApplyRadialDeadzone(const Config& cfg, float ax, float ay, float& ox, float& oy) {
    const float m = std::sqrt(ax * ax + ay * ay);
    const float r = RemapMagnitude(cfg, m);
    if (m < 1e-6f || r <= 0.0f) {
        ox = oy = 0.0f;
        return;
    }
    ox = ax / m * r;
    oy = ay / m * r;
}

// Deadzone + curve, direction preserved.
inline void ApplyDeadzoneAndCurve(const Config& cfg, float ax, float ay, float& ox, float& oy) {
    const float m = std::sqrt(ax * ax + ay * ay);
    const float shaped = EvalCurve(cfg, RemapMagnitude(cfg, m));
    if (m < 1e-6f || shaped <= 0.0f) {
        ox = 0.0f;
        oy = 0.0f;
        return;
    }
    ox = ax / m * shaped;
    oy = ay / m * shaped;
}

struct StickFilter {
    float x = 0.f;
    float y = 0.f;
};

// Deadzone, curve, invert Y. Output stays in the unit disc. The filter keeps the last output (no smoothing).
inline void ShapeStick(const Config& cfg, StickFilter& filter, float rawX, float rawY, float /*dt*/,
                       float& outX, float& outY) {
    float sx = 0.f, sy = 0.f;
    ApplyDeadzoneAndCurve(cfg, rawX, rawY, sx, sy);
    if (cfg.invertY) sy = -sy;
    filter.x = outX = sx;
    filter.y = outY = sy;
}

// True when the stick is outside the deadzone (used for aim-device arbitration).
inline bool StickActive(const Config& cfg, float rawX, float rawY) {
    return RemapMagnitude(cfg, std::sqrt(rawX * rawX + rawY * rawY)) > 1e-4f;
}

// Classic Controller scheme: the game moves its own reticle from the Classic right stick and applies its own
// response, so only the radial deadzone and sensitivity (as a gain) are applied, clamped to the unit box.
inline void ClassicStick(const Config& cfg, StickFilter& filter, float rawX, float rawY, float /*dt*/,
                         float& outX, float& outY) {
    float sx = 0.f, sy = 0.f;
    ApplyRadialDeadzone(cfg, rawX, rawY, sx, sy);
    if (cfg.invertY) sy = -sy;
    filter.x = outX = std::clamp(sx * cfg.sensitivity, -1.0f, 1.0f);
    filter.y = outY = std::clamp(sy * cfg.sensitivity, -1.0f, 1.0f);
}

// ---- Pointer synthesis (Wii Remote scheme) -----------------------------------------------------

struct PointerState {
    float x = 512.f;
    float y = 384.f;
    bool visible = false;  // true while the stick is deflected
    StickFilter filter;
};

inline float CenterX(const Config& cfg) { return (cfg.cameraMinX + cfg.cameraMaxX) * 0.5f; }
inline float CenterY(const Config& cfg) { return (cfg.cameraMinY + cfg.cameraMaxY) * 0.5f; }

inline void Recenter(PointerState& state, const Config& cfg) {
    state.x = CenterX(cfg);
    state.y = CenterY(cfg);
}

inline void ClampToCamera(PointerState& state, const Config& cfg) {
    state.x = std::clamp(state.x, cfg.cameraMinX, cfg.cameraMaxX);
    state.y = std::clamp(state.y, cfg.cameraMinY, cfg.cameraMaxY);
}

// One integration step. rawStick* are raw SDL-convention stick values after the caller's axis flip for the
// sensor-bar view (stick right moves the pointer right, which moves the bar LEFT in the camera, so the
// caller passes -x; Y is passed as is). dt in seconds of real time, clamped to 50 ms. Use a config from
// StickPointerConfig() so the speeds and camera span are derived.
inline void Advance(PointerState& state, const Config& cfg, float rawStickX, float rawStickY, float dt) {
    dt = std::clamp(dt, 0.0f, 0.05f);
    float ax = 0.f, ay = 0.f;
    ShapeStick(cfg, state.filter, rawStickX, rawStickY, dt, ax, ay);
    state.visible = std::fabs(ax) + std::fabs(ay) > 1e-6f;
    if (!state.visible) return;  // the pointer holds its position, like a mouse
    state.x += ax * cfg.maxSpeedX * dt;
    state.y += ay * cfg.maxSpeedY * dt;
    ClampToCamera(state, cfg);
}

// The two synthetic sensor-bar dots for the camera report: symmetric about the pointer, horizontal pair like
// a real bar above/below the TV. KPAD computes their midpoint into the on-screen pointer.
inline void EmitDots(const PointerState& state, const Config& cfg, uint16_t& dot0X, uint16_t& dot0Y,
                     uint16_t& dot1X, uint16_t& dot1Y) {
    const float half = cfg.dotSeparation * 0.5f;
    dot0X = static_cast<uint16_t>(state.x - half);
    dot0Y = static_cast<uint16_t>(state.y);
    dot1X = static_cast<uint16_t>(state.x + half);
    dot1Y = static_cast<uint16_t>(state.y);
}

// ---- Mouse as pointer --------------------------------------------------------------------------

struct Rect {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

// The part of the window the game picture occupies. With letterbox off the picture fills the window
// (stretch); with letterbox on it is the largest centred rectangle of the given aspect (w / h).
inline Rect ContentRect(float winW, float winH, bool letterbox, float aspect) {
    if (!letterbox || aspect <= 0.f || winW <= 0.f || winH <= 0.f) return {0.f, 0.f, winW, winH};
    Rect r;
    if (winW / winH > aspect) {
        r.h = winH;
        r.w = winH * aspect;
    } else {
        r.w = winW;
        r.h = winW / aspect;
    }
    r.x = (winW - r.w) * 0.5f;
    r.y = (winH - r.h) * 0.5f;
    return r;
}

// Window coordinates -> position inside the picture in [0, 1] (clamped: the cursor outside the picture or the
// window pins the pointer to the edge).
inline void WindowToNormalized(const Rect& r, float mx, float my, float& u, float& v) {
    u = r.w > 0.f ? std::clamp((mx - r.x) / r.w, 0.f, 1.f) : 0.5f;
    v = r.h > 0.f ? std::clamp((my - r.y) / r.h, 0.f, 1.f) : 0.5f;
}

// Picture position -> synthetic IR camera position. Picture x grows to the right while the bar moves LEFT in
// the camera, so x is mirrored. rangeX/Y (1 = nominal) scale the usable camera span for calibration.
inline void NormalizedToCamera(const Config& cfg, float u, float v, float rangeX, float rangeY, float& camX,
                               float& camY) {
    const float hx = (cfg.cameraMaxX - cfg.cameraMinX) * 0.5f * rangeX;
    const float hy = (cfg.cameraMaxY - cfg.cameraMinY) * 0.5f * rangeY;
    camX = std::clamp(CenterX(cfg) - (u - 0.5f) * 2.0f * hx, cfg.cameraMinX, cfg.cameraMaxX);
    camY = std::clamp(CenterY(cfg) + (v - 0.5f) * 2.0f * hy, cfg.cameraMinY, cfg.cameraMaxY);
}

// Relative mouse motion (counts) moves the camera position. sensitivity = camera px per count.
inline void MouseRelativeStep(PointerState& state, const Config& cfg, float dx, float dy, float sensitivity) {
    state.x -= dx * sensitivity;
    state.y += dy * sensitivity;
    ClampToCamera(state, cfg);
}

// Mouse motion as an emulated stick (Classic scheme): speed in counts/s over pxPerSec, clamped to the unit disc.
inline void MouseDeltaToStick(float dx, float dy, float dt, float pxPerSec, float& sx, float& sy) {
    if (dt <= 0.f || pxPerSec <= 0.f) {
        sx = sy = 0.f;
        return;
    }
    sx = dx / dt / pxPerSec;
    sy = dy / dt / pxPerSec;
    const float m = std::sqrt(sx * sx + sy * sy);
    if (m > 1.f) {
        sx /= m;
        sy /= m;
    }
}

// 1-pole smoothing step toward a target (used for absolute mouse smoothing).
inline float SmoothToward(float current, float target, float dt, float tauMs) {
    if (tauMs <= 0.f || dt <= 0.f) return target;
    return current + (target - current) * (1.0f - std::exp(-dt / (tauMs * 0.001f)));
}

// ---- Left stick -> Nunchuk / Classic left stick ------------------------------------------------

// Applies the radial deadzone with edge remap so full deflection stays full.
inline void ApplyDeadzone(float& ax, float& ay, float deadzone) {
    const float magnitude = std::sqrt(ax * ax + ay * ay);
    if (magnitude <= deadzone || magnitude < 1e-6f) {
        ax = 0.f;
        ay = 0.f;
        return;
    }
    const float scaled = std::min(1.0f, (magnitude - deadzone) / (1.f - deadzone));
    const float normalized = scaled / magnitude;
    ax *= normalized;
    ay *= normalized;
}

// 128 centred, +-96 = the 0x20..0xE0 range the virtual Nunchuk calibration block advertises. stickUp: +1 = up.
struct NunchukState {
    uint8_t x = 128;
    uint8_t y = 128;
};

inline NunchukState NunchukFromStick(float stickX, float stickUp, float deadzone = 0.12f) {
    ApplyDeadzone(stickX, stickUp, deadzone);
    NunchukState out;
    out.x = static_cast<uint8_t>(std::clamp(128.f + stickX * 96.f, 0.f, 255.f));
    out.y = static_cast<uint8_t>(std::clamp(128.f + stickUp * 96.f, 0.f, 255.f));
    return out;
}

// Quantises a stick value in [-1, 1] to an n-bit centred raw value using +-halfRange steps.
inline uint8_t QuantizeStick(float v, int centre, int halfRange) {
    const int q = centre + static_cast<int>(std::lround(std::clamp(v, -1.f, 1.f) * static_cast<float>(halfRange)));
    return static_cast<uint8_t>(std::clamp(q, centre - halfRange, centre + halfRange));
}

} // namespace DualAnalog

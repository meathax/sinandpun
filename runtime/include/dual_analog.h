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

enum class DeadzoneMode : uint8_t {
    ScaledRadial,  // radial; remapped so output is 0 at the inner edge and 1 at saturation (no jump)
    Radial,        // radial hard cut at the inner edge, no remap (output jumps at the edge)
    Axial,         // each axis handled independently, remapped per axis
};
inline constexpr int kDeadzoneModeCount = 3;
inline constexpr std::array<const char*, kDeadzoneModeCount> kDeadzoneModeKeys = {"scaled_radial", "radial", "axial"};
inline constexpr std::array<const char*, kDeadzoneModeCount> kDeadzoneModeLabels = {"Scaled radial", "Radial", "Axial"};

enum class Curve : uint8_t { Linear, Power, SCurve, DualZone, Classic, Raw, PrecisionSweep };
inline constexpr int kCurveCount = 7;
inline constexpr std::array<const char*, kCurveCount> kCurveKeys = {"linear", "power", "s_curve", "dual_zone", "classic", "raw", "precision_sweep"};
inline constexpr std::array<const char*, kCurveCount> kCurveLabels = {
    "Linear", "Exponential / Power", "S-curve (smoothstep)", "Dual-zone", "Classic (slow centre, fast edge)", "Instant / Raw", "Precision sweep"};

// All units: stick in [-1, 1] (deadzone not yet applied); camera in IR pixel space; time in seconds.
struct Config {
    // Right stick shaping
    DeadzoneMode deadzoneMode = DeadzoneMode::ScaledRadial;
    float radialDeadzone = 0.12f;      // inner deadzone
    float outerSaturation = 0.95f;     // deflection at which output reaches 1
    Curve curve = Curve::Linear;
    float curveExponent = 1.6f;        // Power: <1 faster centre, >1 precision
    float dualZoneSplit = 0.5f;        // Dual-zone: end of the precision zone
    float dualZoneOutput = 0.25f;      // Dual-zone: output reached at the end of the precision zone
    float trackingSpeed = 420.f;       // inner-region gain in camera px/s per unit of remapped travel
    float sensitivityX = 1.0f;
    float sensitivityY = 1.0f;
    bool invertY = false;
    float smoothingMs = 0.0f;          // 1-pole low-pass time constant, 0 = off
    float precisionScale = 0.35f;      // output factor while the precision control is held
    // Pointer synthesis (Wii Remote scheme)
    bool absoluteMode = false;         // stick position maps straight to pointer position
    float maxSpeedX = 900.0f;          // camera px/s at full deflection
    float maxSpeedY = 700.0f;
    float speedCap = 1600.0f;          // camera px/s ceiling after ramp/boost
    float rampSeconds = 0.12f;         // slow-start: time to reach full speed after leaving the deadzone
    float edgeBoost = 1.0f;            // extra speed multiplier reached after holding the stick at the edge (1 = off)
    float edgeAccelSeconds = 0.4f;     // time constant of the edge boost
    bool recenterOnRelease = false;    // spring the pointer back to centre when the stick is released
    float recenterSeconds = 0.25f;     // spring time constant
    // Synthetic sensor bar geometry, in IR camera pixels (1024x768 space).
    float dotSeparation = 72.0f;
    float cameraMinX = 60.f, cameraMaxX = 964.f;
    float cameraMinY = 60.f, cameraMaxY = 708.f;
    // Left stick
    float leftDeadzone = 0.12f;
    float leftSaturation = 1.0f;       // deflection that counts as full on the left stick (applied when sampling the pad)
    bool invertMoveY = false;          // swap left stick up/down (applied when sampling the pad)
    float triggerThreshold = 0.5f;     // analog trigger pull that counts as pressed (applied when sampling the pad)
};

struct SweepPreset {
    const char* name;
    float tracking, crossover, peak;
};
inline constexpr SweepPreset kSweepPresets[] = {
    {"Balanced", 420.f, 0.52f, 3200.f},
    {"Sharpshooter", 260.f, 0.65f, 2400.f},
    {"Arcade rush", 600.f, 0.42f, 5200.f},
};

inline void ApplySweepPreset(Config& c, const SweepPreset& p) {
    c.curve = Curve::PrecisionSweep;
    c.deadzoneMode = DeadzoneMode::ScaledRadial;
    c.trackingSpeed = p.tracking;
    c.dualZoneSplit = p.crossover;
    c.maxSpeedX = p.peak;
    c.maxSpeedY = p.peak * (700.f / 900.f);
    c.speedCap = 8000.f;
    c.rampSeconds = c.smoothingMs = 0.f;
    c.edgeBoost = c.sensitivityX = c.sensitivityY = 1.f;
    c.absoluteMode = c.recenterOnRelease = false;
}

inline Config ModernAim() {
    Config c;
    ApplySweepPreset(c, kSweepPresets[0]);
    return c;
}

// With the virtual 72-pixel bar, the guest reports ~0.00443 KPAD units per
// camera pixel. TV bar placement offsets the vertical origin by ~103 pixels.
// Keep a stick sweep inside the visible -1..1 playfield, even at its corners.
inline Config StickPointerConfig(Config c, bool barAbove) {
    if (c.curve != Curve::PrecisionSweep) return c;
    c.cameraMinX = 287.f;
    c.cameraMaxX = 736.f;
    const float centerY = barAbove ? 487.f : 281.f;
    c.cameraMinY = centerY - 224.f;
    c.cameraMaxY = centerY + 224.f;
    // GunSight draws in an 810x456 layer. Equal screen-pixel motion on
    // both axes needs more vertical camera travel per unit of stick input.
    c.maxSpeedY = c.maxSpeedX * (810.f / 456.f);
    return c;
}

inline void Sanitize(Config& c) {
    auto fin = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    c.radialDeadzone = std::clamp(fin(c.radialDeadzone, 0.12f), 0.0f, 0.6f);
    c.outerSaturation = std::clamp(fin(c.outerSaturation, 0.95f), c.radialDeadzone + 0.05f, 1.0f);
    c.leftDeadzone = std::clamp(fin(c.leftDeadzone, 0.12f), 0.0f, 0.6f);
    c.leftSaturation = std::clamp(fin(c.leftSaturation, 1.0f), 0.5f, 1.0f);
    c.triggerThreshold = std::clamp(fin(c.triggerThreshold, 0.5f), 0.1f, 0.9f);
    c.trackingSpeed = std::clamp(fin(c.trackingSpeed, 420.f), 50.f, 1200.f);
    c.curveExponent = std::clamp(fin(c.curveExponent, 1.6f), 0.3f, 4.0f);
    c.dualZoneSplit = std::clamp(fin(c.dualZoneSplit, 0.5f), 0.1f, 0.9f);
    c.dualZoneOutput = std::clamp(fin(c.dualZoneOutput, 0.25f), 0.05f, 0.9f);
    c.sensitivityX = std::clamp(fin(c.sensitivityX, 1.0f), 0.1f, 4.0f);
    c.sensitivityY = std::clamp(fin(c.sensitivityY, 1.0f), 0.1f, 4.0f);
    c.smoothingMs = std::clamp(fin(c.smoothingMs, 0.0f), 0.0f, 150.0f);
    c.precisionScale = std::clamp(fin(c.precisionScale, 0.35f), 0.1f, 1.0f);
    c.maxSpeedX = std::clamp(fin(c.maxSpeedX, 900.0f), 100.0f, 8000.0f);
    c.maxSpeedY = std::clamp(fin(c.maxSpeedY, 700.0f), 100.0f, 8000.0f);
    c.speedCap = std::clamp(fin(c.speedCap, 1600.0f), 200.0f, 8000.0f);
    c.rampSeconds = std::clamp(fin(c.rampSeconds, 0.12f), 0.0f, 0.5f);
    c.edgeBoost = std::clamp(fin(c.edgeBoost, 1.0f), 1.0f, 3.0f);
    c.edgeAccelSeconds = std::clamp(fin(c.edgeAccelSeconds, 0.4f), 0.05f, 2.0f);
    c.recenterSeconds = std::clamp(fin(c.recenterSeconds, 0.25f), 0.03f, 2.0f);
}

// ---- Stick shaping -----------------------------------------------------------------------------

// Response curve on a normalised magnitude m in [0, 1]. f(0) = 0, f(1) = 1, monotonic non-decreasing.
inline float EvalCurve(const Config& cfg, float m) {
    m = std::clamp(m, 0.0f, 1.0f);
    switch (cfg.curve) {
    case Curve::Linear:
    case Curve::Raw:
        return m;
    case Curve::Power:
        return std::pow(m, cfg.curveExponent);
    case Curve::SCurve:
        return m * m * (3.0f - 2.0f * m);
    case Curve::DualZone: {
        const float s = cfg.dualZoneSplit;
        const float o = cfg.dualZoneOutput;
        return m <= s ? o * m / s : o + (1.0f - o) * (m - s) / (1.0f - s);
    }
    case Curve::PrecisionSweep: {
        // Linear tracking plus a quadratic outer term. Both speed and slope
        // are continuous at crossover. Peak changes leave the inner gain alone.
        const float gain = std::min(cfg.trackingSpeed / cfg.maxSpeedX, 1.f);
        const float edge = std::max(0.f, (m - cfg.dualZoneSplit) / (1.f - cfg.dualZoneSplit));
        return gain * m + (1.f - gain) * edge * edge;
    }
    case Curve::Classic:
        return m * m * (0.4f + 0.6f * m);
    }
    return m;
}

// Deadzone remap of one magnitude (radial vector length, or a single axis): raw -> [0, 1] before the curve.
inline float RemapMagnitude(const Config& cfg, float m) {
    const float dz = cfg.radialDeadzone;
    const float sat = std::max(cfg.outerSaturation, dz + 0.05f);
    if (m <= dz) return 0.0f;
    if (cfg.deadzoneMode == DeadzoneMode::Radial) return std::min(1.0f, m / sat);
    return std::clamp((m - dz) / (sat - dz), 0.0f, 1.0f);
}

// Deadzone + curve. Direction is preserved (radial modes) or per-axis (Axial).
inline void ApplyDeadzoneAndCurve(const Config& cfg, float ax, float ay, float& ox, float& oy) {
    if (cfg.deadzoneMode == DeadzoneMode::Axial) {
        auto one = [&](float a) {
            const float f = EvalCurve(cfg, RemapMagnitude(cfg, std::fabs(a)));
            return a < 0.0f ? -f : f;
        };
        ox = one(ax);
        oy = one(ay);
        return;
    }
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

// Deadzone, curve, invert Y, precision scale, 1-pole smoothing. Output stays in the unit disc/box.
inline void ShapeStick(const Config& cfg, StickFilter& filter, float rawX, float rawY, float dt, bool precision,
                       float& outX, float& outY) {
    float sx = 0.f, sy = 0.f;
    ApplyDeadzoneAndCurve(cfg, rawX, rawY, sx, sy);
    if (cfg.invertY) sy = -sy;
    const float p = precision ? cfg.precisionScale : 1.0f;
    sx *= p;
    sy *= p;
    if (cfg.smoothingMs > 0.0f && cfg.curve != Curve::Raw && dt > 0.0f) {
        const float alpha = 1.0f - std::exp(-dt / (cfg.smoothingMs * 0.001f));
        filter.x += (sx - filter.x) * alpha;
        filter.y += (sy - filter.y) * alpha;
        if (std::fabs(filter.x) < 1e-4f) filter.x = 0.f;
        if (std::fabs(filter.y) < 1e-4f) filter.y = 0.f;
    } else {
        filter.x = sx;
        filter.y = sy;
    }
    outX = filter.x;
    outY = filter.y;
}

// True when the shaped stick is outside the deadzone (used for aim-device arbitration).
inline bool StickActive(const Config& cfg, float rawX, float rawY) {
    float ox = 0.f, oy = 0.f;
    ApplyDeadzoneAndCurve(cfg, rawX, rawY, ox, oy);
    return std::fabs(ox) > 1e-4f || std::fabs(oy) > 1e-4f;
}

// Classic Controller scheme: shaped stick with per-axis sensitivity as a gain, clamped to the unit box.
inline void ClassicStick(const Config& cfg, StickFilter& filter, float rawX, float rawY, float dt, bool precision,
                         float& outX, float& outY) {
    ShapeStick(cfg, filter, rawX, rawY, dt, precision, outX, outY);
    outX = std::clamp(outX * cfg.sensitivityX, -1.0f, 1.0f);
    outY = std::clamp(outY * cfg.sensitivityY, -1.0f, 1.0f);
}

// ---- Pointer synthesis (Wii Remote scheme) -----------------------------------------------------

struct PointerState {
    float x = 512.f;
    float y = 384.f;
    bool visible = false;      // true while the stick is deflected
    float heldSeconds = 0.f;   // time the stick has been continuously deflected
    float edgeSeconds = 0.f;   // time spent at the edge (>= 90 % deflection)
    StickFilter filter;
};

inline float CenterX(const Config& cfg) { return (cfg.cameraMinX + cfg.cameraMaxX) * 0.5f; }
inline float CenterY(const Config& cfg) { return (cfg.cameraMinY + cfg.cameraMaxY) * 0.5f; }

inline void Recenter(PointerState& state, const Config& cfg) {
    state.x = CenterX(cfg);
    state.y = CenterY(cfg);
    state.heldSeconds = 0.f;
    state.edgeSeconds = 0.f;
}

inline void ClampToCamera(PointerState& state, const Config& cfg) {
    state.x = std::clamp(state.x, cfg.cameraMinX, cfg.cameraMaxX);
    state.y = std::clamp(state.y, cfg.cameraMinY, cfg.cameraMaxY);
}

// One integration step. rawStick* are raw SDL-convention stick values after the caller's axis flip for the
// sensor-bar view (stick right moves the pointer right, which moves the bar LEFT in the camera, so the
// caller passes -x; Y is passed as is). dt in seconds, clamped to 50 ms.
inline void Advance(PointerState& state, const Config& cfg, float rawStickX, float rawStickY, float dt,
                    bool precision = false) {
    dt = std::clamp(dt, 0.0f, 0.05f);
    float ax = 0.f, ay = 0.f;
    ShapeStick(cfg, state.filter, rawStickX, rawStickY, dt, precision, ax, ay);
    if (cfg.absoluteMode) {
        state.visible = true;
        state.x = CenterX(cfg) + ax * cfg.sensitivityX * (cfg.cameraMaxX - cfg.cameraMinX) * 0.5f;
        state.y = CenterY(cfg) + ay * cfg.sensitivityY * (cfg.cameraMaxY - cfg.cameraMinY) * 0.5f;
        ClampToCamera(state, cfg);
        return;
    }
    const float mag = std::sqrt(ax * ax + ay * ay);
    state.visible = mag > 1e-4f;
    if (!state.visible) {
        state.heldSeconds = 0.f;
        state.edgeSeconds = std::max(0.f, state.edgeSeconds - dt);
        if (cfg.recenterOnRelease) {
            const float k = 1.0f - std::exp(-dt / std::max(cfg.recenterSeconds, 0.01f));
            state.x += (CenterX(cfg) - state.x) * k;
            state.y += (CenterY(cfg) - state.y) * k;
        }
        return;  // otherwise the pointer holds position
    }
    float ramp = 1.0f;
    if (cfg.rampSeconds > 0.0f && cfg.curve != Curve::Raw) {
        ramp = 0.5f + 0.5f * std::min(1.0f, state.heldSeconds / cfg.rampSeconds);
    }
    state.heldSeconds += dt;
    if (mag >= 0.9f) {
        state.edgeSeconds += dt;
    } else {
        state.edgeSeconds = std::max(0.f, state.edgeSeconds - dt);
    }
    const float boost = 1.0f + (cfg.edgeBoost - 1.0f) * (1.0f - std::exp(-state.edgeSeconds / std::max(cfg.edgeAccelSeconds, 0.01f)));
    float vx = ax * cfg.maxSpeedX * cfg.sensitivityX * ramp * boost;
    float vy = ay * cfg.maxSpeedY * cfg.sensitivityY * ramp * boost;
    const float speed = std::sqrt(vx * vx + vy * vy);
    if (speed > cfg.speedCap) {
        vx *= cfg.speedCap / speed;
        vy *= cfg.speedCap / speed;
    }
    state.x += vx * dt;
    state.y += vy * dt;
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

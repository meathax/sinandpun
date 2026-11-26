// Unit tests for the pure controls logic: dual_analog.h and input_map.h (no SDL, no aim_settings.h).
// Standalone: clang++ -std=c++20 -I runtime/include runtime/tests/controls_tests.cpp -o controls_tests && ./controls_tests

#include "gyro_aim.h"
#include "dual_analog.h"
#include "input_map.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

using namespace DualAnalog;

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static void TestCurves() {
    for (float e : {1.f, 1.6f, 2.2f, 3.f, 4.f}) {
        Config cfg;
        cfg.curveExponent = e;
        CHECK(Near(EvalCurve(cfg, 0.f), 0.f));
        CHECK(Near(EvalCurve(cfg, 1.f), 1.f));
        float previous = -1.f;
        for (int i = 0; i <= 1000; ++i) {
            const float v = EvalCurve(cfg, static_cast<float>(i) / 1000.f);
            CHECK(v > previous);  // strictly increasing: every bit of travel changes speed
            CHECK(v >= 0.f && v <= 1.0001f);
            previous = v;
        }
    }
    Config linear;
    linear.curveExponent = 1.f;
    CHECK(Near(EvalCurve(linear, 0.37f), 0.37f));
    // Default: fine near the centre, slope never zero (the linear share), fast at the rim.
    const Config d;
    CHECK(EvalCurve(d, 0.2f) < 0.08f);
    CHECK(EvalCurve(d, 0.02f) / 0.02f >= kLinearShare - 1e-4f);
    CHECK(EvalCurve(d, 0.9f) > 0.75f);
    const float slopeEdge = (EvalCurve(d, 1.f) - EvalCurve(d, 0.99f)) / 0.01f;
    CHECK(slopeEdge > 1.5f);  // unlike smoothstep, still accelerating at full tilt
}

static void TestDeadzoneEdges() {
    Config cfg;
    float ox = 9.f, oy = 9.f;
    ApplyDeadzoneAndCurve(cfg, cfg.radialDeadzone * 0.99f, 0.f, ox, oy);
    CHECK(ox == 0.f && oy == 0.f);  // inside the deadzone
    ApplyDeadzoneAndCurve(cfg, 1.0f, 0.f, ox, oy);
    CHECK(Near(ox, 1.f) && Near(oy, 0.f));  // full deflection reaches 1
    ApplyDeadzoneAndCurve(cfg, 0.f, -1.0f, ox, oy);
    CHECK(Near(oy, -1.f));
    ApplyDeadzoneAndCurve(cfg, cfg.outerSaturation, 0.f, ox, oy);
    CHECK(Near(ox, 1.f));  // saturation reached at the outer edge
    // A square-gate diagonal (|v| > 1) stays in the unit disc and keeps its direction.
    ApplyDeadzoneAndCurve(cfg, 1.f, 1.f, ox, oy);
    CHECK(Near(std::sqrt(ox * ox + oy * oy), 1.f) && Near(ox, oy));
    // Radial deadzone: a stick resting slightly off-centre on both axes is still ignored.
    CHECK(!StickActive(cfg, 0.06f, 0.06f));
    CHECK(StickActive(cfg, 0.2f, 0.f));
}

static void TestContinuity() {
    for (float e : {1.f, 2.2f, 4.f}) {
        Config cfg;
        cfg.curveExponent = e;
        float below = 0.f, above = 0.f, y = 0.f;
        ApplyDeadzoneAndCurve(cfg, cfg.radialDeadzone - 1e-4f, 0.f, below, y);
        ApplyDeadzoneAndCurve(cfg, cfg.radialDeadzone + 1e-3f, 0.f, above, y);
        CHECK(std::fabs(above - below) < 0.01f);  // no jump at the inner edge
        ApplyDeadzoneAndCurve(cfg, cfg.outerSaturation - 1e-3f, 0.f, below, y);
        ApplyDeadzoneAndCurve(cfg, cfg.outerSaturation + 1e-3f, 0.f, above, y);
        CHECK(std::fabs(above - below) < 0.05f);
    }
}

static void TestSymmetryAndMonotonicSweep() {
    Config cfg;
    float previous = 0.f;
    for (int i = 0; i <= 400; ++i) {
        const float m = static_cast<float>(i) / 400.f;
        float px = 0.f, py = 0.f, nx = 0.f, ny = 0.f;
        ApplyDeadzoneAndCurve(cfg, m, 0.f, px, py);
        ApplyDeadzoneAndCurve(cfg, -m, 0.f, nx, ny);
        CHECK(Near(px, -nx));
        CHECK(px + 1e-5f >= previous);
        previous = px;
        ApplyDeadzoneAndCurve(cfg, 0.f, m, px, py);
        ApplyDeadzoneAndCurve(cfg, 0.f, -m, nx, ny);
        CHECK(Near(py, -ny));
    }
    for (int deg = 0; deg < 360; deg += 15) {  // every direction keeps its angle
        const float a = static_cast<float>(deg) * 3.14159265f / 180.f;
        float ox = 0.f, oy = 0.f;
        ApplyDeadzoneAndCurve(cfg, 0.7f * std::cos(a), 0.7f * std::sin(a), ox, oy);
        CHECK(Near(std::atan2(oy, ox), std::atan2(std::sin(a), std::cos(a)), 1e-3f) || deg == 180);
    }
}

static void TestShapeAndSensitivity() {
    Config cfg;
    cfg.invertY = true;
    StickFilter filter;
    float x = 0.f, y = 0.f;
    ShapeStick(cfg, filter, 0.f, 1.f, 0.01f, x, y);
    CHECK(y < 0.f);  // inverted
    cfg.invertY = false;
    ShapeStick(cfg, filter, 0.f, 1.f, 0.01f, x, y);
    CHECK(Near(y, 1.f));
    // Classic: deadzone only (the game applies its own response), sensitivity as a clamped gain.
    float cx = 0.f, cy = 0.f;
    ClassicStick(cfg, filter, 0.55f, 0.f, 0.01f, cx, cy);
    CHECK(Near(cx, (0.55f - cfg.radialDeadzone) / (cfg.outerSaturation - cfg.radialDeadzone)));
    cfg.sensitivity = 2.f;
    ClassicStick(cfg, filter, 1.f, 0.f, 0.01f, cx, cy);
    CHECK(Near(cx, 1.f));
    // No hidden smoothing: the output follows the stick in a single step.
    cfg = Config{};
    ShapeStick(cfg, filter, 1.f, 0.f, 0.01f, x, y);
    CHECK(Near(x, 1.f));
    ShapeStick(cfg, filter, 0.f, 0.f, 0.01f, x, y);
    CHECK(x == 0.f);
}

static void TestStickPointerCalibration() {
    for (bool above : {false, true}) {
        const Config calibrated = StickPointerConfig(Config{}, above);
        const float bias = above ? -0.457f : 0.457f;
        auto guestY = [&](float y) { return bias + (y - 384.f) * 0.00443f; };
        CHECK(std::fabs(guestY(CenterY(calibrated))) < 0.004f);
        CHECK(Near(guestY(calibrated.cameraMinY), -1.f, 0.012f));
        CHECK(Near(guestY(calibrated.cameraMaxY), 1.f, 0.012f));
        // Equal screen-pixel speed on both axes (GunSight 810x456 layer).
        CHECK(Near(calibrated.maxSpeedX * 405.f / (calibrated.maxSpeedY * 228.f), 1.f));
    }
    Config c;
    c.sensitivity = 2.f;
    c.verticalRatio = 0.5f;
    const Config k = StickPointerConfig(c, false);
    CHECK(Near(k.maxSpeedX, 2.f * kFullTiltSpeed));
    CHECK(Near(k.maxSpeedY, k.maxSpeedX * kScreenAspectY * 0.5f));
}

static void TestTraversalAndFrameRate() {
    const Config c = StickPointerConfig(Config{}, false);
    const float span = c.cameraMaxX - c.cameraMinX;
    // Full tilt crosses the playfield in about a third of a second, at any report/frame rate.
    for (int hz : {30, 60, 100, 120, 144, 240, 500}) {
        PointerState p;
        p.x = c.cameraMinX;
        int ticks = 0;
        while (p.x < c.cameraMaxX && ticks < 4 * hz) {
            Advance(p, c, 1.f, 0.f, 1.f / hz);
            ++ticks;
        }
        const float seconds = static_cast<float>(ticks) / hz;
        CHECK(std::fabs(seconds - span / kFullTiltSpeed) <= 1.001f / hz);
        CHECK(seconds > 0.25f && seconds < 0.45f);
        const float stopped = p.x;
        Advance(p, c, 0.f, 0.f, 1.f / hz);
        CHECK(p.x == stopped);  // releasing holds position immediately (no glide, no spring)
    }
    // Half tilt: same distance after one second whatever the rate (no ramp, no history).
    float reference = 0.f;
    for (int hz : {60, 120, 240, 1000}) {
        PointerState p;
        p.x = c.cameraMinX;
        for (int i = 0; i < hz / 4; ++i) Advance(p, c, 0.5f, 0.f, 1.f / hz);
        if (hz == 60) reference = p.x;
        CHECK(Near(p.x, reference, 0.05f));
    }
    // First report after leaving the deadzone already moves at the curve speed (no ramp-up).
    PointerState fine;
    Recenter(fine, c);
    Advance(fine, c, 1.f, 0.f, 0.01f);
    CHECK(Near(fine.x - CenterX(c), kFullTiltSpeed * 0.01f, 0.01f));
    // Small corrections: 30 % of usable travel is slow enough for precise tracking.
    PointerState slow;
    Recenter(slow, c);
    const float raw = c.radialDeadzone + 0.3f * (c.outerSaturation - c.radialDeadzone);
    Advance(slow, c, raw, 0.f, 0.1f * 0.5f);
    CHECK(slow.x - CenterX(c) < 0.15f * kFullTiltSpeed * 0.05f);
    // Clamped at the visible edge.
    PointerState edge;
    Recenter(edge, c);
    for (int i = 0; i < 400; ++i) Advance(edge, c, 1.f, 1.f, 0.016f);
    CHECK(Near(edge.x, c.cameraMaxX) && Near(edge.y, c.cameraMaxY));
}

static void TestGyroTightening() {
    // Below the tightening threshold motion is scaled down, above it it passes unchanged.
    GyroAim::Config cfg;
    cfg.space = 1;  // local yaw: no sensor-fusion warm-up in a synthetic run
    GyroAim::Motion m;
    m.fusion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Manual);
    auto run = [&](float degPerSec) {
        m.Reset();
        m.fusion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Manual);
        uint64_t t = 1'000'000'000ull;
        const float rad = degPerSec / 57.295779513f;
        m.Sample(t, 0.f, 0.f, 0.f, cfg, true);
        for (int i = 0; i < 100; ++i) {
            t += 5'000'000ull;
            m.Sample(t, 0.f, rad, 0.f, cfg, true);
        }
        float x = 0.f, y = 0.f;
        m.Consume(x, y);
        return std::fabs(x);
    };
    const float fast = run(40.f), tremor = run(0.3f);
    CHECK(fast > 0.f);
    // 0.5 s at 40 deg/s = 20 degrees: ~sensitivity pixels per degree.
    CHECK(Near(fast, 20.f * cfg.sensitivityX, 0.05f * 20.f * cfg.sensitivityX));
    CHECK(tremor < 0.3f * 0.5f * cfg.sensitivityX * 0.5f);  // scaled down by speed/threshold
}

static void TestSanitize() {
    Config c;
    c.radialDeadzone = 5.f;
    c.outerSaturation = -1.f;
    c.sensitivity = std::nanf("");
    c.curveExponent = 99.f;
    c.verticalRatio = 0.f;
    Sanitize(c);
    CHECK(c.radialDeadzone <= 0.5f);
    CHECK(c.outerSaturation >= c.radialDeadzone + 0.1f - 1e-6f && c.outerSaturation <= 1.f);
    CHECK(std::isfinite(c.sensitivity) && c.sensitivity == 1.f);
    CHECK(c.curveExponent <= 4.f && c.verticalRatio >= 0.5f);
}
static void TestMouseMapping() {
    using namespace DualAnalog;
    Config cfg;
    // 16:9 picture letterboxed (force aspect) in a 4:3 window: bars top and bottom.
    const Rect r = ContentRect(1600.f, 1200.f, true, 16.f / 9.f);
    CHECK(Near(r.w, 1600.f) && Near(r.h, 900.f) && Near(r.x, 0.f) && Near(r.y, 150.f));
    const Rect wide = ContentRect(2000.f, 1000.f, true, 4.f / 3.f);  // pillarbox
    CHECK(Near(wide.h, 1000.f) && Near(wide.w, 1333.3333f, 0.01f) && Near(wide.x, 333.3333f, 0.01f));
    const Rect stretch = ContentRect(1280.f, 720.f, false, 4.f / 3.f);
    CHECK(Near(stretch.w, 1280.f) && Near(stretch.h, 720.f));
    float u = 0.f, v = 0.f;
    WindowToNormalized(r, 800.f, 600.f, u, v);
    CHECK(Near(u, 0.5f) && Near(v, 0.5f));
    WindowToNormalized(r, -50.f, 10.f, u, v);  // outside clamps to the edge
    CHECK(u == 0.f && v == 0.f);
    WindowToNormalized(r, 5000.f, 5000.f, u, v);
    CHECK(u == 1.f && v == 1.f);
    float cx = 0.f, cy = 0.f;
    NormalizedToCamera(cfg, 0.5f, 0.5f, 1.f, 1.f, cx, cy);
    CHECK(Near(cx, CenterX(cfg)) && Near(cy, CenterY(cfg)));
    NormalizedToCamera(cfg, 0.f, 0.f, 1.f, 1.f, cx, cy);
    CHECK(Near(cx, cfg.cameraMaxX) && Near(cy, cfg.cameraMinY));  // left edge = bar at the camera's right
    NormalizedToCamera(cfg, 1.f, 1.f, 1.f, 1.f, cx, cy);
    CHECK(Near(cx, cfg.cameraMinX) && Near(cy, cfg.cameraMaxY));
    // Monotonic: moving the cursor right never moves the camera x right.
    float previous = 1e9f;
    for (int i = 0; i <= 100; ++i) {
        NormalizedToCamera(cfg, i / 100.f, 0.5f, 1.f, 1.f, cx, cy);
        CHECK(cx <= previous + 1e-4f);
        previous = cx;
    }
    PointerState p;
    Recenter(p, cfg);
    MouseRelativeStep(p, cfg, 10.f, -10.f, 0.5f);
    CHECK(p.x < CenterX(cfg) && p.y < CenterY(cfg));
    float sx = 0.f, sy = 0.f;
    MouseDeltaToStick(1000.f, 0.f, 0.01f, 1500.f, sx, sy);
    CHECK(Near(sx, 1.f) && Near(sy, 0.f));
    CHECK(Near(SmoothToward(0.f, 10.f, 0.01f, 0.f), 10.f));
    CHECK(SmoothToward(0.f, 10.f, 0.01f, 100.f) < 10.f);
}

static void TestNunchukAndQuantize() {
    const NunchukState centre = NunchukFromStick(0.f, 0.f);
    CHECK(centre.x == 128 && centre.y == 128);
    const NunchukState full = NunchukFromStick(1.f, 0.f, 0.f);
    CHECK(full.x == 224 && full.y == 128);
    CHECK(QuantizeStick(0.f, 32, 31) == 32);
    CHECK(QuantizeStick(1.f, 32, 31) == 63);
    CHECK(QuantizeStick(-1.f, 16, 15) == 1);
    CHECK(QuantizeStick(5.f, 16, 15) == 31);
}

static void TestBindings() {
    using namespace InputMap;
    // Names round-trip.
    for (const auto& n : kPadButtonNames) {
        const Binding b = Parse(std::string("pad:") + n.name);
        CHECK(b.Valid() && Format(b) == std::string("pad:") + n.name);
    }
    for (const auto& n : kPadAxisNames) {
        const Binding b = Parse(std::string("pad:") + n.name);
        CHECK(b.Valid() && Format(b) == std::string("pad:") + n.name);
    }
    for (const auto& n : kMouseNames) {
        const Binding b = Parse(std::string("mouse:") + n.name);
        CHECK(b.Valid() && Format(b) == std::string("mouse:") + n.name);
    }
    CHECK(Parse("key:Space") == Key(44));
    CHECK(Format(Key(44)) == "key:Space");
    CHECK(Parse("key:#99") == Key(99));
    CHECK(!Parse("pad:nonsense").Valid());
    CHECK(!Parse("garbage").Valid());
    CHECK(!Parse("key:#9999").Valid());

    // Generic joystick / HOTAS bindings: parse, format, profile filtering, and resolution.
    {
        const Binding jb = Parse("joy0:btn3");
        CHECK(jb == JoyButton(0, 3) && JoyDevice(jb) == 0 && JoyLocalCode(jb) == 3);
        CHECK(Format(jb) == "joy0:btn3");
        const Binding jaPos = Parse("joy1:axis2+");
        const Binding jaNeg = Parse("joy1:axis2-");
        CHECK(jaPos == JoyAxis(1, 2, 1) && Format(jaPos) == "joy1:axis2+");
        CHECK(jaNeg == JoyAxis(1, 2, -1) && Format(jaNeg) == "joy1:axis2-");
        const Binding jhUp = Parse("joy2:hat0_up");
        const Binding jhRight = Parse("joy2:hat0_right");
        const Binding jhDown = Parse("joy2:hat0_down");
        const Binding jhLeft = Parse("joy2:hat0_left");
        CHECK(jhUp == JoyHat(2, 0, 0) && Format(jhUp) == "joy2:hat0_up");
        CHECK(jhRight == JoyHat(2, 0, 1) && Format(jhRight) == "joy2:hat0_right");
        CHECK(jhDown == JoyHat(2, 0, 2) && Format(jhDown) == "joy2:hat0_down");
        CHECK(jhLeft == JoyHat(2, 0, 3) && Format(jhLeft) == "joy2:hat0_left");
        CHECK(!Parse("joy9:btn0").Valid());
        CHECK(!Parse("joy0:axis99+").Valid());
        CHECK(!Parse("joy0:hat0_bogus").Valid());
        const Slots padJoy = ParseList("joy0:btn3 | joy1:axis2+ | joy2:hat0_up", Profile::Gamepad);
        CHECK(padJoy[0] == jb && padJoy[1] == jaPos && padJoy[2] == jhUp);
        const Slots kbmJoy = ParseList("joy0:btn3 | key:A", Profile::KeyboardMouse);
        CHECK(kbmJoy[0] == Key(4) && !kbmJoy[1].Valid());

        RawSample jraw;
        CHECK(Value(jb, jraw) == 0.f);  // disconnected returns 0
        jraw.joys[0].connected = true;
        jraw.joys[0].buttons[3] = true;
        jraw.joys[1].connected = true;
        jraw.joys[1].axes[2] = 0.75f;
        jraw.joys[2].connected = true;
        jraw.joys[2].hats[0] = (1u << 0) | (1u << 1);  // up + right
        CHECK(Value(jb, jraw) == 1.f);
        CHECK(Value(JoyButton(0, 4), jraw) == 0.f);
        CHECK(Near(Value(jaPos, jraw), 0.75f));
        CHECK(Value(jaNeg, jraw) == 0.f);
        jraw.joys[1].axes[2] = -0.4f;
        CHECK(Value(jaPos, jraw) == 0.f);
        CHECK(Near(Value(jaNeg, jraw), 0.4f));
        CHECK(Value(jhUp, jraw) == 1.f && Value(jhRight, jraw) == 1.f);
        CHECK(Value(jhDown, jraw) == 0.f && Value(jhLeft, jraw) == 0.f);
    }

    // Lists: profile filtering, duplicates, slot limit, tolerance.
    const Slots pad = ParseList("pad:south | key:A | pad:south | pad:east | pad:west | pad:north", Profile::Gamepad);
    CHECK(pad[0] == PadButton(0) && pad[1] == PadButton(1) && pad[2] == PadButton(2));
    const Slots kbm = ParseList("key:J | bogus | mouse:left", Profile::KeyboardMouse);
    CHECK(kbm[0] == Key(13) && kbm[1] == MouseButton(1) && !kbm[2].Valid());
    CHECK(FormatList(kbm) == "key:J | mouse:left");
    CHECK(FormatList(ParseList("", Profile::Gamepad)).empty());

    // Resolution.
    RawSample raw;
    raw.padButtons[0] = true;
    raw.padAxes[5] = 0.8f;
    raw.padAxes[0] = -0.6f;
    raw.keys[13] = true;
    raw.mouseButtons = 0x01;
    raw.wheelUp = true;
    CHECK(Value(PadButton(0), raw) == 1.f);
    CHECK(Value(PadButton(1), raw) == 0.f);
    CHECK(Near(Value(PadAxis(5, 1), raw), 0.8f));
    CHECK(Near(Value(PadAxis(0, -1), raw), 0.6f));
    CHECK(Value(PadAxis(0, 1), raw) == 0.f);
    CHECK(Value(Key(13), raw) == 1.f && Value(Key(14), raw) == 0.f);
    CHECK(Value(MouseButton(1), raw) == 1.f && Value(MouseButton(3), raw) == 0.f);
    CHECK(Value(MouseButton(kMouseWheelUp), raw) == 1.f && Value(MouseButton(kMouseWheelDown), raw) == 0.f);
    CHECK(SlotsValue(kbm, raw) == 1.f);
    CHECK(Value(Binding{}, raw) == 0.f);

    // Conflict detection and swap.
    std::array<Slots, 3> table{};
    AssignSwap(table, 0, 0, Key(4));
    AssignSwap(table, 1, 0, Key(5));
    CHECK(FindOwner(table, Key(4), 1) == std::optional<size_t>(0));
    CHECK(!FindOwner(table, Key(4), 0).has_value());
    CHECK(CountConflicts(table) == 0);
    const auto swapped = AssignSwap(table, 1, 0, Key(4));  // steals action 0's key
    CHECK(swapped == std::optional<size_t>(0));
    CHECK(table[1][0] == Key(4) && table[0][0] == Key(5));  // swapped, nothing lost
    CHECK(CountConflicts(table) == 0);
    AssignSwap(table, 1, 1, Key(4));  // duplicate inside one action collapses
    CHECK(table[1][0] == Binding{} && table[1][1] == Key(4));
    AssignSwap(table, 1, 1, Binding{});  // clear
    CHECK(!table[1][1].Valid());
    table[2][0] = Key(7);
    table[2][1] = Key(7);
    CHECK(CountConflicts(table) == 0);  // same-action duplicates are not cross-action conflicts
    table[0][1] = Key(7);
    CHECK(CountConflicts(table) >= 1);
}

static void TestArbitration() {
    using namespace InputMap;
    Arbiter a;
    CHECK(Arbitrate(a, false, false) == AimSource::Stick);
    CHECK(Arbitrate(a, false, true) == AimSource::Mouse);   // mouse starts moving
    CHECK(Arbitrate(a, false, false) == AimSource::Mouse);  // idle: stays
    CHECK(Arbitrate(a, true, false) == AimSource::Stick);   // stick starts
    CHECK(Arbitrate(a, true, true) == AimSource::Mouse);    // mouse starts while the stick is held: it takes over
    Arbiter b;
    Arbitrate(b, true, false);
    CHECK(b.active == AimSource::Stick);
    Arbitrate(b, true, true);
    CHECK(b.active == AimSource::Mouse);
    Arbitrate(b, true, true);  // both steady: no fighting, stays
    CHECK(b.active == AimSource::Mouse);
    Arbitrate(b, true, false);  // mouse stops, stick still held: stick drives again
    CHECK(b.active == AimSource::Stick);
    Arbiter c;
    Arbitrate(c, true, true);  // both start in the same poll: current (Stick) stays
    CHECK(c.active == AimSource::Stick);
}

// ---- scheme selection ------------------------------------------------------------------------------

static InputMap::DeviceSense Pad(bool input, bool held) {
    InputMap::DeviceSense s;
    s.padInput = input;
    s.padHeld = held;
    return s;
}
static InputMap::DeviceSense Kbm(bool input, bool held) {
    InputMap::DeviceSense s;
    s.kbmInput = input;
    s.kbmHeld = held;
    return s;
}

static void TestSchemeModeParsing() {
    using namespace InputMap;
    CHECK(ParseSchemeMode("auto", 2, SchemeMode::Pointer) == SchemeMode::Auto);
    CHECK(ParseSchemeMode(" Classic ", 2, SchemeMode::Auto) == SchemeMode::Classic);
    CHECK(ParseSchemeMode("pointer", 2, SchemeMode::Auto) == SchemeMode::Pointer);
    CHECK(ParseSchemeMode("kbm", 2, SchemeMode::Auto) == SchemeMode::KeyboardMouse);
    CHECK(ParseSchemeMode(" M+KB ", 2, SchemeMode::Auto) == SchemeMode::KeyboardMouse);
    CHECK(ParseSchemeMode("mkb", 2, SchemeMode::Auto) == SchemeMode::KeyboardMouse);
    CHECK(ParseSchemeMode("keyboard_mouse", 2, SchemeMode::Auto) == SchemeMode::KeyboardMouse);
    CHECK(ParseSchemeMode("pointer", 1, SchemeMode::Classic) == SchemeMode::Auto);  // old default migrates
    CHECK(ParseSchemeMode("classic", 1, SchemeMode::Auto) == SchemeMode::Classic);  // deliberate opt-in kept
    CHECK(ParseSchemeMode("bogus", 2, SchemeMode::Classic) == SchemeMode::Classic);
    for (size_t i = 0; i < kSchemeModeKeys.size(); ++i) {  // round trip
        const SchemeMode m = static_cast<SchemeMode>(i);
        CHECK(ParseSchemeMode(SchemeModeKey(m), kSchemeSchemaVersion, SchemeMode::Auto) == m);
    }
    CHECK(kSchemeModeLabels.size() == kSchemeModeKeys.size());
    CHECK(EffectiveScheme(SchemeMode::Classic, SchemeArbiter{}) == Scheme::Classic);
    SchemeArbiter a;
    a.device = ActiveDevice::Gamepad;
    CHECK(EffectiveScheme(SchemeMode::Auto, a) == Scheme::Pointer);
    CHECK(EffectiveScheme(SchemeMode::Pointer, a) == Scheme::Pointer);
    CHECK(EffectiveScheme(SchemeMode::KeyboardMouse, a) == Scheme::Pointer);
}

static void TestSense() {
    using namespace InputMap;
    const SenseConfig cfg;
    std::bitset<512> bound;
    bound[26] = true;  // W
    RawSample prev, cur;
    // Stick noise below the threshold is ignored; a deliberate push counts and is "held".
    cur.padAxes[2] = 0.2f;
    CHECK(!Sense(prev, cur, 0.f, bound, cfg).padInput);
    cur.padAxes[2] = 0.7f;
    DeviceSense s = Sense(prev, cur, 0.f, bound, cfg);
    CHECK(s.padInput && s.padHeld && !s.kbmInput);
    // Trigger.
    cur = RawSample{};
    cur.padAxes[5] = 0.9f;
    CHECK(Sense(prev, cur, 0.f, bound, cfg).padInput);
    // Button edge versus steady hold.
    cur = RawSample{};
    cur.padButtons[0] = true;
    CHECK(Sense(prev, cur, 0.f, bound, cfg).padInput);
    prev = cur;
    s = Sense(prev, cur, 0.f, bound, cfg);
    CHECK(!s.padInput && s.padHeld);
    // Keys: only bound keys count.
    prev = cur = RawSample{};
    cur.keys[27] = true;  // X, unbound
    CHECK(!Sense(prev, cur, 0.f, bound, cfg).kbmInput);
    cur.keys[26] = true;
    s = Sense(prev, cur, 0.f, bound, cfg);
    CHECK(s.kbmInput && s.kbmHeld);
    // Mouse: tiny jitter ignored, real motion, buttons and wheel count.
    prev = cur = RawSample{};
    CHECK(!Sense(prev, cur, 1.f, bound, cfg).kbmInput);
    CHECK(Sense(prev, cur, 12.f, bound, cfg).kbmInput);
    cur.mouseButtons = 1;
    s = Sense(prev, cur, 0.f, bound, cfg);
    CHECK(s.kbmInput && s.kbmHeld);
    cur = RawSample{};
    cur.wheelDown = true;
    CHECK(Sense(prev, cur, 0.f, bound, cfg).kbmInput);
}

static void TestSchemeArbitration() {
    using namespace InputMap;
    const SchemeMode kAuto = SchemeMode::Auto;
    SchemeArbiter a;
    int64_t t = 10000;
    // First ever input is accepted immediately.
    SchemeUpdate u = UpdateScheme(a, kAuto, Pad(true, true), t);
    CHECK(u.deviceChanged && !u.schemeChanged && a.scheme == Scheme::Pointer && a.device == ActiveDevice::Gamepad);
    // Pad idle, dwell passed, mouse moves: switch.
    t += 600;
    u = UpdateScheme(a, kAuto, Kbm(true, false), t);
    CHECK(u.deviceChanged && a.scheme == Scheme::Pointer);
    // Immediately back with the pad: dwell blocks it.
    t += 100;
    u = UpdateScheme(a, kAuto, Pad(true, true), t);
    CHECK(!u.deviceChanged && a.scheme == Scheme::Pointer);
    // After the dwell it switches.
    t += 400;
    u = UpdateScheme(a, kAuto, Pad(true, true), t);
    CHECK(u.deviceChanged && a.scheme == Scheme::Pointer);
    // Never switch while the current device still holds something.
    t += 1000;
    DeviceSense mix = Kbm(true, false);
    mix.padHeld = true;
    u = UpdateScheme(a, kAuto, mix, t);
    CHECK(!u.deviceChanged && a.scheme == Scheme::Pointer);
    // Both devices active in one poll: nothing changes.
    DeviceSense both = Pad(true, false);
    both.kbmInput = true;
    u = UpdateScheme(a, kAuto, both, t + 1000);
    CHECK(!u.deviceChanged);
    // Held pointer-side key blocks a pad takeover.
    SchemeArbiter b;
    UpdateScheme(b, kAuto, Kbm(true, true), 0);
    DeviceSense padWhileKey = Pad(true, true);
    padWhileKey.kbmHeld = true;
    CHECK(!UpdateScheme(b, kAuto, padWhileKey, 5000).deviceChanged && b.scheme == Scheme::Pointer);
    CHECK(UpdateScheme(b, kAuto, Pad(true, true), 5100).deviceChanged && b.scheme == Scheme::Pointer);
    // Idle polls never change anything.
    SchemeArbiter c;
    SeedScheme(c, kAuto, ActiveDevice::Gamepad);
    CHECK(c.scheme == Scheme::Pointer);
    for (int i = 0; i < 100; ++i) CHECK(!UpdateScheme(c, kAuto, DeviceSense{}, 20000 + i * 16).deviceChanged);
    // Pinned modes keep the scheme but track the device.
    SchemeArbiter d;
    SeedScheme(d, SchemeMode::Pointer, ActiveDevice::None);
    u = UpdateScheme(d, SchemeMode::Pointer, Pad(true, true), 0);
    CHECK(u.deviceChanged && !u.schemeChanged && d.scheme == Scheme::Pointer && d.device == ActiveDevice::Gamepad);
    u = UpdateScheme(d, SchemeMode::Classic, DeviceSense{}, 10);
    CHECK(u.schemeChanged && d.scheme == Scheme::Classic);  // changing the setting applies live
    u = UpdateScheme(d, kAuto, DeviceSense{}, 20);
    CHECK(u.schemeChanged && d.scheme == Scheme::Pointer);  // auto restores modern pointer aim
    // Change of the setting from pinned classic to pinned pointer with the pad as last device.
    u = UpdateScheme(d, SchemeMode::Pointer, DeviceSense{}, 30);
    CHECK(!u.schemeChanged && d.scheme == Scheme::Pointer);
    // KeyboardMouse pins ActiveDevice::KeyboardMouse and ignores gamepad takeovers.
    SchemeArbiter e;
    SeedScheme(e, SchemeMode::KeyboardMouse, ActiveDevice::Gamepad);
    CHECK(e.device == ActiveDevice::KeyboardMouse && e.scheme == Scheme::Pointer);
    u = UpdateScheme(e, SchemeMode::KeyboardMouse, Pad(true, true), 1000);
    CHECK(!u.deviceChanged && !u.schemeChanged && e.device == ActiveDevice::KeyboardMouse && e.scheme == Scheme::Pointer);
    // Switching an existing Gamepad arbiter to KeyboardMouse pins KeyboardMouse immediately.
    SeedScheme(e, SchemeMode::Auto, ActiveDevice::Gamepad);
    u = UpdateScheme(e, SchemeMode::KeyboardMouse, Pad(true, true), 2000);
    CHECK(u.deviceChanged && !u.schemeChanged && e.device == ActiveDevice::KeyboardMouse && e.scheme == Scheme::Pointer);
}

static void TestPadConditioning() {
    using namespace InputMap;
    CHECK(Near(ConditionTrigger(0.f, 0.5f), 0.f));
    CHECK(Near(ConditionTrigger(0.49f, 0.5f), 0.f));
    CHECK(Near(ConditionTrigger(0.5f, 0.5f), 0.5f));
    CHECK(Near(ConditionTrigger(1.f, 0.5f), 1.f));
    CHECK(Near(ConditionTrigger(0.2f, 0.2f), 0.5f));  // lighter threshold still presses at 0.5
    float previous = 0.f;
    for (int i = 0; i <= 100; ++i) {
        const float v = ConditionTrigger(i / 100.f, 0.3f);
        CHECK(v + 1e-6f >= previous);
        previous = v;
    }
    RawSample raw;
    raw.padAxes[0] = 0.45f;
    raw.padAxes[1] = -0.45f;
    PadConditioning c;
    c.leftSaturation = 0.9f;
    c.invertMoveY = true;
    ConditionPad(raw, c);
    CHECK(Near(raw.padAxes[0], 0.5f));
    CHECK(Near(raw.padAxes[1], 0.5f));  // -0.45/0.9 inverted
    raw.padAxes[0] = 1.f;
    raw.padAxes[1] = 1.f;
    c = PadConditioning{};
    c.leftSaturation = 0.5f;
    ConditionPad(raw, c);
    CHECK(std::sqrt(raw.padAxes[0] * raw.padAxes[0] + raw.padAxes[1] * raw.padAxes[1]) <= 1.0001f);
}

static void TestRanges() {
    Config c;
    c.leftSaturation = 9.f;
    c.triggerThreshold = -3.f;
    c.leftDeadzone = std::nanf("");
    Sanitize(c);
    CHECK(c.leftSaturation == 1.f && c.triggerThreshold == 0.1f && std::isfinite(c.leftDeadzone));
    c.leftSaturation = 0.f;
    c.triggerThreshold = 5.f;
    Sanitize(c);
    CHECK(c.leftSaturation == 0.5f && c.triggerThreshold == 0.9f);
}

static void TestDefaultBindings() {
    using namespace InputMap;
    // Every default binding string of the real table must parse; mirrored here to stay free of aim_settings.h.
    struct D { const char* pad; const char* kbm; };
    static const D defaults[] = {
        {"pad:right_shoulder", "key:J | mouse:left"}, {"pad:right_trigger", "key:K | mouse:right"},
        {"pad:left_shoulder", "key:Space | mouse:x1 | mouse:wheel_up"},
        {"pad:left_trigger", "key:Left Shift | mouse:x2 | mouse:wheel_down"},
        {"pad:east", "key:Return"}, {"pad:south", "key:Backspace"}, {"pad:start", "key:P"}, {"pad:back", "key:Tab"},
        {"pad:guide", "key:Home"}, {"pad:north", "key:Q"}, {"pad:west", "key:E"},
        {"pad:left_stick_left", "key:A"}, {"pad:left_stick_right", "key:D"}, {"pad:left_stick_up", "key:W"},
        {"pad:left_stick_down", "key:S"}, {"pad:right_stick_left", "key:Left"}, {"pad:right_stick_right", "key:Right"},
    };
    std::array<Slots, std::size(defaults)> pad{}, kbm{};
    for (size_t i = 0; i < std::size(defaults); ++i) {
        pad[i] = ParseList(defaults[i].pad, Profile::Gamepad);
        kbm[i] = ParseList(defaults[i].kbm, Profile::KeyboardMouse);
        CHECK(pad[i][0].Valid());
        CHECK(kbm[i][0].Valid());
        CHECK(FormatList(pad[i]) == defaults[i].pad);
    }
    CHECK(CountConflicts(pad) == 0);
    CHECK(CountConflicts(kbm) == 0);
}

int main() {
    {
        GyroAim::Config c; c.space = 1; c.sensitivityX = c.sensitivityY = 1.f;
        GyroAim::Motion motion;
        motion.fusion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Manual);
        motion.Sample(1000000000, 0, 1, 0, c, true);
        motion.Sample(1010000000, 0, 1, 0, c, true);
        float x, y; motion.Consume(x, y);
        CHECK(std::fabs(x + 0.5729578f) < 0.001f && y == 0);
        motion.Consume(x, y); CHECK(x == 0 && y == 0);
        motion.Sample(2010000000, 0, 1, 0, c, true);
        motion.Consume(x, y); CHECK(x == 0 && y == 0);
        motion.Sample(2020000000, 0, 1, 0, c, false);
        motion.Consume(x, y); CHECK(x == 0 && y == 0);
        c.invertX = true;
        motion.Sample(2030000000, 0, 1, 0, c, true);
        motion.Consume(x, y); CHECK(std::fabs(x - 0.5729578f) < 0.001f);
        motion.Clear(); motion.Consume(x, y); CHECK(x == 0 && y == 0);
    }

    TestSchemeModeParsing();
    TestSense();
    TestSchemeArbitration();
    TestPadConditioning();
    TestRanges();
    TestDefaultBindings();
    TestCurves();
    TestDeadzoneEdges();
    TestContinuity();
    TestSymmetryAndMonotonicSweep();
    TestShapeAndSensitivity();
    TestStickPointerCalibration();
    TestTraversalAndFrameRate();
    TestGyroTightening();
    TestSanitize();
    TestMouseMapping();
    TestNunchukAndQuantize();
    TestBindings();
    TestArbitration();
    if (g_failures == 0) std::puts("controls_tests: all passed");
    return g_failures == 0 ? 0 : 1;
}

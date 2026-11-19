// Unit tests for the pure controls logic: dual_analog.h and input_map.h (no SDL, no aim_settings.h).
// Standalone: clang++ -std=c++20 -I runtime/include runtime/tests/controls_tests.cpp -o controls_tests && ./controls_tests

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
    for (int ci = 0; ci < kCurveCount; ++ci) {
        Config cfg;
        cfg.curve = static_cast<Curve>(ci);
        CHECK(Near(EvalCurve(cfg, 0.f), 0.f));
        CHECK(Near(EvalCurve(cfg, 1.f), 1.f));
        float previous = 0.f;
        for (int i = 0; i <= 1000; ++i) {
            const float v = EvalCurve(cfg, static_cast<float>(i) / 1000.f);
            CHECK(v + 1e-6f >= previous);  // monotonic non-decreasing
            CHECK(v >= -1e-6f && v <= 1.0001f);
            previous = v;
        }
    }
    Config power;
    power.curve = Curve::Power;
    for (float e : {0.3f, 0.65f, 1.f, 2.f, 4.f}) {
        power.curveExponent = e;
        float previous = 0.f;
        for (int i = 0; i <= 200; ++i) {
            const float v = EvalCurve(power, static_cast<float>(i) / 200.f);
            CHECK(v + 1e-6f >= previous);
            previous = v;
        }
    }
    Config dual;
    dual.curve = Curve::DualZone;
    CHECK(Near(EvalCurve(dual, dual.dualZoneSplit), dual.dualZoneOutput));
}

static void TestDeadzoneEdges() {
    for (DeadzoneMode mode : {DeadzoneMode::ScaledRadial, DeadzoneMode::Axial, DeadzoneMode::Radial}) {
        Config cfg;
        cfg.deadzoneMode = mode;
        float ox = 9.f, oy = 9.f;
        ApplyDeadzoneAndCurve(cfg, cfg.radialDeadzone * 0.99f, 0.f, ox, oy);
        CHECK(ox == 0.f && oy == 0.f);  // inside the deadzone
        ApplyDeadzoneAndCurve(cfg, 1.0f, 0.f, ox, oy);
        CHECK(Near(ox, 1.f) && Near(oy, 0.f));  // full deflection reaches 1
        ApplyDeadzoneAndCurve(cfg, 0.f, -1.0f, ox, oy);
        CHECK(Near(oy, -1.f));
        ApplyDeadzoneAndCurve(cfg, cfg.outerSaturation, 0.f, ox, oy);
        CHECK(Near(ox, 1.f));  // saturation reached at the outer edge
    }
}

static void TestContinuity() {
    // Scaled radial and axial: no jump at the inner edge or at saturation, for every curve.
    for (DeadzoneMode mode : {DeadzoneMode::ScaledRadial, DeadzoneMode::Axial}) {
        for (int ci = 0; ci < kCurveCount; ++ci) {
            Config cfg;
            cfg.deadzoneMode = mode;
            cfg.curve = static_cast<Curve>(ci);
            float below = 0.f, above = 0.f, y = 0.f;
            ApplyDeadzoneAndCurve(cfg, cfg.radialDeadzone - 1e-4f, 0.f, below, y);
            ApplyDeadzoneAndCurve(cfg, cfg.radialDeadzone + 1e-3f, 0.f, above, y);
            CHECK(std::fabs(above - below) < 0.02f);
            ApplyDeadzoneAndCurve(cfg, cfg.outerSaturation - 1e-3f, 0.f, below, y);
            ApplyDeadzoneAndCurve(cfg, cfg.outerSaturation + 1e-3f, 0.f, above, y);
            CHECK(std::fabs(above - below) < 0.05f);
        }
    }
    // Plain radial deliberately jumps at the inner edge.
    Config radial;
    radial.deadzoneMode = DeadzoneMode::Radial;
    float jump = 0.f, y = 0.f;
    ApplyDeadzoneAndCurve(radial, radial.radialDeadzone + 1e-3f, 0.f, jump, y);
    CHECK(jump > 0.05f);
}

static void TestSymmetryAndMonotonicSweep() {
    for (int mi = 0; mi < kDeadzoneModeCount; ++mi) {
        for (int ci = 0; ci < kCurveCount; ++ci) {
            Config cfg;
            cfg.deadzoneMode = static_cast<DeadzoneMode>(mi);
            cfg.curve = static_cast<Curve>(ci);
            float previous = 0.f;
            for (int i = 0; i <= 400; ++i) {
                const float m = static_cast<float>(i) / 400.f;
                float px = 0.f, py = 0.f, nx = 0.f, ny = 0.f;
                ApplyDeadzoneAndCurve(cfg, m, 0.f, px, py);
                ApplyDeadzoneAndCurve(cfg, -m, 0.f, nx, ny);
                CHECK(Near(px, -nx));  // point symmetry
                CHECK(px + 1e-5f >= previous);  // monotonic in deflection
                previous = px;
                ApplyDeadzoneAndCurve(cfg, 0.f, m, px, py);
                ApplyDeadzoneAndCurve(cfg, 0.f, -m, nx, ny);
                CHECK(Near(py, -ny));
            }
            // Radial modes keep the direction.
            if (cfg.deadzoneMode != DeadzoneMode::Axial) {
                float ox = 0.f, oy = 0.f;
                ApplyDeadzoneAndCurve(cfg, 0.6f, 0.6f, ox, oy);
                CHECK(Near(ox, oy));
            }
        }
    }
}

static void TestShapeAndSensitivity() {
    Config cfg;
    cfg.invertY = true;
    StickFilter filter;
    float x = 0.f, y = 0.f;
    ShapeStick(cfg, filter, 0.f, 1.f, 0.01f, false, x, y);
    CHECK(y < 0.f);  // inverted
    cfg.invertY = false;
    ShapeStick(cfg, filter, 0.f, 1.f, 0.01f, true, x, y);
    CHECK(Near(y, cfg.precisionScale));
    cfg.sensitivityX = 2.f;
    cfg.sensitivityY = 0.5f;
    float cx = 0.f, cy = 0.f;
    ClassicStick(cfg, filter, 1.f, 1.f, 0.01f, false, cx, cy);
    CHECK(cx <= 1.f && cy <= 1.f && cx > cy);
    // Smoothing converges and never overshoots.
    cfg = Config{};
    cfg.smoothingMs = 50.f;
    StickFilter smooth;
    float last = 0.f;
    for (int i = 0; i < 200; ++i) {
        ShapeStick(cfg, smooth, 1.f, 0.f, 0.01f, false, x, y);
        CHECK(x + 1e-6f >= last && x <= 1.0001f);
        last = x;
    }
    CHECK(x > 0.99f);
}

static void TestPointer() {
    Config cfg;
    PointerState p;
    Recenter(p, cfg);
    for (int i = 0; i < 400; ++i) Advance(p, cfg, 1.f, 0.f, 0.016f);
    CHECK(Near(p.x, cfg.cameraMaxX));  // saturates at the camera edge
    CHECK(Near(p.y, CenterY(cfg)));
    const float held = p.x;
    Advance(p, cfg, 0.f, 0.f, 0.016f);
    CHECK(Near(p.x, held));  // hold position on release
    cfg.recenterOnRelease = true;
    for (int i = 0; i < 400; ++i) Advance(p, cfg, 0.f, 0.f, 0.016f);
    CHECK(std::fabs(p.x - CenterX(cfg)) < 1.f);  // spring back
    cfg = Config{};
    cfg.speedCap = 200.f;
    Recenter(p, cfg);
    Advance(p, cfg, 1.f, 0.f, 0.05f);
    CHECK(p.x - CenterX(cfg) <= 200.f * 0.05f + 1e-3f);  // speed cap
    cfg = Config{};
    cfg.absoluteMode = true;
    Advance(p, cfg, 1.f, 0.f, 0.016f);
    CHECK(Near(p.x, cfg.cameraMaxX));  // full deflection maps to the camera edge
    cfg = Config{};
    cfg.edgeBoost = 2.f;
    PointerState slow, boosted;
    Config noBoost = cfg;
    noBoost.edgeBoost = 1.f;
    Recenter(slow, noBoost);
    Recenter(boosted, cfg);
    for (int i = 0; i < 20; ++i) {
        Advance(slow, noBoost, 0.f, 1.f, 0.016f);
        Advance(boosted, cfg, 0.f, 1.f, 0.016f);
    }
    CHECK(boosted.y > slow.y);
    // Frame-rate independence (dt below the clamp): same distance after one simulated second.
    Config c;
    c.rampSeconds = 0.f;
    PointerState a, b;
    Recenter(a, c);
    Recenter(b, c);
    for (int i = 0; i < 60; ++i) Advance(a, c, 0.5f, 0.f, 1.f / 60.f);
    for (int i = 0; i < 120; ++i) Advance(b, c, 0.5f, 0.f, 1.f / 120.f);
    CHECK(std::fabs(a.x - b.x) < 2.f);
}

static void TestSanitize() {
    Config c;
    c.radialDeadzone = 5.f;
    c.outerSaturation = -1.f;
    c.sensitivityX = std::nanf("");
    c.smoothingMs = 1e9f;
    Sanitize(c);
    CHECK(c.radialDeadzone <= 0.6f);
    CHECK(c.outerSaturation >= c.radialDeadzone + 0.05f - 1e-6f && c.outerSaturation <= 1.f);
    CHECK(std::isfinite(c.sensitivityX));
    CHECK(c.smoothingMs <= 150.f);
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

int main() {
    TestCurves();
    TestDeadzoneEdges();
    TestContinuity();
    TestSymmetryAndMonotonicSweep();
    TestShapeAndSensitivity();
    TestPointer();
    TestSanitize();
    TestMouseMapping();
    TestNunchukAndQuantize();
    TestBindings();
    TestArbitration();
    if (g_failures == 0) std::puts("controls_tests: all passed");
    return g_failures == 0 ? 0 : 1;
}

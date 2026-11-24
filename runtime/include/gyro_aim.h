#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

// Upstream expects the nonstandard M_PI macro; keep strict C++20 builds portable.
#ifndef M_PI
#define SIN_GYRO_LOCAL_PI
#define M_PI std::numbers::pi
#endif
#include "../../third_party/GamepadMotionHelpers/GamepadMotion.hpp"
#ifdef SIN_GYRO_LOCAL_PI
#undef M_PI
#undef SIN_GYRO_LOCAL_PI
#endif

namespace GyroAim {
inline constexpr float kDefaultSensitivity = 12.f;
// Rotation slower than this (deg/s) is scaled down proportionally ("tightening"): hand tremor and sensor noise
// do not shake the reticle, while deliberate motion is unaffected. Small enough not to feel like a deadzone.
inline constexpr float kTighteningDegPerSec = 1.2f;

struct Config {
    bool enabled = false;
    int activation = 0; // Always, hold gyro button, hold gyro button to pause.
    int space = 0;      // Player space, local yaw, local roll.
    // Horizontal: IR camera pixels per degree. The virtual remote scales vertical motion so one degree moves
    // the reticle the same number of screen pixels on both axes.
    float sensitivityX = kDefaultSensitivity;
    float sensitivityY = kDefaultSensitivity;
    bool invertX = false, invertY = false;
};

// Sensor timestamps, never render/report dt, integrate angular velocity exactly once.
struct Motion {
    GamepadMotion fusion;
    uint64_t stamp = 0;
    float ax = 0, ay = 1, az = 0;
    float dx = 0, dy = 0;
    Motion() { Reset(); }
    void Reset() {
        fusion.Reset();
        fusion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Stillness |
                                  GamepadMotionHelpers::CalibrationMode::SensorFusion);
        stamp = 0; ax = 0; ay = 1; az = 0; dx = dy = 0;
    }
    void Clear() { stamp = 0; dx = dy = 0; }
    void Accel(float x, float y, float z) {
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
            ax = x / 9.80665f; ay = y / 9.80665f; az = z / 9.80665f;
        }
    }
    void Sample(uint64_t ns, float x, float y, float z, const Config& c, bool active) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) { Clear(); return; }
        if (!stamp || ns <= stamp || ns - stamp > 100000000) { stamp = ns; return; }
        const float dt = static_cast<float>(ns - stamp) * 1e-9f;
        stamp = ns;
        constexpr float degrees = 57.295779513f;
        fusion.ProcessMotion(x * degrees, y * degrees, z * degrees, ax, ay, az, dt);
        if (!active) { dx = dy = 0; return; }
        float pitch, yaw, roll;
        if (c.space == 0) fusion.GetPlayerSpaceGyro(pitch, yaw);
        else {
            fusion.GetCalibratedGyro(pitch, yaw, roll);
            if (c.space == 2) yaw = -roll;
        }
        const float speed = std::sqrt(pitch * pitch + yaw * yaw);
        if (speed < kTighteningDegPerSec) {
            const float k = speed / kTighteningDegPerSec;
            pitch *= k;
            yaw *= k;
        }
        dx += -yaw * dt * c.sensitivityX * (c.invertX ? -1.f : 1.f);
        dy += -pitch * dt * c.sensitivityY * (c.invertY ? -1.f : 1.f);
    }
    void Consume(float& x, float& y) { x = dx; y = dy; dx = dy = 0; }
};
} // namespace GyroAim

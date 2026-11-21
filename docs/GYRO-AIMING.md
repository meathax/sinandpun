# Gyro aiming

Open the recomp Controls menu, then the pointer/live-preview page. Enable **Gyro** and apply the settings. Gyro is off by default. Enabling it selects Wii Remote + Nunchuk pointer aiming, including when the saved scheme is Automatic or Classic. Disabling it restores the saved scheme. The right stick remains available for large movements; gyro adds fine movement to the same reticle.

- **Player space** uses gravity to compensate for controller tilt. Local yaw and local roll are also available.
- Sensitivity X/Y is measured in pointer camera pixels per degree. Both axes can be inverted independently.
- Choose always on, hold Precision to aim, or hold Precision to pause. Precision defaults to R3 and can be remapped.
- Recenter defaults to L3 (or R/middle mouse). This centers the pointer without changing calibration.
- Use **Calibrate gyro**, then rest the controller on a stable surface for a few seconds. Stillness/sensor-fusion calibration also runs automatically. The calibration row reports missing sensors or missing sensor data.

Direct motion needs both a gyro and accelerometer exposed by SDL3 on player one's controller. Common examples are DualSense, DualShock 4 and Switch Pro; connection/driver support varies. An Xbox/XInput-only device has no direct motion data. If a mapper hides the physical controller behind XInput, expose the physical device instead. Steam Input gyro-to-mouse can use the existing relative mouse aiming path with direct gyro disabled; do not enable both for the same motion. DSU/network/phone motion is not implemented.

Gyro uses velocity-style stick aiming while enabled, overriding absolute-stick and spring-to-center behavior. A stationary absolute mouse does not continually overwrite gyro movement. Explicit mouse movement can still reposition the pointer. Calibration runs in menus, but menu/focus/capture blocking discards aim deltas; reconnecting starts fresh. Motion never changes synthetic Wii accelerometer gameplay gestures.

## Implementation references

The architecture follows [Zelda64Recomp's input implementation](https://github.com/Zelda64Recomp/Zelda64Recomp/blob/dev/src/game/input.cpp): SDL sensor events, conversion from radians/s to degrees/s and acceleration to g, GamepadMotionHelpers calibration and player-space conversion. The integration here uses SDL3 nanosecond sensor timestamps to integrate angular velocity once per event, then consumes the accumulated displacement once per Wii report. Duplicate/backward timestamps, gaps over 100 ms and nonfinite samples are rejected.

[GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers) is vendored under its MIT license at `third_party/GamepadMotionHelpers`. Retrieved 2026-11-21; GamepadMotion.hpp SHA-256: `f070bb1327cf5b1fbc336d570382d4c088e5ffae9f031dd0af8f81153581e690`.

Automated checks cover integration units, consumption, inversion, gaps, SDL sensor enable/disable, device filtering, focus/menu suppression and settings round-trip. Physical controller orientation, drift and gameplay feel require hands-on acceptance; automated tests do not establish those.

#pragma once

namespace aurora::gx {
// Input clip coordinates exclude viewport jitter. XY is the previous sample
// displacement in render pixels, Z carries the W ratio for a later native
// present, and alpha supplies coverage when GX alpha blending is active.
inline constexpr char RasterVelocityWGSL[] = R"(
fn raster_velocity(before: vec4f, now: vec4f, size: vec2f, coverage: f32) -> vec4f {
    if (before.w <= 0.00001 || now.w <= 0.00001) { return vec4f(0); }
    let delta = (before.xy / before.w - now.xy / now.w) * size * vec2f(0.5, -0.5);
    let ratio = before.w / now.w;
    if (all(abs(delta) < vec2f(65504)) && ratio < 65504.0) {
        return vec4f(delta, ratio, coverage);
    }
    return vec4f(0);
}
)";
} // namespace aurora::gx

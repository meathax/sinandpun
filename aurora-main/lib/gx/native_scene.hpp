#pragma once
#include "gx.hpp"
#include <memory>
#include <vector>

namespace aurora::gx {
// Only the renderer consumes these snapshots. Guest memory and clocks are never
// changed to evaluate an intermediate pose.
void begin_velocity_frame() noexcept;
bool velocity_capture_enabled() noexcept;
void append_velocity_chunk(const ShaderConfig&, GXPrimitive, const uint8_t*, uint16_t, uint32_t& base);
void end_velocity_batch();
uint32_t record_previous_clip(const ShaderConfig&, GXPrimitive, const uint8_t*, uint16_t,
                              const Mat4x4<float>&, HashType material);
bool native_scene_enabled() noexcept;
void begin_native_scene() noexcept;
void discard_native_scene() noexcept;
struct NativeSceneDraw {
  uint32_t base = 0;
  bool velocity = false;
};
void append_native_scene(const ShaderConfig&, const uint8_t*, uint16_t) noexcept;
std::vector<std::shared_ptr<NativeSceneDraw>> retain_native_scene();
NativeSceneDraw* record_native_scene(const ShaderConfig&, GXPrimitive, const uint8_t*, uint16_t, const Mat4x4<float>&,
                                     HashType material, bool translucent) noexcept;
void record_ray_trace_geometry(const ShaderConfig&, GXPrimitive, const uint8_t*, uint16_t);
bool seal_native_scene() noexcept;
// Call at the start of every draw chunk: record_ray_trace_geometry's decoded positions are reused by the
// native-scene capture of the same chunk only.
void reset_decoded_chunk() noexcept;
extern thread_local float g_nativeSceneWeight;
extern thread_local uint64_t g_velocityFrame, g_presentedVelocityFrame;
extern thread_local float g_velocityWeight, g_presentedVelocityWeight;
} // namespace aurora::gx

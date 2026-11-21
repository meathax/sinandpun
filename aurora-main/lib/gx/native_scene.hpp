#pragma once
#include "gx.hpp"
#include <memory>
#include <vector>

namespace aurora::gx {
// Only the renderer consumes these snapshots. Guest memory and clocks are never
// changed to evaluate an intermediate pose.
bool native_scene_enabled() noexcept;
void begin_native_scene() noexcept;
void discard_native_scene() noexcept;
struct NativeSceneDraw {
  uint32_t base = 0;
};
void append_native_scene(const ShaderConfig&, const uint8_t*, uint16_t) noexcept;
std::vector<std::shared_ptr<NativeSceneDraw>> retain_native_scene();
NativeSceneDraw* record_native_scene(const ShaderConfig&, GXPrimitive, const uint8_t*, uint16_t, const Mat4x4<float>&,
                                     HashType material) noexcept;
bool seal_native_scene() noexcept;
extern thread_local float g_nativeSceneWeight;
} // namespace aurora::gx

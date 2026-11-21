#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace aurora::gx {
// Positions are validated to |x| <= 1e9 before this conversion. Avoid the
// Windows CRT llround call (including floating-environment handling) per axis.
inline int64_t scene_coordinate(float value) {
  const double scaled = double(value) * 1024.0;
  return static_cast<int64_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}
struct ScenePose {
  std::array<float, 4> clip{};
  std::array<float, 3> pos{}, normal{};
};
// WGSL scene_mix reads these same offsets from the retained storage buffer.
struct ScenePosePair {
  std::array<float, 4> clipBefore, clipNow;
  std::array<float, 3> posBefore, posNow, normalBefore, normalNow;
};
static_assert(sizeof(ScenePosePair) == 80);
static_assert(offsetof(ScenePosePair, posBefore) == 32);
static_assert(offsetof(ScenePosePair, normalBefore) == 56);
inline ScenePosePair scene_pair(const ScenePose& before, const ScenePose& now) {
  return {before.clip, now.clip, before.pos, now.pos, before.normal, now.normal};
}
inline ScenePosePair joined_scene_pair(const ScenePose& before, const ScenePose& now, const ScenePose& canonicalBefore,
                                       const ScenePose& canonicalNow) {
  return {canonicalBefore.clip, canonicalNow.clip, canonicalBefore.pos, canonicalNow.pos, before.normal, now.normal};
}
inline float scene_weight(uint64_t now, uint64_t anchor, uint64_t interval) {
  if (!interval || now >= anchor && now - anchor >= interval)
    return 1.f;
  if (now <= anchor)
    return 0.f;
  return static_cast<float>(double(now - anchor) / double(interval));
}
} // namespace aurora::gx

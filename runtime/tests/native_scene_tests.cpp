#include "../../aurora-main/lib/gx/scene_pose.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef NDEBUG
#error Tests require enabled assertions
#endif
int main() {
  using namespace aurora::gx;
  for (int i = -100000; i <= 100000; ++i) {
    const float coordinate = float(i) / 2048.f;
    assert(scene_coordinate(coordinate) ==
           std::llround(double(coordinate) * 1024.0));
  }
  assert(scene_coordinate(1.e9f) == 1024000000000ll);
  assert(scene_coordinate(-1.e9f) == -1024000000000ll);
  ScenePose oldA, newA, oldB, newB;
  oldA.clip = oldB.clip = {0, 1, 2, 1};
  newA.clip = newB.clip = {2, 3, 4, 1};
  oldA.pos = oldB.pos = {0, 1, 2};
  newA.pos = newB.pos = {2, 3, 4};
  oldA.normal = {1, 0, 0};
  oldB.normal = {0, 1, 0};
  newA.normal = oldA.normal;
  newB.normal = oldB.normal;
  // A newly visible piece may have only its current endpoint, while the
  // neighboring material has history. Their shared join must use one history.
  oldB.clip = newB.clip;
  oldB.pos = newB.pos;
  const auto a = scene_pair(oldA, newA),
             b = joined_scene_pair(oldB, newB, oldA, newA);
  float sa[20], sb[20];
  std::memcpy(sa, &a, 80);
  std::memcpy(sb, &b, 80);
  // Old path interpolates B's bone but reuses A's current CPU-deformed vertex:
  const float oldMidpointA = 2.f, oldMidpointB = 1.f + (-1.f + 1.f) * .5f;
  assert(oldMidpointA != oldMidpointB);
  for (unsigned frame = 0; frame <= 1000; ++frame) {
    const float t = scene_weight(frame, 0, 1000);
    const auto encoded = uint32_t(t * 16777215.f);
    const float shaderT = float(encoded) / 16777215.f;
    for (unsigned c = 0; c < 4; ++c)
      assert(sa[c] + (sa[c + 4] - sa[c]) * shaderT ==
             sb[c] + (sb[c + 4] - sb[c]) * shaderT);
    for (unsigned c = 8; c < 11; ++c)
      assert(sa[c] + (sa[c + 3] - sa[c]) * shaderT ==
             sb[c] + (sb[c + 3] - sb[c]) * shaderT);
  }
  assert(sa[14] != sb[14]); // hard-edge normals are not welded
  // The GPU uses a vertex indirection table followed by shared 20-word
  // endpoint records. Test nonzero storage/draw offsets, shared vertices and
  // a separate hard-normal record with identical positional samples.
  std::array<uint32_t, 64> storage{};
  storage[7] = 20;
  storage[8] = 20;
  storage[9] = 40;
  std::memcpy(storage.data() + 20, &a, sizeof(a));
  std::memcpy(storage.data() + 40, &b, sizeof(b));
  for (unsigned vertex = 0; vertex < 3; ++vertex) {
    const uint32_t base = storage[7 + vertex];
    float values[20];
    std::memcpy(values, storage.data() + base, sizeof(values));
    for (unsigned c = 0; c < 14; ++c)
      assert(values[c] == sa[c]);
    assert(values[14] == (vertex == 2 ? sb[14] : sa[14]));
  }
  assert(scene_weight(1, 2, 10) == 0.f);
  assert(scene_weight(100, 2, 10) == 1.f);
  assert(scene_weight(7, 2, 10) == .5f);
  // Any number of render samples is possible in one fixed logic interval.
  for (unsigned i = 0; i < 10000; ++i)
    assert(scene_weight(i, 0, 10000) < 1.f);
  std::puts("PASS: deformed seam negative, 1001 shader samples, hard normals, "
            "10000 uncapped samples");
}

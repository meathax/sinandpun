#pragma once

#include "rtgi.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace aurora::webgpu::rtgi {

// The camera rows are sampled on the thread submitting GX draws. A rigid
// camera permits an inexpensive inverse and preserves world-space identity.
struct Camera {
  std::array<float, 12> view{};
  uint32_t scene = 0;

  bool valid() const {
    for (unsigned i = 0; i < 3; ++i) {
      for (unsigned j = i; j < 3; ++j) {
        float dot = 0;
        for (unsigned k = 0; k < 3; ++k) dot += view[i*4+k] * view[j*4+k];
        if (!(std::abs(dot - float(i == j)) < 0.012f)) return false;
      }
    }
    return true;
  }

  Point to_world(const Point& p) const {
    Point result{};
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned j = 0; j < 3; ++j) result[i] += view[j*4+i] * (p[j] - view[j*4+3]);
    return result;
  }

  Point to_view(const Point& p) const {
    Point result{};
    for (unsigned i = 0; i < 3; ++i)
      result[i] = view[i*4]*p[0] + view[i*4+1]*p[1] + view[i*4+2]*p[2] + view[i*4+3];
    return result;
  }
};

class GeometryHistory {
public:
  void clear() { entries.clear(); lastCamera = {}; frame = 0; }

  size_t extend(std::vector<Triangle>& live, const Camera& camera, const std::array<float, 4>& projection,
                const std::array<float, 2>& screenCenter) {
    if (!camera.valid()) {
      clear();
      return 0;
    }
    const Point position = camera.to_world({});
    const Point old = lastCamera.to_world({});
    const float facing = camera.view[8]*lastCamera.view[8] + camera.view[9]*lastCamera.view[9] +
                         camera.view[10]*lastCamera.view[10];
    if (frame && (lastCamera.scene != camera.scene || facing < 0.82f ||
                  std::hypot(position[0]-old[0], position[1]-old[1], position[2]-old[2]) > 130.f)) clear();
    lastCamera = camera;
    ++frame;
    const auto liveCount = live.size();
    // Observe a stable subset of large triangles. Thin and small triangles
    // provide weak diffuse bounce and are poor candidates for retention.
    const size_t stride = std::max<size_t>(2, (liveCount+2399)/2400);
    for (size_t i = 0; i < liveCount; i += stride) {
      const auto& t = live[i];
      float largest = 0;
      for (unsigned axis = 0; axis < 3; ++axis)
        largest = std::max(largest, std::max({t.a[axis],t.b[axis],t.c[axis]}) -
                                    std::min({t.a[axis],t.b[axis],t.c[axis]}));
      if (largest < 12.f) continue;
      const Triangle world{camera.to_world(t.a),t.red,camera.to_world(t.b),t.green,camera.to_world(t.c),t.blue};
      const uint64_t key = world_key(world);
      auto it = entries.find(key);
      if (it == entries.end()) {
        if (entries.size() < HistoryLimit) entries.emplace(key, Entry{world,frame,1});
        continue;
      }
      auto& entry = it->second;
      if (entry.lastSeen == frame) continue;
      entry.stability = frame - entry.lastSeen <= 4 ? std::min(30u, entry.stability + 1) : 1;
      entry.lastSeen = frame;
      entry.world = world;
    }

    for (auto it = entries.begin(); it != entries.end();) {
      const auto& entry = it->second;
      const unsigned age = frame - entry.lastSeen;
      if (age > (entry.stability >= 5 ? 120u : 8u)) {
        it = entries.erase(it);
        continue;
      }
      ++it;
      if (age < 2 || entry.stability < 5 || live.size() >= TriangleLimit || live.size()-liveCount >= RetainLimit) continue;
      const auto& w = entry.world;
      const Triangle local{camera.to_view(w.a),w.red,camera.to_view(w.b),w.green,camera.to_view(w.c),w.blue};
      if (outside_view(local,projection,screenCenter))
        live.push_back(local);
    }
    return live.size() - liveCount;
  }

private:
  static constexpr size_t HistoryLimit = 9000;
  static constexpr size_t RetainLimit = 1600;
  struct Entry { Triangle world; unsigned lastSeen; unsigned stability; };
  std::unordered_map<uint64_t, Entry> entries;
  Camera lastCamera{};
  unsigned frame = 0;

  static uint64_t world_key(const Triangle& world) {
    uint64_t hash = 14695981039346656037ull;
    const auto append = [&](int32_t value) { hash = (hash ^ uint32_t(value)) * 1099511628211ull; };
    for (const Point& p : {world.a, world.b, world.c})
      for (float x : p) append(int32_t(std::round(x * 0.5f)));
    for (float c : {world.red,world.green,world.blue}) append(int32_t(c * 63.f));
    return hash;
  }

  static bool outside_view(const Triangle& t, const std::array<float, 4>& projection,
                           const std::array<float, 2>& center) {
    unsigned common = 0x1f;
    float distance = std::numeric_limits<float>::max();
    for (const Point& p : {t.a, t.b, t.c}) {
      const float z = -p[2];
      const float x = p[0]*projection[0] + z*center[0];
      const float y = -p[1]*projection[1] + z*center[1];
      const float edge = 1.04f * std::max(z, 0.f);
      common &= unsigned(x < -edge) | (unsigned(x > edge)<<1) | (unsigned(y < -edge)<<2) |
                (unsigned(y > edge)<<3) | (unsigned(z <= 0.f)<<4);
      distance = std::min(distance, p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
    }
    return common && distance < 3500.f*3500.f;
  }
};
} // namespace aurora::webgpu::rtgi

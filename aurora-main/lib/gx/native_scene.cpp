#include "native_scene.hpp"
#include "scene_pose.hpp"
#include "frame_interpolation.hpp"
#include "../gfx/recording.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <absl/container/flat_hash_map.h>
#include <chrono>
#include <vector>

namespace aurora::gx {
thread_local float g_nativeSceneWeight = -1.f;
namespace {
using Pose = ScenePose;
struct Draw {
  HashType key{}, camera{};
  Hasher hasher;
  uint64_t topology = 14695981039346656037ull;
  Mat4x4<float> projection;
  std::shared_ptr<NativeSceneDraw> output;
  std::vector<Pose> pose;
  std::vector<uint32_t> vertexPoses;
  std::vector<uint32_t> joins;
  size_t previousIndex = SIZE_MAX;
  std::array<float, 3> center{};
  bool matched = false, used = false, translucent = false;
};
std::vector<Draw> previous, current, recycled;
std::unordered_map<HashType, std::vector<size_t>> history;
// A merged draw has one immutable matrix palette and attribute-array state.
// Cache indexed vertices only within that draw, across its primitive chunks.
absl::flat_hash_map<uint64_t, uint32_t> vertexCache;
bool safe = true;
uint32_t rejected = 0;
double captureMs = 0;
const bool profileCapture = [] {
  const char* value = std::getenv("SIN_SCENE_CAPTURE_PROF");
  return value && *value == '1';
}();
bool finite(float f) { return (std::bit_cast<uint32_t>(f) & 0x7f800000u) != 0x7f800000u; }

uint32_t scalar_bits(const uint8_t* p, unsigned bytes, bool le) {
  if (bytes == 4) {
    uint32_t value;
    std::memcpy(&value, p, 4);
    return le ? value : __builtin_bswap32(value);
  }
  if (bytes == 2) {
    uint16_t value;
    std::memcpy(&value, p, 2);
    return le ? value : __builtin_bswap16(value);
  }
  return *p;
}
bool attribute(const ShaderConfig& config, const uint8_t* vertex, GXAttr attr, float* dst, unsigned count) {
  const auto& a = config.attrs[attr];
  if (a.attrType == GX_NONE)
    return true;
  const uint8_t* p = vertex + a.offset;
  bool le = false;
  const unsigned bytes = a.compType == GX_F32 ? 4 : (a.compType == GX_S16 || a.compType == GX_U16 ? 2 : 1);
  if (a.attrType == GX_INDEX8 || a.attrType == GX_INDEX16) {
    const uint32_t index = scalar_bits(p, a.attrType == GX_INDEX8 ? 1 : 2, false);
    const auto& array = g_gxState.arrays[attr];
    const uint64_t offset = uint64_t(index) * a.stride;
    if (!array.data || offset + std::min<unsigned>(count, a.cnt) * bytes > array.size)
      return false;
    p = static_cast<const uint8_t*>(array.data) + offset;
    le = a.le;
  }
  for (unsigned c = 0; c < std::min<unsigned>(count, a.cnt); ++c) {
    const uint32_t bits = scalar_bits(p + bytes * c, bytes, le);
    float value;
    switch (a.compType) {
    case GX_F32:
      value = std::bit_cast<float>(bits);
      break;
    case GX_S8:
      value = static_cast<int8_t>(bits);
      break;
    case GX_S16:
      value = static_cast<int16_t>(bits);
      break;
    default:
      value = static_cast<float>(bits);
      break;
    }
    if (a.compType != GX_F32) {
      value = a.frac <= 126 ? value * std::bit_cast<float>((127u - a.frac) << 23) : std::ldexp(value, -int(a.frac));
    }
    if (!finite(value))
      return false;
    dst[c] = value;
  }
  return true;
}
std::array<float, 3> transform(const Mat3x4<float>& m, const float* p, float w) {
  std::array<float, 3> result{};
  const std::array<Vec4<float>, 3> rows{m.m0, m.m1, m.m2};
  for (unsigned r = 0; r < 3; ++r)
    result[r] = rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2] + rows[r][3] * w;
  return result;
}
struct Join {
  HashType camera;
  std::array<int64_t, 3> xyz;
  bool operator==(const Join&) const = default;
};
struct JoinHash {
  size_t operator()(const Join& j) const { return xxh3_hash_s(j.xyz.data(), sizeof(j.xyz), j.camera); }
};
} // namespace
bool native_scene_enabled() noexcept {
  AuroraFrameRateMode mode;
  uint32_t fixed, cap;
  get_frame_rate(mode, fixed, cap);
  return mode == AURORA_FRAME_RATE_UNCAPPED;
}
void begin_native_scene() noexcept {
  current.clear();
  history.clear();
  safe = true;
  rejected = 0;
  captureMs = 0;
  for (size_t i = 0; i < previous.size(); ++i) {
    previous[i].used = false;
    history[previous[i].key].push_back(i);
  }
}
void discard_native_scene() noexcept {
  safe = false;
  current.clear();
}

bool capture_scene_vertices(Draw& draw, const ShaderConfig& config, const uint8_t* vertices, uint16_t count,
                            const Mat4x4<float>& projection) {
  const auto started = profileCapture ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  auto& key = draw.hasher;
  const auto& posAttr = config.attrs[GX_VA_POS];
  const auto& nrmAttr = config.attrs[GX_VA_NRM];
  const auto indexed = [](const auto& a) { return a.attrType == GX_INDEX8 || a.attrType == GX_INDEX16; };
  const bool cacheable = indexed(posAttr) && (indexed(nrmAttr) || nrmAttr.attrType == GX_NONE);

  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* vertex = vertices + size_t(i) * config.vtxStride;
    for (unsigned t = GX_VA_TEX0; t <= GX_VA_TEX7; ++t) {
      const auto& a = config.attrs[t];
      if (a.attrType == GX_NONE)
        continue;
      const unsigned bytes = a.attrType == GX_INDEX8    ? 1
                             : a.attrType == GX_INDEX16 ? 2
                                                        : a.cnt * (a.compType == GX_F32                           ? 4
                                                                   : a.compType == GX_S16 || a.compType == GX_U16 ? 2
                                                                                                                  : 1);
      uint64_t topology = 0;
      std::memcpy(&topology, vertex + a.offset, bytes);
      draw.topology = (draw.topology ^ topology) * 1099511628211ull;
    }
    uint32_t matrix = g_gxState.currentPnMtx;
    if (config.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE)
      matrix = (vertex[config.attrs[GX_VA_PNMTXIDX].offset] & 63u) / 3u;
    if (matrix >= MaxPnMtx) {
      ++rejected;
      return false;
    }
    uint64_t vertexKey = 0;
    if (cacheable) {
      const uint32_t pi = scalar_bits(vertex + posAttr.offset, posAttr.attrType == GX_INDEX8 ? 1 : 2, false);
      const uint32_t ni = nrmAttr.attrType == GX_NONE
                              ? 0
                              : scalar_bits(vertex + nrmAttr.offset, nrmAttr.attrType == GX_INDEX8 ? 1 : 2, false);
      vertexKey = (uint64_t(matrix) << 32) | (uint64_t(pi) << 16) | ni;
      if (const auto found = vertexCache.find(vertexKey); found != vertexCache.end()) {
        for (unsigned r = 0; r < 3; ++r)
          draw.center[r] += draw.pose[found->second].pos[r];
        draw.vertexPoses.push_back(found->second);
        continue;
      }
    }
    float p[3]{}, n[3]{1.f, 0.f, 0.f};
    if (!attribute(config, vertex, GX_VA_POS, p, 3) || !attribute(config, vertex, GX_VA_NRM, n, 3)) {
      ++rejected;
      return false;
    }
    // Emitters can replace particles without changing their count, UVs or
    // material. Require the same local geometry before borrowing a pose.
    if (draw.translucent)
      key.update(p, sizeof(p));
    Pose pose;
    pose.pos = transform(g_gxState.pnMtx[matrix].pos, p, 1.f);
    pose.normal = transform(g_gxState.pnMtx[matrix].nrm, n, 0.f);
    for (unsigned r = 0; r < 3; ++r)
      if (!finite(pose.pos[r]) || std::abs(pose.pos[r]) > 1.e9f || !finite(pose.normal[r])) {
        ++rejected;
        return false;
      }
    const std::array<Vec4<float>, 4> rows{projection.m0, projection.m1, projection.m2, projection.m3};
    for (unsigned r = 0; r < 4; ++r) {
      pose.clip[r] = rows[r][0] * pose.pos[0] + rows[r][1] * pose.pos[1] + rows[r][2] * pose.pos[2] + rows[r][3];
      if (!finite(pose.clip[r])) {
        ++rejected;
        return false;
      }
    }
    for (unsigned r = 0; r < 3; ++r)
      draw.center[r] += pose.pos[r];
    const uint32_t poseIndex = static_cast<uint32_t>(draw.pose.size());
    if (cacheable)
      vertexCache.emplace(vertexKey, poseIndex);
    draw.vertexPoses.push_back(poseIndex);
    draw.pose.push_back(pose);
  }
  if (profileCapture)
    captureMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return true;
}
NativeSceneDraw* record_native_scene(const ShaderConfig& config, GXPrimitive prim, const uint8_t* vertices,
                                     uint16_t count, const Mat4x4<float>& projection, HashType material,
                                     bool translucent) noexcept {
  if (!safe || !count)
    return {};
  if (!vertices || config.vtxStride == 0) {
    ++rejected;
    discard_native_scene();
    return {};
  }
  Draw draw;
  draw.translucent = translucent;
  vertexCache.erase(vertexCache.begin(), vertexCache.end());
  if (!recycled.empty()) {
    draw.pose = std::move(recycled.back().pose);
    draw.vertexPoses = std::move(recycled.back().vertexPoses);
    draw.joins = std::move(recycled.back().joins);
    recycled.pop_back();
    draw.pose.clear();
    draw.vertexPoses.clear();
    draw.joins.clear();
  }
  draw.projection = projection;
  draw.output = std::make_shared<NativeSceneDraw>();
  auto& key = draw.hasher;
  key.update(config);
  key.update(prim);
  key.update(material);
  const auto& viewport = g_gxState.logicalViewport;
  const std::array camera{viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar};
  draw.camera = xxh3_hash_s(camera.data(), sizeof(camera), xxh3_hash_s(&projection, sizeof(projection)));
  key.update(draw.camera);
  if (!capture_scene_vertices(draw, config, vertices, count, projection)) {
    discard_native_scene();
    return {};
  }
  auto* result = draw.output.get();
  current.push_back(std::move(draw));
  return result;
}
void append_native_scene(const ShaderConfig& config, const uint8_t* vertices, uint16_t count) noexcept {
  if (current.empty())
    return;
  if (!capture_scene_vertices(current.back(), config, vertices, count, current.back().projection))
    discard_native_scene();
}

std::vector<std::shared_ptr<NativeSceneDraw>> retain_native_scene() {
  std::vector<std::shared_ptr<NativeSceneDraw>> owners;
  for (const auto& draw : previous)
    owners.push_back(draw.output);
  return owners;
}

bool seal_native_scene() noexcept {
  const auto started = std::chrono::steady_clock::now();
  if (current.empty()) {
    previous.clear();
    return false;
  }
  if (!native_scene_enabled() || !safe || !frame_interpolation_replay_safe()) {
    previous.clear();
    current.clear();
    return false;
  }
  for (auto& draw : current) {
    for (auto& value : draw.center)
      value /= draw.vertexPoses.size();
    draw.hasher.update(draw.vertexPoses.size());
    draw.hasher.update(draw.topology);
    draw.hasher.update(draw.vertexPoses.data(), draw.vertexPoses.size() * sizeof(uint32_t));
    draw.key = draw.hasher.digest();
    size_t best = SIZE_MAX;
    float distance = 1500.f * 1500.f;
    if (const auto it = history.find(draw.key); it != history.end()) {
      for (size_t index : it->second) {
        const auto& old = previous[index];
        if (old.used || old.pose.size() != draw.pose.size())
          continue;
        float d = 0;
        for (unsigned r = 0; r < 3; ++r) {
          const float delta = old.center[r] - draw.center[r];
          d += delta * delta;
        }
        if (d < distance) {
          best = index;
          distance = d;
        }
      }
    }
    draw.previousIndex = best;
    if (best != SIZE_MAX) {
      previous[best].used = true;
      draw.matched = true;
    }
  }
  struct Owner {
    size_t draw, vertex;
  };
  static absl::flat_hash_map<Join, uint32_t, JoinHash> joins;
  static std::vector<Owner> owners;
  // Abseil clear() frees backing arrays larger than 127 buckets. Range erase
  // keeps this multi-megabyte table resident instead of faulting it each frame.
  joins.erase(joins.begin(), joins.end());
  owners.clear();
  size_t totalPoses = 0, totalVertices = 0;
  for (const auto& d : current) {
    totalPoses += d.pose.size();
    totalVertices += d.vertexPoses.size();
  }
  joins.reserve(totalPoses);
  owners.reserve(totalPoses);
  auto joinKey = [](const Draw& draw, const Pose& pose) {
    Join j{draw.camera, {}};
    for (unsigned r = 0; r < 3; ++r)
      j.xyz[r] = scene_coordinate(pose.pos[r]);
    return j;
  };
  // Prefer an established history over a newly visible material piece. Normal
  // discontinuities are intentional at hard edges, so only position is shared.
  for (size_t d = 0; d < current.size(); ++d) {
    current[d].joins.resize(current[d].pose.size());
    for (size_t v = 0; v < current[d].pose.size(); ++v) {
      // Coincident translucent vertices can belong to independent particles
      // and layers. Never inherit another layer's previous position.
      if (current[d].translucent) {
        current[d].joins[v] = static_cast<uint32_t>(owners.size());
        owners.push_back({d, v});
        continue;
      }
      const auto key = joinKey(current[d], current[d].pose[v]);
      auto [it, inserted] = joins.emplace(key, static_cast<uint32_t>(owners.size()));
      if (inserted)
        owners.push_back({d, v});
      else if (current[d].matched && !current[owners[it->second].draw].matched)
        owners[it->second] = {d, v};
      current[d].joins[v] = it->second;
    }
  }
  size_t matched = 0, welded = 0, translucent = 0, translucentMatched = 0;
  const size_t vertices = totalVertices;
  const auto beforePose = [](const Draw& draw, size_t v) -> const Pose& {
    return draw.matched ? previous[draw.previousIndex].pose[v] : draw.pose[v];
  };
  static std::vector<ScenePosePair> packed;
  packed.clear();
  packed.reserve(totalPoses);
  for (const auto& owner : owners) {
    const auto& draw = current[owner.draw];
    packed.push_back(scene_pair(beforePose(draw, owner.vertex), draw.pose[owner.vertex]));
  }
  for (auto& draw : current) {
    matched += draw.matched;
    translucent += draw.translucent;
    translucentMatched += draw.translucent && draw.matched;
    for (size_t v = 0; v < draw.pose.size(); ++v) {
      const auto owner = owners[draw.joins[v]];
      const auto& canonical = current[owner.draw];
      Pose before = beforePose(draw, v), now = draw.pose[v];
      const auto& canonicalBefore = beforePose(canonical, owner.vertex);
      const auto& canonicalNow = canonical.pose[owner.vertex];
      // Share the whole endpoint record only when its normals also agree.
      // Hard-edge normals retain independent records with shared positions.
      if (before.normal != canonicalBefore.normal || now.normal != canonicalNow.normal) {
        draw.joins[v] = static_cast<uint32_t>(packed.size());
        packed.push_back(joined_scene_pair(before, now, canonicalBefore, canonicalNow));
      }
      welded += &draw != &canonical || v != owner.vertex;
    }
  }
  static std::vector<uint32_t> upload;
  const size_t words = vertices + packed.size() * 20;
  upload.resize(words);
  const auto storage = gfx::reserve_scene_storage(words * sizeof(uint32_t));
  if (!storage.size) {
    previous.clear();
    current.clear();
    return false;
  }
  const uint32_t recordBase = storage.offset / 4 + static_cast<uint32_t>(vertices);
  size_t cursor = 0;
  for (auto& draw : current) {
    draw.output->base = storage.offset / 4 + static_cast<uint32_t>(cursor) + 1;
    for (uint32_t pose : draw.vertexPoses)
      upload[cursor++] = recordBase + draw.joins[pose] * 20;
  }
  std::memcpy(upload.data() + vertices, packed.data(), packed.size() * sizeof(ScenePosePair));
  std::memcpy(gfx::scene_storage_data(storage), upload.data(), words * sizeof(uint32_t));
  static uint64_t frames = 0;
  if (++frames % 60 == 0)
    std::fprintf(stderr,
                 "[native-scene] draws=%zu matched=%zu vertices=%zu shared=%zu records=%zu rejected=%u capture=%.3fms "
                 "seal=%.3fms translucent=%zu translucent_matched=%zu\n",
                 current.size(), matched, vertices, welded, packed.size(), rejected, captureMs,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
                 translucent, translucentMatched);
  recycled.clear();
  for (auto it = previous.rbegin(); it != previous.rend(); ++it)
    recycled.push_back(std::move(*it));
  previous = std::move(current);
  current.clear();
  return true;
}
} // namespace aurora::gx

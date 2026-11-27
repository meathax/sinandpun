#include "native_scene.hpp"
#include "scene_pose.hpp"
#include "../webgpu/upscale.hpp"
#include "../webgpu/dlss.hpp"
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
thread_local uint64_t g_velocityFrame = 0, g_presentedVelocityFrame = UINT64_MAX;
thread_local float g_velocityWeight = 1.f, g_presentedVelocityWeight = 1.f;
namespace {
using Pose = ScenePose;
struct Draw {
  HashType key{}, camera{};
  Hasher hasher;
  uint64_t topology = 14695981039346656037ull;
  Mat4x4<float> projection;
  std::shared_ptr<NativeSceneDraw> output;
  std::vector<Pose> pose;
  std::vector<std::array<float,4>> priorClip;
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
struct VelocityDraw {
  std::vector<std::array<float,4>> clip;
  std::vector<std::array<float,4>> prior;
  std::array<float,3> center;
  std::vector<HashType> topology;
  bool used = false;
};
using VelocityHistory = std::unordered_map<HashType, std::vector<VelocityDraw>>;
std::atomic<bool> captureVelocity{false};
VelocityHistory previousVelocity, currentVelocity;
VelocityDraw* recordingVelocity = nullptr;
VelocityDraw* matchedVelocity = nullptr;
Mat4x4<float> velocityProjection;

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
void record_ray_trace_geometry(const ShaderConfig& config, GXPrimitive primitive, const uint8_t* vertices, uint16_t count) {
  if (primitive != GX_TRIANGLES && primitive != GX_TRIANGLESTRIP && primitive != GX_TRIANGLEFAN &&
      primitive != GX_QUADS) return;
  auto* triangles = gfx::ray_trace_geometry();
  if (!triangles || triangles->size() >= webgpu::rtgi::LiveTriangleLimit) return;
  const auto& paint = g_gxState.colorChannelState[0].matColor;
  const float r = std::clamp(paint[0], 0.05f, 1.f);
  const float g = std::clamp(paint[1], 0.05f, 1.f);
  const float b = std::clamp(paint[2], 0.05f, 1.f);
  // Off-screen hits use the authored GX material tint. A visible hit can
  // recover its actual textured radiance from the matched EFB.
  thread_local std::vector<webgpu::rtgi::Point> positions;
  positions.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const auto* vertex = vertices + size_t(i) * config.vtxStride;
    const auto& matrixAttribute = config.attrs[GX_VA_PNMTXIDX];
    const unsigned matrix = matrixAttribute.attrType == GX_NONE ? g_gxState.currentPnMtx
                                                               : (vertex[matrixAttribute.offset] & 63u) / 3u;
    float p[3]{};
    if (matrix >= MaxPnMtx || !attribute(config, vertex, GX_VA_POS, p, 3)) return;
    positions[i] = transform(g_gxState.pnMtx[matrix].pos, p, 1.f);
    for (float value : positions[i])
      if (!finite(value) || std::abs(value) > 1.e7f) return;
  }
  const auto emit = [&](unsigned a, unsigned bIndex, unsigned c) {
    if (triangles->size() < webgpu::rtgi::LiveTriangleLimit && positions[a] != positions[bIndex] &&
        positions[a] != positions[c] && positions[bIndex] != positions[c])
      triangles->push_back({positions[a], r, positions[bIndex], g, positions[c], b});
  };
  switch (primitive) {
  case GX_TRIANGLES:
    for (unsigned i = 0; i + 2 < count; i += 3) emit(i, i+1, i+2);
    break;
  case GX_QUADS:
    for (unsigned i = 0; i + 3 < count; i += 4) {
      emit(i, i+1, i+2);
      emit(i, i+2, i+3);
    }
    break;
  case GX_TRIANGLESTRIP:
    for (unsigned i = 2; i < count; ++i) emit(i-2, i-1, i);
    break;
  case GX_TRIANGLEFAN:
    for (unsigned i = 2; i < count; ++i) emit(0, i-1, i);
    break;
  default: break;
  }
}

bool native_scene_enabled() noexcept {
  AuroraFrameRateMode mode;
  uint32_t fixed, cap;
  get_frame_rate(mode, fixed, cap);
  return mode == AURORA_FRAME_RATE_UNCAPPED;
}
void begin_native_scene() noexcept {
  recordingVelocity = matchedVelocity = nullptr;
  previousVelocity.swap(currentVelocity);
  currentVelocity.clear();
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
void begin_velocity_frame() noexcept {
  captureVelocity.store(webgpu::upscale::settings().mode == AURORA_UPSCALER_DLSS && webgpu::dlss::ready(), std::memory_order_relaxed);
}

bool velocity_capture_enabled() noexcept {
  return captureVelocity.load(std::memory_order_relaxed);
}

// First and subsequent primitive chunks use the same GX batch. The CPU keeps
// their topology separately so a changed chunk never borrows unrelated vertices.
static HashType velocity_topology(const ShaderConfig& config, GXPrimitive prim,
                                  const uint8_t* vertices, uint16_t count) {
  Hasher hash;
  hash.update(prim);
  hash.update(count);
  for (auto attr : {GX_VA_POS, GX_VA_TEX0}) {
    const auto& a = config.attrs[attr];
    unsigned bytes = a.attrType == GX_INDEX8 ? 1 : a.attrType == GX_INDEX16 ? 2 : 0;
    if (attr == GX_VA_TEX0 && a.attrType == GX_DIRECT)
      bytes = a.cnt * (a.compType == GX_F32 ? 4 : a.compType == GX_S16 || a.compType == GX_U16 ? 2 : 1);
    if (bytes) for (unsigned i = 0; i < count; ++i)
      hash.update(vertices + size_t(i) * config.vtxStride + a.offset, bytes);
  }
  return hash.digest();
}

struct ClipHistory { std::array<float,4> older, before, current; };

static uint32_t velocity_chunk(const ShaderConfig& config, const uint8_t* vertices, uint16_t count,
                               HashType topology, uint32_t base, bool first) {
  if (!recordingVelocity) return 0;
  auto& draw = *recordingVelocity;
  const auto offset = draw.clip.size(), chunk = draw.topology.size();
  topology = xxh3_hash(offset, topology);
  draw.topology.push_back(topology);
  const bool match = matchedVelocity && chunk < matchedVelocity->topology.size() &&
                     matchedVelocity->topology[chunk] == topology &&
                     offset + count <= matchedVelocity->clip.size();
  draw.clip.resize(offset + count);
  draw.prior.resize(offset + count);
  const auto& idx = config.attrs[GX_VA_PNMTXIDX];
  for (unsigned i = 0; i < count; ++i) {
    const auto* v = vertices + size_t(i)*config.vtxStride;
    unsigned matrix = idx.attrType == GX_NONE ? g_gxState.currentPnMtx : (v[idx.offset] & 63u) / 3u;
    float local[3]{};
    if (matrix >= MaxPnMtx || !attribute(config, v, GX_VA_POS, local, 3)) {
      draw.topology.back() = 0;
      recordingVelocity = nullptr;
      return 0;
    }
    const auto eye = transform(g_gxState.pnMtx[matrix].pos, local, 1.f);
    auto& clip = draw.clip[offset+i];
    const auto project = [&](const auto& r) { return r[0]*eye[0]+r[1]*eye[1]+r[2]*eye[2]+r[3]; };
    clip = {project(velocityProjection.m0), project(velocityProjection.m1),
            project(velocityProjection.m2), project(velocityProjection.m3)};
    draw.prior[offset+i] = match ? matchedVelocity->clip[offset+i] : clip;
  }
  if (!matchedVelocity || (!first && !base)) return 0;
  const auto end = first ? UINT32_MAX : (base & 0x7fffffffu)*4 + offset*sizeof(ClipHistory);
  const auto range = gfx::reserve_scene_storage(count*sizeof(ClipHistory), static_cast<uint32_t>(end));
  if (!range.size) return 0;
  auto* dst = gfx::scene_storage_data(range);
  for (unsigned i = 0; i < count; ++i) {
    const auto j = offset+i;
    const ClipHistory vertex{match ? matchedVelocity->prior[j] : draw.clip[j], draw.prior[j], draw.clip[j]};
    std::memcpy(dst + i*sizeof(vertex), &vertex, sizeof(vertex));
  }
  return first ? 0x80000000u | (range.offset / 4) : base;
}

void append_velocity_chunk(const ShaderConfig& config, GXPrimitive prim, const uint8_t* vertices,
                           uint16_t count, uint32_t& base) {
  if (recordingVelocity)
    base = velocity_chunk(config, vertices, count, velocity_topology(config, prim, vertices, count), base, false);
}

void end_velocity_batch() { recordingVelocity = matchedVelocity = nullptr; }

uint32_t record_previous_clip(const ShaderConfig& config, GXPrimitive prim, const uint8_t* vertices,
                              uint16_t count, const Mat4x4<float>& projection, HashType material) {
  if (!vertices || !count) return 0;
  const auto topology = velocity_topology(config, prim, vertices, count);
  Hasher key;
  key.update(config);
  key.update(material);
  key.update(g_gxState.projType);
  key.update(topology);
  const auto& vp = g_gxState.renderViewport;
  const std::array viewport{vp.left, vp.top, vp.width, vp.height};
  key.update(viewport.data(), sizeof(viewport));
  const auto& pos = config.attrs[GX_VA_POS];
  if (pos.attrType == GX_INDEX8 || pos.attrType == GX_INDEX16)
    key.update(g_gxState.arrays[GX_VA_POS].data);
  const auto identity = key.digest();
  auto& now = currentVelocity[identity].emplace_back();
  const auto& idx = config.attrs[GX_VA_PNMTXIDX];
  const unsigned matrix = idx.attrType == GX_NONE ? g_gxState.currentPnMtx : (vertices[idx.offset] & 63u) / 3u;
  float local[3]{};
  if (matrix >= MaxPnMtx || !attribute(config, vertices, GX_VA_POS, local, 3)) return 0;
  now.center = transform(g_gxState.pnMtx[matrix].pos, local, 1.f);
  const auto hits = previousVelocity.find(identity);
  float nearest = 1500.f * 1500.f;
  if (hits != previousVelocity.end()) for (auto& old : hits->second) {
    if (old.used || old.clip.empty()) continue;
    float distance = 0.f;
    for (unsigned i = 0; i < 3; ++i) {
      const auto delta = now.center[i] - old.center[i];
      distance += delta * delta;
    }
    if (distance < nearest) { matchedVelocity = &old; nearest = distance; }
  }
  if (matchedVelocity) {
    matchedVelocity->used = true;
    now.clip.reserve(matchedVelocity->clip.size());
    now.prior.reserve(matchedVelocity->clip.size());
  }
  recordingVelocity = &now;
  velocityProjection = projection;
  return velocity_chunk(config, vertices, count, topology, 0, true);
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

// Identical draws (same mesh, material and camera; e.g. every enemy's harpoon rope) are told apart by
// nearest center. When one leaves or appears, the nearest old copy can belong to another object, and the
// interpolated frames then blend between two objects' poses: long stretched slabs for one frame. Accept a
// previous draw only if its vertices keep their layout around the center: the squared residual over the
// mean squared radius is 2(1 - cos t) for a rotation by t, so 0.27 allows about 30 degrees (or ~1.5x
// scale) per guest frame, far beyond real motion, while differently oriented copies fail.
bool same_shape(const Draw& old, const Draw& now) noexcept {
  // Per vertex as well: one vertex pulled from another object's place passes the average.
  static std::vector<std::pair<double, double>> perVertex;  // (residual, radius), seal thread only
  perVertex.resize(now.pose.size());
  double residual = 0, extent = 0;
  for (size_t v = 0; v < now.pose.size(); ++v) {
    double res = 0, rad = 0;
    for (unsigned r = 0; r < 3; ++r) {
      const double a = now.pose[v].pos[r] - now.center[r];
      const double b = old.pose[v].pos[r] - old.center[r];
      res += (a - b) * (a - b);
      rad += 0.5 * (a * a + b * b);
    }
    perVertex[v] = {res, rad};
    residual += res;
    extent += rad;
  }
  const double n = double(now.pose.size());
  if (residual > 0.27 * extent + 1e-6 * n) return false;
  const double meanRadius = n > 0 ? extent / n : 0;
  for (const auto& [res, rad] : perVertex)
    if (res > 0.27 * (rad + meanRadius) + 1e-6) return false;
  return true;
}

bool seal_native_scene() noexcept {
  const auto started = std::chrono::steady_clock::now();
  size_t shapeRejected = 0;
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
      // Nearest first; the first previous copy with the same shape wins (usually the first checked).
      static std::vector<std::pair<float, size_t>> candidates;
      candidates.clear();
      for (size_t index : it->second) {
        const auto& old = previous[index];
        if (old.used || old.pose.size() != draw.pose.size())
          continue;
        float d = 0;
        for (unsigned r = 0; r < 3; ++r) {
          const float delta = old.center[r] - draw.center[r];
          d += delta * delta;
        }
        if (d < distance)
          candidates.emplace_back(d, index);
      }
      std::sort(candidates.begin(), candidates.end());
      for (const auto& [d, index] : candidates) {
        if (same_shape(previous[index], draw)) {
          best = index;
          distance = d;
          break;
        }
        ++shapeRejected;
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
  static size_t shapeRejectedTotal = 0;
  shapeRejectedTotal += shapeRejected;
  const size_t vertices = totalVertices;
  const auto beforePose = [](const Draw& draw, size_t v) -> const Pose& {
    return draw.matched ? previous[draw.previousIndex].pose[v] : draw.pose[v];
  };
  const bool velocity = velocity_capture_enabled();
  struct VelocityPair { ScenePosePair pair; std::array<float,4> older; };
  std::vector<VelocityPair> motionPairs;
  if (velocity) {
    motionPairs.reserve(totalPoses);
    for (auto& draw : current) {
      draw.priorClip.reserve(draw.pose.size());
      for (size_t v = 0; v < draw.pose.size(); ++v)
        draw.priorClip.push_back(beforePose(draw,v).clip);
    }
  }
  auto olderClip = [&](const Draw& draw, size_t v) {
    if (draw.matched && v < previous[draw.previousIndex].priorClip.size())
      return previous[draw.previousIndex].priorClip[v];
    return beforePose(draw, v).clip;
  };
  static std::vector<ScenePosePair> packed;
  packed.clear();
  packed.reserve(totalPoses);
  for (const auto& owner : owners) {
    const auto& draw = current[owner.draw];
    packed.push_back(scene_pair(beforePose(draw, owner.vertex), draw.pose[owner.vertex]));
    if (velocity) motionPairs.push_back({packed.back(), olderClip(draw,owner.vertex)});
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
        if (velocity) motionPairs.push_back({packed.back(), olderClip(canonical,owner.vertex)});
      }
      welded += &draw != &canonical || v != owner.vertex;
    }
  }
  static std::vector<uint32_t> upload;
  const size_t stride = (velocity ? sizeof(VelocityPair) : sizeof(ScenePosePair)) / sizeof(uint32_t);
  const size_t words = vertices + packed.size() * stride;
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
    draw.output->velocity = velocity;
    draw.output->base = storage.offset / 4 + static_cast<uint32_t>(cursor) + 1;
    for (uint32_t pose : draw.vertexPoses)
      upload[cursor++] = recordBase + draw.joins[pose] * stride;
  }
  if (velocity) std::memcpy(upload.data()+vertices, motionPairs.data(), motionPairs.size()*sizeof(VelocityPair));
  else std::memcpy(upload.data() + vertices, packed.data(), packed.size() * sizeof(ScenePosePair));
  std::memcpy(gfx::scene_storage_data(storage), upload.data(), words * sizeof(uint32_t));
  static uint64_t frames = 0;
  if (++frames % 60 == 0)
    std::fprintf(stderr,
                 "[native-scene] draws=%zu matched=%zu vertices=%zu shared=%zu records=%zu rejected=%u capture=%.3fms "
                 "seal=%.3fms translucent=%zu translucent_matched=%zu shape_rejected=%zu\n",
                 current.size(), matched, vertices, welded, packed.size(), rejected, captureMs,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
                 translucent, translucentMatched, shapeRejectedTotal);
  recycled.clear();
  for (auto it = previous.rbegin(); it != previous.rend(); ++it)
    recycled.push_back(std::move(*it));
  previous = std::move(current);
  current.clear();
  return true;
}
} // namespace aurora::gx

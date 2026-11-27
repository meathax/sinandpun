#pragma once

#include <algorithm>
#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#include <cstring>
#endif
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aurora::webgpu::rtgi {
using Point = std::array<float, 3>;
struct alignas(16) Triangle {
  Point a;
  float red;
  Point b;
  float green;
  Point c;
  float blue;
};
struct alignas(16) Node {
  Point low;
  uint32_t escape;
  Point high;
  uint32_t leaf; // First triangle index in the upper bits, count (0 for branches) in the low three.
};
static_assert(sizeof(Triangle) == 48 && sizeof(Node) == 32);
constexpr size_t TriangleLimit = 40960;
constexpr size_t LiveTriangleLimit = TriangleLimit - 1600;

namespace detail {
// One node of the build: bounds over [first, end) and, for a branch, the partition point. The
// triangles of the range are partitioned in place, so disjoint ranges can be split concurrently.
struct BvhSplit {
  Node node;
  uint32_t middle;
  bool leaf;
};
inline BvhSplit split_bvh_range(std::vector<Triangle>& triangles, uint32_t first, uint32_t end) {
  Node node{.low = triangles[first].a, .high = triangles[first].a};
#if defined(__SSE2__) || defined(_M_X64)
  // The three positions are stored as aligned float4 records with the
  // material color in the fourth lane. Only XYZ is written to the node.
  const auto vertex4 = [](const Triangle& t, size_t offset) {
    __m128 vertex;
    std::memcpy(&vertex, reinterpret_cast<const char*>(&t) + offset, sizeof(vertex));
    return vertex;
  };
  __m128 low = vertex4(triangles[first], offsetof(Triangle, a));
  __m128 high = low;
  for (uint32_t i = first; i < end; ++i) {
    const auto& t = triangles[i];
    const __m128 a = vertex4(t, offsetof(Triangle, a));
    const __m128 b = vertex4(t, offsetof(Triangle, b));
    const __m128 c = vertex4(t, offsetof(Triangle, c));
    low = _mm_min_ps(low, _mm_min_ps(a, _mm_min_ps(b,c)));
    high = _mm_max_ps(high, _mm_max_ps(a, _mm_max_ps(b,c)));
  }
  std::memcpy(node.low.data(), &low, sizeof(Point));
  std::memcpy(node.high.data(), &high, sizeof(Point));
#else
  for (uint32_t i = first; i < end; ++i) {
    const auto& triangle = triangles[i];
    for (unsigned axis = 0; axis < 3; ++axis) {
      node.low[axis] = std::min({node.low[axis], triangle.a[axis], triangle.b[axis], triangle.c[axis]});
      node.high[axis] = std::max({node.high[axis], triangle.a[axis], triangle.b[axis], triangle.c[axis]});
    }
  }
#endif
  if (end - first <= 6) {
    node.leaf = (first << 3) | (end - first);
    return {node, end, true};
  }
  unsigned axis = 0;
  for (unsigned i = 1; i < 3; ++i)
    if (node.high[i] - node.low[i] > node.high[axis] - node.low[axis]) axis = i;
  const float splitAt = 1.5f * (node.low[axis] + node.high[axis]);
  const auto coordinate = [axis](const Triangle& t) {
    return t.a[axis] + t.b[axis] + t.c[axis];
  };
  const auto start = triangles.begin() + first;
  const auto finish = triangles.begin() + end;
  auto partition = std::partition(start, finish, [&](const Triangle& t) { return coordinate(t) < splitAt; });
  const auto minimumSide = (end - first) / 4;
  if (partition - start < minimumSide || finish - partition < minimumSide) {
    partition = start + (end - first) / 2;
    std::nth_element(start, partition, finish, [&](const Triangle& a, const Triangle& b) {
      return coordinate(a) < coordinate(b);
    });
  }
  return {node, uint32_t(partition - triangles.begin()), false};
}

// Depth-first subtree over [first, end) appended to `nodes`; escape links are indices into `nodes`.
inline void build_bvh_range(std::vector<Triangle>& triangles, uint32_t first, uint32_t end, std::vector<Node>& nodes) {
  const auto split = split_bvh_range(triangles, first, end);
  const auto index = nodes.size();
  nodes.push_back(split.node);
  if (!split.leaf) {
    build_bvh_range(triangles, first, split.middle, nodes);
    build_bvh_range(triangles, split.middle, end, nodes);
  }
  nodes[index].escape = uint32_t(nodes.size());
}
} // namespace detail

inline void build_bvh(std::vector<Triangle>& triangles, std::vector<Node>& nodes) {
  nodes.clear();
  if (triangles.empty()) return;
  nodes.reserve(triangles.size() / 2);
  detail::build_bvh_range(triangles, 0, uint32_t(triangles.size()), nodes);
}

// Same tree, node for node, as build_bvh: the top two levels are split serially, the four subtrees
// below them are built concurrently into separate arrays and appended in depth-first order with their
// escape links rebased.
inline void build_bvh_parallel(std::vector<Triangle>& triangles, std::vector<Node>& nodes) {
  nodes.clear();
  const auto count = uint32_t(triangles.size());
  if (count < 4096) {  // small scenes: threads cost more than they save
    build_bvh(triangles, nodes);
    return;
  }
  nodes.reserve(triangles.size() / 2);
  const auto root = detail::split_bvh_range(triangles, 0, count);
  if (root.leaf) {
    nodes.push_back(root.node);
    nodes.back().escape = 1;
    return;
  }
  const detail::BvhSplit halves[2] = {detail::split_bvh_range(triangles, 0, root.middle),
                                      detail::split_bvh_range(triangles, root.middle, count)};
  // Subtree ranges in depth-first order; a leaf half has no subtrees.
  struct Range { uint32_t first, end; };
  std::array<Range, 4> ranges{};
  std::array<std::vector<Node>, 4> parts;
  const Range spans[2] = {{0, root.middle}, {root.middle, count}};
  for (int h = 0; h < 2; ++h) {
    if (!halves[h].leaf) {
      ranges[h * 2] = {spans[h].first, halves[h].middle};
      ranges[h * 2 + 1] = {halves[h].middle, spans[h].end};
    }
  }
  const auto build = [&](int i) {
    if (ranges[i].end > ranges[i].first) {
      parts[i].reserve((ranges[i].end - ranges[i].first) / 2);
      detail::build_bvh_range(triangles, ranges[i].first, ranges[i].end, parts[i]);
    }
  };
  std::array<std::thread, 3> helpers;
  for (int i = 1; i < 4; ++i) helpers[i - 1] = std::thread(build, i);
  build(0);
  for (auto& helper : helpers) helper.join();
  const auto append = [&](std::vector<Node>& part) {
    const auto base = uint32_t(nodes.size());
    for (auto node : part) {
      node.escape += base;
      nodes.push_back(node);
    }
  };
  nodes.push_back(root.node);
  for (int h = 0; h < 2; ++h) {
    const auto index = nodes.size();
    nodes.push_back(halves[h].node);
    if (!halves[h].leaf) {
      append(parts[h * 2]);
      append(parts[h * 2 + 1]);
    }
    nodes[index].escape = uint32_t(nodes.size());
  }
  nodes[0].escape = uint32_t(nodes.size());
}

// A BVH built off the recording (FIFO) thread: submit_bvh_job hands it to a background builder, the
// render worker joins it with wait_bvh_job right before the pass's first encode.
struct BvhJob {
  std::vector<Triangle> triangles;
  std::vector<Node> nodes;
  std::atomic<bool> done{false};
};
void submit_bvh_job(std::shared_ptr<BvhJob> job);
inline void wait_bvh_job(BvhJob& job) { job.done.wait(false, std::memory_order_acquire); }
} // namespace aurora::webgpu::rtgi

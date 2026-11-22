#pragma once

#include <algorithm>
#include <array>
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
  uint32_t first;
  uint32_t count;
};
static_assert(sizeof(Triangle) == 48 && sizeof(Node) == 48);
constexpr size_t TriangleLimit = 40960;
constexpr size_t LiveTriangleLimit = TriangleLimit - 1600;

inline void build_bvh(std::vector<Triangle>& triangles, std::vector<Node>& nodes) {
  nodes.clear();
  if (triangles.empty()) return;
  nodes.reserve(triangles.size() / 2);
  const auto split = [&](auto&& self, uint32_t first, uint32_t end) -> void {
    Node node{.low = triangles[first].a, .high = triangles[first].a, .first = first};
    for (uint32_t i = first; i < end; ++i) {
      const auto& triangle = triangles[i];
      for (unsigned axis = 0; axis < 3; ++axis) {
        node.low[axis] = std::min({node.low[axis], triangle.a[axis], triangle.b[axis], triangle.c[axis]});
        node.high[axis] = std::max({node.high[axis], triangle.a[axis], triangle.b[axis], triangle.c[axis]});
      }
    }
    const auto index = nodes.size();
    nodes.push_back(node);
    if (end - first <= 6) {
      nodes[index].count = end - first;
    } else {
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
      const uint32_t middle = uint32_t(partition - triangles.begin());
      self(self, first, middle);
      self(self, middle, end);
    }
    nodes[index].escape = uint32_t(nodes.size());
  };
  split(split, 0, uint32_t(triangles.size()));
}
} // namespace aurora::webgpu::rtgi

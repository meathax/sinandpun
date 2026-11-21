#pragma once

#include <cstdint>
#include <unordered_set>

namespace aurora::gx {
// GX draws carry no model/connected-component identity. A viewport is the
// smallest known domain that contains every piece of a split model.
class InterpolationCoherence {
public:
  void observe(uint64_t viewport, bool staged, bool geometryUnchanged, bool transformsValid) {
    if (!staged || !geometryUnchanged || !transformsValid) {
      m_blocked.insert(viewport);
    }
  }

  bool allows(uint64_t viewport) const { return !m_blocked.contains(viewport); }

private:
  std::unordered_set<uint64_t> m_blocked;
};
} // namespace aurora::gx

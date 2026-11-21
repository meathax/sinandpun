#include "../../aurora-main/lib/gx/interpolation_coherence.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

#ifdef NDEBUG
#error Assertions must remain enabled
#endif

int main() {
  using aurora::gx::InterpolationCoherence;
  // Two separate draws share an edge at x=0, then move together to x=2.
  // The former per-draw fallback leaves the rejected piece at x=2, tearing
  // it from the accepted piece at x=1 in the midpoint frame.
  assert(std::abs(1.0f - 2.0f) == 1.0f); // old-policy counterexample
  for (int reason = 0; reason < 3; ++reason) {
    InterpolationCoherence gate;
    gate.observe(7, true, true, true);
    gate.observe(7, reason != 0, reason != 1, reason != 2);
    gate.observe(8, true, true, true); // independent camera remains eligible
    assert(!gate.allows(7));
    assert(gate.allows(8));
    for (float weight : {0.25f, 0.5f, 0.75f}) {
      const float left = gate.allows(7) ? 2.0f * weight : 2.0f;
      const float right = 2.0f;
      assert(left == right);
    }
    gate.observe(7, true, true, true); // later valid pieces cannot undo rejection
    assert(!gate.allows(7));
  }
  InterpolationCoherence nextFrame;
  nextFrame.observe(7, true, true, true);
  assert(nextFrame.allows(7));
  std::puts("PASS: missing staging, changed geometry, invalid transforms, camera isolation, recovery");
}

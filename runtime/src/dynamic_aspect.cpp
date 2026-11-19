#include "aurora_events.h"

#include <atomic>

#include <dolphin/vi.h>

// Viewport/aspect policy. Sin & Punishment renders through the stock GX path,
// so presentation is handled entirely by Aurora: either the surface is
// stretched (the original game's 16:9 or 4:3 output scaled to the window) or
// the aspect is forced (letter/pillar-boxed).
namespace {

bool g_widescreen = true;
bool g_forceAspect = false;
std::atomic_bool g_requestedForceAspect{false};
bool g_policyDirty = false;

} // namespace

void UpdateDynamicAspectSurface(uint32_t surfaceWidth, uint32_t surfaceHeight) {
    const bool requestedForceAspect = g_requestedForceAspect.load(std::memory_order_acquire);
    if (g_forceAspect != requestedForceAspect) {
        g_forceAspect = requestedForceAspect;
        g_policyDirty = true;
    }
    if (surfaceWidth == 0 || surfaceHeight == 0) {
        return;
    }
    if (g_policyDirty) {
        if (g_forceAspect && !g_widescreen) {
            AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
            VILockAspectRatio(4, 3);
        } else {
            AuroraSetViewportPolicy(g_forceAspect ? AURORA_VIEWPORT_16_9 : AURORA_VIEWPORT_STRETCH);
            VIUnlockAspectRatio();
        }
        g_policyDirty = false;
    }
}

void SetForceAspect(bool enabled) {
    g_requestedForceAspect.store(enabled, std::memory_order_release);
}

void ConfigureDynamicAspect(bool widescreen, bool forceAspect, uint32_t surfaceWidth, uint32_t surfaceHeight) {
    (void)surfaceWidth;
    (void)surfaceHeight;
    g_widescreen = widescreen;
    g_forceAspect = forceAspect;
    g_requestedForceAspect.store(forceAspect, std::memory_order_relaxed);
    g_policyDirty = true;
}

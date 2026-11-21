#pragma once

#include <aurora/gfx.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx::presentation {
struct SurfaceAcquisition {
  wgpu::Surface surface;
  wgpu::SurfaceTexture texture;
  uint64_t waitedNanos = 0;
};
// Acquire on a dedicated thread; a display-buffer wait must not block guest GPU
// completion on the render worker. There is at most one outstanding acquisition.
void request_native_surface();
bool native_surface_ready();
bool wait_native_surface();
std::optional<SurfaceAcquisition> take_native_surface(bool wait);
void finish_native_surface(bool requestNext);
void shutdown_native_surface();
class SurfaceAcquirePause {
public:
  SurfaceAcquirePause();
  ~SurfaceAcquirePause();
  SurfaceAcquirePause(const SurfaceAcquirePause&) = delete;
  SurfaceAcquirePause& operator=(const SurfaceAcquirePause&) = delete;
};
// Records a successful surface present. `duplicated` marks a present that repeated the previous
// image instead of showing new motion.
void record_present(bool duplicated) noexcept;
AuroraPresentTiming snapshot_timing() noexcept;

struct Schedule {
  uint64_t baseNanos = 0;
  uint64_t intervalNanos = 0;
};
// Schedule set by the producer for the frame currently being ended.
Schedule present_schedule() noexcept;
bool producer_paced() noexcept;

// Paces presents in blocking present modes to the display refresh so surplus frames are dropped
// instead of stalling the producer. Render worker only.
bool admit_display_present(uint64_t atNanos) noexcept;
// Reports how long acquiring and presenting the surface blocked.
void report_display_present_wait(uint64_t waitNanos) noexcept;
// Whether the display refreshes fast enough to show frames between native frames `intervalNanos` apart.
bool display_shows_interpolation(uint64_t intervalNanos) noexcept;

// steady_clock time in nanoseconds, the clock present schedules are expressed in.
uint64_t now_nanos() noexcept;
// Blocks until `deadlineNanos` with sub-millisecond precision; 0 returns immediately.
void wait_until(uint64_t deadlineNanos) noexcept;

// ---- VRR (G-SYNC / FreeSync) presentation ----
// Resolved from the configured mode, the surface capabilities, the display refresh and the frame-rate
// mode. When active the swapchain uses the tearing-allowed Immediate present mode (DXGI flip model,
// Present(0, ALLOW_TEARING)) and presents are limited to `limitFps` (refresh - margin).
struct VrrState {
  bool tearingSupported = false;
  bool active = false;
  bool limiterActive = false;
  double refreshHz = 0.0;
  double limitFps = 0.0; // planner target (refresh - margin); 0 when the limiter is not active
  // Minimum present spacing enforced by pace_present as a rate (refresh - margin / 2): slightly above the
  // planner target so a late present is recovered instead of ratcheting the whole cadence late.
  double floorFps = 0.0;
};
VrrState resolve_vrr() noexcept;
// Whether the swapchain should use the VRR (tearing-allowed) present mode.
bool vrr_wanted() noexcept;
// Planner cap in Hz; 0 when the limiter is inactive.
double vrr_limit_fps() noexcept;
// gpu.cpp: whether the surface offers the Immediate (tearing) present mode, and on which backend.
void vrr_set_surface_caps(bool immediateSupported, bool d3d12Backend) noexcept;
// gpu.cpp: the present mode select_present_mode returned (true = the VRR mode).
void vrr_note_applied(bool vrrPresentMode) noexcept;
// Recomputes the limiter and requests a swapchain rebuild when the wanted present mode changed.
// Safe to call from any thread whenever the mode, frame rate or display refresh changes.
void vrr_update() noexcept;
// Paces a present to `dueNanos` (0 = as soon as possible) and enforces the VRR minimum present
// spacing. Render worker only; equals wait_until(dueNanos) when the limiter is inactive.
void pace_present(uint64_t dueNanos) noexcept;

// Guest-RAM write generation for a host range, or AURORA_GUEST_WRITE_UNTRACKED.
uint64_t guest_write_generation(const void* hostPtr, size_t size) noexcept;
// Reports a write Aurora made to guest RAM.
void notify_guest_write(const void* hostPtr, size_t size) noexcept;
} // namespace aurora::gfx::presentation

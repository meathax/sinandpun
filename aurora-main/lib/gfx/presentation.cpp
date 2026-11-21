#include "presentation.hpp"

#include "render_worker.hpp"
#include "resources.hpp"
#include "../gx/frame_interpolation.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../window.hpp"

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>
#include <condition_variable>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_5.h>
#endif

namespace aurora::gfx::presentation {
namespace {
struct SurfaceAcquirer {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread worker;
  SurfaceAcquisition result;
  unsigned paused = 0;
  bool requested = false, acquiring = false, ready = false, inUse = false, stopping = false;
  ~SurfaceAcquirer() {
    { std::lock_guard lock{mutex}; stopping=true; cv.notify_all(); }
    if(worker.joinable()) worker.join();
  }
} surfaceAcquirer;

void acquire_surface_main() {
  auto& state=surfaceAcquirer;
  std::unique_lock lock{state.mutex};
  for (;;) {
    state.cv.wait(lock,[&]{return state.stopping || state.requested;});
    if(state.stopping) return;
    state.requested=false; state.acquiring=true;
    lock.unlock();
    SurfaceAcquisition result;
    result.texture.status=wgpu::SurfaceGetCurrentTextureStatus::Error;
    const auto start=now_nanos();
    {
      window::SurfaceLock surfaceLock;
      if(window::is_presentable() && webgpu::g_surface) {
        result.surface=webgpu::g_surface;
        result.surface.GetCurrentTexture(&result.texture);
      }
    }
    result.waitedNanos=now_nanos()-start;
    lock.lock();
    state.result=std::move(result);
    state.acquiring=false; state.ready=true;
    state.cv.notify_all();
  }
}
void request_surface_locked(SurfaceAcquirer& state) {
  if(state.paused || state.stopping || state.requested || state.acquiring || state.ready || state.inUse || !window::is_presentable()) return;
  if(!state.worker.joinable()) state.worker=std::thread(acquire_surface_main);
  state.requested=true; state.cv.notify_all();
}
}
void request_native_surface() {
  auto& state=surfaceAcquirer;
  std::lock_guard lock{state.mutex};
  request_surface_locked(state);
}
bool native_surface_ready() {
  auto& state=surfaceAcquirer;
  std::lock_guard lock{state.mutex};
  request_surface_locked(state);
  return !state.paused && state.ready;
}
std::optional<SurfaceAcquisition> take_native_surface(bool wait) {
  auto& state=surfaceAcquirer;
  std::unique_lock lock{state.mutex};
  if(wait && (state.requested || state.acquiring))
    state.cv.wait(lock,[&]{return state.ready || state.paused || state.stopping;});
  if(!state.ready || state.paused) return {};
  state.ready=false; state.inUse=true;
  return std::exchange(state.result,{});
}
bool wait_native_surface() {
  auto& state=surfaceAcquirer;
  std::unique_lock lock{state.mutex};
  state.cv.wait(lock,[&]{return state.ready || state.paused || state.stopping;});
  return state.ready && !state.paused && !state.stopping;
}
void finish_native_surface(bool requestNext) {
  auto& state=surfaceAcquirer;
  std::lock_guard lock{state.mutex};
  state.inUse=false;
  if(requestNext) request_surface_locked(state);
  state.cv.notify_all();
}
SurfaceAcquirePause::SurfaceAcquirePause() {
  render_worker::request_idle_retire();
  auto& state=surfaceAcquirer;
  std::unique_lock lock{state.mutex};
  ++state.paused; state.requested=false; state.cv.notify_all();
  state.cv.wait(lock,[&]{return !state.acquiring && (!state.inUse || render_worker::is_worker_thread());});
  state.result={}; state.ready=false;
}
SurfaceAcquirePause::~SurfaceAcquirePause() {
  auto& state=surfaceAcquirer;
  std::lock_guard lock{state.mutex};
  --state.paused; state.cv.notify_all();
}
void shutdown_native_surface() {
  auto& state=surfaceAcquirer;
  { std::lock_guard lock{state.mutex}; state.stopping=true; state.requested=false; state.cv.notify_all(); }
  if(state.worker.joinable()) state.worker.join();
  std::lock_guard lock{state.mutex};
  state.result={}; state.ready=false; state.inUse=false;
  state.stopping=false;
}
namespace {
using PresentClock = std::chrono::steady_clock;

struct PresentTimingSample {
  PresentClock::time_point presentedAt{};
  std::chrono::nanoseconds interval{};
  bool duplicated = false;
};

std::mutex g_timingMutex;
std::array<PresentTimingSample, 512> g_timingSamples{};
size_t g_timingWriteIndex = 0;
size_t g_timingSampleCount = 0;
std::optional<PresentClock::time_point> g_lastPresent;
uint64_t g_totalPresentCount = 0;

std::atomic_uint64_t g_scheduleBaseNanos = 0;
std::atomic_uint64_t g_scheduleIntervalNanos = 0;
std::atomic_uint64_t g_nativeIntervalNanos = 0;
std::atomic_bool g_producerPaced = true;

// Display present budget in refresh periods. The cap and the admit threshold absorb frame jitter.
constexpr double MaxDisplayCredit = 2.0;
constexpr double AdmitDisplayCredit = 0.75;
double g_displayCredit = MaxDisplayCredit;
uint64_t g_displayCreditAt = 0;

std::atomic<AuroraGuestWriteGenerationCallback> g_guestWriteGeneration = nullptr;
std::atomic<AuroraGuestWriteNotifyCallback> g_guestWriteNotify = nullptr;
} // namespace

void record_present(bool duplicated) noexcept {
  const auto now = PresentClock::now();
  std::lock_guard lock(g_timingMutex);
  std::chrono::nanoseconds interval{};
  if (g_lastPresent) {
    interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *g_lastPresent);
  }
  g_lastPresent = now;
  g_timingSamples[g_timingWriteIndex] = {
      .presentedAt = now,
      .interval = interval,
      .duplicated = duplicated,
  };
  g_timingWriteIndex = (g_timingWriteIndex + 1) % g_timingSamples.size();
  g_timingSampleCount = std::min(g_timingSampleCount + 1, g_timingSamples.size());
  ++g_totalPresentCount;
}

AuroraPresentTiming snapshot_timing() noexcept {
  constexpr auto Window = std::chrono::seconds(1);
  const auto cutoff = PresentClock::now() - Window;
  std::vector<double> milliseconds;
  uint64_t totalPresentCount = 0;
  size_t newMotionSamples = 0;
  {
    std::lock_guard lock(g_timingMutex);
    totalPresentCount = g_totalPresentCount;
    milliseconds.reserve(g_timingSampleCount);
    for (size_t i = 0; i < g_timingSampleCount; ++i) {
      const auto& sample = g_timingSamples[i];
      if (sample.presentedAt >= cutoff && sample.interval.count() > 0) {
        milliseconds.push_back(std::chrono::duration<double, std::milli>(sample.interval).count());
        if (!sample.duplicated) {
          ++newMotionSamples;
        }
      }
    }
  }

  AuroraPresentTiming result{
      .totalPresentCount = totalPresentCount,
      .sampleCount = static_cast<uint32_t>(milliseconds.size()),
  };
  if (milliseconds.empty()) {
    return result;
  }

  double totalMilliseconds = 0.0;
  for (const double value : milliseconds) {
    totalMilliseconds += value;
  }
  const auto count = static_cast<double>(milliseconds.size());
  result.averageFrameTimeMs = totalMilliseconds / count;
  result.framesPerSecond = result.averageFrameTimeMs > 0.0 ? 1000.0 / result.averageFrameTimeMs : 0.0;
  result.effectiveFramesPerSecond = result.framesPerSecond * (static_cast<double>(newMotionSamples) / count);
  for (const double value : milliseconds) {
    const double difference = value - result.averageFrameTimeMs;
    result.jitterMs += difference * difference;
  }
  result.jitterMs = std::sqrt(result.jitterMs / count);
  std::sort(milliseconds.begin(), milliseconds.end());
  result.p95FrameTimeMs = milliseconds[static_cast<size_t>(std::floor((count - 1.0) * 0.95))];
  result.p50FrameTimeMs = milliseconds[static_cast<size_t>(std::floor((count - 1.0) * 0.50))];
  result.p99FrameTimeMs = milliseconds[static_cast<size_t>(std::floor((count - 1.0) * 0.99))];
  result.maxFrameTimeMs = milliseconds.back();
  return result;
}

Schedule present_schedule() noexcept {
  return {
      .baseNanos = g_scheduleBaseNanos.load(std::memory_order_acquire),
      .intervalNanos = g_scheduleIntervalNanos.load(std::memory_order_acquire),
  };
}

bool admit_display_present(uint64_t atNanos) noexcept {
  const uint64_t period = window::refresh_period_nanos();
  if (period == 0) {
    return true;
  }
  if (g_displayCreditAt != 0 && atNanos > g_displayCreditAt) {
    g_displayCredit += static_cast<double>(atNanos - g_displayCreditAt) / static_cast<double>(period);
  }
  g_displayCreditAt = std::max(g_displayCreditAt, atNanos);
  g_displayCredit = std::min(g_displayCredit, MaxDisplayCredit);
  if (g_displayCredit < AdmitDisplayCredit) {
    return false;
  }
  g_displayCredit -= 1.0;
  return true;
}

void report_display_present_wait(uint64_t waitNanos) noexcept {
  // A present that waited for a vblank means the display queue is full.
  const uint64_t period = window::refresh_period_nanos();
  if (period != 0 && waitNanos > period / 2) {
    // Hold off for a full period before the next admit.
    g_displayCredit = std::min(g_displayCredit, AdmitDisplayCredit - 1.0);
  }
}

bool display_shows_interpolation(uint64_t intervalNanos) noexcept {
  const uint64_t period = window::refresh_period_nanos();
  return period == 0 || intervalNanos == 0 || period * 20 < intervalNanos * 19;
}

bool producer_paced() noexcept { return g_producerPaced.load(std::memory_order_acquire); }

uint64_t now_nanos() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now().time_since_epoch()).count());
}

void wait_until(uint64_t deadlineNanos) noexcept {
  if (deadlineNanos == 0) {
    return;
  }
  const PresentClock::time_point deadline{std::chrono::nanoseconds{deadlineNanos}};
  // Spin tail covering the waitable timer's wakeup jitter.
  constexpr auto SpinWindow = std::chrono::microseconds(400);
  auto now = PresentClock::now();
  if (now >= deadline) {
    return;
  }
  const auto timerDeadline = deadline - now > SpinWindow ? deadline - SpinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  struct HighResolutionTimer {
    HANDLE handle = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_MODIFY_STATE | SYNCHRONIZE);
    ~HighResolutionTimer() {
      if (handle != nullptr) {
        ::CloseHandle(handle);
      }
    }
  };
  static thread_local HighResolutionTimer timer;
  if (timer.handle != nullptr && timerDeadline > now) {
    const auto remaining = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
        timerDeadline - now);
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<int64_t>(remaining.count(), 1);
    if (::SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
      ::WaitForSingleObject(timer.handle, INFINITE);
    }
  } else
#endif
  {
    std::this_thread::sleep_until(timerDeadline);
  }
  while (PresentClock::now() < deadline) {
#if defined(_WIN32)
    YieldProcessor();
#else
    std::this_thread::yield();
#endif
  }
}

namespace {
std::atomic_uint32_t g_vrrMode = AURORA_VRR_AUTO;
std::atomic_uint32_t g_vrrMargin = 3;
std::atomic_bool g_vrrLimiter = true;
std::atomic_bool g_vrrSurfaceImmediate = false;
std::atomic_bool g_vrrD3D12 = false;
std::atomic_bool g_vrrCapsKnown = false;
std::atomic_bool g_vrrApplied = false;
std::atomic_bool g_vrrRequested = false;
std::atomic_uint64_t g_vrrMinIntervalNanos = 0;
uint64_t g_paceLastNanos = 0; // render worker only

// A display that cannot show frames between two native frames gets native pacing (same threshold as
// display_shows_interpolation / the frame-rate planner).
constexpr double VrrMinRefreshHz = 63.2;
// The limiter never goes down to the guest rate: a floor at or below 60 Hz would throttle the game.
constexpr double VrrMinLimitHz = 60.5;

// IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING), loaded dynamically.
bool dxgi_tearing_supported() noexcept {
#if defined(_WIN32)
  static const bool supported = [] {
    HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) {
      return false;
    }
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory1>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));
    bool result = false;
    IDXGIFactory5* factory = nullptr;
    if (create != nullptr && SUCCEEDED(create(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory))) &&
        factory != nullptr) {
      BOOL allow = FALSE;
      result = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) &&
               allow != FALSE;
      factory->Release();
    }
    ::FreeLibrary(dxgi);
    return result;
  }();
  return supported;
#else
  return true;
#endif
}
} // namespace

VrrState resolve_vrr() noexcept {
  VrrState state;
  const uint64_t period = window::refresh_period_nanos();
  state.refreshHz = period != 0 ? 1.0e9 / static_cast<double>(period) : 0.0;
  state.tearingSupported =
      g_vrrCapsKnown.load(std::memory_order_acquire) && g_vrrSurfaceImmediate.load(std::memory_order_acquire) &&
      (!g_vrrD3D12.load(std::memory_order_acquire) || dxgi_tearing_supported());

  AuroraFrameRateMode rateMode = AURORA_FRAME_RATE_OFF;
  uint32_t fixedFps = 0;
  uint32_t capFps = 0;
  gx::get_frame_rate(rateMode, fixedFps, capFps);
  const auto mode = static_cast<AuroraVrrMode>(g_vrrMode.load(std::memory_order_acquire));
  const bool wanted = mode == AURORA_VRR_ON || (mode == AURORA_VRR_AUTO && state.refreshHz > VrrMinRefreshHz &&
                                                 rateMode != AURORA_FRAME_RATE_OFF);
  state.active = state.tearingSupported && wanted;
  if (state.active && g_vrrLimiter.load(std::memory_order_acquire) && state.refreshHz > 0.0) {
    const double limit = state.refreshHz - static_cast<double>(g_vrrMargin.load(std::memory_order_acquire));
    if (limit > VrrMinLimitHz) {
      state.limiterActive = true;
      state.limitFps = limit;
      state.floorFps = state.refreshHz - 0.5 * static_cast<double>(g_vrrMargin.load(std::memory_order_acquire));
    }
  }
  return state;
}

bool vrr_wanted() noexcept { return resolve_vrr().active; }

double vrr_limit_fps() noexcept { return resolve_vrr().limitFps; }

void vrr_set_surface_caps(bool immediateSupported, bool d3d12Backend) noexcept {
  g_vrrSurfaceImmediate.store(immediateSupported, std::memory_order_release);
  g_vrrD3D12.store(d3d12Backend, std::memory_order_release);
  g_vrrCapsKnown.store(true, std::memory_order_release);
}

void vrr_note_applied(bool vrrPresentMode) noexcept {
  g_vrrApplied.store(vrrPresentMode, std::memory_order_release);
  g_vrrRequested.store(vrrPresentMode, std::memory_order_release);
}

void vrr_update() noexcept {
  if (!g_vrrCapsKnown.load(std::memory_order_acquire)) {
    return;
  }
  const VrrState state = resolve_vrr();
  g_vrrMinIntervalNanos.store(state.limiterActive ? static_cast<uint64_t>(1.0e9 / state.floorFps) : 0,
                              std::memory_order_release);
  if (state.active != g_vrrApplied.load(std::memory_order_acquire) &&
      g_vrrRequested.exchange(state.active, std::memory_order_acq_rel) != state.active) {
    // The swapchain's present mode (and ALLOW_TEARING flag) depends on it; rebuild at the next frame boundary.
    window::push_custom_event(window::CustomEvent::RefreshSurface);
  }
}

void pace_present(uint64_t dueNanos) noexcept {
  const uint64_t minInterval = g_vrrMinIntervalNanos.load(std::memory_order_acquire);
  uint64_t target = dueNanos;
  if (minInterval != 0 && g_paceLastNanos != 0) {
    target = std::max(target, g_paceLastNanos + minInterval);
  }
  wait_until(target);
  if (minInterval == 0) {
    g_paceLastNanos = 0;
    return;
  }
  // Stamp the actual wake time so the minimum spacing holds between the presents the display sees.
  g_paceLastNanos = now_nanos();
}

uint64_t guest_write_generation(const void* hostPtr, size_t size) noexcept {
  const auto callback = g_guestWriteGeneration.load(std::memory_order_acquire);
  return callback != nullptr ? callback(hostPtr, size) : AURORA_GUEST_WRITE_UNTRACKED;
}

void notify_guest_write(const void* hostPtr, size_t size) noexcept {
  if (const auto callback = g_guestWriteNotify.load(std::memory_order_acquire)) {
    callback(hostPtr, size);
  }
}
} // namespace aurora::gfx::presentation

namespace {
std::atomic_bool g_skipUnreadyPipelines = true;
} // namespace

void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback) {
  aurora::gfx::render_worker::set_producer_wait_callback(callback);
}

void aurora_wait_for_frame_worker() { aurora::gfx::render_worker::synchronize(); }

bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros) {
  return aurora::gfx::render_worker::synchronize_for(std::chrono::microseconds{timeoutMicros});
}

void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos) {
  using namespace aurora::gfx::presentation;
  g_scheduleBaseNanos.store(baseNanos, std::memory_order_release);
  g_scheduleIntervalNanos.store(intervalNanos, std::memory_order_release);
  if (intervalNanos != 0) {
    g_nativeIntervalNanos.store(intervalNanos, std::memory_order_relaxed);
  }
}

bool aurora_display_shows_interpolation() {
  using namespace aurora::gfx::presentation;
  return display_shows_interpolation(g_nativeIntervalNanos.load(std::memory_order_relaxed));
}

void aurora_report_producer_paced(bool paced) {
  aurora::gfx::presentation::g_producerPaced.store(paced, std::memory_order_release);
  aurora::gx::report_producer_paced(paced);
}

void aurora_get_present_timing(AuroraPresentTiming* timing) {
  if (timing != nullptr) {
    *timing = aurora::gfx::presentation::snapshot_timing();
  }
}

void aurora_set_skip_unready_pipelines(bool enabled) {
  g_skipUnreadyPipelines.store(enabled, std::memory_order_release);
  AuroraSetDrawsBlocking(!enabled);
}

bool aurora_get_skip_unready_pipelines() { return g_skipUnreadyPipelines.load(std::memory_order_acquire); }

uint32_t aurora_get_queued_pipeline_count() { return aurora_get_stats()->queuedPipelines; }

void aurora_set_disable_copy_filter(bool disabled) { aurora::g_config.disableCopyFilter = disabled; }

bool aurora_get_disable_copy_filter() { return aurora::g_config.disableCopyFilter; }

void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify) {
  aurora::gfx::presentation::g_guestWriteGeneration.store(generation, std::memory_order_release);
  aurora::gfx::presentation::g_guestWriteNotify.store(notify, std::memory_order_release);
}

void aurora_set_frame_rate(AuroraFrameRateMode mode, uint32_t fixedFps, uint32_t capFps) {
  aurora::gx::set_frame_rate(mode, fixedFps, capFps);
}

void aurora_get_frame_rate(AuroraFrameRateMode* mode, uint32_t* fixedFps, uint32_t* capFps) {
  AuroraFrameRateMode m = AURORA_FRAME_RATE_OFF;
  uint32_t fixed = 0;
  uint32_t cap = 0;
  aurora::gx::get_frame_rate(m, fixed, cap);
  if (mode != nullptr) {
    *mode = m;
  }
  if (fixedFps != nullptr) {
    *fixedFps = fixed;
  }
  if (capFps != nullptr) {
    *capFps = cap;
  }
}

void aurora_set_vrr(AuroraVrrMode mode, uint32_t marginFps, bool limiter) {
  using namespace aurora::gfx::presentation;
  g_vrrMode.store(mode == AURORA_VRR_ON || mode == AURORA_VRR_OFF ? mode : AURORA_VRR_AUTO, std::memory_order_release);
  g_vrrMargin.store(std::min<uint32_t>(marginFps, 30), std::memory_order_release);
  g_vrrLimiter.store(limiter, std::memory_order_release);
  vrr_update();
}

void aurora_get_vrr_status(AuroraVrrStatus* status) {
  using namespace aurora::gfx::presentation;
  if (status == nullptr) {
    return;
  }
  const VrrState state = resolve_vrr();
  AuroraFrameInterpolationDiagnostics diagnostics{};
  aurora_get_frame_interpolation_diagnostics(&diagnostics);
  const char* mode = aurora::webgpu::present_mode_name();
  *status = AuroraVrrStatus{
      .mode = g_vrrMode.load(std::memory_order_acquire),
      .marginFps = g_vrrMargin.load(std::memory_order_acquire),
      .limiterEnabled = g_vrrLimiter.load(std::memory_order_acquire),
      .tearingSupported = state.tearingSupported,
      .active = state.active,
      .swapchainTearing = std::string_view(mode) == "Immediate",
      .limiterActive = state.limiterActive,
      .refreshHz = state.refreshHz,
      .limitFps = state.limitFps,
      .plannedFps = static_cast<double>(diagnostics.plannedFps),
      .presentMode = mode,
  };
}

void aurora_set_frame_interpolation_fps(uint32_t targetFps) { aurora::gx::set_frame_interpolation_fps(targetFps); }

uint32_t aurora_get_frame_interpolation_fps() { return aurora::gx::frame_interpolation_fps(); }

void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics) {
  if (diagnostics != nullptr) {
    aurora::gx::get_frame_interpolation_diagnostics(*diagnostics);
  }
}

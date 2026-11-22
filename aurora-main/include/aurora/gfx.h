#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stddef.h"
#include "stdint.h"
#endif

#if !defined(NDEBUG) && !defined(AURORA_GFX_DEBUG_GROUPS)
#define AURORA_GFX_DEBUG_GROUPS
#endif

void push_debug_group(const char* label);
void pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
  uint32_t presentedFrameCount;
  uint32_t interpolatedFrameCount;
} AuroraStats;

typedef struct {
  uint64_t totalPresentCount;
  uint32_t sampleCount;
  double framesPerSecond;
  double averageFrameTimeMs;
  double p95FrameTimeMs;
  double jitterMs;
  // framesPerSecond with duplicated presentation slots scaled out: the rate of frames that
  // carried new motion.
  double effectiveFramesPerSecond;
  // Present-to-present interval distribution over the same one-second window.
  double p50FrameTimeMs;
  double p99FrameTimeMs;
  double maxFrameTimeMs;
} AuroraPresentTiming;

const AuroraStats* aurora_get_stats();
float aurora_get_fps();
void aurora_get_present_timing(AuroraPresentTiming* timing);
void aurora_get_present_intervals(float* milliseconds, uint32_t count);

// Interpolation health: the per-frame fields describe the last frame, the counters accumulate
// since it was configured.
typedef struct {
  uint32_t targetFps;
  uint32_t targetSamples;
  uint32_t activeSamples;
  uint32_t candidates;
  uint32_t matchable;
  uint32_t matches;
  uint32_t eligible;
  uint32_t replaySafe;
  uint64_t framesSealed;
  uint64_t framesLowMatch;
  uint64_t framesReplayUnsafe;
  uint64_t slotReductions;
  uint64_t lateSealDrops;
  // Frame-rate plan: the mode (AuroraFrameRateMode), the target the planner is currently
  // using in Hz (after the display, VSync, slot-budget and cap limits) and the last frame's
  // present points (interpolated slots plus the native frame when it is shown).
  uint32_t frameRateMode;
  float plannedFps;
  uint32_t plannedPresents;
} AuroraFrameInterpolationDiagnostics;

void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics);

// Generates transform-interpolated perspective frames between consecutive 60 Hz logical frames.
// Guest simulation, VI retrace, time base and audio stay at the original rate; only the number of
// presented frames per guest frame changes.
typedef enum {
  AURORA_FRAME_RATE_OFF = 0,      // original pacing: one present per guest frame
  AURORA_FRAME_RATE_FIXED = 1,    // present at `fixedFps` (61..capFps), e.g. 90/120/144/165/180/240
  AURORA_FRAME_RATE_DISPLAY = 2,  // present at the display refresh rate
  AURORA_FRAME_RATE_UNCAPPED = 3, // as fast as the GPU allows (non-blocking present, up to capFps)
} AuroraFrameRateMode;

// `fixedFps` is only used by AURORA_FRAME_RATE_FIXED. `capFps` is the hard safety cap for every
// mode (clamped to 60..1000; 0 keeps 1000).
void aurora_set_frame_rate(AuroraFrameRateMode mode, uint32_t fixedFps, uint32_t capFps);
void aurora_get_frame_rate(AuroraFrameRateMode* mode, uint32_t* fixedFps, uint32_t* capFps);

// Legacy form of the above: 0 is off, anything above 60 a fixed target.
void aurora_set_frame_interpolation_fps(uint32_t targetFps);
// Non-zero while any frame-rate mode above the original pacing is active (the fixed target, or
// the cap for the display/uncapped modes).
uint32_t aurora_get_frame_interpolation_fps();

// Variable refresh rate (NVIDIA G-SYNC, G-SYNC Compatible / FreeSync, Windows VRR).
// A VRR display needs presents that are not synchronized to a fixed vblank, so with VRR active the
// swapchain uses the tearing-allowed present mode (D3D12: DXGI flip model with
// DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING and Present(0, DXGI_PRESENT_ALLOW_TEARING)), VSync is bypassed and
// presents are limited to (refresh - marginFps) so frames stay inside the display's VRR window.
// Guest timing is unaffected; only the host present cadence changes.
typedef enum {
  AURORA_VRR_AUTO = 0, // on when supported, the display is faster than 60 Hz and the frame rate is not Original
  AURORA_VRR_ON = 1,   // on whenever supported
  AURORA_VRR_OFF = 2,
} AuroraVrrMode;

// `marginFps` is clamped to 0..30. `limiter` false disables the (refresh - margin) limiter; the
// frame-rate mode then runs unlimited in the tearing-allowed present mode.
void aurora_set_vrr(AuroraVrrMode mode, uint32_t marginFps, bool limiter);

typedef struct {
  uint32_t mode;           // AuroraVrrMode as configured
  uint32_t marginFps;
  bool limiterEnabled;     // as configured
  bool tearingSupported;   // the swapchain can allow tearing (DXGI query + surface present modes)
  bool active;             // the VRR present path is wanted now
  bool swapchainTearing;   // the live swapchain was created in the tearing-allowed mode
  bool limiterActive;
  double refreshHz;        // current refresh of the window's display (0 unknown)
  double limitFps;         // limiter target (0 when inactive)
  double plannedFps;       // frame-rate planner target
  const char* presentMode; // "Fifo", "Mailbox", "Immediate", ... of the live swapchain
} AuroraVrrStatus;

void aurora_get_vrr_status(AuroraVrrStatus* status);

// When enabled, draws whose pipeline is still compiling are skipped instead of stalling.
void aurora_set_skip_unready_pipelines(bool enabled);
bool aurora_get_skip_unready_pipelines();
uint32_t aurora_get_queued_pipeline_count();

// Controls whether display copies bypass the vertical copy filter.
typedef enum AuroraAtmosphere {
  AURORA_ATMOSPHERE_ORIGINAL,
  AURORA_ATMOSPHERE_UNDERWATER,
  AURORA_ATMOSPHERE_DUST
} AuroraAtmosphere;
void aurora_set_atmosphere_type(AuroraAtmosphere type);
void aurora_set_ambient_occlusion(bool enabled);
void aurora_set_global_illumination(bool enabled);
void aurora_set_ray_traced_gi(bool enabled);
void aurora_set_retain_culled_gi(bool enabled);
void aurora_set_strong_bounce(bool enabled);
void aurora_set_projected_caustics(bool enabled);
void aurora_set_surface_highlights(bool enabled);
typedef struct AuroraSceneLight {
  float cameraUp[4], combat[4], shadow[4], shadowNormal[4];
} AuroraSceneLight;
unsigned aurora_scene_effect_mask(void);
void aurora_update_scene_light(const AuroraSceneLight* scene);
void aurora_read_scene_light(AuroraSceneLight* scene);
void aurora_set_probe_materials(bool enabled);
void aurora_set_combat_light(bool enabled);
void aurora_set_contact_shadow(bool enabled);
void aurora_set_water_frame(uint32_t frame);
void aurora_set_stage_atmosphere(bool enabled);
void aurora_set_pixel_lighting(bool enabled);
bool aurora_enhanced_bloom_enabled(void);
bool aurora_ambient_occlusion_enabled(void);
bool aurora_global_illumination_enabled(void);
bool aurora_ray_traced_gi_enabled(void);
bool aurora_retain_culled_gi_enabled(void);
typedef bool (*AuroraRtCameraProvider)(float* worldToView, uint32_t* scene);
void aurora_set_rt_camera_provider(AuroraRtCameraProvider provider);
bool aurora_strong_bounce_enabled(void);
void aurora_set_enhanced_bloom(bool enabled);
void aurora_set_disable_copy_filter(bool disabled);
bool aurora_get_disable_copy_filter();

// Guest-RAM write tracking. `generation` changes whenever guest RAM covering a host range was
// written (or returns AURORA_GUEST_WRITE_UNTRACKED); `notify` reports writes aurora made itself.
#define AURORA_GUEST_WRITE_UNTRACKED UINT64_MAX
typedef uint64_t (*AuroraGuestWriteGenerationCallback)(const void* hostPtr, size_t size);
typedef void (*AuroraGuestWriteNotifyCallback)(const void* hostPtr, size_t size);
void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify);

void aurora_enable_vsync(bool enabled);

#ifdef __cplusplus
}
#endif

#endif

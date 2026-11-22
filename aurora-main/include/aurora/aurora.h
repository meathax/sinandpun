#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  SAMPLER_BILINEAR,
  SAMPLER_AREA,
  SAMPLER_INTEGER,
} AuroraSampler;

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef enum {
  AURORA_DISPLAY_MODE_WINDOWED,
  AURORA_DISPLAY_MODE_BORDERLESS,
  AURORA_DISPLAY_MODE_EXCLUSIVE,
} AuroraDisplayMode;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

#define MEM1_DEFAULT_SIZE (24 * 1024 * 1024)
#define MEM2_DEFAULT_SIZE (64 * 1024 * 1024)
#define ARAM_DEFAULT_SIZE (16 * 1024 * 1024)

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  bool vsync;
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureDumps;
  bool allowTextureReplacements;
  bool generateReplacementMips;
  bool allowCpuAdapter;
  bool disableCopyFilter;
  // When false, Aurora centers the first window. When true, windowPosX/Y are restored verbatim,
  // including negative coordinates on monitors left of or above the primary display.
  bool hasWindowPosition;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;

  // Keeps the window hidden for the whole run without pausing rendering.
  bool startHidden;
} AuroraConfig;

typedef enum {
  AURORA_INITIALIZATION_SUCCESS = 0,
  AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE = 1,
} AuroraInitializationStatus;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
  AuroraInitializationStatus initializationStatus;
  // On failure, owned by SDL on the calling thread. Copy before another SDL call.
  const char* initializationError;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
void aurora_end_frame();

// aurora_begin_frame/aurora_end_frame split for a GX producer running on its own thread. The
// window halves stay on the thread that owns the window and ImGui; the GX halves run on the
// producer thread.
typedef struct AuroraImguiFrame AuroraImguiFrame;
bool aurora_begin_frame_window();
bool aurora_begin_frame_gx();
AuroraImguiFrame* aurora_end_frame_window();
// Takes ownership of frame.
void aurora_end_frame_gx(AuroraImguiFrame* frame);
// Frees a frame that will not be ended.
void aurora_release_imgui_frame(AuroraImguiFrame* frame);

typedef void (*AuroraFrameWorkerWaitCallback)();
// Called from the producer thread at bounded intervals while Aurora blocks on rendering.
// The callback must not enter Aurora.
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback);
// Waits until every frame handed to the renderer has been encoded.
void aurora_wait_for_frame_worker();
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros);
// Absolute schedule for the next frame, on steady_clock: baseNanos anchors the group and
// intervalNanos is the period, so slot k of N+1 presents at base + k*interval/(N+1). Zeros clear it.
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos);
// Reports whether the frame about to be ended met its display boundary. Interpolation sizes its
// slot group from this, backing off after misses. Only paced presents may report.
void aurora_report_producer_paced(bool paced);

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_display_mode(AuroraDisplayMode mode);
AuroraDisplayMode aurora_get_display_mode();
// Refresh rate of the display showing the window in Hz, or 0 when unknown.
double aurora_get_display_refresh_rate();
// False when the display is too slow to show interpolated frames, which are then dropped.
bool aurora_display_shows_interpolation();

typedef enum {
  AURORA_METALFX_DISABLED,
  AURORA_METALFX_UNSUPPORTED,
  AURORA_METALFX_NOT_UPSCALING,
  AURORA_METALFX_ACTIVE,
  AURORA_METALFX_ERROR,
} AuroraMetalFXStatus;

// MetalFX only applies when both source dimensions are smaller than the aspect-fitted output.
void aurora_set_metalfx_spatial(bool enabled);
bool aurora_get_metalfx_spatial();
bool aurora_is_metalfx_spatial_supported();
AuroraMetalFXStatus aurora_get_metalfx_status();

// Single upscaler selector shared by FSR and DLSS. Only OFF and FSR1 are implemented in aurora itself;
// FSR3 selects the AMD temporal provider. Both temporal modes fall back to spatial scaling when unavailable.
typedef enum {
  AURORA_UPSCALER_OFF = 0,
  AURORA_UPSCALER_FSR1 = 1,
  AURORA_UPSCALER_FSR3 = 2,
  AURORA_UPSCALER_DLSS = 3,
} AuroraUpscaler;

typedef struct {
  AuroraUpscaler requested;
  AuroraUpscaler active; // what the last presented frame really used (OFF when it fell back to plain resampling)
  float scaleFactor;
  float sharpness;       // 0..1
  uint32_t inputWidth;   // size of the image handed to the upscaler (the game's internal resolution)
  uint32_t inputHeight;
  uint32_t outputWidth;  // size of the upscaled image on the swapchain
  uint32_t outputHeight;
  bool sharpened;        // RCAS ran on the last frame
} AuroraUpscaleInfo;

// scaleFactor is the per-axis output/internal ratio (1.0 native, 1.3, 1.5, 1.7, 2.0, 3.0). With an upscaler on
// and scaleFactor > 1 the game renders at (aspect-fitted window size / scaleFactor) and this overrides the
// resolution multiplier. sharpness 0 disables RCAS. Safe to call from the producer thread at a frame boundary.
void aurora_set_upscaler(AuroraUpscaler mode, float scaleFactor, float sharpness);
void aurora_get_upscale_info(AuroraUpscaleInfo* info);

// NVIDIA DLSS (AURORA_UPSCALER_DLSS). DLSS needs Windows, the Direct3D 12 backend, an NVIDIA RTX GPU and the DLLs
// next to the executable (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll). Detection runs on a
// background thread shortly after start-up: poll until the state leaves CHECKING. `reason` says what is missing
// when the state is UNAVAILABLE or FAILED. With AURORA_UPSCALER_DLSS requested, scaleFactor picks the mode (3.0
// Ultra Performance, 2.0 Performance, 1.72 Balanced, 1.5 Quality, 1.0 DLAA) and DLSS replaces the FSR1 path as soon
// as it is available (until then, or when it is not, the FSR1 path runs at the same scale). sharpness 0..1 drives a
// small contrast-adaptive sharpening pass after DLSS (0 = none).
typedef enum {
  AURORA_DLSS_NOT_STARTED = 0,
  AURORA_DLSS_CHECKING,
  AURORA_DLSS_AVAILABLE,
  AURORA_DLSS_UNAVAILABLE,
  AURORA_DLSS_FAILED,
} AuroraDlssState;

typedef struct {
  AuroraDlssState state;
  char reason[192];
  char version[96]; // "Streamline 2.14.16 / DLSS 310.9.16" once available
} AuroraDlssStatus;

void aurora_get_dlss_status(AuroraDlssStatus* status);
// The game has no velocity buffer: true uses motion vectors estimated from the images (optical flow), false sends zero
// vectors (DLSS then rejects history by itself). Takes effect on the next frame.
bool aurora_reflex_active(void);
void aurora_set_reflex(bool enabled);
void aurora_set_dlss_motion_vectors(bool enabled);
// Saves the final game image (upscaled, before the overlay is drawn) of the next presented frame as a PNG.
void aurora_capture_present_image(const char* pngPath);

void aurora_set_resampler(AuroraSampler sampler);
/** Sets the clock timescale. Default 1.0f. 0.0f is paused. Range 0.0f-16.0f. */
void aurora_set_timescale(float scale);

SDL_Window* aurora_get_window();
AuroraWindowSize aurora_get_window_size();
/** Picture rectangle of the last presented frame as fractions of the window client area (letterbox aware). */
bool aurora_get_present_viewport(float* left, float* top, float* width, float* height);

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);
float aurora_get_timescale();

#ifdef __cplusplus
}
#endif

#endif

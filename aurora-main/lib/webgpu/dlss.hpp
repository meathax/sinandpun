#pragma once

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstdint>
#include <string>

// NVIDIA DLSS Super Resolution / DLAA backend (Windows + D3D12 backend + NVIDIA RTX only).
//
// Streamline (sl.interposer.dll, loaded at run time, nothing is linked) is used in manual-hooking mode: Dawn owns
// the D3D12 device and the swap chain, so no Streamline proxy is involved. DLSS runs on a *second* D3D12 device
// created on the same adapter; the colour/depth/motion inputs and the output are D3D12 shared-handle textures
// imported into Dawn (SharedTextureMemoryDXGISharedHandle) and ordering between the two devices goes through
// shared fences (SharedFenceDXGISharedHandle). See docs/DLSS-NOTES.md for the full rationale and the probes in
// tools/dlss_probe/ that validated each step.
//
// Threading: start()/status()/shutdown() from any thread; advance_jitter()/dispatch()/after_submit() only on the
// render worker (the thread that encodes and presents frames).
namespace aurora::webgpu::dlss {

enum class Mode : uint32_t { Off = 0, UltraPerformance, Performance, Balanced, Quality, DLAA };

const char* mode_name(Mode mode) noexcept;
// Per-axis output / render ratio of a mode (3.0, 2.0, 1/0.58, 1.5, 1.0); 1.0 for Off.
float mode_scale_factor(Mode mode) noexcept;
// Closest mode for a per-axis scale factor (<= 1.05 -> DLAA).
Mode mode_for_scale_factor(float scaleFactor) noexcept;

// 0 = info, 1 = warning, 2 = error. Without a sink messages go to stderr.
using LogFn = void (*)(int level, const char* message);
void set_log(LogFn fn) noexcept;
void log(int level, const char* message) noexcept;

enum class State : uint32_t { NotStarted = 0, Probing, Ready, Unavailable, Failed };

struct Status {
  State state = State::NotStarted;
  // Why DLSS cannot be used (empty when Ready). Suitable for the settings UI.
  std::string reason;
  // "Streamline 2.14.16 / DLSS 310.9.16" once Ready.
  std::string version;
};

// Begins asynchronous capability detection and Streamline start-up (about 2 s, off the calling thread). `dllDir`
// is the folder holding sl.interposer.dll, sl.common.dll, sl.dlss.dll and nvngx_dlss.dll (empty = the executable's
// folder). `deviceHasSharing` is whether the Dawn device was created with SharedTextureMemoryDXGISharedHandle
// and SharedFenceDXGISharedHandle. Does nothing on non-D3D12 backends or non-NVIDIA adapters (state becomes
// Unavailable with the reason).
void start(const wgpu::Device& device, const wgpu::AdapterInfo& adapterInfo, wgpu::BackendType backend,
           bool deviceHasSharing, const std::string& dllDir);
Status status();
// Cheap (one atomic load); true once start() finished and DLSS can run.
bool ready() noexcept;
bool fsr_ready() noexcept;
// Releases Streamline and the second device. Call before the Dawn device is destroyed.
void shutdown() noexcept;

// ---- jitter (render worker) ---------------------------------------------------------------------------------
// Chooses the sub-pixel offset of the next scene encode (native frame or interpolated slot) from the
// DLSS-recommended Halton(2,3) sequence. The offset (in render pixels, |x|,|y| < 0.5) is applied by the viewport.
void advance_jitter(bool interpolatedSlot) noexcept;
// Re-selects the native frame's offset (call before presenting the native frame after its slots).
void restore_native_jitter() noexcept;
// Offset to add to the viewport origin; (0, 0) while no DLSS mode is active.
void current_jitter(float& x, float& y) noexcept;
// Turns jitter production on or off (on while a DLSS mode other than Off is requested and Ready).
void set_jitter_enabled(bool enabled, uint32_t renderWidth, uint32_t outputWidth) noexcept;

// ---- per present (render worker) ---------------------------------------------------------------------------
struct DispatchParams {
  wgpu::TextureView color;   // render-resolution game image (float-sampleable)
  uint32_t renderWidth = 0, renderHeight = 0;
  uint32_t outputWidth = 0, outputHeight = 0;
  Mode mode = Mode::Quality;
  float sharpness = 0.f;     // 0..1, post-DLSS contrast-adaptive sharpening (0 = off)
  wgpu::TextureView depth;
  std::array<float,4> depthRect{};
  bool reversedDepth = false;
  bool fsr = false;
  bool reset = false;        // drop temporal history (scene cut, mode change, ...)
};

struct Output {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size{};
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  wgpu::Sampler sampler;
};

// Encodes the input preparation (copy, motion vectors) into `encoder`, submits it, runs DLSS on the second device
// and replaces `encoder` with a fresh one that is ordered after the result. On success `output` is valid until the
// next dispatch(); false means "not used this frame" (caller falls back to its normal upscaler) and the encoder is
// left untouched whenever possible.
bool dispatch(wgpu::CommandEncoder& encoder, const DispatchParams& params, Output& output);
// Call after the command buffer built from the encoder passed back by dispatch() was submitted.
void set_fg(bool enabled) noexcept;
bool fg_marker(uint64_t frame, unsigned marker) noexcept;
void fg_render_frame(uint64_t frame) noexcept;
bool fg_sleep(uint64_t frame) noexcept;
bool fg_active() noexcept;
void capture_fg(wgpu::CommandEncoder& encoder, const wgpu::Texture& source, void* window) noexcept;
wgpu::Texture fg_target(unsigned width, unsigned height, wgpu::TextureFormat format);
void present_fg(bool vsync) noexcept;
void after_submit() noexcept;

// A/B switch: with false the motion vectors are all zero (DLSS then relies on its own history rectification).
void set_motion_vectors_enabled(bool enabled) noexcept;
// Test hook (probes): every pixel gets this motion vector (render pixels, previous minus current).
void debug_constant_motion(bool enabled, float x, float y) noexcept;

} // namespace aurora::webgpu::dlss

// Built instead of dlss.cpp / motion_flow.cpp on non-Windows targets and when the Streamline headers are not staged
// in third_party/nvidia/streamline/include (see docs/DLSS-NOTES.md). DLSS is simply reported as unavailable.
#include "dlss.hpp"

#include <cstdio>

namespace aurora::webgpu::dlss {

void set_fg(bool) noexcept {}
bool fg_marker(uint64_t, unsigned) noexcept { return false; }
void fg_render_frame(uint64_t) noexcept {}
bool fg_sleep(uint64_t) noexcept { return false; }
bool fg_active() noexcept { return false; }
void capture_fg(wgpu::CommandEncoder&, const wgpu::Texture&, void*) noexcept {}
wgpu::Texture fg_target(unsigned, unsigned, wgpu::TextureFormat) { return {}; }
void present_fg(bool) noexcept {}

const char* mode_name(Mode mode) noexcept {
  switch (mode) {
  case Mode::UltraPerformance: return "Ultra Performance";
  case Mode::Performance: return "Performance";
  case Mode::Balanced: return "Balanced";
  case Mode::Quality: return "Quality";
  case Mode::DLAA: return "DLAA";
  default: return "Off";
  }
}

float mode_scale_factor(Mode mode) noexcept {
  switch (mode) {
  case Mode::UltraPerformance: return 3.0f;
  case Mode::Performance: return 2.0f;
  case Mode::Balanced: return 1.0f / 0.58f;
  case Mode::Quality: return 1.5f;
  default: return 1.0f;
  }
}

Mode mode_for_scale_factor(float s) noexcept {
  if (s <= 1.05f) return Mode::DLAA;
  if (s < 1.6f) return Mode::Quality;
  if (s < 1.86f) return Mode::Balanced;
  if (s < 2.5f) return Mode::Performance;
  return Mode::UltraPerformance;
}

namespace {
LogFn g_log = nullptr;
}

void set_log(LogFn fn) noexcept { g_log = fn; }
void log(int level, const char* message) noexcept {
  if (g_log) g_log(level, message);
  else fprintf(stderr, "[dlss %d] %s\n", level, message);
}

void start(const wgpu::Device&, const wgpu::AdapterInfo&, wgpu::BackendType, bool, const std::string&) {}

Status status() {
  Status s;
  s.state = State::Unavailable;
#if defined(_WIN32)
  s.reason = "this build has no NVIDIA DLSS support (Streamline headers were not available at build time)";
#else
  s.reason = "NVIDIA DLSS needs Windows and the Direct3D 12 backend";
#endif
  return s;
}

bool fsr_ready() noexcept { return false; }
bool ready() noexcept { return false; }
void shutdown() noexcept {}
void advance_jitter(bool) noexcept {}
void restore_native_jitter() noexcept {}
void current_jitter(float& x, float& y) noexcept { x = y = 0.f; }
void set_jitter_enabled(bool, uint32_t, uint32_t) noexcept {}
bool dispatch(wgpu::CommandEncoder&, const DispatchParams&, Output&) { return false; }
void after_submit() noexcept {}
void set_motion_vectors_enabled(bool) noexcept {}
void debug_constant_motion(bool, float, float) noexcept {}

} // namespace aurora::webgpu::dlss

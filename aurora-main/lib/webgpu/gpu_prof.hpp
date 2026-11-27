#pragma once

#include <string_view>

#include <webgpu/webgpu_cpp.h>

namespace aurora::webgpu::gpu_prof {

void initialize();
void shutdown();

// idleRender: the uncapped mode's retained-scene re-render. SIN_GPU_PROF=idle profiles only those, otherwise only
// native (guest) frames are profiled.
void frame_begin(const wgpu::CommandEncoder& encoder, bool idleRender = false);
void frame_end(const wgpu::CommandEncoder& encoder);
void after_submit();
const wgpu::PassTimestampWrites* pass_writes(std::string_view name);

class Zone {
public:
  Zone(const wgpu::CommandEncoder& encoder, std::string_view name);
  ~Zone();
  Zone(const Zone&) = delete;
  Zone& operator=(const Zone&) = delete;

private:
  const wgpu::CommandEncoder* m_encoder = nullptr;
  uint32_t m_endQuery = 0;
};

} // namespace aurora::webgpu::gpu_prof

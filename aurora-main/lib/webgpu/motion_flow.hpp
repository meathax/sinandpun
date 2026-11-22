#pragma once

#include <webgpu/webgpu_cpp.h>

#include <cstdint>

// Screen-space motion vectors for temporal upscalers.
//
// The GameCube/Wii GX pipeline has no velocity buffer and the draw stream carries no camera matrices, so motion
// is estimated from the image itself: a 4-level luma pyramid of the current and the previous presented frame,
// hierarchical block matching (SAD, coarse search +-6 px at 1/8 resolution, +-1..2 px refinement per finer level,
// parabolic sub-pixel fit at full resolution) and one render pass that writes the result into an RG16Float
// target. The vector is "previous position minus current position" in render pixels (Streamline / DLSS
// convention with mvecScale = 1 / size).
//
// Everything runs on the Dawn device passed to init(); no aurora globals are used so the module can also be
// driven by the standalone probes under tools/dlss_probe/.
namespace aurora::webgpu::dlss {

class MotionFlow {
public:
  MotionFlow() = default;
  ~MotionFlow() = default;
  MotionFlow(const MotionFlow&) = delete;
  MotionFlow& operator=(const MotionFlow&) = delete;

  bool init(const wgpu::Device& device);
  // (Re)creates the pyramids for a render size. Drops history (the next encode is treated as a reset).
  bool resize(uint32_t width, uint32_t height);
  void invalidate_history() noexcept { m_haveHistory = false; }

  // Encodes the flow computation for `color` (current frame, float-sampleable, size == resize() size) and writes
  // the vectors into `target` (RG16Float, RenderAttachment). With reset (or no history) the target is cleared to
  // zero motion and only the luma pyramid is built.
  // `correction` is the change of the sub-pixel jitter between the previous and this frame (jitterCurrent -
  // jitterPrevious, render pixels): the output pass removes it from the measured displacement.
  // The parameters go through queue writes, so submit the encoder before the next encode() call.
  bool encode(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color, const wgpu::TextureView& target,
              bool reset, float correctionX = 0.f, float correctionY = 0.f);

  bool ready() const noexcept { return m_ready; }
  uint32_t width() const noexcept { return m_width; }
  uint32_t height() const noexcept { return m_height; }

private:
  static constexpr uint32_t kLevels = 4;
  struct LevelParams {
    uint32_t width, height;
    int32_t radius, patch;
    uint32_t hasPredictor, subPixel;
    float lambda;
    uint32_t padding;
  };

  wgpu::Device m_device;
  wgpu::Queue m_queue;
  bool m_ready = false;
  bool m_failed = false;
  bool m_haveHistory = false;
  uint32_t m_width = 0, m_height = 0;
  uint32_t m_parity = 0;

  wgpu::BindGroupLayout m_lumaLayout, m_downLayout, m_flowLayout, m_mvLayout;
  wgpu::ComputePipeline m_lumaPipeline, m_downPipeline, m_flowPipeline;
  wgpu::RenderPipeline m_mvPipeline;

  uint32_t m_levelW[kLevels]{}, m_levelH[kLevels]{};
  wgpu::Texture m_luma[2][kLevels];
  wgpu::TextureView m_lumaView[2][kLevels];
  wgpu::Texture m_flow[kLevels];
  wgpu::TextureView m_flowView[kLevels];
  wgpu::Texture m_dummy;
  wgpu::TextureView m_dummyView;
  wgpu::Buffer m_params[kLevels];
  wgpu::BindGroup m_downGroup[2][kLevels - 1];
  wgpu::BindGroup m_flowGroup[2][kLevels];
  wgpu::BindGroup m_mvGroup;
  wgpu::Buffer m_mvParams;
};

} // namespace aurora::webgpu::dlss

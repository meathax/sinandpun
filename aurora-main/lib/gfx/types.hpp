#pragma once

#include "hash.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <aurora/math.hpp>
#include <dolphin/gx/GXEnum.h>
#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace aurora::gfx {
namespace detail { struct RenderPass; }
struct PixelPerfectGuiPresent {
  webgpu::TextureWithSampler background;
  const detail::RenderPass* pass = nullptr;
  wgpu::TextureView depth;
  wgpu::TextureView temporalDepth;
  std::array<float,4> depthRect{};
  wgpu::TextureView velocity, hudVelocity;
};
using BindGroupRef = HashType;
using PipelineRef = HashType;
using SamplerRef = HashType;
using ShaderRef = HashType;

struct ClipRect {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;

  bool operator==(const ClipRect& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const ClipRect& rhs) const { return !(*this == rhs); }
};

using webgpu::Viewport;

struct TextureRef;
using TextureHandle = std::shared_ptr<TextureRef>;
using AfterSubmitCallback = std::function<void()>;
// Most interpolated slots staged per guest frame (so up to 16x the guest rate).
inline constexpr uint32_t MaxInterpolationSlots = 15;
// Present points of one guest frame. Interpolated slot k shows the state `weight[k]` of the way from
// the previous guest frame to the current one (0 < weight < 1) and goes out at
// anchor + interval * (weight - step), where `step` is the spacing of the present grid in guest-frame
// fractions (60 / target Hz). When `nativeShown` the native frame (weight 1) is presented last.
struct InterpolationPlan {
  uint32_t slots = 0;
  bool nativeShown = true;
  float step = 1.f;
  std::array<float, MaxInterpolationSlots> weight{};
};
// Re-encodes the frame for interpolated presentation slot `slot` into `cmd`.
struct InterpolationReplay {
    bool nativeUncapped = false;
    bool scenePresentOnly = false;
  uint32_t slots = 0;
  InterpolationPlan plan;
  std::function<bool(wgpu::CommandEncoder& cmd, uint32_t slot)> encode;
  std::function<void()> restoreBeforeNative;
};
using EndFrameCallback =
    std::function<void(wgpu::CommandEncoder&, std::vector<AfterSubmitCallback>, const InterpolationReplay&)>;
} // namespace aurora::gfx

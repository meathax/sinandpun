#include "upscale.hpp"

#include "../internal.hpp"
#include "dlss.hpp"

#include <SDL3/SDL_surface.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <webgpu/webgpu_cpp.h>

using namespace std::string_view_literals;

namespace aurora::webgpu::upscale {
namespace {
Module Log("aurora::upscale");

std::atomic<AuroraUpscaler> g_mode{AURORA_UPSCALER_OFF};
std::atomic<float> g_scaleFactor{1.f};
std::atomic<float> g_sharpness{0.f};

std::atomic<AuroraUpscaler> g_activeMode{AURORA_UPSCALER_OFF};
std::atomic<uint32_t> g_inW{0}, g_inH{0}, g_outW{0}, g_outH{0};
std::atomic<bool> g_sharpened{false};

// FSR 1.0 EASU and RCAS, ported from ffx_fsr1.h (FSR_EASU_F / FSR_RCAS_F, 32-bit path) of
//   https://github.com/GPUOpen-Effects/FidelityFX-FSR   (MIT, Copyright (c) 2021 Advanced Micro Devices, Inc.)
// The algorithm, constants and the approximate rcp/rsq bit tricks are unchanged; only the HLSL plumbing is
// replaced: gather4 becomes textureGather, the packed uint constants become plain floats in a uniform block,
// and the 0 * inf NaN hazards of the original RCAS limiter divisions are clamped (WGSL leaves NaN min/max
// results unspecified, HLSL does not).
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
// copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
constexpr std::string_view easuShader = R"(
struct EasuConstants {
    con0: vec4<f32>,
    con1: vec4<f32>,
    con2: vec4<f32>,
    con3: vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: EasuConstants;
@group(0) @binding(1) var smp: sampler;
@group(0) @binding(2) var tex: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4<f32> {
    var p = array<vec2<f32>, 3>(vec2<f32>(-1.0, 1.0), vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0));
    return vec4<f32>(p[idx], 0.0, 1.0);
}

fn rcp_lo(a: f32) -> f32 { return bitcast<f32>(0x7ef07ebbu - bitcast<u32>(a)); }
fn rsq_lo(a: f32) -> f32 { return bitcast<f32>(0x5f347d74u - (bitcast<u32>(a) >> 1u)); }

fn easu_tap(aC: ptr<function, vec3<f32>>, aW: ptr<function, f32>, off: vec2<f32>, dir: vec2<f32>, len: vec2<f32>,
            lob: f32, clp: f32, c: vec3<f32>) {
    var v = vec2<f32>(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x);
    v = v * len;
    var d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    var wB = 0.4 * d2 - 1.0;
    var wA = lob * d2 - 1.0;
    wB = wB * wB;
    wA = wA * wA;
    wB = 1.5625 * wB - 0.5625;
    let w = wB * wA;
    *aC = *aC + c * w;
    *aW = *aW + w;
}

fn easu_set(dir: ptr<function, vec2<f32>>, len: ptr<function, f32>, w: f32,
            lA: f32, lB: f32, lC: f32, lD: f32, lE: f32) {
    let dc = lD - lC;
    let cb = lC - lB;
    var lenX = max(abs(dc), abs(cb));
    lenX = rcp_lo(lenX);
    let dirX = lD - lB;
    (*dir).x = (*dir).x + dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX = lenX * lenX;
    *len = *len + lenX * w;
    let ec = lE - lC;
    let ca = lC - lA;
    var lenY = max(abs(ec), abs(ca));
    lenY = rcp_lo(lenY);
    let dirY = lE - lA;
    (*dir).y = (*dir).y + dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY = lenY * lenY;
    *len = *len + lenY * w;
}

@fragment
fn fs_main(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {
    let ip = floor(frag.xy);
    var pp = ip * u.con0.xy + u.con0.zw;
    let fp = floor(pp);
    pp = pp - fp;

    let p0 = fp * u.con1.xy + u.con1.zw;
    let p1 = p0 + u.con2.xy;
    let p2 = p0 + u.con2.zw;
    let p3 = p0 + u.con3.xy;
    let bczzR = textureGather(0, tex, smp, p0);
    let bczzG = textureGather(1, tex, smp, p0);
    let bczzB = textureGather(2, tex, smp, p0);
    let ijfeR = textureGather(0, tex, smp, p1);
    let ijfeG = textureGather(1, tex, smp, p1);
    let ijfeB = textureGather(2, tex, smp, p1);
    let klhgR = textureGather(0, tex, smp, p2);
    let klhgG = textureGather(1, tex, smp, p2);
    let klhgB = textureGather(2, tex, smp, p2);
    let zzonR = textureGather(0, tex, smp, p3);
    let zzonG = textureGather(1, tex, smp, p3);
    let zzonB = textureGather(2, tex, smp, p3);

    let bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    let ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    let klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    let zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    let bL = bczzL.x;
    let cL = bczzL.y;
    let iL = ijfeL.x;
    let jL = ijfeL.y;
    let fL = ijfeL.z;
    let eL = ijfeL.w;
    let kL = klhgL.x;
    let lL = klhgL.y;
    let hL = klhgL.z;
    let gL = klhgL.w;
    let oL = zzonL.z;
    let nL = zzonL.w;

    var dir = vec2<f32>(0.0, 0.0);
    var len = 0.0;
    easu_set(&dir, &len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easu_set(&dir, &len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easu_set(&dir, &len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easu_set(&dir, &len, pp.x * pp.y, gL, jL, kL, lL, oL);

    let dir2 = dir * dir;
    var dirR = dir2.x + dir2.y;
    let zro = dirR < (1.0 / 32768.0);
    dirR = rsq_lo(dirR);
    dirR = select(dirR, 1.0, zro);
    dir.x = select(dir.x, 1.0, zro);
    dir = dir * dirR;
    len = len * 0.5;
    len = len * len;
    let stretch = (dir.x * dir.x + dir.y * dir.y) * rcp_lo(max(abs(dir.x), abs(dir.y)));
    let len2 = vec2<f32>(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    let lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    let clp = rcp_lo(lob);

    let fC = vec3<f32>(ijfeR.z, ijfeG.z, ijfeB.z);
    let gC = vec3<f32>(klhgR.w, klhgG.w, klhgB.w);
    let jC = vec3<f32>(ijfeR.y, ijfeG.y, ijfeB.y);
    let kC = vec3<f32>(klhgR.x, klhgG.x, klhgB.x);
    let min4 = min(min(fC, min(gC, jC)), kC);
    let max4 = max(max(fC, max(gC, jC)), kC);

    var aC = vec3<f32>(0.0, 0.0, 0.0);
    var aW = 0.0;
    easu_tap(&aC, &aW, vec2<f32>(0.0, -1.0) - pp, dir, len2, lob, clp, vec3<f32>(bczzR.x, bczzG.x, bczzB.x)); // b
    easu_tap(&aC, &aW, vec2<f32>(1.0, -1.0) - pp, dir, len2, lob, clp, vec3<f32>(bczzR.y, bczzG.y, bczzB.y)); // c
    easu_tap(&aC, &aW, vec2<f32>(-1.0, 1.0) - pp, dir, len2, lob, clp, vec3<f32>(ijfeR.x, ijfeG.x, ijfeB.x)); // i
    easu_tap(&aC, &aW, vec2<f32>(0.0, 1.0) - pp, dir, len2, lob, clp, vec3<f32>(ijfeR.y, ijfeG.y, ijfeB.y));  // j
    easu_tap(&aC, &aW, vec2<f32>(0.0, 0.0) - pp, dir, len2, lob, clp, vec3<f32>(ijfeR.z, ijfeG.z, ijfeB.z));  // f
    easu_tap(&aC, &aW, vec2<f32>(-1.0, 0.0) - pp, dir, len2, lob, clp, vec3<f32>(ijfeR.w, ijfeG.w, ijfeB.w)); // e
    easu_tap(&aC, &aW, vec2<f32>(1.0, 1.0) - pp, dir, len2, lob, clp, vec3<f32>(klhgR.x, klhgG.x, klhgB.x));  // k
    easu_tap(&aC, &aW, vec2<f32>(2.0, 1.0) - pp, dir, len2, lob, clp, vec3<f32>(klhgR.y, klhgG.y, klhgB.y));  // l
    easu_tap(&aC, &aW, vec2<f32>(2.0, 0.0) - pp, dir, len2, lob, clp, vec3<f32>(klhgR.z, klhgG.z, klhgB.z));  // h
    easu_tap(&aC, &aW, vec2<f32>(1.0, 0.0) - pp, dir, len2, lob, clp, vec3<f32>(klhgR.w, klhgG.w, klhgB.w));  // g
    easu_tap(&aC, &aW, vec2<f32>(1.0, 2.0) - pp, dir, len2, lob, clp, vec3<f32>(zzonR.z, zzonG.z, zzonB.z));  // o
    easu_tap(&aC, &aW, vec2<f32>(0.0, 2.0) - pp, dir, len2, lob, clp, vec3<f32>(zzonR.w, zzonG.w, zzonB.w));  // n

    let pix = min(max4, max(min4, aC * (1.0 / aW)));
    return vec4<f32>(pix, 1.0);
}
)"sv;

constexpr std::string_view rcasShader = R"(
struct RcasConstants {
    con: vec4<f32>,
};

@group(0) @binding(0) var<uniform> u: RcasConstants;
@group(0) @binding(1) var tex: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4<f32> {
    var p = array<vec2<f32>, 3>(vec2<f32>(-1.0, 1.0), vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0));
    return vec4<f32>(p[idx], 0.0, 1.0);
}

fn load_px(p: vec2<i32>) -> vec3<f32> {
    let d = vec2<i32>(textureDimensions(tex));
    return textureLoad(tex, clamp(p, vec2<i32>(0, 0), d - vec2<i32>(1, 1)), 0).rgb;
}

fn rcp_med(a: f32) -> f32 {
    let b = bitcast<f32>(0x7ef19fffu - bitcast<u32>(a));
    return b * (-b * a + 2.0);
}

@fragment
fn fs_main(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {
    let sp = vec2<i32>(floor(frag.xy));
    let b = load_px(sp + vec2<i32>(0, -1));
    let d = load_px(sp + vec2<i32>(-1, 0));
    let e = load_px(sp);
    let f = load_px(sp + vec2<i32>(1, 0));
    let h = load_px(sp + vec2<i32>(0, 1));

    let mn4 = min(min(b, min(d, f)), h);
    let mx4 = max(max(b, max(d, f)), h);
    // Limiters. The original divides by (4 * mx4) and (4 * mn4 - 4), which are 0 for pure black / white
    // neighbourhoods where the numerators are 0 as well; the clamps keep that 0 * big instead of 0 * inf.
    let hitMin = min(mn4, e) * (1.0 / max(4.0 * mx4, vec3<f32>(1e-20)));
    let hitMax = (vec3<f32>(1.0) - max(mx4, e)) * (1.0 / min(4.0 * mn4 - vec3<f32>(4.0), vec3<f32>(-1e-20)));
    let lobeV = max(-hitMin, hitMax);
    let lobe = max(-(0.25 - (1.0 / 16.0)), min(max(lobeV.x, max(lobeV.y, lobeV.z)), 0.0)) * u.con.x;
    let rcpL = rcp_med(4.0 * lobe + 1.0);
    let pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    return vec4<f32>(pix, 1.0);
}
)"sv;

struct EasuConstants {
  float con0[4];
  float con1[4];
  float con2[4];
  float con3[4];
};
struct RcasConstants {
  float con[4];
};

// FsrEasuCon() with inputViewport == inputSize (no dynamic-resolution sub-rect).
EasuConstants make_easu_constants(float inW, float inH, float outW, float outH) noexcept {
  EasuConstants c{};
  c.con0[0] = inW / outW;
  c.con0[1] = inH / outH;
  c.con0[2] = 0.5f * inW / outW - 0.5f;
  c.con0[3] = 0.5f * inH / outH - 0.5f;
  c.con1[0] = 1.f / inW;
  c.con1[1] = 1.f / inH;
  c.con1[2] = 1.f / inW;
  c.con1[3] = -1.f / inH;
  c.con2[0] = -1.f / inW;
  c.con2[1] = 2.f / inH;
  c.con2[2] = 1.f / inW;
  c.con2[3] = 2.f / inH;
  c.con3[0] = 0.f;
  c.con3[1] = 4.f / inH;
  return c;
}

// UI sharpness in (0, 1] -> FsrRcasCon() stops: 1 is 0 stops (strongest), 0+ is 2 stops (weakest).
float rcas_linear_sharpness(float sharpness) noexcept {
  const float stops = 2.f * (1.f - std::clamp(sharpness, 0.f, 1.f));
  return std::exp2(-stops);
}

struct Pipelines {
  wgpu::BindGroupLayout easuLayout;
  wgpu::BindGroupLayout rcasLayout;
  wgpu::RenderPipeline easuToHalf;    // EASU -> RGBA16Float, RCAS follows
  wgpu::RenderPipeline easuToSurface; // EASU -> surface format, no sharpening
  wgpu::RenderPipeline rcas;
  wgpu::Sampler gatherSampler;
  wgpu::Buffer easuUniform;
  wgpu::Buffer rcasUniform;
  wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::Undefined;
  bool failed = false;
};

struct State {
  Pipelines pipelines;
  TextureWithSampler intermediate; // RGBA16Float EASU output feeding RCAS
  wgpu::TextureView intermediateView;
  TextureWithSampler output;       // surface format, viewport sized
  wgpu::BindGroup easuBindGroup;
  wgpu::TextureView easuSourceView;
  wgpu::BindGroup rcasBindGroup;
  EasuConstants easuConstants{};
  RcasConstants rcasConstants{};
  bool constantsValid = false;
};
State g_state;

wgpu::RenderPipeline make_pipeline(const wgpu::ShaderModule& module, const wgpu::BindGroupLayout& layout,
                                   wgpu::TextureFormat format, const char* label) {
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout};
  const auto pipelineLayout = g_device.CreatePipelineLayout(&layoutDescriptor);
  const std::array colorTargets{wgpu::ColorTargetState{.format = format, .writeMask = wgpu::ColorWriteMask::All}};
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = pipelineLayout,
      .vertex = wgpu::VertexState{.module = module, .entryPoint = "vs_main"},
      .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = wgpu::MultisampleState{.count = 1, .mask = UINT32_MAX},
      .fragment = &fragmentState,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

wgpu::ShaderModule make_module(std::string_view code, const char* label) {
  wgpu::ShaderSourceWGSL source{};
  source.code = code;
  const wgpu::ShaderModuleDescriptor descriptor{.nextInChain = &source, .label = label};
  return g_device.CreateShaderModule(&descriptor);
}

bool ensure_pipelines() {
  auto& p = g_state.pipelines;
  const auto surfaceFormat = g_graphicsConfig.surfaceConfiguration.format;
  if (p.failed) {
    return false;
  }
  if (p.easuToHalf && p.surfaceFormat == surfaceFormat) {
    return true;
  }
  p = {};
  p.surfaceFormat = surfaceFormat;

  const std::array easuEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Uniform},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = wgpu::SamplerBindingLayout{.type = wgpu::SamplerBindingType::NonFiltering},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float,
                                                .viewDimension = wgpu::TextureViewDimension::e2D},
      },
  };
  const wgpu::BindGroupLayoutDescriptor easuLayoutDescriptor{.entryCount = easuEntries.size(),
                                                             .entries = easuEntries.data()};
  p.easuLayout = g_device.CreateBindGroupLayout(&easuLayoutDescriptor);

  const std::array rcasEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Uniform},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float,
                                                .viewDimension = wgpu::TextureViewDimension::e2D},
      },
  };
  const wgpu::BindGroupLayoutDescriptor rcasLayoutDescriptor{.entryCount = rcasEntries.size(),
                                                             .entries = rcasEntries.data()};
  p.rcasLayout = g_device.CreateBindGroupLayout(&rcasLayoutDescriptor);

  const auto easuModule = make_module(easuShader, "FSR1 EASU module");
  const auto rcasModule = make_module(rcasShader, "FSR1 RCAS module");
  p.easuToHalf = make_pipeline(easuModule, p.easuLayout, wgpu::TextureFormat::RGBA16Float, "FSR1 EASU (to RGBA16F)");
  p.easuToSurface = make_pipeline(easuModule, p.easuLayout, surfaceFormat, "FSR1 EASU");
  p.rcas = make_pipeline(rcasModule, p.rcasLayout, surfaceFormat, "FSR1 RCAS");

  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "FSR1 gather sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Nearest,
      .minFilter = wgpu::FilterMode::Nearest,
      .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
  };
  p.gatherSampler = g_device.CreateSampler(&samplerDescriptor);

  const wgpu::BufferDescriptor easuBuffer{
      .label = "FSR1 EASU constants",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
      .size = sizeof(EasuConstants),
  };
  p.easuUniform = g_device.CreateBuffer(&easuBuffer);
  const wgpu::BufferDescriptor rcasBuffer{
      .label = "FSR1 RCAS constants",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
      .size = 16,
  };
  p.rcasUniform = g_device.CreateBuffer(&rcasBuffer);

  g_state.intermediate = {};
  g_state.intermediateView = {};
  g_state.output = {};
  g_state.easuBindGroup = {};
  g_state.easuSourceView = {};
  g_state.rcasBindGroup = {};
  g_state.constantsValid = false;
  return true;
}

void ensure_targets(uint32_t width, uint32_t height, bool needIntermediate) {
  auto& s = g_state;
  const auto format = g_graphicsConfig.surfaceConfiguration.format;
  if (!s.output.view || s.output.size.width != width || s.output.size.height != height || s.output.format != format) {
    s.output = create_render_texture(width, height, false);
    s.easuBindGroup = {};
    s.rcasBindGroup = {};
    s.constantsValid = false;
  }
  if (needIntermediate &&
      (!s.intermediate.view || s.intermediate.size.width != width || s.intermediate.size.height != height)) {
    const wgpu::Extent3D size{width, height, 1};
    const wgpu::TextureDescriptor descriptor{
        .label = "FSR1 EASU output",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = size,
        .format = wgpu::TextureFormat::RGBA16Float,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    s.intermediate.texture = g_device.CreateTexture(&descriptor);
    s.intermediate.view = s.intermediate.texture.CreateView();
    s.intermediate.size = size;
    s.intermediate.format = wgpu::TextureFormat::RGBA16Float;
    s.rcasBindGroup = {};
  }
}

void run_pass(const wgpu::CommandEncoder& encoder, const wgpu::RenderPipeline& pipeline,
              const wgpu::BindGroup& bindGroup, const wgpu::TextureView& target, uint32_t width, uint32_t height,
              const char* label) {
  const std::array attachments{wgpu::RenderPassColorAttachment{
      .view = target,
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
  }};
  const wgpu::RenderPassDescriptor descriptor{
      .label = label,
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
  };
  const auto pass = encoder.BeginRenderPass(&descriptor);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bindGroup, 0, nullptr);
  pass.SetViewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f);
  pass.Draw(3);
  pass.End();
}

// ---- capture ----------------------------------------------------------------------------------------
struct Capture {
  std::mutex mutex;
  std::string path;
  bool pending = false;
  bool encoded = false;
  wgpu::Buffer buffer;
  uint32_t width = 0, height = 0, rowBytes = 0;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
};
Capture g_capture;
} // namespace

void set_settings(const Settings& s) noexcept {
  g_scaleFactor.store(std::clamp(s.scaleFactor, 1.f, 8.f), std::memory_order_relaxed);
  g_sharpness.store(std::clamp(s.sharpness, 0.f, 1.f), std::memory_order_relaxed);
  g_mode.store(s.mode, std::memory_order_release);
}

Settings settings() noexcept {
  Settings s;
  s.mode = g_mode.load(std::memory_order_acquire);
  s.scaleFactor = g_scaleFactor.load(std::memory_order_relaxed);
  s.sharpness = g_sharpness.load(std::memory_order_relaxed);
  return s;
}

float resolution_divisor() noexcept {
  const auto s = settings();
  // Spatial fallback preserves the internal size requested by a temporal upscaler.
  return s.mode == AURORA_UPSCALER_OFF ? 1.f : std::max(s.scaleFactor, 1.f);
}

bool locks_native_resolution() noexcept {
  const auto s = settings();
  return s.mode == AURORA_UPSCALER_DLSS && s.scaleFactor <= 1.001f && dlss::ready();
}

Result fsr1_dispatch(const wgpu::CommandEncoder& encoder, const TextureWithSampler& source, const Viewport& viewport) {
  const auto s = settings();
  const uint32_t outW = std::max(1u, static_cast<uint32_t>(std::lround(std::max(viewport.width, 1.f))));
  const uint32_t outH = std::max(1u, static_cast<uint32_t>(std::lround(std::max(viewport.height, 1.f))));
  const uint32_t inW = source.size.width;
  const uint32_t inH = source.size.height;
  note_present(inW, inH, outW, outH, AURORA_UPSCALER_OFF, false);
  if (s.mode == AURORA_UPSCALER_OFF || s.scaleFactor <= 1.001f || inW == 0 || inH == 0 ||
      inW > outW || inH > outH || (inW == outW && inH == outH) || !source.view || !g_device) {
    return {};
  }
  if (!ensure_pipelines()) {
    return {};
  }
  auto& st = g_state;
  const bool sharpen = s.sharpness > 0.001f;
  ensure_targets(outW, outH, sharpen);
  auto& p = st.pipelines;

  const auto easu = make_easu_constants(static_cast<float>(inW), static_cast<float>(inH), static_cast<float>(outW),
                                        static_cast<float>(outH));
  RcasConstants rcas{};
  rcas.con[0] = rcas_linear_sharpness(s.sharpness);
  if (!st.constantsValid || std::memcmp(&easu, &st.easuConstants, sizeof(easu)) != 0) {
    g_queue.WriteBuffer(p.easuUniform, 0, &easu, sizeof(easu));
    st.easuConstants = easu;
  }
  if (!st.constantsValid || std::memcmp(&rcas, &st.rcasConstants, sizeof(rcas)) != 0) {
    g_queue.WriteBuffer(p.rcasUniform, 0, &rcas, sizeof(rcas));
    st.rcasConstants = rcas;
  }
  st.constantsValid = true;

  if (!st.easuBindGroup || st.easuSourceView.Get() != source.view.Get()) {
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .buffer = p.easuUniform, .size = sizeof(EasuConstants)},
        wgpu::BindGroupEntry{.binding = 1, .sampler = p.gatherSampler},
        wgpu::BindGroupEntry{.binding = 2, .textureView = source.view},
    };
    const wgpu::BindGroupDescriptor descriptor{.layout = p.easuLayout, .entryCount = entries.size(),
                                               .entries = entries.data()};
    st.easuBindGroup = g_device.CreateBindGroup(&descriptor);
    st.easuSourceView = source.view;
  }

  if (sharpen) {
    if (!st.rcasBindGroup) {
      const std::array entries{
          wgpu::BindGroupEntry{.binding = 0, .buffer = p.rcasUniform, .size = 16},
          wgpu::BindGroupEntry{.binding = 1, .textureView = st.intermediate.view},
      };
      const wgpu::BindGroupDescriptor descriptor{.layout = p.rcasLayout, .entryCount = entries.size(),
                                                 .entries = entries.data()};
      st.rcasBindGroup = g_device.CreateBindGroup(&descriptor);
    }
    run_pass(encoder, p.easuToHalf, st.easuBindGroup, st.intermediate.view, outW, outH, "FSR1 EASU");
    run_pass(encoder, p.rcas, st.rcasBindGroup, st.output.view, outW, outH, "FSR1 RCAS");
  } else {
    run_pass(encoder, p.easuToSurface, st.easuBindGroup, st.output.view, outW, outH, "FSR1 EASU");
  }

  note_present(inW, inH, outW, outH, AURORA_UPSCALER_FSR1, sharpen);
  return {create_copy_bind_group(st.output), &st.output};
}

void note_present(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, AuroraUpscaler active,
                  bool sharpened) noexcept {
  g_inW.store(inW, std::memory_order_relaxed);
  g_inH.store(inH, std::memory_order_relaxed);
  g_outW.store(outW, std::memory_order_relaxed);
  g_outH.store(outH, std::memory_order_relaxed);
  g_sharpened.store(sharpened, std::memory_order_relaxed);
  g_activeMode.store(active, std::memory_order_release);
}

AuroraUpscaleInfo info() noexcept {
  const auto s = settings();
  AuroraUpscaleInfo i{};
  i.requested = s.mode;
  i.active = g_activeMode.load(std::memory_order_acquire);
  i.scaleFactor = s.scaleFactor;
  i.sharpness = s.sharpness;
  i.inputWidth = g_inW.load(std::memory_order_relaxed);
  i.inputHeight = g_inH.load(std::memory_order_relaxed);
  i.outputWidth = g_outW.load(std::memory_order_relaxed);
  i.outputHeight = g_outH.load(std::memory_order_relaxed);
  i.sharpened = g_sharpened.load(std::memory_order_relaxed);
  return i;
}

void request_capture(const std::string& path) {
  std::lock_guard lock{g_capture.mutex};
  g_capture.path = path;
  g_capture.pending = !path.empty();
  g_capture.encoded = false;
}

void capture_encode(const wgpu::CommandEncoder& encoder, const TextureWithSampler& image) {
  std::lock_guard lock{g_capture.mutex};
  if (!g_capture.pending || g_capture.encoded || !image.texture) {
    return;
  }
  const auto format = image.format;
  if (format != wgpu::TextureFormat::RGBA8Unorm && format != wgpu::TextureFormat::BGRA8Unorm) {
    Log.warn("Capture skipped: unsupported present format");
    g_capture.pending = false;
    return;
  }
  g_capture.width = image.size.width;
  g_capture.height = image.size.height;
  g_capture.rowBytes = (g_capture.width * 4 + 255u) & ~255u;
  g_capture.format = format;
  const wgpu::BufferDescriptor descriptor{
      .label = "Present capture",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = static_cast<uint64_t>(g_capture.rowBytes) * g_capture.height,
  };
  g_capture.buffer = g_device.CreateBuffer(&descriptor);
  const wgpu::TexelCopyTextureInfo source{.texture = image.texture};
  const wgpu::TexelCopyBufferInfo destination{
      .layout = {.bytesPerRow = g_capture.rowBytes, .rowsPerImage = g_capture.height},
      .buffer = g_capture.buffer,
  };
  const wgpu::Extent3D extent{g_capture.width, g_capture.height, 1};
  encoder.CopyTextureToBuffer(&source, &destination, &extent);
  g_capture.encoded = true;
}

void capture_after_submit() {
  std::lock_guard lock{g_capture.mutex};
  if (!g_capture.pending || !g_capture.encoded) {
    return;
  }
  const uint64_t size = static_cast<uint64_t>(g_capture.rowBytes) * g_capture.height;
  std::vector<uint8_t> pixels;
  bool ok = false;
  const auto future = g_capture.buffer.MapAsync(
      wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
        if (status != wgpu::MapAsyncStatus::Success) {
          return;
        }
        const auto* data = static_cast<const uint8_t*>(g_capture.buffer.GetConstMappedRange(0, size));
        pixels.assign(data, data + size);
        g_capture.buffer.Unmap();
        ok = true;
      });
  if (g_instance.WaitAny(future, 5'000'000'000ull) != wgpu::WaitStatus::Success || !ok) {
    Log.warn("Present capture readback failed");
  } else {
    const auto pixelFormat =
        g_capture.format == wgpu::TextureFormat::BGRA8Unorm ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32;
    for (uint32_t y = 0; y < g_capture.height; ++y) {
      uint8_t* row = pixels.data() + static_cast<size_t>(y) * g_capture.rowBytes;
      for (uint32_t x = 0; x < g_capture.width; ++x) {
        row[x * 4 + 3] = 255;
      }
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(g_capture.width), static_cast<int>(g_capture.height),
                                                 pixelFormat, pixels.data(), static_cast<int>(g_capture.rowBytes));
    if (surface != nullptr && SDL_SavePNG(surface, g_capture.path.c_str())) {
      Log.info("Saved present image {}x{} to {}", g_capture.width, g_capture.height, g_capture.path);
    } else {
      Log.warn("Saving {} failed: {}", g_capture.path, SDL_GetError());
    }
    SDL_DestroySurface(surface);
  }
  g_capture.buffer = {};
  g_capture.pending = false;
  g_capture.encoded = false;
}

void shutdown() noexcept {
  g_state = {};
  std::lock_guard lock{g_capture.mutex};
  g_capture.buffer = {};
  g_capture.pending = false;
  g_capture.encoded = false;
  g_activeMode.store(AURORA_UPSCALER_OFF);
}

uint32_t jitter_phase_count(uint32_t renderWidth, uint32_t outputWidth) noexcept {
  if (renderWidth == 0) {
    return 8;
  }
  const float ratio = static_cast<float>(outputWidth) / static_cast<float>(renderWidth);
  return std::max(1u, static_cast<uint32_t>(8.0f * std::pow(ratio, 2.0f)));
}

namespace {
float halton(int32_t index, int32_t base) noexcept {
  float f = 1.0f, result = 0.0f;
  for (int32_t current = index; current > 0;) {
    f /= static_cast<float>(base);
    result += f * static_cast<float>(current % base);
    current = static_cast<int32_t>(std::floor(static_cast<float>(current) / static_cast<float>(base)));
  }
  return result;
}
} // namespace

void halton_jitter(uint32_t index, uint32_t phaseCount, float& x, float& y) noexcept {
  const auto phase = static_cast<int32_t>(index % std::max(phaseCount, 1u)) + 1;
  x = halton(phase, 2) - 0.5f;
  y = halton(phase, 3) - 0.5f;
}

} // namespace aurora::webgpu::upscale

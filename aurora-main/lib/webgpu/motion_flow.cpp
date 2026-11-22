#include "motion_flow.hpp"

#include "dlss.hpp"

#include <algorithm>
#include <array>
#include <string_view>

using namespace std::string_view_literals;

namespace aurora::webgpu::dlss {
namespace {

constexpr std::string_view kLumaShader = R"(
@group(0) @binding(0) var srcT: texture_2d<f32>;
@group(0) @binding(1) var dstT: texture_storage_2d<r32float, write>;
@compute @workgroup_size(8, 8)
fn luma(@builtin(global_invocation_id) id: vec3<u32>) {
    let d = textureDimensions(dstT);
    if (id.x >= d.x || id.y >= d.y) { return; }
    let s = vec2<i32>(textureDimensions(srcT));
    // Band-limit luma before fitting subpixel motion to sampled edges.
    var c = vec3f(0);
    let kernel = array<f32,5>(1,4,6,4,1);
    for (var y = -2; y <= 2; y++) {
        for (var x = -2; x <= 2; x++) {
            let xy = clamp(vec2i(id.xy) + vec2i(x,y), vec2i(0), s-1);
            c += textureLoad(srcT, xy, 0).rgb * kernel[x+2] * kernel[y+2] / 256.0;
        }
    }
    textureStore(dstT, id.xy, vec4<f32>(dot(c, vec3<f32>(0.299, 0.587, 0.114)), 0.0, 0.0, 1.0));
}

@compute @workgroup_size(8, 8)
fn down(@builtin(global_invocation_id) id: vec3<u32>) {
    let d = textureDimensions(dstT);
    if (id.x >= d.x || id.y >= d.y) { return; }
    let s = vec2<i32>(textureDimensions(srcT));
    let b = vec2<i32>(id.xy) * 2;
    let hi = s - vec2<i32>(1);
    let a0 = textureLoad(srcT, min(b, hi), 0).r;
    let a1 = textureLoad(srcT, min(b + vec2<i32>(1, 0), hi), 0).r;
    let a2 = textureLoad(srcT, min(b + vec2<i32>(0, 1), hi), 0).r;
    let a3 = textureLoad(srcT, min(b + vec2<i32>(1, 1), hi), 0).r;
    textureStore(dstT, id.xy, vec4<f32>(0.25 * (a0 + a1 + a2 + a3), 0.0, 0.0, 1.0));
}
)"sv;

constexpr std::string_view kFlowShader = R"(
struct P {
    dims: vec2<u32>,
    radius: i32,
    pr: i32,
    hasPred: u32,
    subPix: u32,
    lambda: f32,

};
@group(0) @binding(0) var<uniform> p: P;
@group(0) @binding(1) var curT: texture_2d<f32>;
@group(0) @binding(2) var prevT: texture_2d<f32>;
@group(0) @binding(3) var predT: texture_2d<f32>;
@group(0) @binding(4) var outT: texture_storage_2d<rgba16float, write>;

fn ld(t: texture_2d<f32>, q: vec2<i32>) -> f32 {
    let hi = vec2<i32>(p.dims) - vec2<i32>(1);
    return textureLoad(t, clamp(q, vec2<i32>(0), hi), 0).r;
}

var<private> cp: array<f32, 49>;

// Sum of absolute luma differences between the current patch (cached in cp) and the previous frame displaced by o.
fn sad(pc: vec2<i32>, o: vec2<i32>) -> f32 {
    var s = 0.0;
    var k = 0;
    for (var y = -p.pr; y <= p.pr; y++) {
        for (var x = -p.pr; x <= p.pr; x++) {
            s += abs(cp[k] - ld(prevT, pc + o + vec2<i32>(x, y) * 3));
            k++;
        }
    }
    return s;
}

// Sub-pixel offset from the costs at -1, 0, +1. SAD of an image is V-shaped around the minimum, not parabolic, so the
// equiangular (V) fit is used: a parabola under-estimates by about a third of a pixel.
// bilinear fetch, q in texel-index space (texel centres at integers)
fn ldf(t: texture_2d<f32>, q: vec2<f32>) -> f32 {
    let f0 = floor(q);
    let a = q - f0;
    let i = vec2<i32>(f0);
    return mix(mix(ld(t, i), ld(t, i + vec2<i32>(1, 0)), a.x), mix(ld(t, i + vec2<i32>(0, 1)), ld(t, i + vec2<i32>(1, 1)), a.x), a.y);
}

// Lucas-Kanade refinement: a few Gauss-Newton steps of cur(p) = prev(p + d) over the patch; stops where the patch has
// no usable gradient in both directions (flat areas, single edges) and keeps the integer vector there.
fn refine(pc: vec2<i32>, d0: vec2<f32>) -> vec2<f32> {
    var d = d0;
    for (var it = 0; it < 4; it++) {
        var axx = 0.0; var axy = 0.0; var ayy = 0.0; var bx = 0.0; var by = 0.0;

        for (var y = -12; y <= 12; y += 4) {
            for (var x = -12; x <= 12; x += 4) {
                let mid = vec2<f32>(pc + vec2<i32>(x, y));
                let q = mid + 0.5*d;
                let cur = mid - 0.5*d;
                let i1 = ldf(prevT, q);
                let gx = 0.25 * (ldf(prevT, q + vec2f(1,0)) - ldf(prevT, q - vec2f(1,0)) + ldf(curT, cur + vec2f(1,0)) - ldf(curT, cur - vec2f(1,0)));
                let gy = 0.25 * (ldf(prevT, q + vec2f(0,1)) - ldf(prevT, q - vec2f(0,1)) + ldf(curT, cur + vec2f(0,1)) - ldf(curT, cur - vec2f(0,1)));
                let e = ldf(curT, cur) - i1;
                // Avoid letting a single high-contrast, aliased line dominate a patch.
                let weight = 1.0 / (1.0 + 900.0 * (gx*gx + gy*gy + e*e));
                axx += weight * gx * gx; axy += weight * gx * gy; ayy += weight * gy * gy;
                bx += weight * gx * e; by += weight * gy * e;

            }
        }
        let det = axx * ayy - axy * axy;
        let tr = axx + ayy;
        // well-conditioned: enough energy and both eigenvalues clearly non-zero
        if (tr < 0.00002 || det < 0.0001 * tr * tr) { break; }
        let delta = vec2<f32>(ayy * bx - axy * by, axx * by - axy * bx) / det;
        d = d + clamp(delta, vec2<f32>(-1.0), vec2<f32>(1.0));
    }
    return d;
}

fn parab(a: f32, b: f32, c: f32) -> f32 {
    let m = max(a, c) - b;
    if (m > 1e-5) { return clamp(0.5 * (a - c) / m, -0.5, 0.5); }
    return 0.0;
}

@compute @workgroup_size(8, 8)
fn flow(@builtin(global_invocation_id) id: vec3<u32>) {
    let pixels = select(1u, 2u, p.subPix == 2u);
    let origin = id.xy * pixels;
    if (any(origin >= p.dims)) { return; }
    let pc = vec2i(min(origin + pixels/2u, p.dims-1u));
    var k = 0;
    for (var y = -p.pr; y <= p.pr; y++) {
        for (var x = -p.pr; x <= p.pr; x++) {
            cp[k] = ld(curT, pc + vec2<i32>(x, y) * 3);
            k++;
        }
    }
    var predicted = vec2f(0);
    var base = vec2<i32>(0, 0);
    if (p.hasPred != 0u) {
        let pd = vec2<i32>(textureDimensions(predT));
        // Use the locally dominant coarse displacement so an ambiguous patch cannot
        // seed a large, incorrect search offset in all four of its children.
        let center = pc / 2;
        var values = array<vec2f,5>();
        let offsets = array<vec2i,5>(vec2i(0), vec2i(-1,0), vec2i(1,0), vec2i(0,-1), vec2i(0,1));
        for (var i = 0; i < 5; i++) {
            values[i] = textureLoad(predT, clamp(center + offsets[i], vec2i(0), pd-1), 0).xy * 2.0;
        }
        for (var i = 0; i < 3; i++) {
            for (var j = i+1; j < 5; j++) {
                let lo = min(values[i], values[j]);
                values[j] = max(values[i], values[j]);
                values[i] = lo;
            }
        }
        let pf = values[2];
        predicted = pf;
        base = vec2<i32>(round(pf));
    }
    var bestCost = 1e30;
    var best = base;
    for (var dy = -p.radius; dy <= p.radius; dy++) {
        for (var dx = -p.radius; dx <= p.radius; dx++) {
            let o = base + vec2<i32>(dx, dy);
            let c = sad(pc, o) + p.lambda * f32(abs(o.x) + abs(o.y));
            if (c < bestCost) { bestCost = c; best = o; }
        }
    }
    // Prefer a small displacement in ambiguous regions: noisy vectors
    // are far more harmful to a temporal upscaler than a missing small one.
    let c0 = sad(pc, vec2<i32>(0, 0));
    if (c0 <= bestCost * 1.25 + 0.06) {
        best = vec2<i32>(0, 0);
    }
    var f = vec2<f32>(best);
    if (p.subPix != 0u && c0 > 0.00001) {
        let cb = sad(pc, best);
        let xm = sad(pc, best - vec2<i32>(1, 0));
        let xp = sad(pc, best + vec2<i32>(1, 0));
        let ym = sad(pc, best - vec2<i32>(0, 1));
        let yp = sad(pc, best + vec2<i32>(0, 1));
        f = f + vec2<f32>(parab(xm, cb, xp), parab(ym, cb, yp));
        let r = refine(pc, f);
        if (length(r - f) < 1.5) { f = r; }
    }
    if (p.hasPred != 0u && distance(predicted, f) < 1.25 && c0 > 0.001) {
        f = mix(predicted, f, 0.8);
    }
    for (var y = 0u; y < pixels; y++) {
        for (var x = 0u; x < pixels; x++) {
            let dst = origin + vec2u(x,y);
            if (all(dst < p.dims)) { textureStore(outT, dst, vec4f(f,0,0)); }
        }
    }
}
)"sv;

constexpr std::string_view kMvShader = R"(
@group(0) @binding(0) var flowT: texture_2d<f32>;
struct Corr { v: vec4<f32> };
@group(0) @binding(1) var<uniform> corr: Corr;

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4<f32> {
    var q = array<vec2<f32>, 3>(vec2<f32>(-1.0, 1.0), vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0));
    return vec4<f32>(q[idx], 0.0, 1.0);
}

fn local_motion(c: vec2i, stride: i32) -> vec2f {
    let d = vec2i(textureDimensions(flowT));
    // Reject isolated vectors at edges using the component medians of a 5 x 5 neighborhood.
    var vx: array<f32, 25>;
    var vy: array<f32, 25>;
    var n = 0;
    for (var y = -2; y <= 2; y++) {
        for (var x = -2; x <= 2; x++) {
            let f = textureLoad(flowT, clamp(c + vec2<i32>(x, y) * stride, vec2<i32>(0), d - vec2<i32>(1)), 0).xy;
            vx[n] = f.x;
            vy[n] = f.y;
            n++;
        }
    }
    for (var i = 0; i < 13; i++) {
        var mi = i;
        var mj = i;
        for (var j = i + 1; j < 25; j++) {
            if (vx[j] < vx[mi]) { mi = j; }
            if (vy[j] < vy[mj]) { mj = j; }
        }
        let tx = vx[i]; vx[i] = vx[mi]; vx[mi] = tx;
        let ty = vy[i]; vy[i] = vy[mj]; vy[mj] = ty;
    }
    return vec2f(vx[12], vy[12]);
}

@fragment
fn fs_main(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let c = vec2i(pos.xy);
    let local = local_motion(c, 3);
    let region = local_motion(c, 14);
    // Share the better-conditioned regional estimate only across similar motion.
    let agreement = 1.0 - smoothstep(0.5, 2.5, distance(local, region));
    return vec4f(mix(local, region, agreement * 0.85) + corr.v.xy, 0, 0);
}
)"sv;

wgpu::ShaderModule make_module(const wgpu::Device& device, std::string_view code, const char* label) {
  wgpu::ShaderSourceWGSL source{};
  source.code = code;
  const wgpu::ShaderModuleDescriptor descriptor{.nextInChain = &source, .label = label};
  return device.CreateShaderModule(&descriptor);
}

wgpu::Texture make_texture(const wgpu::Device& device, uint32_t w, uint32_t h, wgpu::TextureFormat format,
                           const char* label) {
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding,
      .size = {w, h, 1},
      .format = format,
  };
  return device.CreateTexture(&descriptor);
}

uint32_t groups(uint32_t n) { return (n + 7) / 8; }

} // namespace

bool MotionFlow::init(const wgpu::Device& device) {
  if (m_ready) {
    return true;
  }
  if (m_failed) {
    return false;
  }
  m_device = device;
  m_queue = device.GetQueue();

  const auto texEntry = [](uint32_t binding, wgpu::ShaderStage stage, wgpu::TextureSampleType type) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding,
        .visibility = stage,
        .texture = wgpu::TextureBindingLayout{.sampleType = type, .viewDimension = wgpu::TextureViewDimension::e2D},
    };
  };
  const auto storeEntry = [](uint32_t binding, wgpu::TextureFormat format) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding,
        .visibility = wgpu::ShaderStage::Compute,
        .storageTexture = wgpu::StorageTextureBindingLayout{.access = wgpu::StorageTextureAccess::WriteOnly,
                                                            .format = format,
                                                            .viewDimension = wgpu::TextureViewDimension::e2D},
    };
  };
  const auto makeLayout = [&](const auto& entries, const char* label) {
    const wgpu::BindGroupLayoutDescriptor d{.label = label, .entryCount = entries.size(), .entries = entries.data()};
    return device.CreateBindGroupLayout(&d);
  };

  const std::array lumaEntries{texEntry(0, wgpu::ShaderStage::Compute, wgpu::TextureSampleType::Float),
                               storeEntry(1, wgpu::TextureFormat::R32Float)};
  const std::array downEntries{texEntry(0, wgpu::ShaderStage::Compute, wgpu::TextureSampleType::UnfilterableFloat),
                               storeEntry(1, wgpu::TextureFormat::R32Float)};
  const std::array flowEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Compute,
          .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Uniform},
      },
      texEntry(1, wgpu::ShaderStage::Compute, wgpu::TextureSampleType::UnfilterableFloat),
      texEntry(2, wgpu::ShaderStage::Compute, wgpu::TextureSampleType::UnfilterableFloat),
      texEntry(3, wgpu::ShaderStage::Compute, wgpu::TextureSampleType::UnfilterableFloat),
      storeEntry(4, wgpu::TextureFormat::RGBA16Float),
  };
  const std::array mvEntries{texEntry(0, wgpu::ShaderStage::Fragment, wgpu::TextureSampleType::UnfilterableFloat),
                             wgpu::BindGroupLayoutEntry{.binding = 1,
                                                        .visibility = wgpu::ShaderStage::Fragment,
                                                        .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Uniform}}};
  m_lumaLayout = makeLayout(lumaEntries, "dlss flow luma layout");
  m_downLayout = makeLayout(downEntries, "dlss flow down layout");
  m_flowLayout = makeLayout(flowEntries, "dlss flow layout");
  m_mvLayout = makeLayout(mvEntries, "dlss mv layout");

  const auto makePipelineLayout = [&](const wgpu::BindGroupLayout& l) {
    const wgpu::PipelineLayoutDescriptor d{.bindGroupLayoutCount = 1, .bindGroupLayouts = &l};
    return device.CreatePipelineLayout(&d);
  };
  const auto lumaModule = make_module(device, kLumaShader, "dlss flow luma");
  const auto flowModule = make_module(device, kFlowShader, "dlss flow search");
  const auto mvModule = make_module(device, kMvShader, "dlss mv write");

  const auto makeCompute = [&](const char* label, const wgpu::BindGroupLayout& layout, const wgpu::ShaderModule& module,
                               const char* entry) {
    wgpu::ComputePipelineDescriptor d{};
    d.label = label;
    d.layout = makePipelineLayout(layout);
    d.compute.module = module;
    d.compute.entryPoint = entry;
    return device.CreateComputePipeline(&d);
  };
  m_lumaPipeline = makeCompute("dlss flow luma", m_lumaLayout, lumaModule, "luma");
  m_downPipeline = makeCompute("dlss flow down", m_downLayout, lumaModule, "down");
  m_flowPipeline = makeCompute("dlss flow search", m_flowLayout, flowModule, "flow");
  const std::array colorTargets{
      wgpu::ColorTargetState{.format = wgpu::TextureFormat::RG16Float, .writeMask = wgpu::ColorWriteMask::All}};
  const wgpu::FragmentState fragment{
      .module = mvModule,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const wgpu::RenderPipelineDescriptor mvDescriptor{
      .label = "dlss mv write",
      .layout = makePipelineLayout(m_mvLayout),
      .vertex = wgpu::VertexState{.module = mvModule, .entryPoint = "vs_main"},
      .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = wgpu::MultisampleState{.count = 1, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  m_mvPipeline = device.CreateRenderPipeline(&mvDescriptor);
  if (!m_lumaPipeline || !m_downPipeline || !m_flowPipeline || !m_mvPipeline) {
    m_failed = true;
    log(2, "motion flow: pipeline creation failed");
    return false;
  }
  m_ready = true;
  return true;
}

bool MotionFlow::resize(uint32_t width, uint32_t height) {
  if (!m_ready || width == 0 || height == 0) {
    return false;
  }
  if (width == m_width && height == m_height && m_flow[0]) {
    return true;
  }
  m_width = width;
  m_height = height;
  m_haveHistory = false;
  m_parity = 0;
  for (uint32_t l = 0; l < kLevels; ++l) {
    m_levelW[l] = std::max(1u, l == 0 ? width : (m_levelW[l - 1] + 1) / 2);
    m_levelH[l] = std::max(1u, l == 0 ? height : (m_levelH[l - 1] + 1) / 2);
  }
  for (uint32_t par = 0; par < 2; ++par) {
    for (uint32_t l = 0; l < kLevels; ++l) {
      m_luma[par][l] = make_texture(m_device, m_levelW[l], m_levelH[l], wgpu::TextureFormat::R32Float, "dlss flow luma");
      m_lumaView[par][l] = m_luma[par][l].CreateView();
    }
  }
  for (uint32_t l = 0; l < kLevels; ++l) {
    m_flow[l] = make_texture(m_device, m_levelW[l], m_levelH[l], wgpu::TextureFormat::RGBA16Float, "dlss flow");
    m_flowView[l] = m_flow[l].CreateView();
  }
  m_dummy = make_texture(m_device, 1, 1, wgpu::TextureFormat::RGBA16Float, "dlss flow dummy");
  m_dummyView = m_dummy.CreateView();

  // search radius per level: wide at the coarsest, small refinements below (reach ~ 6*8 + 2*4 + 2*2 + 1 px)
  const int32_t radius[kLevels] = {1, 2, 2, 6};
  for (uint32_t l = 0; l < kLevels; ++l) {
    const LevelParams params{m_levelW[l], m_levelH[l], radius[l], l == 0 ? 2 : 3, l == kLevels - 1 ? 0u : 1u, l == 0 ? 2u : 1u,
                                   0.003f, 0u};
    const wgpu::BufferDescriptor bd{.label = "dlss flow params",
                                    .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                    .size = sizeof(LevelParams)};
    m_params[l] = m_device.CreateBuffer(&bd);
    m_queue.WriteBuffer(m_params[l], 0, &params, sizeof(params));
  }
  for (uint32_t par = 0; par < 2; ++par) {
    for (uint32_t l = 0; l + 1 < kLevels; ++l) {
      const std::array entries{wgpu::BindGroupEntry{.binding = 0, .textureView = m_lumaView[par][l]},
                               wgpu::BindGroupEntry{.binding = 1, .textureView = m_lumaView[par][l + 1]}};
      const wgpu::BindGroupDescriptor d{.layout = m_downLayout, .entryCount = entries.size(), .entries = entries.data()};
      m_downGroup[par][l] = m_device.CreateBindGroup(&d);
    }
    for (uint32_t l = 0; l < kLevels; ++l) {
      const std::array entries{
          wgpu::BindGroupEntry{.binding = 0, .buffer = m_params[l], .size = sizeof(LevelParams)},
          wgpu::BindGroupEntry{.binding = 1, .textureView = m_lumaView[par][l]},
          wgpu::BindGroupEntry{.binding = 2, .textureView = m_lumaView[1 - par][l]},
          wgpu::BindGroupEntry{.binding = 3, .textureView = l + 1 < kLevels ? m_flowView[l + 1] : m_dummyView},
          wgpu::BindGroupEntry{.binding = 4, .textureView = m_flowView[l]},
      };
      const wgpu::BindGroupDescriptor d{.layout = m_flowLayout, .entryCount = entries.size(), .entries = entries.data()};
      m_flowGroup[par][l] = m_device.CreateBindGroup(&d);
    }
  }
  const wgpu::BufferDescriptor mvb{.label = "dlss mv correction",
                                   .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                   .size = 16};
  m_mvParams = m_device.CreateBuffer(&mvb);
  const std::array mvEntries{wgpu::BindGroupEntry{.binding = 0, .textureView = m_flowView[0]},
                             wgpu::BindGroupEntry{.binding = 1, .buffer = m_mvParams, .size = 16}};
  const wgpu::BindGroupDescriptor mvd{.layout = m_mvLayout, .entryCount = mvEntries.size(), .entries = mvEntries.data()};
  m_mvGroup = m_device.CreateBindGroup(&mvd);
  return true;
}

bool MotionFlow::encode(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& color,
                        const wgpu::TextureView& target, bool reset, float correctionX, float correctionY) {
  if (!m_ready || !m_flow[0] || !color || !target) {
    return false;
  }
  const uint32_t cur = m_parity;

  {
    const std::array entries{wgpu::BindGroupEntry{.binding = 0, .textureView = color},
                             wgpu::BindGroupEntry{.binding = 1, .textureView = m_lumaView[cur][0]}};
    const wgpu::BindGroupDescriptor d{.layout = m_lumaLayout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = m_device.CreateBindGroup(&d);
    const wgpu::ComputePassDescriptor pd{.label = "dlss flow luma"};
    auto pass = encoder.BeginComputePass(&pd);
    pass.SetPipeline(m_lumaPipeline);
    pass.SetBindGroup(0, group);
    pass.DispatchWorkgroups(groups(m_levelW[0]), groups(m_levelH[0]), 1);
    pass.End();
  }
  for (uint32_t l = 0; l + 1 < kLevels; ++l) {
    const wgpu::ComputePassDescriptor pd{.label = "dlss flow down"};
    auto pass = encoder.BeginComputePass(&pd);
    pass.SetPipeline(m_downPipeline);
    pass.SetBindGroup(0, m_downGroup[cur][l]);
    pass.DispatchWorkgroups(groups(m_levelW[l + 1]), groups(m_levelH[l + 1]), 1);
    pass.End();
  }

  const bool haveFlow = m_haveHistory && !reset;
  if (haveFlow) {
    // Measure displacement between the original samples, then remove camera jitter in pixel units.
    const float correction[4]{correctionX, correctionY, 0, 0};
    m_queue.WriteBuffer(m_mvParams, 0, correction, sizeof correction);
    for (int l = static_cast<int>(kLevels) - 1; l >= 0; --l) {
      const wgpu::ComputePassDescriptor pd{.label = "dlss flow search"};
      auto pass = encoder.BeginComputePass(&pd);
      pass.SetPipeline(m_flowPipeline);
      pass.SetBindGroup(0, m_flowGroup[cur][l]);
      const uint32_t tile = l == 0 ? 2u : 1u;
      pass.DispatchWorkgroups(groups((m_levelW[l]+tile-1)/tile), groups((m_levelH[l]+tile-1)/tile), 1);
      pass.End();
    }
  }
  {
    const wgpu::RenderPassColorAttachment attachment{
        .view = target,
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const wgpu::RenderPassDescriptor rd{.label = "dlss mv write", .colorAttachmentCount = 1, .colorAttachments = &attachment};
    auto pass = encoder.BeginRenderPass(&rd);
    if (haveFlow) {
      pass.SetPipeline(m_mvPipeline);
      pass.SetBindGroup(0, m_mvGroup);
      pass.Draw(3);
    }
    pass.End();
  }
  m_parity = 1 - m_parity;
  m_haveHistory = true;
  return true;
}

} // namespace aurora::webgpu::dlss

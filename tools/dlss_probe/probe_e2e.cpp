// End-to-end test of the in-game DLSS module (aurora-main/lib/webgpu/dlss.cpp + motion_flow.cpp) with a synthetic
// scene on a real Dawn D3D12 device, without the game.
//
//  1. motion-flow accuracy: a textured pattern shifted by known amounts between two frames; the RG16F vectors
//     written by MotionFlow are read back and compared with the true shift.
//  2. DLSS dispatch(): the same pattern rendered at render resolution with Halton viewport jitter, moving at a
//     constant speed, upscaled by DLSS (given the flow vectors, or zero vectors), compared by PSNR against the
//     full-resolution ground truth and against plain bilinear upscaling.
//
// Build (repo root, mingw clang; see tools/dlss_probe/README.md):
//   clang++ -std=gnu++20 -O1 -DNOMINMAX -DWEBGPU_DAWN -w -Ithird_party/nvidia/streamline/include -isystem <dawn>/include \
//     tools/dlss_probe/probe_e2e.cpp aurora-main/lib/webgpu/dlss.cpp aurora-main/lib/webgpu/motion_flow.cpp \
//     -o build/tmp_dlss/probe_e2e.exe <dawn>/lib/webgpu_dawn.lib -lole32 -luuid
// Run: build/tmp_dlss/probe_e2e.exe <quality|balanced|performance|ultra|dlaa> [frames] [nomotion]
#include "../../aurora-main/lib/webgpu/dlss.hpp"
#include "../../aurora-main/lib/webgpu/motion_flow.hpp"

#include <webgpu/webgpu_cpp.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace aurora::webgpu;

static wgpu::Instance g_inst;
static wgpu::Adapter g_adapter;
static wgpu::Device g_device;
static wgpu::Queue g_queue;

static const char* kScene = R"(
struct U { a: vec4<f32>, b: vec4<f32>, c: vec4<f32> };   // a = (jx, jy, renderW, renderH)  b = (outW, outH, shiftX, shiftY)  c.x = supersampling grid
@group(0) @binding(0) var<uniform> u: U;

fn hash(q: vec2<i32>) -> f32 {
    var h = u32(q.x) * 374761393u + u32(q.y) * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return f32(h ^ (h >> 16u)) / 4294967295.0;
}
fn vnoise(p: vec2<f32>) -> f32 {
    let i = vec2<i32>(floor(p));
    let f = fract(p);
    let s = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2<i32>(1, 0)), s.x), mix(hash(i + vec2<i32>(0, 1)), hash(i + vec2<i32>(1, 1)), s.x), s.y);
}
fn pattern(p: vec2<f32>) -> vec3<f32> {
    // smooth colour field + crisp checker edges + thin diagonal lines + thin rings: typical stylised game content
    var col = vec3<f32>(0.5 + 0.5 * sin(p.x / 97.0), 0.5 + 0.5 * sin(p.y / 131.0 + 1.0), 0.5 + 0.5 * sin((p.x + p.y) / 173.0 + 2.0)) * 0.5 + vec3<f32>(0.2);
    let c = floor(p / 40.0);
    let edge = f32((i32(c.x) + i32(c.y)) & 1);
    col = col + (edge - 0.5) * vec3<f32>(0.25, -0.15, 0.2);
    let d1 = abs(fract((p.x + p.y) / 23.0) - 0.5) * 23.0;
    col = mix(vec3<f32>(1.0, 0.95, 0.8), col, smoothstep(0.7, 1.6, d1));
    let d2 = abs(fract(length(p - vec2<f32>(700.0, 450.0)) / 31.0) - 0.5) * 31.0;
    col = mix(vec3<f32>(0.05, 0.05, 0.1), col, smoothstep(0.6, 1.5, d2));
    col = col + (vnoise(p / 7.0) - 0.5) * 0.16;   // fine texture everywhere (flat regions have no measurable motion)
    return clamp(col, vec3<f32>(0.0), vec3<f32>(1.0));
}
@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4<f32> {
    var q = array<vec2<f32>, 3>(vec2<f32>(-1.0, 1.0), vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0));
    return vec4<f32>(q[idx], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    // geometry is fixed in NDC; the viewport (jitter) moves the pixel grid, so scene uv = (pos - jitter) / size
    let n = i32(u.c.x);
    var acc = vec3<f32>(0.0);
    for (var j = 0; j < n; j++) {
        for (var i = 0; i < n; i++) {
            let o = (vec2<f32>(f32(i), f32(j)) + 0.5) / f32(n) - 0.5;
            let s = (pos.xy + o - u.a.xy) / u.a.zw;
            acc += pattern(s * u.b.xy + u.b.zw);
        }
    }
    return vec4<f32>(acc / f32(n * n), 1.0);
}
)";

static const char* kUp = R"(
@group(0) @binding(0) var smp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;
@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4<f32> {
    var q = array<vec2<f32>, 3>(vec2<f32>(-1.0, 1.0), vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0));
    return vec4<f32>(q[idx], 0.0, 1.0);
}
@fragment
fn fs_main(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let d = vec2<f32>(textureDimensions(src));
    let o = vec2<f32>(textureDimensions(src)) ;
    return textureSampleLevel(src, smp, pos.xy / vec2<f32>(OUTW, OUTH), 0.0);
}
)";

struct Gpu {
    wgpu::RenderPipeline scene;
    wgpu::BindGroupLayout sceneLayout;
    wgpu::Buffer ubo;
};

static wgpu::Texture makeTex(uint32_t w, uint32_t h, wgpu::TextureFormat f, wgpu::TextureUsage u) {
    wgpu::TextureDescriptor d{};
    d.size = {w, h, 1}; d.format = f; d.usage = u;
    return g_device.CreateTexture(&d);
}

static bool readback(const wgpu::Texture& t, uint32_t w, uint32_t h, uint32_t bpp, std::vector<uint8_t>& out) {
    const uint32_t row = (w * bpp + 255) & ~255u;
    wgpu::BufferDescriptor bd{};
    bd.size = size_t(row) * h; bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    auto buf = g_device.CreateBuffer(&bd);
    auto enc = g_device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo s{}; s.texture = t;
    wgpu::TexelCopyBufferInfo d{}; d.buffer = buf; d.layout.bytesPerRow = row; d.layout.rowsPerImage = h;
    wgpu::Extent3D e{w, h, 1};
    enc.CopyTextureToBuffer(&s, &d, &e);
    auto cb = enc.Finish();
    g_queue.Submit(1, &cb);
    bool ok = false;
    auto f = buf.MapAsync(wgpu::MapMode::Read, 0, bd.size, wgpu::CallbackMode::WaitAnyOnly, [&](wgpu::MapAsyncStatus st, wgpu::StringView) { ok = st == wgpu::MapAsyncStatus::Success; });
    g_inst.WaitAny(f, 10000000000ull);
    if (!ok) return false;
    const uint8_t* p = static_cast<const uint8_t*>(buf.GetConstMappedRange(0, bd.size));
    out.resize(size_t(w) * h * bpp);
    for (uint32_t y = 0; y < h; ++y) memcpy(&out[size_t(y) * w * bpp], p + size_t(y) * row, size_t(w) * bpp);
    buf.Unmap();
    return true;
}

static float half2f(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(float(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -v : v;
}

static wgpu::ShaderModule module(const std::string& code) {
    wgpu::ShaderSourceWGSL s{};
    s.code = code.c_str();
    wgpu::ShaderModuleDescriptor d{}; d.nextInChain = &s;
    return g_device.CreateShaderModule(&d);
}

struct SceneRenderer {
    wgpu::RenderPipeline pipe;
    wgpu::BindGroupLayout layout;
    wgpu::Buffer ubo;
    void init() {
        wgpu::BindGroupLayoutEntry e{};
        e.binding = 0; e.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        e.buffer.type = wgpu::BufferBindingType::Uniform;
        wgpu::BindGroupLayoutDescriptor bl{}; bl.entryCount = 1; bl.entries = &e;
        layout = g_device.CreateBindGroupLayout(&bl);
        wgpu::PipelineLayoutDescriptor pl{}; pl.bindGroupLayoutCount = 1; pl.bindGroupLayouts = &layout;
        auto m = module(kScene);
        wgpu::ColorTargetState ct{}; ct.format = wgpu::TextureFormat::RGBA8Unorm; ct.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState fs{}; fs.module = m; fs.entryPoint = "fs_main"; fs.targetCount = 1; fs.targets = &ct;
        wgpu::RenderPipelineDescriptor rp{};
        rp.layout = g_device.CreatePipelineLayout(&pl);
        rp.vertex.module = m; rp.vertex.entryPoint = "vs_main";
        rp.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        rp.multisample.count = 1; rp.multisample.mask = UINT32_MAX;
        rp.fragment = &fs;
        pipe = g_device.CreateRenderPipeline(&rp);
        wgpu::BufferDescriptor bd{}; bd.size = 48; bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        ubo = g_device.CreateBuffer(&bd);
    }
    // Records a pass; the uniform is written through a fresh buffer so several passes can share one encoder.
    void draw(wgpu::CommandEncoder& enc, const wgpu::TextureView& target, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
              float jx, float jy, float shx, float shy, int ss = 1) {
        wgpu::BufferDescriptor bd{}; bd.size = 48; bd.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        auto b = g_device.CreateBuffer(&bd);
        const float u[12] = {jx, jy, float(rw), float(rh), float(ow), float(oh), shx, shy, float(ss), 0, 0, 0};
        g_queue.WriteBuffer(b, 0, u, sizeof u);
        wgpu::BindGroupEntry be{}; be.binding = 0; be.buffer = b; be.size = 48;
        wgpu::BindGroupDescriptor bgd{}; bgd.layout = layout; bgd.entryCount = 1; bgd.entries = &be;
        auto bg = g_device.CreateBindGroup(&bgd);
        wgpu::RenderPassColorAttachment ca{}; ca.view = target; ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store; ca.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor rd{}; rd.colorAttachmentCount = 1; rd.colorAttachments = &ca;
        auto pass = enc.BeginRenderPass(&rd);
        pass.SetPipeline(pipe);
        pass.SetBindGroup(0, bg);
        pass.SetViewport(jx, jy, float(rw), float(rh), 0, 1);
        pass.Draw(3);
        pass.End();
    }
};

static double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t w, uint32_t h, uint32_t border) {
    double se = 0; size_t n = 0;
    for (uint32_t y = border; y + border < h; ++y)
        for (uint32_t x = border; x + border < w; ++x)
            for (int c = 0; c < 3; ++c) { double d = double(a[(size_t(y) * w + x) * 4 + c]) - double(b[(size_t(y) * w + x) * 4 + c]); se += d * d; ++n; }
    const double mse = se / double(n);
    return mse < 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

static int testFlow(SceneRenderer& scene) {
    printf("== motion flow accuracy (640x360 render, previous minus current, render pixels)\n");
    const uint32_t W = 640, H = 360;
    dlss::MotionFlow flow;
    if (!flow.init(g_device) || !flow.resize(W, H)) { printf("flow init failed\n"); return 2; }
    auto colorA = makeTex(W, H, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding);
    auto mv = makeTex(W, H, wgpu::TextureFormat::RG16Float, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc);
    // shift (content motion, render px), jitter of frame 0, jitter of frame 1 (picture shift, render px)
    const float tests[][6] = {{0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}, {0.5f, -0.5f, 0, 0, 0, 0}, {3.5f, -2.25f, 0, 0, 0, 0}, {17, 9, 0, 0, 0, 0}, {-40, 25, 0, 0, 0, 0}, {-70, 0, 0, 0, 0, 0},
                              {0, 0, 0.25f, -0.3f, -0.4f, 0.2f}, {0, 0, -0.1f, 0.45f, 0.35f, -0.4f}, {2.5f, 1.5f, 0.25f, -0.3f, -0.4f, 0.2f}, {-6.25f, 3.0f, -0.4f, 0.1f, 0.3f, 0.4f}};
    int bad = 0;
    for (const auto& t : tests) {
        flow.invalidate_history();
        // frame 0 (reset: builds the pyramid), frame 1 shifted by t
        for (int f = 0; f < 2; ++f) {
            auto enc = g_device.CreateCommandEncoder();
            scene.draw(enc, colorA.CreateView(), W, H, W, H, f ? t[4] : t[2], f ? t[5] : t[3], f * t[0], f * t[1]);
            flow.encode(enc, colorA.CreateView(), mv.CreateView(), false, f ? t[4] - t[2] : 0.f, f ? t[5] - t[3] : 0.f);
            auto cb = enc.Finish();
            g_queue.Submit(1, &cb);
        }
        std::vector<uint8_t> raw;
        readback(mv, W, H, 4, raw);
        const uint16_t* h = reinterpret_cast<const uint16_t*>(raw.data());
        std::vector<float> ex, ey;
        for (uint32_t y = 100; y < H - 100; y += 3)
            for (uint32_t x = 100; x < W - 100; x += 3) { ex.push_back(half2f(h[(size_t(y) * W + x) * 2])); ey.push_back(half2f(h[(size_t(y) * W + x) * 2 + 1])); }
        size_t good = 0;
        for (size_t i = 0; i < ex.size(); ++i) good += (std::abs(ex[i] - t[0]) < 0.25f && std::abs(ey[i] - t[1]) < 0.25f);
        std::sort(ex.begin(), ex.end()); std::sort(ey.begin(), ey.end());
        const float mx = ex[ex.size() / 2], my = ey[ey.size() / 2];
        const double frac = double(good) / double(ex.size());
        printf("  shift (%6.2f,%6.2f) jitter (%5.2f,%5.2f)->(%5.2f,%5.2f): median (%7.3f,%7.3f)  within 0.25px: %5.1f%%\n", t[0], t[1], t[2], t[3], t[4], t[5], mx, my, frac * 100);
        if (frac < 0.5 && std::abs(t[0]) < 60) ++bad;
    }
    printf("flow: %s\n", bad == 0 ? "PASS" : "FAIL (see above; shifts beyond ~55 px are out of range by design)");
    return bad == 0 ? 0 : 3;
}

int main(int argc, char** argv) {
    std::string modeArg = argc > 1 ? argv[1] : "quality";
    const int frames = argc > 2 ? atoi(argv[2]) : 40;
    const bool noMotion = argc > 3 && std::string(argv[3]) == "nomotion";
    dlss::Mode mode = modeArg == "dlaa" ? dlss::Mode::DLAA : modeArg == "balanced" ? dlss::Mode::Balanced : modeArg == "performance" ? dlss::Mode::Performance : modeArg == "ultra" ? dlss::Mode::UltraPerformance : dlss::Mode::Quality;

    wgpu::InstanceDescriptor idesc{};
    wgpu::InstanceFeatureName tw = wgpu::InstanceFeatureName::TimedWaitAny;
    idesc.requiredFeatureCount = 1; idesc.requiredFeatures = &tw;
    g_inst = wgpu::CreateInstance(&idesc);
    wgpu::RequestAdapterOptions ao{};
    ao.backendType = wgpu::BackendType::D3D12; ao.powerPreference = wgpu::PowerPreference::HighPerformance; ao.featureLevel = wgpu::FeatureLevel::Compatibility;
    g_inst.WaitAny(g_inst.RequestAdapter(&ao, wgpu::CallbackMode::WaitAnyOnly, [](wgpu::RequestAdapterStatus s, wgpu::Adapter a, wgpu::StringView) { if (s == wgpu::RequestAdapterStatus::Success) g_adapter = a; }), 5000000000ull);
    if (!g_adapter) { printf("no adapter\n"); return 1; }
    wgpu::AdapterInfo ainfo{};
    g_adapter.GetInfo(&ainfo);
    std::vector<wgpu::FeatureName> feats;
    const bool sharing = g_adapter.HasFeature(wgpu::FeatureName::SharedTextureMemoryDXGISharedHandle) && g_adapter.HasFeature(wgpu::FeatureName::SharedFenceDXGISharedHandle);
    if (sharing) { feats.push_back(wgpu::FeatureName::SharedTextureMemoryDXGISharedHandle); feats.push_back(wgpu::FeatureName::SharedFenceDXGISharedHandle); }
    if (g_adapter.HasFeature(wgpu::FeatureName::CoreFeaturesAndLimits)) feats.push_back(wgpu::FeatureName::CoreFeaturesAndLimits);
    wgpu::DeviceDescriptor dd{};
    dd.requiredFeatureCount = feats.size(); dd.requiredFeatures = feats.data();
    dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType t, wgpu::StringView m) { printf("  [Dawn error %d] %.*s\n", (int)t, (int)m.length, m.data); });
    g_inst.WaitAny(g_adapter.RequestDevice(&dd, wgpu::CallbackMode::WaitAnyOnly, [](wgpu::RequestDeviceStatus s, wgpu::Device d, wgpu::StringView m) { if (s == wgpu::RequestDeviceStatus::Success) g_device = d; else printf("RequestDevice: %.*s\n", (int)m.length, m.data); }), 5000000000ull);
    if (!g_device) return 1;
    g_queue = g_device.GetQueue();

    SceneRenderer scene;
    scene.init();
    int rc = testFlow(scene);

    char full[MAX_PATH];
    GetFullPathNameA("ready", MAX_PATH, full, nullptr);
    dlss::set_log([](int level, const char* msg) { if (level >= 1 || strstr(msg, "DLSS") || strstr(msg, "FSR")) printf("  [dlss %d] %s\n", level, msg); });
    dlss::start(g_device, ainfo, wgpu::BackendType::D3D12, sharing, full);
    for (int i = 0; i < 200 && dlss::status().state == dlss::State::Probing; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto st = dlss::status();
    printf("== DLSS status: state=%d version='%s' reason='%s'\n", (int)st.state, st.version.c_str(), st.reason.c_str());
    if (modeArg == "fsr" ? !dlss::fsr_ready() : st.state != dlss::State::Ready) return 4;

    const float vxEnv = getenv("DLSS_VX") ? (float)atof(getenv("DLSS_VX")) : 3.7f, vyEnv = getenv("DLSS_VY") ? (float)atof(getenv("DLSS_VY")) : -2.2f;
    const uint32_t OW = getenv("DLSS_OW") ? (uint32_t)atoi(getenv("DLSS_OW")) : 1920, OH = getenv("DLSS_OH") ? (uint32_t)atoi(getenv("DLSS_OH")) : 1080;
    const float scaleF = dlss::mode_scale_factor(mode);
    const uint32_t RW = uint32_t(std::lround(OW / scaleF)) + (getenv("DLSS_RWADJ") ? atoi(getenv("DLSS_RWADJ")) : 0), RH = uint32_t(std::lround(OH / scaleF)) + (getenv("DLSS_RWADJ") ? atoi(getenv("DLSS_RWADJ")) : 0);
    printf("== %s %s %s: %ux%u -> %ux%u, %d frames\n", modeArg == "fsr" ? "FSR" : "DLSS", dlss::mode_name(mode), noMotion ? "(zero motion vectors)" : "(flow motion vectors)", RW, RH, OW, OH, frames);
    dlss::set_motion_vectors_enabled(!noMotion);
    if (getenv("DLSS_TRUEMV")) dlss::debug_constant_motion(true, vxEnv / scaleF, vyEnv / scaleF);
    dlss::set_jitter_enabled(true, RW, OW);

    auto low = makeTex(std::max<uint32_t>(RW, uint32_t(std::lround(OW / 1.0f))), std::max<uint32_t>(RH, uint32_t(std::lround(OH / 1.0f))), wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding);
    auto lowRef = makeTex(RW, RH, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding);
    auto gt = makeTex(OW, OH, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc);
    auto base = makeTex(OW, OH, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc);

    // bilinear upscaler for the baseline
    std::string up = kUp;
    up.replace(up.find("OUTW"), 4, std::to_string(OW) + ".0"); up.replace(up.find("OUTH"), 4, std::to_string(OH) + ".0");
    wgpu::BindGroupLayoutEntry ble[2]{};
    ble[0].binding = 0; ble[0].visibility = wgpu::ShaderStage::Fragment; ble[0].sampler.type = wgpu::SamplerBindingType::Filtering;
    ble[1].binding = 1; ble[1].visibility = wgpu::ShaderStage::Fragment; ble[1].texture.sampleType = wgpu::TextureSampleType::Float; ble[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    wgpu::BindGroupLayoutDescriptor bld{}; bld.entryCount = 2; bld.entries = ble;
    auto upLayout = g_device.CreateBindGroupLayout(&bld);
    wgpu::PipelineLayoutDescriptor upl{}; upl.bindGroupLayoutCount = 1; upl.bindGroupLayouts = &upLayout;
    auto upm = module(up);
    wgpu::ColorTargetState uct{}; uct.format = wgpu::TextureFormat::RGBA8Unorm; uct.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState ufs{}; ufs.module = upm; ufs.entryPoint = "fs_main"; ufs.targetCount = 1; ufs.targets = &uct;
    wgpu::RenderPipelineDescriptor urp{}; urp.layout = g_device.CreatePipelineLayout(&upl); urp.vertex.module = upm; urp.vertex.entryPoint = "vs_main";
    urp.primitive.topology = wgpu::PrimitiveTopology::TriangleList; urp.multisample.count = 1; urp.multisample.mask = UINT32_MAX; urp.fragment = &ufs;
    auto upPipe = g_device.CreateRenderPipeline(&urp);
    wgpu::SamplerDescriptor sd{}; sd.magFilter = wgpu::FilterMode::Linear; sd.minFilter = wgpu::FilterMode::Linear;
    auto sampler = g_device.CreateSampler(&sd);

    // the sequence moves by (vx, vy) output pixels per frame
    const float vx = getenv("DLSS_VX") ? (float)atof(getenv("DLSS_VX")) : 3.7f, vy = getenv("DLSS_VY") ? (float)atof(getenv("DLSS_VY")) : -2.2f;
    double sumDlss = 0, sumBase = 0; int counted = 0;
    auto benchStart = std::chrono::steady_clock::now();
    std::vector<uint8_t> gtPix, outPix, basePix;
    const char* switchTo = getenv("DLSS_SWITCH");
    const int repeat = getenv("DLSS_REPEAT") ? atoi(getenv("DLSS_REPEAT")) : 1;
    uint32_t curRW = RW, curRH = RH;
    dlss::Mode curMode = mode;
    // render targets are allocated for the largest render size; each mode renders into their top-left corner
    for (int f = 0; f < frames; ++f) {
        if (switchTo && f == frames / 2) {
            curMode = std::string(switchTo) == "performance" ? dlss::Mode::Performance : std::string(switchTo) == "dlaa" ? dlss::Mode::DLAA : dlss::Mode::Quality;
            const float s2 = dlss::mode_scale_factor(curMode);
            curRW = uint32_t(std::lround(OW / s2)); curRH = uint32_t(std::lround(OH / s2));
            dlss::set_jitter_enabled(true, curRW, OW);
            printf("  -- frame %d: switching to %s %ux%u\n", f, dlss::mode_name(curMode), curRW, curRH);
        }
        for (int rep = 0; rep < repeat; ++rep) {
        dlss::advance_jitter(false);
        float jx, jy;
        dlss::current_jitter(jx, jy);
        const float shx = vx * f, shy = vy * f;
        auto enc = g_device.CreateCommandEncoder();
        scene.draw(enc, low.CreateView(), curRW, curRH, OW, OH, jx, jy, shx, shy);       // jittered render, what the game would produce
        dlss::DispatchParams dp;
        dp.fsr = modeArg == "fsr";
        dp.color = low.CreateView();
        dp.renderWidth = curRW; dp.renderHeight = curRH; dp.outputWidth = OW; dp.outputHeight = OH;
        dp.mode = curMode; dp.sharpness = 0.f; dp.reset = f == 0;
        dlss::Output out;
        if (!dlss::dispatch(enc, dp, out)) { printf("dispatch failed at frame %d\n", f); return 5; }
        // after dispatch `enc` is a new encoder ordered after DLSS; read the result back
        const bool bench = getenv("DLSS_BENCH") != nullptr;
        const bool readThis = !bench && f >= frames - 8 && rep == repeat - 1;
        if (readThis) {
            scene.draw(enc, gt.CreateView(), OW, OH, OW, OH, 0, 0, shx, shy, 6);       // ground truth at full resolution
            scene.draw(enc, lowRef.CreateView(), RW, RH, OW, OH, 0, 0, shx, shy);   // unjittered low-res for the baseline
            wgpu::BindGroupEntry be[2]{}; be[0].binding = 0; be[0].sampler = sampler; be[1].binding = 1; be[1].textureView = lowRef.CreateView();
            wgpu::BindGroupDescriptor bgd{}; bgd.layout = upLayout; bgd.entryCount = 2; bgd.entries = be;
            wgpu::RenderPassColorAttachment ca{}; ca.view = base.CreateView(); ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store; ca.clearValue = {0, 0, 0, 1};
            wgpu::RenderPassDescriptor rd{}; rd.colorAttachmentCount = 1; rd.colorAttachments = &ca;
            auto pass = enc.BeginRenderPass(&rd);
            pass.SetPipeline(upPipe); pass.SetBindGroup(0, g_device.CreateBindGroup(&bgd)); pass.Draw(3); pass.End();
        }
        auto cb = enc.Finish();
        g_queue.Submit(1, &cb);
        if (readThis) {
            readback(gt, OW, OH, 4, gtPix);
            readback(base, OW, OH, 4, basePix);
            readback(out.texture, OW, OH, 4, outPix);
            const double pd = psnr(outPix, gtPix, OW, OH, 16), pb = psnr(basePix, gtPix, OW, OH, 16);
            sumDlss += pd; sumBase += pb; ++counted;
            if (f == frames - 1) {
                printf("  frame %d: %s %.2f dB, bilinear %.2f dB\n", f, modeArg == "fsr" ? "FSR" : "DLSS", pd, pb);
                if (const char* dump = getenv("DLSS_DUMP")) {
                    const char* names[3] = {"gt", "dlss", "bilinear"};
                    const std::vector<uint8_t>* v[3] = {&gtPix, &outPix, &basePix};
                    for (int k = 0; k < 3; ++k) {
                        std::string fn = std::string(dump) + "_" + names[k] + ".ppm";
                        FILE* fp = fopen(fn.c_str(), "wb");
                        fprintf(fp, "P6\n%u %u\n255\n", OW, OH);
                        for (size_t i = 0; i < size_t(OW) * OH; ++i) fwrite(&(*v[k])[i * 4], 1, 3, fp);
                        fclose(fp);
                    }
                }
            }
        }
        dlss::after_submit();
        }
    }
    if (getenv("DLSS_BENCH")) {
        bool done = false;
        g_inst.WaitAny(g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly, [&](wgpu::QueueWorkDoneStatus, wgpu::StringView) { done = true; }), 10000000000ull);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - benchStart).count();
        printf("BENCH %s %ux%u -> %ux%u: %.2f ms/frame (scene render + flow + DLSS, %d frames, GPU-bound)\n", dlss::mode_name(mode), RW, RH, OW, OH, ms / frames, frames);
    }
    if (counted) printf("RESULT %s%s: mean PSNR over last %d frames: %.2f dB, bilinear low-res %.2f dB\n", dlss::mode_name(mode), noMotion ? " (no motion)" : "", counted, sumDlss / counted, sumBase / counted);
    dlss::shutdown();
    return rc;
}

// Standalone probe: Dawn (D3D12 backend, the same webgpu_dawn.dll the game uses) <-> private D3D12 device
// running DLSS Super Resolution through Streamline manual hooking, with textures shared through
// SharedTextureMemoryDXGISharedHandle and fences through SharedFenceDXGISharedHandle.
//
// Per frame: Dawn writes colour/depth/motion into shared textures, ends access (fence), the private queue waits
// for Dawn's fence, runs DLSS, signals its own shared fence, Dawn begins access on the output waiting on that
// fence, copies the result to a buffer and the program checks it.
//
// Build (from the repo root, mingw clang):
//   $d = "build/native/_deps/dawn_prebuilt-src"
//   clang++ -std=gnu++20 -O1 -DNOMINMAX -DWEBGPU_DAWN -w -Ithird_party/nvidia/streamline/include -isystem $d/include \
//       tools/dlss_probe/probe_dawn_share.cpp -o build/tmp_dlss/probe_dawn_share.exe $d/lib/webgpu_dawn.lib \
//       -ld3d12 -ldxgi -ldxguid -lole32 -static-libgcc
//   copy ready/webgpu_dawn.dll build/tmp_dlss/   (and dxcompiler.dll / dxil.dll not needed here)
// Run: build/tmp_dlss/probe_dawn_share.exe [debuglayer]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <webgpu/webgpu_cpp.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>

using Microsoft::WRL::ComPtr;

static HMODULE g_slMod = nullptr;
#define SL_TRAMPOLINE(name, fail) \
    static auto fn = g_slMod ? reinterpret_cast<decltype(&name)>(reinterpret_cast<void*>(GetProcAddress(g_slMod, #name))) : nullptr; \
    if (!fn) return fail
extern "C" {
sl::Result slInit(const sl::Preferences& pref, uint64_t sdkVersion) { SL_TRAMPOLINE(slInit, sl::Result::eErrorNotInitialized); return fn(pref, sdkVersion); }
sl::Result slShutdown() { SL_TRAMPOLINE(slShutdown, sl::Result::eErrorNotInitialized); return fn(); }
sl::Result slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo) { SL_TRAMPOLINE(slIsFeatureSupported, sl::Result::eErrorNotInitialized); return fn(feature, adapterInfo); }
sl::Result slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* resources, uint32_t numResources, sl::CommandBuffer* cmdBuffer) { SL_TRAMPOLINE(slSetTagForFrame, sl::Result::eErrorNotInitialized); return fn(frame, viewport, resources, numResources, cmdBuffer); }
sl::Result slSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) { SL_TRAMPOLINE(slSetConstants, sl::Result::eErrorNotInitialized); return fn(values, frame, viewport); }
sl::Result slGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version) { SL_TRAMPOLINE(slGetFeatureVersion, sl::Result::eErrorNotInitialized); return fn(feature, version); }
sl::Result slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer) { SL_TRAMPOLINE(slEvaluateFeature, sl::Result::eErrorNotInitialized); return fn(feature, frame, inputs, numInputs, cmdBuffer); }
sl::Result slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function) { SL_TRAMPOLINE(slGetFeatureFunction, sl::Result::eErrorNotInitialized); return fn(feature, functionName, function); }
sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex) { SL_TRAMPOLINE(slGetNewFrameToken, sl::Result::eErrorNotInitialized); return fn(token, frameIndex); }
sl::Result slSetD3DDevice(void* d3dDevice) { SL_TRAMPOLINE(slSetD3DDevice, sl::Result::eErrorNotInitialized); return fn(d3dDevice); }
sl::Result slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport) { SL_TRAMPOLINE(slFreeResources, sl::Result::eErrorNotInitialized); return fn(feature, viewport); }
}
static void slLog(sl::LogType t, const char* msg) { if (t != sl::LogType::eInfo) printf("  [SL %s] %s\n", t == sl::LogType::eError ? "ERR" : "WRN", msg); }

#define REQUIRE(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); return 2; } } while (0)

static wgpu::Instance g_inst;
static wgpu::Adapter g_adapter;
static wgpu::Device g_device;

struct Shared {
    ComPtr<ID3D12Resource> res;
    wgpu::SharedTextureMemory mem;
    wgpu::Texture tex;
    UINT w = 0, h = 0;
};

static HANDLE makeShared(ID3D12Device* dev, ID3D12DeviceChild* obj) {
    HANDLE h = nullptr;
    if (FAILED(dev->CreateSharedHandle(obj, nullptr, GENERIC_ALL, nullptr, &h))) return nullptr;
    return h;
}

static bool makeSharedTexture(ID3D12Device* dev, Shared& s, UINT w, UINT h, DXGI_FORMAT fmt, wgpu::TextureFormat wf, wgpu::TextureUsage usage, D3D12_RESOURCE_FLAGS extra) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_FLAGS fl = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | extra;
    D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, fmt, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, fl};
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.res)))) return false;
    HANDLE hnd = makeShared(dev, s.res.Get());
    if (!hnd) return false;
    wgpu::SharedTextureMemoryDXGISharedHandleDescriptor dx{};
    dx.handle = hnd;
    wgpu::SharedTextureMemoryDescriptor d{};
    d.nextInChain = &dx;
    s.mem = g_device.ImportSharedTextureMemory(&d);
    CloseHandle(hnd);
    if (!s.mem) return false;
    wgpu::TextureDescriptor td{};
    td.size = {w, h, 1};
    td.format = wf;
    td.usage = usage;
    s.tex = s.mem.CreateTexture(&td);
    s.w = w; s.h = h;
    return (bool)s.tex;
}

struct FenceCache {
    std::unordered_map<void*, ComPtr<ID3D12Fence>> map;
    ID3D12Device* dev = nullptr;
    ComPtr<ID3D12Fence> get(const wgpu::SharedFence& f) {
        auto it = map.find(f.Get());
        if (it != map.end()) return it->second;
        wgpu::SharedFenceDXGISharedHandleExportInfo ex{};
        wgpu::SharedFenceExportInfo info{};
        info.nextInChain = &ex;
        f.ExportInfo(&info);
        ComPtr<ID3D12Fence> fence;
        if (info.type == wgpu::SharedFenceType::DXGISharedHandle && ex.handle) dev->OpenSharedHandle(ex.handle, IID_PPV_ARGS(&fence));
        map[f.Get()] = fence;
        return fence;
    }
};

int main(int argc, char** argv) {
    const bool debugLayer = argc > 1 && std::string(argv[1]) == "debuglayer";
    if (debugLayer) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); printf("D3D12 debug layer on\n"); }
    }
    // ---- Dawn
    wgpu::InstanceDescriptor idesc{};
    wgpu::InstanceFeatureName tw = wgpu::InstanceFeatureName::TimedWaitAny;
    idesc.requiredFeatureCount = 1; idesc.requiredFeatures = &tw;
    g_inst = wgpu::CreateInstance(&idesc);
    REQUIRE(g_inst);
    wgpu::RequestAdapterOptions ao{};
    ao.backendType = wgpu::BackendType::D3D12;
    ao.powerPreference = wgpu::PowerPreference::HighPerformance;
    ao.featureLevel = wgpu::FeatureLevel::Compatibility;
    auto f1 = g_inst.RequestAdapter(&ao, wgpu::CallbackMode::WaitAnyOnly, [](wgpu::RequestAdapterStatus s, wgpu::Adapter a, wgpu::StringView m) { if (s == wgpu::RequestAdapterStatus::Success) g_adapter = a; });
    g_inst.WaitAny(f1, 5000000000ull);
    REQUIRE(g_adapter);
    wgpu::AdapterInfo ainfo{};
    g_adapter.GetInfo(&ainfo);
    printf("Dawn adapter: %.*s vendor=%04x device=%04x backend=%d\n", (int)ainfo.device.length, ainfo.device.data, ainfo.vendorID, ainfo.deviceID, (int)ainfo.backendType);
    const bool hasSTM = g_adapter.HasFeature(wgpu::FeatureName::SharedTextureMemoryDXGISharedHandle);
    const bool hasSF = g_adapter.HasFeature(wgpu::FeatureName::SharedFenceDXGISharedHandle);
    const bool hasD3D12Res = g_adapter.HasFeature(wgpu::FeatureName::SharedTextureMemoryD3D12Resource);
    printf("features: SharedTextureMemoryDXGISharedHandle=%d SharedFenceDXGISharedHandle=%d SharedTextureMemoryD3D12Resource=%d\n", hasSTM, hasSF, hasD3D12Res);
    REQUIRE(hasSTM && hasSF);
    std::vector<wgpu::FeatureName> feats{wgpu::FeatureName::SharedTextureMemoryDXGISharedHandle, wgpu::FeatureName::SharedFenceDXGISharedHandle};
    if (g_adapter.HasFeature(wgpu::FeatureName::CoreFeaturesAndLimits)) feats.push_back(wgpu::FeatureName::CoreFeaturesAndLimits);
    wgpu::DeviceDescriptor dd{};
    dd.requiredFeatureCount = feats.size(); dd.requiredFeatures = feats.data();
    dd.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType t, wgpu::StringView m) { printf("  [Dawn error %d] %.*s\n", (int)t, (int)m.length, m.data); });
    auto f2 = g_adapter.RequestDevice(&dd, wgpu::CallbackMode::WaitAnyOnly, [](wgpu::RequestDeviceStatus s, wgpu::Device d, wgpu::StringView m) { if (s == wgpu::RequestDeviceStatus::Success) g_device = d; else printf("RequestDevice: %.*s\n", (int)m.length, m.data); });
    g_inst.WaitAny(f2, 5000000000ull);
    REQUIRE(g_device);
    wgpu::Queue queue = g_device.GetQueue();

    // ---- Streamline (manual hooking) + private device on the same adapter
    char full[MAX_PATH];
    GetFullPathNameA("third_party/nvidia/streamline/bin/x64", MAX_PATH, full, nullptr);
    std::wstring wdir(full, full + strlen(full));
    g_slMod = LoadLibraryExW((wdir + L"\\sl.interposer.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    REQUIRE(g_slMod);
    static const wchar_t* paths[1] = {wdir.c_str()};
    static const sl::Feature feat[] = {sl::kFeatureDLSS};
    sl::Preferences pref{};
    pref.pathsToPlugins = paths; pref.numPathsToPlugins = 1;
    pref.logMessageCallback = slLog; pref.logLevel = sl::LogLevel::eDefault;
    pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eDisableCLStateTracking;
    pref.featuresToLoad = feat; pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom; pref.engineVersion = "1"; pref.projectId = "754abb53-63e1-4cba-9c0a-3f4e44b18da9";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    REQUIRE(slInit(pref, sl::kSDKVersion) == sl::Result::eOk);

    ComPtr<IDXGIFactory6> fac;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac))));
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0; SUCCEEDED(fac->EnumAdapters1(i, &ad)); ++i) {
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        if (d.VendorId == ainfo.vendorID && d.DeviceId == ainfo.deviceID) break;
        ad.Reset();
    }
    REQUIRE(ad);
    DXGI_ADAPTER_DESC1 desc{};
    ad->GetDesc1(&desc);
    ComPtr<ID3D12Device> dev;
    REQUIRE(SUCCEEDED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))));
    REQUIRE(slSetD3DDevice(dev.Get()) == sl::Result::eOk);
    LUID luid = desc.AdapterLuid;
    sl::AdapterInfo sai{};
    sai.deviceLUID = reinterpret_cast<uint8_t*>(&luid); sai.deviceLUIDSizeInBytes = sizeof(LUID);
    printf("slIsFeatureSupported(DLSS) = %d\n", (int)slIsFeatureSupported(sl::kFeatureDLSS, sai));

    const UINT outW = 1920, outH = 1080;
    sl::DLSSOptions opt{};
    opt.mode = sl::DLSSMode::eMaxQuality; opt.outputWidth = outW; opt.outputHeight = outH;
    sl::DLSSOptimalSettings os{};
    slDLSSGetOptimalSettings(opt, os);
    REQUIRE(slDLSSSetOptions(sl::ViewportHandle{0}, opt) == sl::Result::eOk);
    const UINT rw = os.optimalRenderWidth, rh = os.optimalRenderHeight;
    printf("Quality: %ux%u -> %ux%u\n", rw, rh, outW, outH);

    // ---- shared resources
    Shared color, depth, mvec, out;
    REQUIRE(makeSharedTexture(dev.Get(), color, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding, D3D12_RESOURCE_FLAG_NONE));
    REQUIRE(makeSharedTexture(dev.Get(), depth, rw, rh, DXGI_FORMAT_R32_FLOAT, wgpu::TextureFormat::R32Float, wgpu::TextureUsage::CopyDst, D3D12_RESOURCE_FLAG_NONE));
    REQUIRE(makeSharedTexture(dev.Get(), mvec, rw, rh, DXGI_FORMAT_R16G16_FLOAT, wgpu::TextureFormat::RG16Float, wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
    REQUIRE(makeSharedTexture(dev.Get(), out, outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::TextureBinding, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    printf("shared textures imported into Dawn\n");

    // DLSS->Dawn fence
    ComPtr<ID3D12Fence> myFence;
    REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&myFence))));
    HANDLE fh = makeShared(dev.Get(), myFence.Get());
    REQUIRE(fh);
    wgpu::SharedFenceDXGISharedHandleDescriptor fd{};
    fd.handle = fh;
    wgpu::SharedFenceDescriptor sfd{};
    sfd.nextInChain = &fd;
    wgpu::SharedFence dawnMyFence = g_device.ImportSharedFence(&sfd);
    CloseHandle(fh);
    REQUIRE(dawnMyFence);

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> q;
    REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))));
    ComPtr<ID3D12CommandAllocator> al;
    REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))));
    ComPtr<ID3D12GraphicsCommandList> cl;
    REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl))));
    cl->Close();
    ComPtr<ID3D12Fence> idleFence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idleFence));
    FenceCache fcache; fcache.dev = dev.Get();

    // readback buffer
    const UINT outRow = (outW * 4 + 255) & ~255u;
    wgpu::BufferDescriptor bdsc{};
    bdsc.size = size_t(outRow) * outH; bdsc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    wgpu::Buffer rb = g_device.CreateBuffer(&bdsc);

    // colour pattern (bright gradient + edges) written once per frame by Dawn
    std::vector<uint8_t> pix(size_t(rw) * rh * 4);
    for (UINT y = 0; y < rh; ++y)
        for (UINT x = 0; x < rw; ++x) {
            uint8_t* p = &pix[(size_t(y) * rw + x) * 4];
            bool chk = ((x / 24) ^ (y / 24)) & 1;
            p[0] = uint8_t(60 + x * 150 / rw); p[1] = uint8_t(60 + y * 150 / rh); p[2] = chk ? 230 : 40; p[3] = 255;
        }
    std::vector<uint16_t> mv(size_t(rw) * rh * 2, 0);
    std::vector<float> dp(size_t(rw) * rh, 0.5f);

    uint64_t fenceValue = 0;
    std::vector<std::pair<ComPtr<ID3D12Fence>, uint64_t>> pendingWaits;   // Dawn finished reading `out`
    bool allOk = true;
    double avgOut = 0, avgIn = 0;
    for (uint32_t frame = 0; frame < 6; ++frame) {
        // 1. Dawn: begin access on colour/depth/mvec (wait for the previous DLSS use), write, end access
        Shared* ins[3] = {&color, &depth, &mvec};
        for (auto* s : ins) {
            wgpu::SharedTextureMemoryBeginAccessDescriptor b{};
            b.initialized = frame > 0;
            wgpu::SharedFence fences[1] = {dawnMyFence};
            uint64_t vals[1] = {fenceValue};
            if (frame > 0) { b.fenceCount = 1; b.fences = fences; b.signaledValueCount = 1; b.signaledValues = vals; }
            if (s->mem.BeginAccess(s->tex, &b) != wgpu::Status::Success) { printf("BeginAccess(in) failed\n"); return 2; }
        }
        {
            wgpu::TexelCopyTextureInfo dst{};
            wgpu::TexelCopyBufferLayout lay{};
            dst.texture = color.tex; lay.bytesPerRow = rw * 4; lay.rowsPerImage = rh;
            wgpu::Extent3D ex{rw, rh, 1};
            queue.WriteTexture(&dst, pix.data(), pix.size(), &lay, &ex);
            dst.texture = depth.tex; lay.bytesPerRow = rw * 4;
            queue.WriteTexture(&dst, dp.data(), dp.size() * 4, &lay, &ex);
            dst.texture = mvec.tex; lay.bytesPerRow = rw * 4;
            queue.WriteTexture(&dst, mv.data(), mv.size() * 2, &lay, &ex);
        }
        std::vector<std::pair<ComPtr<ID3D12Fence>, uint64_t>> waits;
        for (auto* s : ins) {
            wgpu::SharedTextureMemoryEndAccessState es{};
            if (s->mem.EndAccess(s->tex, &es) != wgpu::Status::Success) { printf("EndAccess(in) failed\n"); return 2; }
            for (size_t i = 0; i < es.fenceCount; ++i) { auto f = fcache.get(es.fences[i]); if (f) waits.push_back({f, es.signaledValues[i]}); }
        }
        // 2. DLSS on the private queue
        for (auto& w : pendingWaits) q->Wait(w.first.Get(), w.second);
        pendingWaits.clear();
        for (auto& w : waits) q->Wait(w.first.Get(), w.second);
        al->Reset();
        cl->Reset(al.Get(), nullptr);
        sl::FrameToken* tok = nullptr;
        slGetNewFrameToken(tok, &frame);
        sl::Constants c{};
        for (int i = 0; i < 4; ++i) { sl::float4 row(i == 0, i == 1, i == 2, i == 3); c.cameraViewToClip.setRow(i, row); c.clipToCameraView.setRow(i, row); c.clipToPrevClip.setRow(i, row); c.prevClipToClip.setRow(i, row); c.clipToLensClip.setRow(i, row); }
        c.jitterOffset = sl::float2(frame & 1 ? 0.25f : -0.25f, 0.25f);
        c.mvecScale = sl::float2(1.f / rw, 1.f / rh);
        c.cameraPinholeOffset = sl::float2(0, 0);
        c.cameraPos = sl::float3(0, 0, 0); c.cameraUp = sl::float3(0, 1, 0); c.cameraRight = sl::float3(1, 0, 0); c.cameraFwd = sl::float3(0, 0, 1);
        c.cameraNear = 0.1f; c.cameraFar = 1000.f; c.cameraFOV = 1.0f; c.cameraAspectRatio = 16.f / 9.f;
        c.depthInverted = sl::Boolean::eFalse; c.cameraMotionIncluded = sl::Boolean::eTrue; c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = frame == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.orthographicProjection = sl::Boolean::eFalse; c.motionVectorsDilated = sl::Boolean::eFalse; c.motionVectorsJittered = sl::Boolean::eFalse;
        slSetConstants(c, *tok, sl::ViewportHandle{0});
        const uint32_t st = (uint32_t)D3D12_RESOURCE_STATE_COMMON;   // simultaneous-access resources
        sl::Resource rc{sl::ResourceType::eTex2d, color.res.Get(), nullptr, nullptr, st};
        sl::Resource rd{sl::ResourceType::eTex2d, depth.res.Get(), nullptr, nullptr, st};
        sl::Resource rm{sl::ResourceType::eTex2d, mvec.res.Get(), nullptr, nullptr, st};
        sl::Resource ro{sl::ResourceType::eTex2d, out.res.Get(), nullptr, nullptr, st};
        sl::Extent re{0, 0, rw, rh}, oe{0, 0, outW, outH};
        sl::ResourceTag tags[4] = {
            sl::ResourceTag{&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &re},
            sl::ResourceTag{&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &oe},
            sl::ResourceTag{&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &re},
            sl::ResourceTag{&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &re},
        };
        sl::Result r = slSetTagForFrame(*tok, sl::ViewportHandle{0}, tags, 4, cl.Get());
        const sl::ViewportHandle vp{0};
        const sl::BaseStructure* inputs[] = {&vp};
        sl::Result r2 = slEvaluateFeature(sl::kFeatureDLSS, *tok, inputs, 1, cl.Get());
        if (r != sl::Result::eOk || r2 != sl::Result::eOk) { printf("frame %u tag=%d eval=%d\n", frame, (int)r, (int)r2); allOk = false; }
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        q->ExecuteCommandLists(1, lists);
        q->Signal(myFence.Get(), ++fenceValue);

        // 3. Dawn: read the output after DLSS finished
        {
            wgpu::SharedTextureMemoryBeginAccessDescriptor b{};
            b.initialized = true; b.concurrentRead = false;
            wgpu::SharedFence fences[1] = {dawnMyFence};
            uint64_t vals[1] = {fenceValue};
            b.fenceCount = 1; b.fences = fences; b.signaledValueCount = 1; b.signaledValues = vals;
            if (out.mem.BeginAccess(out.tex, &b) != wgpu::Status::Success) { printf("BeginAccess(out) failed\n"); return 2; }
            wgpu::CommandEncoder enc = g_device.CreateCommandEncoder();
            wgpu::TexelCopyTextureInfo src{}; src.texture = out.tex;
            wgpu::TexelCopyBufferInfo dstb{}; dstb.buffer = rb; dstb.layout.bytesPerRow = outRow; dstb.layout.rowsPerImage = outH;
            wgpu::Extent3D ex{outW, outH, 1};
            enc.CopyTextureToBuffer(&src, &dstb, &ex);
            wgpu::CommandBuffer cb = enc.Finish();
            queue.Submit(1, &cb);
            bool done = false;
            auto mf = rb.MapAsync(wgpu::MapMode::Read, 0, bdsc.size, wgpu::CallbackMode::WaitAnyOnly, [&](wgpu::MapAsyncStatus s, wgpu::StringView) { done = s == wgpu::MapAsyncStatus::Success; });
            g_inst.WaitAny(mf, 10000000000ull);
            if (!done) { printf("map failed\n"); return 2; }
            const uint8_t* data = static_cast<const uint8_t*>(rb.GetConstMappedRange(0, bdsc.size));
            double sum = 0; size_t nz = 0;
            for (UINT y = 0; y < outH; y += 7) for (UINT x = 0; x < outW; x += 7) { const uint8_t* p = data + size_t(y) * outRow + x * 4; sum += p[0] + p[1] + p[2]; if (p[0] | p[1] | p[2]) ++nz; }
            avgOut = sum / (3.0 * ((outH + 6) / 7) * ((outW + 6) / 7));
            (void)nz;
            rb.Unmap();
            wgpu::SharedTextureMemoryEndAccessState es{};
            out.mem.EndAccess(out.tex, &es);
            for (size_t i = 0; i < es.fenceCount; ++i) { auto f = fcache.get(es.fences[i]); if (f) pendingWaits.push_back({f, es.signaledValues[i]}); }
        }
        printf("frame %u: output mean = %.1f\n", frame, avgOut);
    }
    double s = 0; for (size_t i = 0; i < pix.size(); i += 4) s += pix[i] + pix[i + 1] + pix[i + 2];
    avgIn = s / (3.0 * rw * rh);
    printf("input mean %.1f, last output mean %.1f\n", avgIn, avgOut);
    if (!(avgOut > 40.0 && avgOut < 220.0 && std::abs(avgOut - avgIn) < 40.0)) { printf("output looks wrong\n"); allOk = false; }
    // idle
    q->Signal(idleFence.Get(), 1);
    if (idleFence->GetCompletedValue() < 1) { HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr); idleFence->SetEventOnCompletion(1, e); WaitForSingleObject(e, 5000); CloseHandle(e); }
    slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle{0});
    slShutdown();
    printf("PROBE %s\n", allOk ? "PASS" : "FAIL");
    return allOk ? 0 : 3;
}

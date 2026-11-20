// Standalone NVIDIA Streamline / DLSS Super Resolution probe (no game, no Dawn).
// Proves: sl.interposer.dll loads dynamically, manual-hooking mode (native D3D12 device, no proxies and no
// swap chain) works, DLSS is supported on this GPU, optimal render sizes per mode, and that slEvaluateFeature
// runs on a plain private D3D12 queue.
//
// Build (from the repo root, mingw clang as used by the game build):
//   clang++ -std=gnu++20 -O1 -DNOMINMAX -Ithird_party/nvidia/streamline/include tools/dlss_probe/probe_sr.cpp \
//       -o build/tmp_dlss/probe_sr.exe -ld3d12 -ldxgi -ldxguid -lole32 -static
// Run:  build/tmp_dlss/probe_sr.exe [sl_dir]      (sl_dir defaults to third_party/nvidia/streamline/bin/x64)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
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

static void slLog(sl::LogType t, const char* msg) { printf("  [SL %s] %s", t == sl::LogType::eError ? "ERR" : t == sl::LogType::eWarn ? "WRN" : "inf", msg); if (!*msg || msg[strlen(msg) - 1] != '\n') printf("\n"); }

#define CHECK_HR(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAIL %s -> 0x%08lx (line %d)\n", #x, (unsigned long)_h, __LINE__); return 2; } } while (0)

static const char* modeName(sl::DLSSMode m) {
    switch (m) {
    case sl::DLSSMode::eMaxPerformance: return "Performance";
    case sl::DLSSMode::eBalanced: return "Balanced";
    case sl::DLSSMode::eMaxQuality: return "Quality";
    case sl::DLSSMode::eUltraPerformance: return "UltraPerformance";
    case sl::DLSSMode::eDLAA: return "DLAA";
    default: return "?";
    }
}

static ComPtr<ID3D12Resource> makeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, fmt, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "third_party/nvidia/streamline/bin/x64";
    char full[MAX_PATH];
    GetFullPathNameA(dir.c_str(), MAX_PATH, full, nullptr);
    std::wstring wdir(full, full + strlen(full));
    std::wstring dll = wdir + L"\\sl.interposer.dll";
    g_slMod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    printf("sl.interposer.dll: %s\n", g_slMod ? "loaded" : "LOAD FAILED");
    if (!g_slMod) return 1;

    static const wchar_t* paths[1] = {wdir.c_str()};
    static const sl::Feature feats[] = {sl::kFeatureDLSS};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = slLog;
    pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eDisableCLStateTracking;
    pref.featuresToLoad = feats;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1";
    pref.projectId = "754abb53-63e1-4cba-9c0a-3f4e44b18da9";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = slInit(pref, sl::kSDKVersion);
    printf("slInit(manual hooking) = %d\n", (int)r);
    if (r != sl::Result::eOk) return 1;

    ComPtr<IDXGIFactory6> fac;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
    ComPtr<IDXGIAdapter1> ad;
    CHECK_HR(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)));
    DXGI_ADAPTER_DESC1 ad1{};
    ad->GetDesc1(&ad1);
    printf("adapter: %ls vendor=%04x\n", ad1.Description, ad1.VendorId);
    ComPtr<ID3D12Device> dev;
    CHECK_HR(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    r = slSetD3DDevice(dev.Get());
    printf("slSetD3DDevice = %d\n", (int)r);

    LUID luid = ad1.AdapterLuid;
    sl::AdapterInfo ai{};
    ai.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    ai.deviceLUIDSizeInBytes = sizeof(LUID);
    r = slIsFeatureSupported(sl::kFeatureDLSS, ai);
    printf("slIsFeatureSupported(DLSS) = %d (0 = supported)\n", (int)r);
    sl::FeatureVersion fv{};
    slGetFeatureVersion(sl::kFeatureDLSS, fv);
    printf("versions: SL %u.%u.%u  NGX/DLSS %u.%u.%u\n", fv.versionSL.major, fv.versionSL.minor, fv.versionSL.build, fv.versionNGX.major, fv.versionNGX.minor, fv.versionNGX.build);

    const UINT outW = 3840, outH = 2160;
    const sl::DLSSMode modes[] = {sl::DLSSMode::eUltraPerformance, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eDLAA};
    for (auto m : modes) {
        sl::DLSSOptions o{};
        o.mode = m; o.outputWidth = outW; o.outputHeight = outH;
        sl::DLSSOptimalSettings s{};
        r = slDLSSGetOptimalSettings(o, s);
        printf("  %-16s -> render %ux%u (min %ux%u max %ux%u) sharp %.2f [%d]\n", modeName(m), s.optimalRenderWidth, s.optimalRenderHeight,
               s.renderWidthMin, s.renderHeightMin, s.renderWidthMax, s.renderHeightMax, s.optimalSharpness, (int)r);
    }

    // Evaluate Performance mode on a private direct queue.
    sl::DLSSOptions opt{};
    opt.mode = sl::DLSSMode::eMaxPerformance; opt.outputWidth = outW; opt.outputHeight = outH;
    opt.colorBuffersHDR = sl::Boolean::eFalse;
    sl::DLSSOptimalSettings os{};
    slDLSSGetOptimalSettings(opt, os);
    r = slDLSSSetOptions(sl::ViewportHandle{0}, opt);
    printf("slDLSSSetOptions = %d\n", (int)r);
    const UINT rw = os.optimalRenderWidth, rh = os.optimalRenderHeight;

    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> q;
    CHECK_HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    ComPtr<ID3D12CommandAllocator> al;
    CHECK_HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)));
    ComPtr<ID3D12GraphicsCommandList> cl;
    CHECK_HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl)));
    ComPtr<ID3D12Fence> fence;
    CHECK_HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

    auto color = makeTex(dev.Get(), rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto depth = makeTex(dev.Get(), rw, rh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto mvec = makeTex(dev.Get(), rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto out = makeTex(dev.Get(), outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!color || !depth || !mvec || !out) { printf("resource creation failed\n"); return 2; }

    // Upload a coloured gradient + checker as the colour input.
    const UINT rowBytes = (rw * 4 + 255) & ~255u;
    std::vector<uint8_t> up(size_t(rowBytes) * rh);
    for (UINT y = 0; y < rh; ++y)
        for (UINT x = 0; x < rw; ++x) {
            uint8_t* p = &up[size_t(y) * rowBytes + x * 4];
            bool chk = ((x / 32) ^ (y / 32)) & 1;
            p[0] = uint8_t(x * 255 / rw); p[1] = uint8_t(y * 255 / rh); p[2] = chk ? 220 : 30; p[3] = 255;
        }
    D3D12_HEAP_PROPERTIES uhp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, up.size(), 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> ub;
    CHECK_HR(dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ub)));
    void* mp = nullptr; ub->Map(0, nullptr, &mp); memcpy(mp, up.data(), up.size()); ub->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{ub.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, rw, rh, 1, rowBytes};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b[3] = {};
    ID3D12Resource* rs[3] = {color.Get(), depth.Get(), mvec.Get()};
    for (int i = 0; i < 3; ++i) { b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[i].Transition = {rs[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}; }
    cl->ResourceBarrier(3, b);

    const sl::ViewportHandle vp{0};
    float jitter[2] = {0.25f, -0.25f};
    double evalMs = 0;
    int okFrames = 0;
    for (uint32_t frame = 0; frame < 8; ++frame) {
        // frame 0 shares the open list with the upload; later frames use fresh lists
        if (frame > 0) {
            al->Reset();
            cl->Reset(al.Get(), nullptr);
        }
        sl::FrameToken* tok = nullptr;
        slGetNewFrameToken(tok, &frame);
        sl::Constants c{};
        c.cameraViewToClip = sl::float4x4{};
        c.clipToCameraView = sl::float4x4{};
        c.clipToPrevClip = sl::float4x4{};
        c.prevClipToClip = sl::float4x4{};
        for (int i = 0; i < 4; ++i) { c.cameraViewToClip.setRow(i, sl::float4(i == 0, i == 1, i == 2, i == 3)); c.clipToCameraView.setRow(i, sl::float4(i == 0, i == 1, i == 2, i == 3)); c.clipToPrevClip.setRow(i, sl::float4(i == 0, i == 1, i == 2, i == 3)); c.prevClipToClip.setRow(i, sl::float4(i == 0, i == 1, i == 2, i == 3)); }
        c.clipToLensClip = c.cameraViewToClip;
        c.jitterOffset = sl::float2(jitter[0], jitter[1]);
        c.mvecScale = sl::float2(1.0f / rw, 1.0f / rh);
        c.cameraNear = 0.1f; c.cameraFar = 1000.f; c.cameraFOV = 1.0f; c.cameraAspectRatio = 16.f / 9.f;
        c.depthInverted = sl::Boolean::eFalse;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = frame == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.orthographicProjection = sl::Boolean::eFalse;
        c.motionVectorsDilated = sl::Boolean::eFalse;
        c.motionVectorsJittered = sl::Boolean::eFalse;
        r = slSetConstants(c, *tok, vp);
        if (r != sl::Result::eOk) printf("slSetConstants=%d\n", (int)r);
        const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        sl::Resource rc{sl::ResourceType::eTex2d, color.Get(), nullptr, nullptr, (uint32_t)srv};
        sl::Resource rd{sl::ResourceType::eTex2d, depth.Get(), nullptr, nullptr, (uint32_t)srv};
        sl::Resource rm{sl::ResourceType::eTex2d, mvec.Get(), nullptr, nullptr, (uint32_t)srv};
        sl::Resource ro{sl::ResourceType::eTex2d, out.Get(), nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        sl::Extent re{0, 0, rw, rh}, oe{0, 0, outW, outH};
        sl::ResourceTag tags[4] = {
            sl::ResourceTag{&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &re},
            sl::ResourceTag{&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &oe},
            sl::ResourceTag{&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &re},
            sl::ResourceTag{&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &re},
        };
        r = slSetTagForFrame(*tok, vp, tags, 4, cl.Get());
        if (r != sl::Result::eOk) printf("slSetTagForFrame=%d\n", (int)r);
        const sl::BaseStructure* inputs[] = {&vp};
        r = slEvaluateFeature(sl::kFeatureDLSS, *tok, inputs, 1, cl.Get());
        if (r != sl::Result::eOk) printf("frame %u slEvaluateFeature=%d\n", frame, (int)r); else ++okFrames;
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        auto t0 = std::chrono::steady_clock::now();
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence.Get(), frame + 1);
        if (fence->GetCompletedValue() < frame + 1) { HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(frame + 1, e); WaitForSingleObject(e, 5000); CloseHandle(e); }
        auto t1 = std::chrono::steady_clock::now();
        if (frame >= 2) evalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        jitter[0] = -jitter[0];
    }
    printf("evaluate: %d/8 frames ok, avg %.2f ms (Performance, %ux%u -> %ux%u, incl. submit+wait)\n", okFrames, evalMs / 6.0, rw, rh, outW, outH);
    DXGI_ADAPTER_DESC1 d2{};
    ad->GetDesc1(&d2);
    slFreeResources(sl::kFeatureDLSS, vp);
    slShutdown();
    printf("PROBE %s\n", okFrames == 8 ? "PASS" : "FAIL");
    return okFrames == 8 ? 0 : 3;
}

// NVIDIA DLSS Super Resolution / DLAA through Streamline manual hooking. See dlss.hpp and docs/DLSS-NOTES.md.
#include "dlss.hpp"

#include "motion_flow.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <softpub.h>
#include <wintrust.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <chrono>
#define PLATFORM_WINDOWS 1
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>

using Microsoft::WRL::ComPtr;
using namespace std::string_view_literals;

// ---- Streamline entry points: trampolines into sl.interposer.dll (no import library is linked) -----------------
namespace {
HMODULE g_slModule = nullptr;
}
#define SL_TRAMPOLINE(name, fail)                                                                                    \
  static auto fn = g_slModule ? reinterpret_cast<decltype(&name)>(reinterpret_cast<void*>(GetProcAddress(g_slModule, #name))) : nullptr; \
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

namespace aurora::webgpu::dlss {

// ---- modes / logging / jitter -------------------------------------------------------------------------------
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
std::atomic<LogFn> g_logFn{nullptr};

void logf(int level, const char* fmt, ...) noexcept __attribute__((format(printf, 2, 3)));
void logf(int level, const char* fmt, ...) noexcept {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof buffer, fmt, args);
  va_end(args);
  log(level, buffer);
}

std::atomic<bool> g_jitterEnabled{false};
std::atomic<bool> g_motionEnabled{true};
std::atomic<bool> g_constantMotion{false};
std::atomic<float> g_constantMotionX{0.f}, g_constantMotionY{0.f};
std::atomic<uint32_t> g_phaseCount{8};
uint64_t g_jitterCounter = 0;
float g_currentJitter[2] = {0.f, 0.f};
float g_nativeJitter[2] = {0.f, 0.f};

float halton(uint32_t index, uint32_t base) noexcept {
  float f = 1.f, r = 0.f;
  for (uint32_t i = index; i > 0; i /= base) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(i % base);
  }
  return r;
}
} // namespace

void set_log(LogFn fn) noexcept { g_logFn.store(fn); }

void set_motion_vectors_enabled(bool enabled) noexcept { g_motionEnabled.store(enabled); }

void debug_constant_motion(bool enabled, float x, float y) noexcept {
  g_constantMotionX.store(x);
  g_constantMotionY.store(y);
  g_constantMotion.store(enabled);
}

void log(int level, const char* message) noexcept {
  if (const auto fn = g_logFn.load()) {
    fn(level, message);
  } else {
    fprintf(stderr, "[dlss %d] %s\n", level, message);
  }
}

void set_jitter_enabled(bool enabled, uint32_t renderWidth, uint32_t outputWidth) noexcept {
  g_jitterEnabled.store(enabled);
  if (renderWidth > 0 && outputWidth > 0) {
    const float ratio = static_cast<float>(outputWidth) / static_cast<float>(renderWidth);
    g_phaseCount.store(std::clamp(static_cast<uint32_t>(8.f * ratio * ratio), 8u, 128u));
  }
}

void advance_jitter(bool interpolatedSlot) noexcept {
  if (!g_jitterEnabled.load()) {
    g_currentJitter[0] = g_currentJitter[1] = 0.f;
    if (!interpolatedSlot) g_nativeJitter[0] = g_nativeJitter[1] = 0.f;
    return;
  }
  const uint32_t phase = static_cast<uint32_t>(g_jitterCounter++ % g_phaseCount.load()) + 1;
  g_currentJitter[0] = halton(phase, 2) - 0.5f;
  g_currentJitter[1] = halton(phase, 3) - 0.5f;
  if (!interpolatedSlot) {
    g_nativeJitter[0] = g_currentJitter[0];
    g_nativeJitter[1] = g_currentJitter[1];
  }
}

void restore_native_jitter() noexcept {
  g_currentJitter[0] = g_nativeJitter[0];
  g_currentJitter[1] = g_nativeJitter[1];
}

void current_jitter(float& x, float& y) noexcept {
  x = g_currentJitter[0];
  y = g_currentJitter[1];
}

// ---- runtime state -------------------------------------------------------------------------------------------
namespace {

struct Runtime {
  std::mutex mutex; // guards status strings
  std::atomic<State> state{State::NotStarted};
  std::string reason;
  std::string version;
  std::thread initThread;

  wgpu::Device dawn;
  wgpu::Queue dawnQueue;
  std::wstring dllDir;

  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence; // shared with Dawn; value = completed DLSS submissions
  ComPtr<ID3D12CommandAllocator> allocator[3];
  uint64_t allocatorFence[3] = {0, 0, 0};
  ComPtr<ID3D12GraphicsCommandList> list;
  wgpu::SharedFence dawnFence; // `fence` imported into Dawn
  uint64_t signaled = 0;
  uint32_t frameIndex = 0;
  HMODULE fsrModule = nullptr;
  ffxFunctions ffx{};
  std::atomic<bool> fsrAvailable{false};
  bool slLoaded = false;
  bool dlssFeature = false;
  ~Runtime() {
    // a joinable std::thread must not be destroyed (shutdown() normally joins it)
    if (initThread.joinable()) initThread.join();
  }
};
Runtime g_rt;

void set_state(State state, const std::string& reason, const std::string& version = {}) {
  {
    std::lock_guard lock{g_rt.mutex};
    g_rt.reason = reason;
    if (!version.empty()) g_rt.version = version;
  }
  g_rt.state.store(state, std::memory_order_release);
}

void sl_log(sl::LogType type, const char* message) {
  // Streamline is chatty at start-up (about 60 info lines); only warnings and errors unless AURORA_DLSS_VERBOSE is set.
  static const bool verbose = getenv("AURORA_DLSS_VERBOSE") != nullptr;
  if (type == sl::LogType::eInfo && !verbose) return;
  logf(type == sl::LogType::eError ? 2 : type == sl::LogType::eWarn ? 1 : 0, "[Streamline] %s", message);
}

std::wstring executable_dir() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s(path, n);
  const auto slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

bool file_exists(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string narrow(const std::wstring& w) {
  std::string s;
  s.reserve(w.size());
  for (wchar_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  return s;
}

// WinVerifyTrust embedded-signature check (sl_security.h cannot be compiled with mingw: no WINTRUST_SIGNATURE_SETTINGS).
bool verify_signature(const std::wstring& path) {
  const HMODULE wintrust = LoadLibraryW(L"wintrust.dll");
  if (!wintrust) return false;
  using WinVerifyTrustFn = LONG(WINAPI*)(HWND, GUID*, LPVOID);
  const auto verify = reinterpret_cast<WinVerifyTrustFn>(reinterpret_cast<void*>(GetProcAddress(wintrust, "WinVerifyTrust")));
  bool ok = false;
  if (verify) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOCATION_CHECK_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    ok = verify(nullptr, &action, &data) == 0;
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    verify(nullptr, &action, &data);
  }
  FreeLibrary(wintrust);
  return ok;
}

const char* sl_result_text(sl::Result r) {
  switch (r) {
  case sl::Result::eErrorOSOutOfDate: return "Windows is too old for DLSS";
  case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old for DLSS";
  case sl::Result::eErrorNoSupportedAdapterFound:
  case sl::Result::eErrorAdapterNotSupported: return "this GPU does not support DLSS (NVIDIA RTX GPU required)";
  case sl::Result::eErrorOSDisabledHWS: return "enable hardware-accelerated GPU scheduling in Windows";
  case sl::Result::eErrorMissingOrInvalidAPI:
  case sl::Result::eErrorFeatureMissing: return "nvngx_dlss.dll or sl.dlss.dll is missing";
  default: return "Streamline reported an error";
  }
}

void init_streamline(uint32_t vendorId, uint32_t deviceId, LUID adapterLuid) {
  const std::wstring& dir = g_rt.dllDir;
  std::string missing;
  for (const wchar_t* name : {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"nvngx_dlss.dll"}) {
    if (!file_exists(dir + L"\\" + name)) missing += (missing.empty() ? "" : ", ") + narrow(name);
  }
  if (!missing.empty()) {
    return set_state(State::Unavailable, "NVIDIA RTX GPU and nvngx_dlss.dll required (missing: " + missing + ")");
  }
  const std::wstring interposer = dir + L"\\sl.interposer.dll";
  if (!verify_signature(interposer)) return set_state(State::Unavailable, "sl.interposer.dll is not signed by NVIDIA");
  g_slModule = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!g_slModule) return set_state(State::Unavailable, "sl.interposer.dll could not be loaded");

  static std::wstring pluginDir;
  pluginDir = dir;
  static const wchar_t* pluginPaths[1];
  pluginPaths[0] = pluginDir.c_str();
  static const sl::Feature features[] = {sl::kFeatureDLSS};
  sl::Preferences pref{};
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.pathsToPlugins = pluginPaths;
  pref.numPathsToPlugins = 1;
  pref.logMessageCallback = sl_log;
  // Manual hooking: Dawn owns the device and swap chain. No command-list state tracking, no downloaded (OTA) plugins.
  pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
               sl::PreferenceFlags::eDisableCLStateTracking;
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = 1;
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "1";
  pref.projectId = "754abb53-63e1-4cba-9c0a-3f4e44b18da9";
  pref.renderAPI = sl::RenderAPI::eD3D12;
  if (const auto r = slInit(pref, sl::kSDKVersion); r != sl::Result::eOk) {
    char b[96];
    snprintf(b, sizeof b, "Streamline failed to start (%d)", static_cast<int>(r));
    return set_state(State::Unavailable, b);
  }
  g_rt.slLoaded = true;

  if (const auto r = slSetD3DDevice(g_rt.device.Get()); r != sl::Result::eOk) {
    return set_state(State::Failed, "slSetD3DDevice failed");
  }
  LUID luid = adapterLuid;
  sl::AdapterInfo sai{};
  sai.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
  sai.deviceLUIDSizeInBytes = sizeof(LUID);
  if (const auto r = slIsFeatureSupported(sl::kFeatureDLSS, sai); r != sl::Result::eOk) {
    return set_state(State::Unavailable, sl_result_text(r));
  }
  g_rt.dlssFeature = true;

  sl::FeatureVersion fv{};
  slGetFeatureVersion(sl::kFeatureDLSS, fv);
  char version[96];
  snprintf(version, sizeof version, "Streamline %u.%u.%u / DLSS %u.%u.%u", fv.versionSL.major, fv.versionSL.minor,
           fv.versionSL.build, fv.versionNGX.major, fv.versionNGX.minor, fv.versionNGX.build);
  logf(0, "DLSS available: %s on adapter %04x:%04x", version, vendorId, deviceId);
  set_state(State::Ready, {}, version);
}

void init_thread_main(uint32_t vendorId, uint32_t deviceId, bool backendOk, bool sharingOk) {
  using Fn1 = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  using Fn2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
  if (!backendOk) return set_state(State::Unavailable, "DLSS needs the Direct3D 12 graphics backend");
  if (!sharingOk) return set_state(State::Unavailable, "the GPU device lacks Direct3D 12 resource sharing support");

  const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  const auto createFactory = dxgi ? reinterpret_cast<Fn2>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2"))) : nullptr;
  const auto createDevice = d3d12 ? reinterpret_cast<Fn1>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"))) : nullptr;
  if (!createFactory || !createDevice) return set_state(State::Failed, "Direct3D 12 is not available");
  ComPtr<IDXGIFactory4> factory;
  if (FAILED(createFactory(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(factory.GetAddressOf())))) {
    return set_state(State::Failed, "DXGI factory creation failed");
  }
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);
    if (d.VendorId == vendorId && d.DeviceId == deviceId) break;
    adapter.Reset();
  }
  if (!adapter) return set_state(State::Failed, "the Dawn GPU was not found in DXGI");
  DXGI_ADAPTER_DESC1 desc{};
  adapter->GetDesc1(&desc);
  if (FAILED(createDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                          reinterpret_cast<void**>(g_rt.device.GetAddressOf())))) {
    return set_state(State::Failed, "second Direct3D 12 device creation failed");
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(g_rt.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(g_rt.queue.GetAddressOf()))) ||
      FAILED(g_rt.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void**>(g_rt.fence.GetAddressOf())))) {
    return set_state(State::Failed, "Direct3D 12 queue creation failed");
  }
  for (auto& a : g_rt.allocator) {
    if (FAILED(g_rt.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(a.GetAddressOf())))) {
      return set_state(State::Failed, "Direct3D 12 allocator creation failed");
    }
  }
  if (FAILED(g_rt.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_rt.allocator[0].Get(), nullptr,
                                            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(g_rt.list.GetAddressOf())))) {
    return set_state(State::Failed, "Direct3D 12 command list creation failed");
  }
  g_rt.list->Close();

  g_rt.fsrModule = LoadLibraryExW((g_rt.dllDir + L"\\amd_fidelityfx_loader_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (g_rt.fsrModule) {
    ffxLoadFunctions(&g_rt.ffx, g_rt.fsrModule);
    g_rt.fsrAvailable.store(g_rt.ffx.CreateContext && g_rt.ffx.DestroyContext && g_rt.ffx.Dispatch);
  }
  if (vendorId == 0x10DE) init_streamline(vendorId, deviceId, desc.AdapterLuid);
  else set_state(State::Unavailable, "DLSS requires an NVIDIA RTX GPU");
}

// ---- Dawn side pipelines ---------------------------------------------------------------------------------------
constexpr std::string_view kBlitShader = R"(
@group(0) @binding(0) var srcT: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4<f32> {
    var q = array<vec2<f32>, 3>(vec2<f32>(-1.0, 1.0), vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0));
    return vec4<f32>(q[idx], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let d = vec2<i32>(textureDimensions(srcT));
    let c = textureLoad(srcT, clamp(vec2<i32>(pos.xy), vec2<i32>(0), d - vec2<i32>(1)), 0);
    return vec4<f32>(c.rgb, 1.0);
}
)"sv;

constexpr std::string_view kDepthShader = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
struct Mapping { rect: vec4f, convention: vec4f };
@group(0) @binding(1) var<uniform> mapping: Mapping;
@vertex fn vs_main(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4f {
    var q = array<vec2f,3>(vec2f(-1,1), vec2f(-1,-3), vec2f(3,1));
    return vec4f(q[idx], 0, 1);
}
@fragment fn fs_main(@builtin(position) p: vec4f) -> @location(0) f32 {
    let xy = vec2i(mapping.rect.xy + p.xy * mapping.rect.zw);
    let z = textureLoad(src, clamp(xy, vec2i(0), vec2i(textureDimensions(src))-1), 0).r;
    return select(z, 1.0-z, mapping.convention.x > 0.0);
}
)"sv;

// Contrast adaptive sharpening (the published CAS algorithm: weights from local min/max, cross pattern).
constexpr std::string_view kCasShader = R"(
struct U { sharp: f32, pad0: f32, pad1: f32, pad2: f32 };
@group(0) @binding(0) var srcT: texture_2d<f32>;
@group(0) @binding(1) var<uniform> u: U;

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> @builtin(position) vec4<f32> {
    var q = array<vec2<f32>, 3>(vec2<f32>(-1.0, 1.0), vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0));
    return vec4<f32>(q[idx], 0.0, 1.0);
}

fn ld(p: vec2<i32>, o: vec2<i32>) -> vec3<f32> {
    let d = vec2<i32>(textureDimensions(srcT));
    return textureLoad(srcT, clamp(p + o, vec2<i32>(0), d - vec2<i32>(1)), 0).rgb;
}

@fragment
fn fs_main(@builtin(position) pos: vec4<f32>) -> @location(0) vec4<f32> {
    let p = vec2<i32>(pos.xy);
    let e = ld(p, vec2<i32>(0, 0));
    let b = ld(p, vec2<i32>(0, -1));
    let d = ld(p, vec2<i32>(-1, 0));
    let f = ld(p, vec2<i32>(1, 0));
    let h = ld(p, vec2<i32>(0, 1));
    let mn = min(min(min(d, e), min(f, b)), h);
    let mx = max(max(max(d, e), max(f, b)), h);
    let amp = sqrt(clamp(min(mn, vec3<f32>(1.0) - mx) / max(mx, vec3<f32>(1e-4)), vec3<f32>(0.0), vec3<f32>(1.0)));
    let peak = -1.0 / mix(8.0, 5.0, clamp(u.sharp, 0.0, 1.0));
    let w = amp * peak;
    let rw = vec3<f32>(1.0) / (vec3<f32>(1.0) + 4.0 * w);
    return vec4<f32>(clamp((w * (b + d + f + h) + e) * rw, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
)"sv;

struct DawnPipelines {
  bool built = false;
  bool failed = false;
  wgpu::BindGroupLayout blitLayout, casLayout;
  wgpu::BindGroupLayout depthLayout;
  wgpu::Buffer depthUniform;
  wgpu::RenderPipeline depthCopy;
  wgpu::RenderPipeline blit;
  wgpu::RenderPipeline cas;
  wgpu::Buffer casUniform;
  wgpu::Sampler sampler;
};
DawnPipelines g_pipes;

wgpu::ShaderModule make_module(std::string_view code, const char* label) {
  wgpu::ShaderSourceWGSL source{};
  source.code = code;
  const wgpu::ShaderModuleDescriptor descriptor{.nextInChain = &source, .label = label};
  return g_rt.dawn.CreateShaderModule(&descriptor);
}

wgpu::RenderPipeline make_pipeline(const wgpu::ShaderModule& module, const wgpu::BindGroupLayout& layout,
                                   wgpu::TextureFormat format, const char* label) {
  const wgpu::PipelineLayoutDescriptor pl{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout};
  const std::array targets{wgpu::ColorTargetState{.format = format, .writeMask = wgpu::ColorWriteMask::All}};
  const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main", .targetCount = targets.size(),
                                     .targets = targets.data()};
  const wgpu::RenderPipelineDescriptor d{
      .label = label,
      .layout = g_rt.dawn.CreatePipelineLayout(&pl),
      .vertex = wgpu::VertexState{.module = module, .entryPoint = "vs_main"},
      .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = wgpu::MultisampleState{.count = 1, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return g_rt.dawn.CreateRenderPipeline(&d);
}

bool ensure_pipelines() {
  if (g_pipes.built) return true;
  if (g_pipes.failed) return false;
  const wgpu::BindGroupLayoutEntry tex{.binding = 0, .visibility = wgpu::ShaderStage::Fragment,
                                       .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float,
                                                                             .viewDimension = wgpu::TextureViewDimension::e2D}};
  const std::array blitEntries{tex};
  const wgpu::BindGroupLayoutDescriptor bd{.entryCount = blitEntries.size(), .entries = blitEntries.data()};
  g_pipes.blitLayout = g_rt.dawn.CreateBindGroupLayout(&bd);
  const std::array casEntries{
      tex, wgpu::BindGroupLayoutEntry{.binding = 1, .visibility = wgpu::ShaderStage::Fragment,
                                      .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Uniform}}};
  const wgpu::BindGroupLayoutDescriptor cd{.entryCount = casEntries.size(), .entries = casEntries.data()};
  g_pipes.casLayout = g_rt.dawn.CreateBindGroupLayout(&cd);
  g_pipes.blit = make_pipeline(make_module(kBlitShader, "dlss blit"), g_pipes.blitLayout, wgpu::TextureFormat::RGBA8Unorm, "dlss input blit");
  g_pipes.cas = make_pipeline(make_module(kCasShader, "dlss cas"), g_pipes.casLayout, wgpu::TextureFormat::RGBA8Unorm, "dlss sharpen");
  const wgpu::BufferDescriptor ub{.label = "dlss cas uniform", .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = 16};
  auto depthEntries = casEntries;
  depthEntries[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
  const wgpu::BindGroupLayoutDescriptor depthLayout{.entryCount = depthEntries.size(), .entries = depthEntries.data()};
  g_pipes.depthLayout = g_rt.dawn.CreateBindGroupLayout(&depthLayout);
  auto depthUniform = ub;
  depthUniform.size = 32;
  g_pipes.depthUniform = g_rt.dawn.CreateBuffer(&depthUniform);
  g_pipes.depthCopy = make_pipeline(make_module(kDepthShader, "Temporal depth"), g_pipes.depthLayout,
                                    wgpu::TextureFormat::R32Float, "Temporal scene depth");
  g_pipes.casUniform = g_rt.dawn.CreateBuffer(&ub);
  const wgpu::SamplerDescriptor sd{.label = "dlss output sampler", .addressModeU = wgpu::AddressMode::ClampToEdge,
                                   .addressModeV = wgpu::AddressMode::ClampToEdge, .addressModeW = wgpu::AddressMode::ClampToEdge,
                                   .magFilter = wgpu::FilterMode::Linear, .minFilter = wgpu::FilterMode::Linear};
  g_pipes.sampler = g_rt.dawn.CreateSampler(&sd);
  if (!g_pipes.depthCopy || !g_pipes.depthUniform || !g_pipes.blit || !g_pipes.cas || !g_pipes.casUniform || !g_pipes.sampler) {
    g_pipes.failed = true;
    log(2, "DLSS: Dawn pipeline creation failed");
    return false;
  }
  g_pipes.built = true;
  return true;
}

// ---- shared textures -------------------------------------------------------------------------------------------
struct SharedTex {
  ComPtr<ID3D12Resource> resource;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureView view;
  bool everUsed = false;
  bool inAccess = false;
};

bool make_shared_tex(SharedTex& s, uint32_t w, uint32_t h, DXGI_FORMAT dxgi, wgpu::TextureFormat format,
                     wgpu::TextureUsage usage, D3D12_RESOURCE_FLAGS flags, const char* label) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = dxgi;
  rd.SampleDesc = {1, 0};
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | flags;
  if (FAILED(g_rt.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  __uuidof(ID3D12Resource), reinterpret_cast<void**>(s.resource.GetAddressOf())))) {
    return false;
  }
  HANDLE handle = nullptr;
  if (FAILED(g_rt.device->CreateSharedHandle(s.resource.Get(), nullptr, GENERIC_ALL, nullptr, &handle)) || !handle) return false;
  wgpu::SharedTextureMemoryDXGISharedHandleDescriptor dx{};
  dx.handle = handle;
  const wgpu::SharedTextureMemoryDescriptor md{.nextInChain = &dx, .label = label};
  s.memory = g_rt.dawn.ImportSharedTextureMemory(&md);
  CloseHandle(handle);
  if (!s.memory) return false;
  const wgpu::TextureDescriptor td{.label = label, .usage = usage, .size = {w, h, 1}, .format = format};
  s.texture = s.memory.CreateTexture(&td);
  if (!s.texture) return false;
  s.view = s.texture.CreateView();
  return true;
}

struct FenceWait {
  ComPtr<ID3D12Fence> fence;
  uint64_t value = 0;
};

std::unordered_map<void*, ComPtr<ID3D12Fence>> g_dawnFences; // Dawn's queue fences opened on the second device

ComPtr<ID3D12Fence> open_dawn_fence(const wgpu::SharedFence& f) {
  if (const auto it = g_dawnFences.find(f.Get()); it != g_dawnFences.end()) return it->second;
  wgpu::SharedFenceDXGISharedHandleExportInfo exportInfo{};
  wgpu::SharedFenceExportInfo info{};
  info.nextInChain = &exportInfo;
  f.ExportInfo(&info);
  ComPtr<ID3D12Fence> fence;
  if (info.type == wgpu::SharedFenceType::DXGISharedHandle && exportInfo.handle) {
    g_rt.device->OpenSharedHandle(exportInfo.handle, __uuidof(ID3D12Fence), reinterpret_cast<void**>(fence.GetAddressOf()));
  }
  g_dawnFences[f.Get()] = fence;
  return fence;
}

bool begin_access(SharedTex& s, bool waitForDlss) {
  wgpu::SharedTextureMemoryBeginAccessDescriptor b{};
  b.initialized = s.everUsed;
  wgpu::SharedFence fences[1] = {g_rt.dawnFence};
  uint64_t values[1] = {g_rt.signaled};
  if (waitForDlss && g_rt.signaled > 0) {
    b.fenceCount = 1;
    b.fences = fences;
    b.signaledValueCount = 1;
    b.signaledValues = values;
  }
  if (s.memory.BeginAccess(s.texture, &b) != wgpu::Status::Success) return false;
  s.inAccess = true;
  return true;
}

void end_access(SharedTex& s, std::vector<FenceWait>& waits) {
  if (!s.inAccess) return;
  wgpu::SharedTextureMemoryEndAccessState state{};
  s.memory.EndAccess(s.texture, &state);
  s.inAccess = false;
  s.everUsed = true;
  for (size_t i = 0; i < state.fenceCount; ++i) {
    if (auto f = open_dawn_fence(state.fences[i])) waits.push_back({f, state.signaledValues[i]});
  }
}

// ---- per-configuration instance --------------------------------------------------------------------------------
struct Instance {
  uint32_t rw = 0, rh = 0, ow = 0, oh = 0; // ow/oh: the DLSS output size actually used (see create_instance)
  uint32_t reqOw = 0, reqOh = 0; // output size the caller asked for
  Mode mode = Mode::Off;
  ffxContext fsrContext{};
  std::chrono::steady_clock::time_point lastFrame{};
  SharedTex color, depth, mv, out;
  wgpu::Texture sharpTex;
  wgpu::TextureView sharpView;
  MotionFlow flow;
  bool hadSceneDepth = false;
  bool needReset = true;
  bool optionsSet = false;
  float prevJitter[2] = {0.f, 0.f};
  std::vector<FenceWait> outReadWaits; // Dawn's reads of `out` from the previous present
  bool outPending = false;
  uint64_t evaluations = 0;
};
std::unique_ptr<Instance> g_instance;
struct ConfigKey {
  uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
  Mode mode = Mode::Off;
  bool fsr = false;
  bool operator==(const ConfigKey&) const = default;
};
bool g_failed = false;       // the current configuration failed; retried when the configuration changes
ConfigKey g_failedKey;
ConfigKey g_currentKey;
sl::ViewportHandle kViewport{0};

void wait_idle() {
  if (!g_rt.queue || !g_rt.fence) return;
  if (g_rt.fence->GetCompletedValue() >= g_rt.signaled) return;
  const HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_rt.fence->SetEventOnCompletion(g_rt.signaled, e);
  WaitForSingleObject(e, 2000);
  CloseHandle(e);
}

void destroy_instance() {
  if (!g_instance) return;
  wait_idle();
  if (g_instance->outPending) {
    std::vector<FenceWait> ignore;
    end_access(g_instance->out, ignore);
  }
  if (g_instance->fsrContext) g_rt.ffx.DestroyContext(&g_instance->fsrContext, nullptr);
  else if (g_rt.slLoaded) slFreeResources(sl::kFeatureDLSS, kViewport);
  g_instance.reset();
}

sl::DLSSMode sl_mode(Mode m) {
  switch (m) {
  case Mode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
  case Mode::Performance: return sl::DLSSMode::eMaxPerformance;
  case Mode::Balanced: return sl::DLSSMode::eBalanced;
  case Mode::Quality: return sl::DLSSMode::eMaxQuality;
  default: return sl::DLSSMode::eDLAA;
  }
}

bool create_instance(const DispatchParams& p) {
  auto inst = std::make_unique<Instance>();
  // Modes with a fixed ratio (Ultra Performance) accept exactly one render size per output size, so a 1 px rounding
  // difference between the window code and DLSS would reject the frame. Look for a nearby output size DLSS accepts for this
  // render size; the present blit resamples the sub-percent difference.
  uint32_t aow = p.outputWidth, aoh = p.outputHeight;
  if (!p.fsr) {
    sl::DLSSOptions probe{};
    probe.mode = sl_mode(p.mode);
    const auto fits = [&](uint32_t w, uint32_t h) {
      probe.outputWidth = w;
      probe.outputHeight = h;
      sl::DLSSOptimalSettings s{};
      return slDLSSGetOptimalSettings(probe, s) == sl::Result::eOk && p.renderWidth >= s.renderWidthMin &&
             p.renderWidth <= s.renderWidthMax && p.renderHeight >= s.renderHeightMin && p.renderHeight <= s.renderHeightMax;
    };
    if (!fits(aow, aoh)) {
      bool found = false;
      for (int d = 1; d <= 4 && !found; ++d) {
        for (int dy = -d; dy <= d && !found; ++dy) {
          for (int dx = -d; dx <= d && !found; ++dx) {
            if (std::max(std::abs(dx), std::abs(dy)) != d) continue;
            const int64_t w = static_cast<int64_t>(p.outputWidth) + dx, h = static_cast<int64_t>(p.outputHeight) + dy;
            if (w < static_cast<int64_t>(p.renderWidth) || h < static_cast<int64_t>(p.renderHeight)) continue;
            if (fits(static_cast<uint32_t>(w), static_cast<uint32_t>(h))) {
              aow = static_cast<uint32_t>(w);
              aoh = static_cast<uint32_t>(h);
              found = true;
            }
          }
        }
      }
      if (found) {
        logf(0, "DLSS %s: output %ux%u adjusted to %ux%u for render size %ux%u", mode_name(p.mode), p.outputWidth,
             p.outputHeight, aow, aoh, p.renderWidth, p.renderHeight);
      }
    }
  }
  inst->rw = p.renderWidth;
  inst->rh = p.renderHeight;
  inst->reqOw = p.outputWidth;
  inst->reqOh = p.outputHeight;
  inst->ow = aow;
  inst->oh = aoh;
  inst->mode = p.mode;
  using U = wgpu::TextureUsage;
  const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const bool ok =
      make_shared_tex(inst->color, p.renderWidth, p.renderHeight, DXGI_FORMAT_R8G8B8A8_UNORM, wgpu::TextureFormat::RGBA8Unorm,
                      U::RenderAttachment | U::TextureBinding, rt, "DLSS color") &&
      make_shared_tex(inst->depth, p.renderWidth, p.renderHeight, DXGI_FORMAT_R32_FLOAT, wgpu::TextureFormat::R32Float,
                      U::RenderAttachment, rt, "DLSS depth") &&
      make_shared_tex(inst->mv, p.renderWidth, p.renderHeight, DXGI_FORMAT_R16G16_FLOAT, wgpu::TextureFormat::RG16Float,
                      U::RenderAttachment | U::TextureBinding | U::CopySrc, rt, "DLSS motion vectors") &&
      make_shared_tex(inst->out, aow, aoh, DXGI_FORMAT_R8G8B8A8_UNORM, wgpu::TextureFormat::RGBA8Unorm,
                      U::TextureBinding | U::CopySrc, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "DLSS output");
  if (!ok) {
    log(2, "DLSS: shared texture creation failed");
    return false;
  }
  if (!inst->flow.init(g_rt.dawn) || !inst->flow.resize(p.renderWidth, p.renderHeight)) return false;
  const wgpu::TextureDescriptor sd{.label = "DLSS sharpened output",
                                   .usage = U::RenderAttachment | U::TextureBinding | U::CopySrc,
                                   .size = {aow, aoh, 1},
                                   .format = wgpu::TextureFormat::RGBA8Unorm};
  inst->sharpTex = g_rt.dawn.CreateTexture(&sd);
  inst->sharpView = inst->sharpTex.CreateView();

  if (p.fsr) {
    ffxCreateBackendDX12Desc backend{{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12}, g_rt.device.Get()};
    ffxCreateContextDescUpscale desc{};
    desc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &backend.header};
    desc.maxRenderSize = {inst->rw, inst->rh};
    desc.maxUpscaleSize = {inst->ow, inst->oh};
    if (g_rt.ffx.CreateContext(&inst->fsrContext, &desc.header, nullptr) != 0) return false;
    ffxQueryGetProviderVersion provider{{FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    if (g_rt.ffx.Query && g_rt.ffx.Query(&inst->fsrContext, &provider.header) == 0 && provider.versionName)
      logf(0, "AMD FSR provider: %s", provider.versionName);
    g_instance = std::move(inst);
    return true;
  }
  sl::DLSSOptions opt{};
  opt.mode = sl_mode(p.mode);
  opt.outputWidth = aow;
  opt.outputHeight = aoh;
  opt.colorBuffersHDR = sl::Boolean::eFalse;
  opt.useAutoExposure = sl::Boolean::eFalse;
  opt.preExposure = 1.0f;
  opt.exposureScale = 1.0f;
  if (p.mode == Mode::DLAA) {
    opt.dlaaPreset = sl::DLSSPreset::ePresetM; // best of the presets on stylised low-resolution content (GDL measurements)
  }
  sl::DLSSOptimalSettings os{};
  if (const auto r = slDLSSGetOptimalSettings(opt, os); r != sl::Result::eOk) {
    logf(2, "DLSS: slDLSSGetOptimalSettings failed (%d)", static_cast<int>(r));
    return false;
  }
  if (p.renderWidth < os.renderWidthMin || p.renderWidth > os.renderWidthMax || p.renderHeight < os.renderHeightMin ||
      p.renderHeight > os.renderHeightMax) {
    logf(1, "DLSS %s: render size %ux%u outside the supported range %ux%u..%ux%u for output %ux%u (optimal %ux%u)",
         mode_name(p.mode), p.renderWidth, p.renderHeight, os.renderWidthMin, os.renderHeightMin, os.renderWidthMax,
         os.renderHeightMax, aow, aoh, os.optimalRenderWidth, os.optimalRenderHeight);
    return false;
  }
  if (const auto r = slDLSSSetOptions(kViewport, opt); r != sl::Result::eOk) {
    logf(2, "DLSS: slDLSSSetOptions failed (%d)", static_cast<int>(r));
    return false;
  }
  logf(0, "DLSS %s: %ux%u -> %ux%u (optimal render %ux%u)", mode_name(p.mode), p.renderWidth, p.renderHeight, aow, aoh,
       os.optimalRenderWidth, os.optimalRenderHeight);
  g_instance = std::move(inst);
  return true;
}

sl::float4x4 identity4() {
  sl::float4x4 m;
  for (int i = 0; i < 4; ++i) m.setRow(i, sl::float4(i == 0, i == 1, i == 2, i == 3));
  return m;
}

bool record_fsr(Instance& inst, const DispatchParams& p) {
  ffxDispatchDescUpscale desc{};
  desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  desc.commandList = g_rt.list.Get();
  desc.color = ffxApiGetResourceDX12(inst.color.resource.Get(), FFX_API_RESOURCE_STATE_COMMON);
  desc.depth = ffxApiGetResourceDX12(inst.depth.resource.Get(), FFX_API_RESOURCE_STATE_COMMON);
  desc.motionVectors = ffxApiGetResourceDX12(inst.mv.resource.Get(), FFX_API_RESOURCE_STATE_COMMON);
  desc.output = ffxApiGetResourceDX12(inst.out.resource.Get(), FFX_API_RESOURCE_STATE_COMMON);
  current_jitter(desc.jitterOffset.x, desc.jitterOffset.y);
  desc.motionVectorScale = {1, 1};
  desc.renderSize = {inst.rw, inst.rh};
  desc.upscaleSize = {inst.ow, inst.oh};
  const auto now = std::chrono::steady_clock::now();
  desc.reset = p.reset || inst.needReset;
  desc.frameTimeDelta = desc.reset ? 16.67f : std::chrono::duration<float, std::milli>(now - inst.lastFrame).count();
  inst.lastFrame = now;
  desc.preExposure = 1;
  desc.cameraNear = 0.1f;
  desc.cameraFar = 1000;
  desc.cameraFovAngleVertical = 1;
  desc.viewSpaceToMetersFactor = 1;
  return g_rt.ffx.Dispatch(&inst.fsrContext, &desc.header) == 0;
}

bool record_dlss(Instance& inst, const DispatchParams& p, uint32_t frameIndex) {
  sl::FrameToken* token = nullptr;
  if (slGetNewFrameToken(token, &frameIndex) != sl::Result::eOk || !token) {
    return false;
  }
  float jx = 0.f, jy = 0.f;
  current_jitter(jx, jy);
  sl::Constants c{};
  c.cameraViewToClip = identity4();
  c.clipToCameraView = identity4();
  c.clipToLensClip = identity4();
  c.clipToPrevClip = identity4();
  c.prevClipToClip = identity4();
  static const float jitterSign = [] { const char* e = getenv("AURORA_DLSS_JITTER_SIGN"); return e ? static_cast<float>(atof(e)) : 1.0f; }();
  c.jitterOffset = sl::float2(jx * jitterSign, jy * jitterSign);
  c.mvecScale = sl::float2(1.0f / static_cast<float>(inst.rw), 1.0f / static_cast<float>(inst.rh));
  c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
  c.cameraPos = sl::float3(0, 0, 0);
  c.cameraUp = sl::float3(0, 1, 0);
  c.cameraRight = sl::float3(1, 0, 0);
  c.cameraFwd = sl::float3(0, 0, 1);
  c.cameraNear = 0.1f;
  c.cameraFar = 1000.0f;
  c.cameraFOV = 1.0f;
  c.cameraAspectRatio = static_cast<float>(inst.rw) / static_cast<float>(inst.rh);
  c.depthInverted = sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.reset = (p.reset || inst.needReset) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.orthographicProjection = sl::Boolean::eFalse;
  c.motionVectorsDilated = sl::Boolean::eFalse;
  c.motionVectorsJittered = sl::Boolean::eFalse;
  c.motionVectorsInvalidValue = 0.0f;
  bool ok = slSetConstants(c, *token, kViewport) == sl::Result::eOk;

  const uint32_t common = static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON); // simultaneous-access resources
  sl::Resource rc{sl::ResourceType::eTex2d, inst.color.resource.Get(), nullptr, nullptr, common};
  sl::Resource rd{sl::ResourceType::eTex2d, inst.depth.resource.Get(), nullptr, nullptr, common};
  sl::Resource rm{sl::ResourceType::eTex2d, inst.mv.resource.Get(), nullptr, nullptr, common};
  sl::Resource ro{sl::ResourceType::eTex2d, inst.out.resource.Get(), nullptr, nullptr, common};
  sl::Extent re{0, 0, inst.rw, inst.rh}, oe{0, 0, inst.ow, inst.oh};
  sl::ResourceTag tags[4] = {
      sl::ResourceTag{&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &re},
      sl::ResourceTag{&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &oe},
      sl::ResourceTag{&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &re},
      sl::ResourceTag{&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &re},
  };
  ok = ok && slSetTagForFrame(*token, kViewport, tags, 4, g_rt.list.Get()) == sl::Result::eOk;
  const sl::BaseStructure* inputs[] = {&kViewport};
  ok = ok && slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, g_rt.list.Get()) == sl::Result::eOk;
  return ok;
}

// Runs DLSS on the second device: waits for Dawn's fences, records tags + evaluate, executes, signals.
bool run_dlss(Instance& inst, const DispatchParams& p, const std::vector<FenceWait>& waits) {
  for (const auto& w : inst.outReadWaits) g_rt.queue->Wait(w.fence.Get(), w.value);
  inst.outReadWaits.clear();
  for (const auto& w : waits) g_rt.queue->Wait(w.fence.Get(), w.value);

  const uint32_t slot = g_rt.frameIndex % 3;
  if (g_rt.fence->GetCompletedValue() < g_rt.allocatorFence[slot]) {
    const HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_rt.fence->SetEventOnCompletion(g_rt.allocatorFence[slot], e);
    WaitForSingleObject(e, 2000);
    CloseHandle(e);
  }
  g_rt.allocator[slot]->Reset();
  g_rt.list->Reset(g_rt.allocator[slot].Get(), nullptr);

  const uint32_t frameIndex = g_rt.frameIndex++;
  const bool ok = p.fsr ? record_fsr(inst, p) : record_dlss(inst, p, frameIndex);
  g_rt.list->Close();
  if (!ok) return false;
  ID3D12CommandList* lists[] = {g_rt.list.Get()};
  g_rt.queue->ExecuteCommandLists(1, lists);
  g_rt.queue->Signal(g_rt.fence.Get(), ++g_rt.signaled);
  g_rt.allocatorFence[slot] = g_rt.signaled;
  inst.needReset = false;
  ++inst.evaluations;
  return true;
}

void fail_current(const char* why) {
  if (g_failed && g_failedKey == g_currentKey) return;
  g_failed = true;
  g_failedKey = g_currentKey;
  logf(2, "DLSS %s %ux%u -> %ux%u failed: %s; falling back to the normal upscaler until the configuration changes",
       mode_name(g_currentKey.mode), g_currentKey.rw, g_currentKey.rh, g_currentKey.ow, g_currentKey.oh, why);
}

} // namespace

// ---- public API ------------------------------------------------------------------------------------------------
void start(const wgpu::Device& device, const wgpu::AdapterInfo& adapterInfo, wgpu::BackendType backend,
           bool deviceHasSharing, const std::string& dllDir) {
  if (g_rt.state.load() != State::NotStarted) shutdown(); // start() restarts after a failed device initialisation
  g_rt.dawn = device;
  g_rt.dawnQueue = device.GetQueue();
  if (dllDir.empty()) {
    g_rt.dllDir = executable_dir();
  } else {
    g_rt.dllDir.assign(dllDir.begin(), dllDir.end());
  }
  g_rt.state.store(State::Probing);
  const bool backendOk = backend == wgpu::BackendType::D3D12;
  const uint32_t vendorId = adapterInfo.vendorID, deviceId = adapterInfo.deviceID;
  g_rt.initThread = std::thread([vendorId, deviceId, backendOk, deviceHasSharing] {
    // Streamline start-up (plugin loading, NGX initialisation) takes a couple of seconds.
    init_thread_main(vendorId, deviceId, backendOk, deviceHasSharing);
  });
}

bool fsr_ready() noexcept { return g_rt.fsrAvailable.load(); }

bool ready() noexcept { return g_rt.state.load(std::memory_order_acquire) == State::Ready; }

Status status() {
  Status s;
  s.state = g_rt.state.load(std::memory_order_acquire);
  std::lock_guard lock{g_rt.mutex};
  s.reason = g_rt.reason;
  s.version = g_rt.version;
  if (s.state == State::Probing) s.reason = "checking DLSS support";
  return s;
}

void shutdown() noexcept {
  if (g_rt.initThread.joinable()) g_rt.initThread.join();
  destroy_instance();
  g_pipes = {};
  g_dawnFences.clear();
  g_rt.dawnFence = {};
  g_rt.list.Reset();
  for (auto& a : g_rt.allocator) a.Reset();
  g_rt.queue.Reset();
  g_rt.fence.Reset();
  if (g_rt.slLoaded) {
    slShutdown();
    g_rt.slLoaded = false;
  }
  g_rt.fsrAvailable.store(false);
  if (g_rt.fsrModule) FreeLibrary(g_rt.fsrModule);
  g_rt.fsrModule = nullptr;
  g_failed = false;
  g_rt.device.Reset();
  g_rt.dawn = {};
  g_rt.dawnQueue = {};
  g_rt.state.store(State::NotStarted);
}

bool dispatch(wgpu::CommandEncoder& encoder, const DispatchParams& p, Output& output) {
  if (p.fsr ? !fsr_ready() : !ready()) return false;
  g_currentKey = ConfigKey{p.renderWidth, p.renderHeight, p.outputWidth, p.outputHeight, p.mode, p.fsr};
  if (g_failed && g_failedKey == g_currentKey) return false;
  g_failed = false;
  if (p.mode == Mode::Off || p.renderWidth == 0 || p.renderHeight == 0 || p.outputWidth < p.renderWidth ||
      p.outputHeight < p.renderHeight || !p.color || !ensure_pipelines()) {
    return false;
  }
  if (!g_rt.dawnFence) {
    // The device-to-device timeline fence, imported into Dawn once.
    HANDLE handle = nullptr;
    if (FAILED(g_rt.device->CreateSharedHandle(g_rt.fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle)) || !handle) {
      fail_current("fence sharing failed");
      return false;
    }
    wgpu::SharedFenceDXGISharedHandleDescriptor fd{};
    fd.handle = handle;
    const wgpu::SharedFenceDescriptor sfd{.nextInChain = &fd, .label = "DLSS fence"};
    g_rt.dawnFence = g_rt.dawn.ImportSharedFence(&sfd);
    CloseHandle(handle);
    if (!g_rt.dawnFence) {
      fail_current("fence import failed");
      return false;
    }
  }
  bool reset = p.reset;
  if (!g_instance || g_instance->rw != p.renderWidth || g_instance->rh != p.renderHeight ||
      g_instance->reqOw != p.outputWidth || g_instance->reqOh != p.outputHeight || g_instance->mode != p.mode || (g_instance->fsrContext != nullptr) != p.fsr) {
    destroy_instance();
    if (!create_instance(p)) {
      fail_current("could not create the DLSS resources (see the messages above)");
      return false;
    }
    reset = true;
  }
  Instance& inst = *g_instance;
  if (reset) {
    inst.needReset = true;
    inst.flow.invalidate_history();
  }

  if (static_cast<bool>(p.depth) != inst.hadSceneDepth) inst.needReset = true;
  inst.hadSceneDepth = static_cast<bool>(p.depth);
  if (!begin_access(inst.color, true) || !begin_access(inst.mv, true) ||
      !begin_access(inst.depth, true)) {
    fail_current("Dawn BeginAccess failed");
    return false;
  }

  // 1. Dawn: copy the game image into the shared input, build the motion vectors, clear the constant depth once.
  {
    const std::array entries{wgpu::BindGroupEntry{.binding = 0, .textureView = p.color}};
    const wgpu::BindGroupDescriptor bg{.layout = g_pipes.blitLayout, .entryCount = entries.size(), .entries = entries.data()};
    const wgpu::RenderPassColorAttachment attachment{.view = inst.color.view, .loadOp = wgpu::LoadOp::Clear,
                                                     .storeOp = wgpu::StoreOp::Store, .clearValue = {0, 0, 0, 1}};
    const wgpu::RenderPassDescriptor rd{.label = "DLSS input copy", .colorAttachmentCount = 1, .colorAttachments = &attachment};
    auto pass = encoder.BeginRenderPass(&rd);
    pass.SetPipeline(g_pipes.blit);
    pass.SetBindGroup(0, g_rt.dawn.CreateBindGroup(&bg));
    pass.Draw(3);
    pass.End();
  }
  {
    const wgpu::RenderPassColorAttachment attachment{.view = inst.depth.view, .loadOp = wgpu::LoadOp::Clear,
                                                     .storeOp = wgpu::StoreOp::Store, .clearValue = {0.5, 0, 0, 0}};
    const wgpu::RenderPassDescriptor rd{.label = "DLSS depth init", .colorAttachmentCount = 1, .colorAttachments = &attachment};
    auto pass = encoder.BeginRenderPass(&rd);
    if (p.depth) {
      const float mapping[8]{p.depthRect[0], p.depthRect[1], p.depthRect[2], p.depthRect[3], p.reversedDepth ? 1.f : 0.f};
      g_rt.dawnQueue.WriteBuffer(g_pipes.depthUniform, 0, mapping, sizeof mapping);
      const std::array entries{wgpu::BindGroupEntry{.binding=0, .textureView=p.depth},
                               wgpu::BindGroupEntry{.binding=1, .buffer=g_pipes.depthUniform, .size=sizeof mapping}};
      const wgpu::BindGroupDescriptor bind{.layout=g_pipes.depthLayout, .entryCount=entries.size(), .entries=entries.data()};
      pass.SetPipeline(g_pipes.depthCopy);
      pass.SetBindGroup(0, g_rt.dawn.CreateBindGroup(&bind));
      pass.Draw(3);
    }
    pass.End();
  }
  float jx = 0.f, jy = 0.f;
  current_jitter(jx, jy);
  const bool constantMotion = g_constantMotion.load();
  const bool estimateMotion = p.fsr || g_motionEnabled.load();
  if (constantMotion || !estimateMotion) {
    // test hook: every pixel moves by the same known vector
    const wgpu::RenderPassColorAttachment attachment{.view = inst.mv.view, .loadOp = wgpu::LoadOp::Clear,
                                                     .storeOp = wgpu::StoreOp::Store,
                                                     .clearValue = {constantMotion ? g_constantMotionX.load() : 0.0f,
                                                                    constantMotion ? g_constantMotionY.load() : 0.0f,
                                                                    0, 0}};
    const wgpu::RenderPassDescriptor rd{.label = "DLSS constant motion", .colorAttachmentCount = 1, .colorAttachments = &attachment};
    auto pass = encoder.BeginRenderPass(&rd);
    if (p.depth) {
      const float mapping[8]{p.depthRect[0], p.depthRect[1], p.depthRect[2], p.depthRect[3], p.reversedDepth ? 1.f : 0.f};
      g_rt.dawnQueue.WriteBuffer(g_pipes.depthUniform, 0, mapping, sizeof mapping);
      const std::array entries{wgpu::BindGroupEntry{.binding=0, .textureView=p.depth},
                               wgpu::BindGroupEntry{.binding=1, .buffer=g_pipes.depthUniform, .size=sizeof mapping}};
      const wgpu::BindGroupDescriptor bind{.layout=g_pipes.depthLayout, .entryCount=entries.size(), .entries=entries.data()};
      pass.SetPipeline(g_pipes.depthCopy);
      pass.SetBindGroup(0, g_rt.dawn.CreateBindGroup(&bind));
      pass.Draw(3);
    }
    pass.End();
    inst.flow.invalidate_history();
  } else {
    inst.flow.encode(encoder, inst.color.view, inst.mv.view, inst.needReset, jx - inst.prevJitter[0],
                     jy - inst.prevJitter[1]);
  }
  inst.prevJitter[0] = jx;
  inst.prevJitter[1] = jy;

  const wgpu::CommandBufferDescriptor cbd{.label = "DLSS input command buffer"};
  const auto buffer = encoder.Finish(&cbd);
  g_rt.dawnQueue.Submit(1, &buffer);
  encoder = g_rt.dawn.CreateCommandEncoder();

  std::vector<FenceWait> waits;
  end_access(inst.color, waits);
  end_access(inst.mv, waits);
  end_access(inst.depth, waits);

  // 2. DLSS on the second device.
  if (!run_dlss(inst, p, waits)) {
    fail_current(p.fsr ? "AMD FSR dispatch failed" : "slEvaluateFeature failed");
    return false;
  }

  // 3. Dawn reads the result.
  inst.out.everUsed = true; // written by DLSS, must not be lazily cleared by Dawn
  if (!begin_access(inst.out, true)) {
    fail_current("Dawn BeginAccess(output) failed");
    return false;
  }
  inst.outPending = true;

  Output result;
  if (p.sharpness > 0.01f) {
    const float u[4] = {p.sharpness, 0, 0, 0};
    g_rt.dawnQueue.WriteBuffer(g_pipes.casUniform, 0, u, sizeof u);
    const std::array entries{wgpu::BindGroupEntry{.binding = 0, .textureView = inst.out.view},
                             wgpu::BindGroupEntry{.binding = 1, .buffer = g_pipes.casUniform, .size = 16}};
    const wgpu::BindGroupDescriptor bg{.layout = g_pipes.casLayout, .entryCount = entries.size(), .entries = entries.data()};
    const wgpu::RenderPassColorAttachment attachment{.view = inst.sharpView, .loadOp = wgpu::LoadOp::Clear,
                                                     .storeOp = wgpu::StoreOp::Store, .clearValue = {0, 0, 0, 1}};
    const wgpu::RenderPassDescriptor rd{.label = "DLSS sharpen", .colorAttachmentCount = 1, .colorAttachments = &attachment};
    auto pass = encoder.BeginRenderPass(&rd);
    pass.SetPipeline(g_pipes.cas);
    pass.SetBindGroup(0, g_rt.dawn.CreateBindGroup(&bg));
    pass.Draw(3);
    pass.End();
    result.texture = inst.sharpTex;
    result.view = inst.sharpView;
  } else {
    result.texture = inst.out.texture;
    result.view = inst.out.view;
  }
  result.size = {inst.ow, inst.oh, 1};
  result.format = wgpu::TextureFormat::RGBA8Unorm;
  result.sampler = g_pipes.sampler;
  output = std::move(result);
  return true;
}

void after_submit() noexcept {
  if (!g_instance || !g_instance->outPending) return;
  end_access(g_instance->out, g_instance->outReadWaits);
  g_instance->outPending = false;
}

} // namespace aurora::webgpu::dlss

// In-game options menu for Sin & Punishment: Star Successor recomp.
//
// The menu is drawn entirely through game_ui.h (blue glass panel, outlined text, chevron rows, pill tabs) and is
// controller first. It can be opened by the game hook (OpenFromGame) or, as a developer fallback, with F10 / F1 /
// Back + Start. Pages: Display, Graphics, Audio, Controls (Aiming, Mapping, Mouse & Keyboard, Devices), System, About.
#include "settings_overlay.h"
#include "gx_worker.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "aim_input.h"
#include "aim_settings.h"
#include "controls_menu.h"
#include "game_ui.h"
#include "input_bindings.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "system_settings.h"
#include "wiimote_host.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

using game_ui::kChanged;
using game_ui::kCommit;
using game_ui::ListView;
using Clock = std::chrono::steady_clock;

const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

constexpr const char* kGameTitle = "Sin & Punishment: Star Successor recomp";

// ---- persisted option mirrors ------------------------------------------------------------------------
bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
// Presentation rate choices. The game itself always runs at 60 Hz; these only change how many frames
// are shown per game frame (see aurora_set_frame_rate).
struct FrameRateItem {
    const char* label;
    RuntimeConfigFile::FrameRateKind kind;
    uint32_t fps;
};
constexpr std::array<FrameRateItem, 9> kFrameRates = {{
    {"Original (60)", RuntimeConfigFile::FrameRateKind::Original, 0},
    {"Match display", RuntimeConfigFile::FrameRateKind::Display, 0},
    {"90 FPS", RuntimeConfigFile::FrameRateKind::Fixed, 90},
    {"120 FPS", RuntimeConfigFile::FrameRateKind::Fixed, 120},
    {"144 FPS", RuntimeConfigFile::FrameRateKind::Fixed, 144},
    {"165 FPS", RuntimeConfigFile::FrameRateKind::Fixed, 165},
    {"180 FPS", RuntimeConfigFile::FrameRateKind::Fixed, 180},
    {"240 FPS", RuntimeConfigFile::FrameRateKind::Fixed, 240},
    {"Uncapped", RuntimeConfigFile::FrameRateKind::Uncapped, 0},
}};
constexpr std::array<uint32_t, 4> kFrameRateCaps{240, 360, 500, 1000};

// A saved value with no row of its own (a hand-edited 200, say) lands on the nearest fixed row.
int FrameRateIndex(const RuntimeConfigFile::FrameRateSetting& setting) {
    int best = 0;
    uint32_t bestDistance = UINT32_MAX;
    for (size_t i = 0; i < kFrameRates.size(); ++i) {
        const FrameRateItem& item = kFrameRates[i];
        if (item.kind != setting.kind) {
            continue;
        }
        if (item.kind != RuntimeConfigFile::FrameRateKind::Fixed) {
            return static_cast<int>(i);
        }
        const uint32_t distance = item.fps > setting.fps ? item.fps - setting.fps : setting.fps - item.fps;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int g_frameInterpolationMode = FrameRateIndex(RuntimeConfigFile::FrameRate());
uint32_t g_frameRateCap = RuntimeConfigFile::FrameRateCap(RuntimeConfigFile::kFrameRateHardCap);
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("windowed");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_vsync = RuntimeConfigFile::Vsync();
// Variable refresh rate (G-SYNC / FreeSync). Live settings; none of them can lose the picture, so they
// bypass the keep/revert countdown. Mode index order matches AuroraVrrMode (auto, on, off).
int g_vrrMode = [] {
    const std::string mode = RuntimeConfigFile::Vrr();
    return mode == "on" ? static_cast<int>(AURORA_VRR_ON)
                        : (mode == "off" ? static_cast<int>(AURORA_VRR_OFF) : static_cast<int>(AURORA_VRR_AUTO));
}();
float g_vrrMargin = static_cast<float>(RuntimeConfigFile::VrrMarginFps(3));
bool g_vrrLimiter = RuntimeConfigFile::VrrLimiter(true);
constexpr std::array<const char*, 3> kVrrConfigNames = {"auto", "on", "off"};

void ApplyVrr() {
    aurora_set_vrr(static_cast<AuroraVrrMode>(g_vrrMode), static_cast<uint32_t>(std::lround(g_vrrMargin)),
                   g_vrrLimiter);
}
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
bool g_forceAspect = RuntimeConfigFile::ForceAspectEnabled();
bool g_widescreen = RuntimeConfigFile::WidescreenEnabled(true);
const bool g_widescreenAtLaunch = g_widescreen;
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;
// Per-port bindings persist as [controller] "<key>_<port + 1>"; keep the config side's port
// count honest against the pad layer's channel count.
static_assert(RuntimeConfigFile::kControllerPortCount == PAD_MAX_CONTROLLERS);

struct ResolutionItem {
    const char* label;
    float scale;
};

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;
std::atomic_bool g_bootShadersReady = false;
bool g_bootShaderNotice = false;
Clock::time_point g_bootShaderWaitStart{};
constexpr uint32_t kBootShaderNoticeThreshold = 100;
constexpr auto kBootShaderWaitLimit = std::chrono::minutes(3);

constexpr std::array<ResolutionItem, 8> kResolutions = {{
    {"Auto (window size)", 0.0f}, {"Native (1x)", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f},
    {"3x", 3.0f}, {"4x", 4.0f}, {"6x", 6.0f}, {"8x", 8.0f},
}};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateIndex(int index) {
    return kFrameRates[static_cast<size_t>(index)].kind != RuntimeConfigFile::FrameRateKind::Original;
}

bool IsHighFrameRateMode() {
    return IsHighFrameRateIndex(g_frameInterpolationMode);
}

AuroraFrameRateMode AuroraModeFor(RuntimeConfigFile::FrameRateKind kind) {
    switch (kind) {
    case RuntimeConfigFile::FrameRateKind::Display:
        return AURORA_FRAME_RATE_DISPLAY;
    case RuntimeConfigFile::FrameRateKind::Fixed:
        return AURORA_FRAME_RATE_FIXED;
    case RuntimeConfigFile::FrameRateKind::Uncapped:
        return AURORA_FRAME_RATE_UNCAPPED;
    default:
        return AURORA_FRAME_RATE_OFF;
    }
}

void ApplyFrameRate(int index) {
    const FrameRateItem& item = kFrameRates[static_cast<size_t>(index)];
    aurora_set_frame_rate(AuroraModeFor(item.kind), item.fps, g_frameRateCap);
}

void SetResolutionScale(float scale) {
    GX_SYNC_INLINE();
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

// ---- upscaler (video.upscaler / fsr_quality / fsr_sharpness; docs/FSR-NOTES.md) -------------------------
// One selector for every upscaler. Index 0 = off, 1 = FSR 1 (spatial); further upscalers (DLSS, FSR 3) append
// a label and a config name here and a branch in ApplyUpscaler().
constexpr std::array<const char*, 3> kUpscalerLabels = {"Off", "FSR 1 (spatial)", "NVIDIA DLSS"};
constexpr std::array<const char*, 3> kUpscalerConfigNames = {"off", "fsr1", "dlss"};
constexpr int kUpscalerFsr1 = 1;
constexpr int kUpscalerDlss = 2;
// Same order as RuntimeConfigFile::kDlssModes (docs/DLSS-NOTES.md).
constexpr std::array<const char*, RuntimeConfigFile::kDlssModes.size()> kDlssModeLabels = {
    "Ultra Performance (33%)", "Performance (50%)", "Balanced (58%)", "Quality (67%)", "DLAA (100%, anti-aliasing)"};
constexpr std::array<const char*, RuntimeConfigFile::kFsrQualities.size()> kFsrQualityLabels = {
    "Native (1.0x, off)", "Ultra Quality (1.3x)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2.0x)",
    "Ultra Performance (3.0x)"};
constexpr int kDefaultFsrQualityIndex = 2;

int UpscalerIndexFromConfig() {
    const std::string name = RuntimeConfigFile::Upscaler("off");
    for (size_t i = 0; i < kUpscalerConfigNames.size(); ++i) {
        if (name == kUpscalerConfigNames[i]) return static_cast<int>(i);
    }
    // fsr3 has no temporal backend yet: it runs the spatial path at the same scale (docs/FSR-NOTES.md).
    return name == "fsr3" ? kUpscalerFsr1 : 0;
}

int FsrQualityIndexFromConfig() {
    const std::string name = RuntimeConfigFile::FsrQuality("quality");
    for (size_t i = 0; i < RuntimeConfigFile::kFsrQualities.size(); ++i) {
        if (name == RuntimeConfigFile::kFsrQualities[i].name) return static_cast<int>(i);
    }
    return kDefaultFsrQualityIndex;
}

int g_upscaler = UpscalerIndexFromConfig();
int g_fsrQuality = FsrQualityIndexFromConfig();
float g_fsrSharpness = static_cast<float>(RuntimeConfigFile::FsrSharpness());
int g_dlssMode = static_cast<int>(RuntimeConfigFile::DlssModeIndex(RuntimeConfigFile::DlssMode()));
float g_dlssSharpness = static_cast<float>(RuntimeConfigFile::DlssSharpness());
bool g_dlssMotion = RuntimeConfigFile::DlssMotionVectors(false);

bool UpscalerActive() {
    return (g_upscaler == kUpscalerFsr1 && g_fsrQuality > 0) || g_upscaler == kUpscalerDlss;
}

// Live apply: the scale factor changes the internal render size, which aurora re-evaluates at the next frame.
void ApplyUpscaler() {
    GX_SYNC_INLINE();
    const bool fsr = g_upscaler == kUpscalerFsr1;
    const bool dlss = g_upscaler == kUpscalerDlss;
    float factor = fsr ? RuntimeConfigFile::kFsrQualities[static_cast<size_t>(g_fsrQuality)].factor : 1.0f;
    float sharpness = g_fsrSharpness;
    if (dlss) {
        // DLSS replaces the FSR 1 path once it is available; until then (or when it is not) aurora runs FSR 1 at the same scale.
        factor = RuntimeConfigFile::kDlssModes[static_cast<size_t>(g_dlssMode)].factor;
        sharpness = g_dlssSharpness;
    }
    aurora_set_dlss_motion_vectors(g_dlssMotion);
    aurora_set_upscaler(dlss ? AURORA_UPSCALER_DLSS : (fsr ? AURORA_UPSCALER_FSR1 : AURORA_UPSCALER_OFF), factor,
                        sharpness / 100.0f);
}

void PersistUpscaler() {
    RuntimeConfigFile::SetUpscaler(kUpscalerConfigNames[static_cast<size_t>(g_upscaler)]);
    RuntimeConfigFile::SetFsrQuality(RuntimeConfigFile::kFsrQualities[static_cast<size_t>(g_fsrQuality)].name);
    RuntimeConfigFile::SetFsrSharpness(static_cast<uint32_t>(std::lround(g_fsrSharpness)));
    RuntimeConfigFile::SetDlssMode(RuntimeConfigFile::kDlssModes[static_cast<size_t>(g_dlssMode)].name);
    RuntimeConfigFile::SetDlssSharpness(static_cast<uint32_t>(std::lround(g_dlssSharpness)));
    RuntimeConfigFile::SetDlssMotionVectors(g_dlssMotion);
}

void ResetUpscaler() {
    g_upscaler = 0;
    g_fsrQuality = kDefaultFsrQualityIndex;
    g_fsrSharpness = static_cast<float>(RuntimeConfigFile::kDefaultFsrSharpness);
    g_dlssMode = static_cast<int>(RuntimeConfigFile::kDefaultDlssModeIndex);
    g_dlssSharpness = static_cast<float>(RuntimeConfigFile::kDefaultDlssSharpness);
    g_dlssMotion = false;
    ApplyUpscaler();
    PersistUpscaler();
}

std::string ResolutionText(uint32_t width, uint32_t height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

// Options launched from the menu that need a fresh process, captured when the menu first looks at them.
struct LaunchValues {
    int msaa;
    int anisotropy;
    bool textureReplacements;
    bool textureDumps;
    bool gxWorker;
    bool wiiRemotes;
};

const bool g_wiiRemotesAtLaunch = RuntimeConfigFile::WiiRemotesEnabled();

const LaunchValues& Launch() {
    static const LaunchValues values = [] {
        const SystemSettings::Values sys = SystemSettings::Get();
        return LaunchValues{sys.msaa, sys.anisotropy, RuntimeConfigFile::TextureReplacements(false),
                            RuntimeConfigFile::TextureDumps(false), RuntimeConfigFile::GxWorkerEnabled(true),
                            g_wiiRemotesAtLaunch};
    }();
    return values;
}

// ---- display changes that can leave the user without a picture: apply live, then keep or revert --------
struct DisplayState {
    int mode;
    float resolution;
    int interpolation;
    bool vsync;
    bool forceAspect;
    bool widescreen;
    uint32_t windowWidth;
    uint32_t windowHeight;

    bool operator==(const DisplayState& o) const {
        return mode == o.mode && resolution == o.resolution && interpolation == o.interpolation &&
               vsync == o.vsync && forceAspect == o.forceAspect && widescreen == o.widescreen &&
               windowWidth == o.windowWidth && windowHeight == o.windowHeight;
    }
};

DisplayState CurrentDisplay() {
    const AuroraWindowSize size = aurora_get_window_size();
    return {g_displayMode, g_resolutionScale, g_frameInterpolationMode, g_vsync, g_forceAspect, g_widescreen,
            size.width, size.height};
}

DisplayState g_applied = CurrentDisplay();
struct GraphicsState {
    DisplayState display;
    int vrrMode = g_vrrMode;
    float vrrMargin = g_vrrMargin;
    bool vrrLimiter = g_vrrLimiter;
    int scalingFilter = SystemSettings::Get().scalingFilter;
    bool showFps = g_showFps;
    bool showFrameTime = SystemSettings::Get().showFrameTime;
    uint32_t frameRateCap = g_frameRateCap;
    int upscaler = g_upscaler;
    int fsrQuality = g_fsrQuality;
    float fsrSharpness = g_fsrSharpness;
    int dlssMode = g_dlssMode;
    float dlssSharpness = g_dlssSharpness;
    bool dlssMotion = g_dlssMotion;
    bool disableCopyFilter = g_disableCopyFilter;
    bool skipUnreadyPipelines = g_skipUnreadyPipelines;
    int msaa = SystemSettings::Get().msaa;
    int anisotropy = SystemSettings::Get().anisotropy;
    bool gxWorker = RuntimeConfigFile::GxWorkerEnabled(true);
    bool textureReplacements = RuntimeConfigFile::TextureReplacements(false);
    bool textureDumps = RuntimeConfigFile::TextureDumps(false);
};

GraphicsState CurrentGraphicsState() {
    const SystemSettings::Values sys = SystemSettings::Get();
    return {
        .display = CurrentDisplay(),
        .vrrMode = g_vrrMode,
        .vrrMargin = g_vrrMargin,
        .vrrLimiter = g_vrrLimiter,
        .scalingFilter = sys.scalingFilter,
        .showFps = g_showFps,
        .showFrameTime = sys.showFrameTime,
        .frameRateCap = g_frameRateCap,
        .upscaler = g_upscaler,
        .fsrQuality = g_fsrQuality,
        .fsrSharpness = g_fsrSharpness,
        .dlssMode = g_dlssMode,
        .dlssSharpness = g_dlssSharpness,
        .dlssMotion = g_dlssMotion,
        .disableCopyFilter = g_disableCopyFilter,
        .skipUnreadyPipelines = g_skipUnreadyPipelines,
        .msaa = sys.msaa,
        .anisotropy = sys.anisotropy,
        .gxWorker = RuntimeConfigFile::GxWorkerEnabled(true),
        .textureReplacements = RuntimeConfigFile::TextureReplacements(false),
        .textureDumps = RuntimeConfigFile::TextureDumps(false),
    };
}

GraphicsState g_graphicsDraft = CurrentGraphicsState();
GraphicsState g_revertBase{};
GraphicsState g_pendingGraphics{};
bool g_revertActive = false;
Clock::time_point g_revertDeadline{};
constexpr auto kRevertTime = std::chrono::seconds(10);

DisplayState NormalizeDisplay(DisplayState s) {
    if (IsHighFrameRateIndex(s.interpolation) && IsHighResolutionScale(s.resolution)) {
        s.resolution = 4.0f;
    }
    return s;
}

void ApplyDisplay(const DisplayState& next) {
    if (next.windowWidth != g_applied.windowWidth || next.windowHeight != g_applied.windowHeight) {
        SDL_SetWindowSize(aurora_get_window(), static_cast<int>(next.windowWidth), static_cast<int>(next.windowHeight));
    }
    if (next.mode != g_applied.mode) {
        aurora_set_display_mode(static_cast<AuroraDisplayMode>(next.mode));
    }
    if (next.interpolation != g_applied.interpolation) {
        GX_SYNC_INLINE();
        ApplyFrameRate(next.interpolation);
        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
            // Re-apply exclusive mode so the display refresh tracks the new target.
            aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
        }
    }
    if (next.resolution != g_applied.resolution) {
        GX_SYNC_INLINE();
        VISetFrameBufferScale(next.resolution);
    }
    if (next.vsync != g_applied.vsync) {
        aurora_enable_vsync(next.vsync);
    }
    if (next.forceAspect != g_applied.forceAspect) {
        SetForceAspect(next.forceAspect);
    }
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    g_resolutionScale = next.resolution;
    g_frameInterpolationMode = next.interpolation;
    g_vsync = next.vsync;
    g_forceAspect = next.forceAspect;
    g_widescreen = next.widescreen;
    g_applied = CurrentDisplay();
}

void PersistDisplay(const DisplayState& s) {
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(s.mode)]));
    RuntimeConfigFile::SetResolutionMultiplier(s.resolution);
    const FrameRateItem& rate = kFrameRates[static_cast<size_t>(s.interpolation)];
    RuntimeConfigFile::SetFrameRate({rate.kind, rate.fps});
    RuntimeConfigFile::SetVsync(s.vsync);
    RuntimeConfigFile::SetForceAspect(s.forceAspect);
    RuntimeConfigFile::SetWidescreen(s.widescreen);
    RuntimeConfigFile::SetWindowSize(s.windowWidth, s.windowHeight);
}

void ApplyGraphicsLive(GraphicsState next) {
    next.display = NormalizeDisplay(next.display);
    g_frameRateCap = next.frameRateCap;
    ApplyDisplay(next.display);

    g_vrrMode = next.vrrMode;
    g_vrrMargin = next.vrrMargin;
    g_vrrLimiter = next.vrrLimiter;
    ApplyVrr();

    g_upscaler = next.upscaler;
    g_fsrQuality = next.fsrQuality;
    g_fsrSharpness = next.fsrSharpness;
    g_dlssMode = next.dlssMode;
    g_dlssSharpness = next.dlssSharpness;
    g_dlssMotion = next.dlssMotion;
    ApplyUpscaler();

    g_disableCopyFilter = next.disableCopyFilter;
    g_skipUnreadyPipelines = next.skipUnreadyPipelines;
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);

    g_showFps = next.showFps;
    SystemSettings::Values sys = SystemSettings::Get();
    sys.scalingFilter = next.scalingFilter;
    sys.showFrameTime = next.showFrameTime;
    SystemSettings::Set(sys);
    aurora_set_resampler(static_cast<AuroraSampler>(next.scalingFilter));
}

void PersistGraphics(const GraphicsState& state) {
    PersistDisplay(state.display);
    RuntimeConfigFile::SetVrr(kVrrConfigNames[static_cast<size_t>(state.vrrMode)]);
    RuntimeConfigFile::SetVrrMarginFps(static_cast<uint32_t>(std::lround(state.vrrMargin)));
    RuntimeConfigFile::SetVrrLimiter(state.vrrLimiter);
    RuntimeConfigFile::SetFrameRateCap(state.frameRateCap);
    RuntimeConfigFile::SetDisableCopyFilter(state.disableCopyFilter);
    RuntimeConfigFile::SetSkipUnreadyPipelines(state.skipUnreadyPipelines);
    RuntimeConfigFile::SetShowFps(state.showFps);
    RuntimeConfigFile::SetUpscaler(kUpscalerConfigNames[static_cast<size_t>(state.upscaler)]);
    RuntimeConfigFile::SetFsrQuality(RuntimeConfigFile::kFsrQualities[static_cast<size_t>(state.fsrQuality)].name);
    RuntimeConfigFile::SetFsrSharpness(static_cast<uint32_t>(std::lround(state.fsrSharpness)));
    RuntimeConfigFile::SetDlssMode(RuntimeConfigFile::kDlssModes[static_cast<size_t>(state.dlssMode)].name);
    RuntimeConfigFile::SetDlssSharpness(static_cast<uint32_t>(std::lround(state.dlssSharpness)));
    RuntimeConfigFile::SetDlssMotionVectors(state.dlssMotion);

    SystemSettings::Values sys = SystemSettings::Get();
    sys.scalingFilter = state.scalingFilter;
    sys.showFrameTime = state.showFrameTime;
    sys.msaa = state.msaa;
    sys.anisotropy = state.anisotropy;
    SystemSettings::Set(sys);
    SystemSettings::Save();

    RuntimeUserConfig& userConfig = RuntimeConfigFile::Mutable();
    userConfig.gxWorker = state.gxWorker;
    userConfig.textureReplacements = state.textureReplacements;
    userConfig.textureDumps = state.textureDumps;
    RuntimeConfigFile::WriteSetting("video", "gx_worker", state.gxWorker ? "true" : "false");
    RuntimeConfigFile::WriteSetting("video", "texture_replacements", state.textureReplacements ? "true" : "false");
    RuntimeConfigFile::WriteSetting("video", "texture_dumps", state.textureDumps ? "true" : "false");
}

bool GraphicsDraftDirty() {
    return !(g_graphicsDraft.display == CurrentDisplay() &&
             g_graphicsDraft.vrrMode == g_vrrMode && std::fabs(g_graphicsDraft.vrrMargin - g_vrrMargin) < 1e-4f &&
             g_graphicsDraft.vrrLimiter == g_vrrLimiter &&
             g_graphicsDraft.scalingFilter == SystemSettings::Get().scalingFilter &&
             g_graphicsDraft.showFps == g_showFps &&
             g_graphicsDraft.showFrameTime == SystemSettings::Get().showFrameTime &&
             g_graphicsDraft.frameRateCap == g_frameRateCap &&
             g_graphicsDraft.upscaler == g_upscaler &&
             g_graphicsDraft.fsrQuality == g_fsrQuality &&
             std::fabs(g_graphicsDraft.fsrSharpness - g_fsrSharpness) < 1e-4f &&
             g_graphicsDraft.dlssMode == g_dlssMode &&
             std::fabs(g_graphicsDraft.dlssSharpness - g_dlssSharpness) < 1e-4f &&
             g_graphicsDraft.dlssMotion == g_dlssMotion &&
             g_graphicsDraft.disableCopyFilter == g_disableCopyFilter &&
             g_graphicsDraft.skipUnreadyPipelines == g_skipUnreadyPipelines &&
             g_graphicsDraft.msaa == SystemSettings::Get().msaa &&
             g_graphicsDraft.anisotropy == SystemSettings::Get().anisotropy &&
             g_graphicsDraft.gxWorker == RuntimeConfigFile::GxWorkerEnabled(true) &&
             g_graphicsDraft.textureReplacements == RuntimeConfigFile::TextureReplacements(false) &&
             g_graphicsDraft.textureDumps == RuntimeConfigFile::TextureDumps(false));
}

void ApplyGraphicsDraft() {
    const GraphicsState next = g_graphicsDraft;
    if (!GraphicsDraftDirty()) {
        return;
    }
    g_revertBase = CurrentGraphicsState();
    g_pendingGraphics = next;
    g_revertDeadline = Clock::now() + kRevertTime;
    g_revertActive = true;
    ApplyGraphicsLive(next);
}

void KeepDisplay() {
    PersistGraphics(g_pendingGraphics);
    g_revertActive = false;
    g_graphicsDraft = CurrentGraphicsState();
}

void RevertDisplay() {
    g_revertActive = false;
    ApplyGraphicsLive(g_revertBase);
    g_graphicsDraft = CurrentGraphicsState();
}

// Applies and saves at once (no countdown); used by the reset buttons.
void ApplyAndPersistDisplay(DisplayState next) {
    g_revertActive = false;
    ApplyDisplay(NormalizeDisplay(next));
    PersistDisplay(CurrentDisplay());
    g_graphicsDraft = CurrentGraphicsState();
}

// ---- controller plumbing (positional [controller] bindings, applied to the pad layer) ------------------
using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};

// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // A Wii U Pro Controller has a fixed, known layout (A on the east position) that aurora
        // already maps by name; applying configured bindings on top would swap A/B and X/Y.
        if (const char* name = PADGetNameForControllerIndex(static_cast<uint32_t>(controllerIndex));
            name != nullptr && std::string_view(name).find("Wii U Pro") != std::string_view::npos) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::PortControllerButton(port, i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled();
bool g_wiiRemotesSaveFailed = false;
bool g_wiiAutoPair = RuntimeConfigFile::WiiAutoPairEnabled(true);
bool g_wiiSensorBarAbove = RuntimeConfigFile::WiiSensorBarAbove(false);
int g_wiiIrSensitivity = RuntimeConfigFile::WiiIrSensitivity(3);

std::string WiiRemoteLabel(const Wiimote::SlotInfo& info) {
    std::string label = info.model;
    if (!info.extension.empty()) {
        label += " + " + info.extension;
    }
    return label;
}

const char* KeyBindingName(int scancode) {
    if (scancode == PAD_KEY_INVALID) return "Unmapped";
    return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
        ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
}

// ---- menu state ----------------------------------------------------------------------------------------
enum Tab { kTabDisplay, kTabGraphics, kTabAudio, kTabControls, kTabSystem, kTabAbout, kTabCount };
constexpr std::array<const char*, kTabCount> kTabNames = {"Display", "Graphics", "Audio", "Controls", "System", "About"};
constexpr int kControlsPageCount = controls_menu::kPageCount + 1;  // + Devices
constexpr std::array<const char*, kControlsPageCount> kControlsPageNames = {"Overview", "Gamepad", "Keyboard & Mouse", "Aiming",
                                                                            "Devices"};

bool g_menuVisible = false;      // drawn (includes the fade in / out)
bool g_menuTarget = false;       // wants to be open
bool g_gameMode = false;
float g_anim = 0.0f;
std::atomic<int> g_request{0};   // 1 = open from game, 2 = close from game
std::atomic_bool g_gameOpenFlag{false};
std::atomic<void (*)(bool)> g_pauseCallback{nullptr};
bool g_pauseSent = false;

int g_tab = 0;
int g_controlsPage = 0;
float g_tabAnim = 1.0f;
int g_tabDir = 1;
std::array<game_ui::ListState, kTabCount + kControlsPageCount> g_states;
game_ui::ModalState g_revertModal;
game_ui::ModalState g_muteModal;
bool g_muteCapture = false;
Clock::time_point g_muteDeadline{};

float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

game_ui::ListState& StateForCurrentPage() {
    return g_states[static_cast<size_t>(g_tab == kTabControls ? kTabCount + g_controlsPage : g_tab)];
}

void SetPaused(bool paused) {
    if (paused == g_pauseSent) return;
    g_pauseSent = paused;
    if (auto callback = g_pauseCallback.load(std::memory_order_acquire)) callback(paused);
}

void OpenMenu(bool fromGame) {
    if (!g_menuTarget) {
        for (auto& state : g_states) state.Reset();
        g_tabAnim = 1.0f;
        if (!g_revertActive) g_graphicsDraft = CurrentGraphicsState();
    }
    g_menuTarget = true;
    g_menuVisible = true;
    g_gameMode = fromGame;
    game_ui::Suppress(0.25f);
    if (fromGame) {
        g_gameOpenFlag.store(true, std::memory_order_release);
        SetPaused(true);
    }
}

void CloseMenu() {
    g_menuTarget = false;
    if (!g_revertActive) g_graphicsDraft = CurrentGraphicsState();
    game_ui::Suppress(0.15f);
}

void ToggleDeveloperMenu() {
    if (g_menuTarget) {
        CloseMenu();
    } else {
        OpenMenu(false);
    }
}

void UpdateMenuState(float dt) {
    const int request = g_request.exchange(0, std::memory_order_acq_rel);
    if (request == 1) {
        OpenMenu(true);
    } else if (request == 2) {
        CloseMenu();
    }
    if (g_menuTarget) {
        g_anim = std::min(1.0f, g_anim + dt * 6.0f);
    } else {
        g_anim = std::max(0.0f, g_anim - dt * 8.0f);
        if (g_anim <= 0.0f && g_menuVisible) {
            g_menuVisible = false;
            g_gameOpenFlag.store(false, std::memory_order_release);
            g_gameMode = false;
            SetPaused(false);
        }
    }
}

void SwitchTab(int delta) {
    g_tab = (g_tab + delta + kTabCount) % kTabCount;
    g_tabAnim = 0.0f;
    g_tabDir = delta >= 0 ? 1 : -1;
}

void SelectTab(int tab) {
    if (tab == g_tab) return;
    g_tabDir = tab > g_tab ? 1 : -1;
    g_tab = tab;
    g_tabAnim = 0.0f;
}

// ---- reset helpers -------------------------------------------------------------------------------------
void ResetDisplayPage() {
    DisplayState next = CurrentDisplay();
    next.mode = static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
    next.vsync = false;
    next.forceAspect = false;
    next.widescreen = true;
    ApplyAndPersistDisplay(next);
    g_vrrMode = static_cast<int>(AURORA_VRR_AUTO);
    g_vrrMargin = 3.0f;
    g_vrrLimiter = true;
    RuntimeConfigFile::SetVrr(kVrrConfigNames[static_cast<size_t>(g_vrrMode)]);
    RuntimeConfigFile::SetVrrMarginFps(3);
    RuntimeConfigFile::SetVrrLimiter(true);
    ApplyVrr();
    SystemSettings::Values sys = SystemSettings::Get();
    sys.scalingFilter = 0;
    sys.showFrameTime = false;
    SystemSettings::Set(sys);
    SystemSettings::Save();
    aurora_set_resampler(SAMPLER_BILINEAR);
    g_showFps = true;
    RuntimeConfigFile::SetShowFps(true);
}

void ResetGraphicsPage() {
    DisplayState next = CurrentDisplay();
    next.resolution = 1.0f;
    next.interpolation = 0;
    ApplyAndPersistDisplay(next);
    ResetUpscaler();
    g_disableCopyFilter = true;
    GX_SYNC_INLINE();
    aurora_set_disable_copy_filter(true);
    RuntimeConfigFile::SetDisableCopyFilter(true);
    g_skipUnreadyPipelines = true;
    aurora_set_skip_unready_pipelines(true);
    RuntimeConfigFile::SetSkipUnreadyPipelines(true);
    SystemSettings::Values sys = SystemSettings::Get();
    sys.msaa = 1;
    sys.anisotropy = 16;
    SystemSettings::Set(sys);
    SystemSettings::Save();
    RuntimeUserConfig& userConfig = RuntimeConfigFile::Mutable();
    userConfig.textureReplacements = false;
    userConfig.textureDumps = false;
    userConfig.gxWorker = true;
    RuntimeConfigFile::WriteSetting("video", "texture_replacements", "false");
    RuntimeConfigFile::WriteSetting("video", "texture_dumps", "false");
    RuntimeConfigFile::WriteSetting("video", "gx_worker", "true");
}

void ResetAudioPage() {
    g_audioVolumePercent = 100;
    g_audioMuted = false;
    g_audioMixWorker = true;
    g_muteHotkey = SDL_SCANCODE_BACKSLASH;
    AudioBackend::Instance().SetMasterVolume(1.0f);
    AudioBackend::Instance().SetMuted(false);
    AxDspHle::SetMixWorkerEnabled(true);
    RuntimeConfigFile::SetAudioVolume(1.0f);
    RuntimeConfigFile::SetAudioMuted(false);
    RuntimeConfigFile::SetAudioMixWorker(true);
    RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
}

void ResetDevicesPage() {
    g_rumbleEnabled = true;
    PAD_HLE_SetRumbleEnabled(true);
    RuntimeConfigFile::SetRumbleEnabled(true);
    g_wiiAutoPair = true;
    RuntimeConfigFile::SetWiiAutoPairEnabled(true);
    g_wiiSensorBarAbove = false;
    RuntimeConfigFile::SetWiiSensorBarAbove(false);
    g_wiiIrSensitivity = 3;
    RuntimeConfigFile::SetWiiIrSensitivity(3);
    g_wiiRemotesEnabled = false;
    g_wiiRemotesSaveFailed = !RuntimeConfigFile::SetWiiRemotesEnabled(false);
}

void ResetSystemPage() {
    SystemSettings::Values sys = SystemSettings::Get();
    sys.startupCover = SystemSettings::StartupCover::Auto;
    SystemSettings::Set(sys);
    SystemSettings::Save();
}

void ResetCurrentPage() {
    switch (g_tab) {
    case kTabDisplay: ResetDisplayPage(); break;
    case kTabGraphics: ResetGraphicsPage(); break;
    case kTabAudio: ResetAudioPage(); break;
    case kTabControls:
        if (g_controlsPage < controls_menu::kPageCount) controls_menu::ResetPage(g_controlsPage);
        else ResetDevicesPage();
        break;
    case kTabSystem: ResetSystemPage(); break;
    default: break;
    }
}

void ResetEverything() {
    ResetDisplayPage();
    ResetGraphicsPage();
    ResetAudioPage();
    controls_menu::ResetAll();
    ResetDevicesPage();
    ResetSystemPage();
}

void RequestResetPage() {
    if (g_tab == kTabAbout) return;
    std::string what = kTabNames[static_cast<size_t>(g_tab)];
    if (g_tab == kTabControls) what += ": " + std::string(kControlsPageNames[static_cast<size_t>(g_controlsPage)]);
    game_ui::RequestConfirm("Reset page", "Reset every option on \"" + what + "\" to its default?", "Reset", [] { ResetCurrentPage(); });
}

bool RestartPending() {
    const LaunchValues& launch = Launch();
    const SystemSettings::Values sys = SystemSettings::Get();
    return sys.msaa != launch.msaa || sys.anisotropy != launch.anisotropy ||
           RuntimeConfigFile::TextureReplacements(false) != launch.textureReplacements ||
           RuntimeConfigFile::TextureDumps(false) != launch.textureDumps ||
           (RuntimeConfigFile::Mutable().gxWorker.value_or(launch.gxWorker) != launch.gxWorker) ||
           g_wiiRemotesEnabled != launch.wiiRemotes || g_widescreen != g_widescreenAtLaunch ||
           controls_menu::RestartPending();
}

// ---- pages ---------------------------------------------------------------------------------------------
struct WindowSizeItem {
    const char* label;
    int width;
    int height;
};
constexpr std::array<WindowSizeItem, 7> kWindowSizes = {{
    {"854 x 480", 854, 480}, {"1280 x 720", 1280, 720}, {"1600 x 900", 1600, 900}, {"1920 x 1080", 1920, 1080},
    {"2560 x 1440", 2560, 1440}, {"960 x 720 (4:3)", 960, 720}, {"1440 x 1080 (4:3)", 1440, 1080},
}};

void BuildDisplayPage(ListView& v) {
    DisplayState next = g_graphicsDraft.display;

    v.Section("Window");
    static constexpr const char* kDisplayModes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
    v.Choice("Display mode", &next.mode, kDisplayModes, 3, static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED),
             next.mode == AURORA_DISPLAY_MODE_EXCLUSIVE
                 ? "Exclusive fullscreen requests the closest native-resolution display mode to the output frame rate. Alt+Enter also switches modes."
                 : "Windowed, borderless fullscreen or exclusive fullscreen. Choose Apply changes to preview them together.",
             game_ui::kRowConfirm);
    if (v.Action("Apply display changes", GraphicsDraftDirty() ? "Apply and confirm" : "Up to date",
                 "Applies all pending Display and Graphics changes together, then asks once whether to keep or revert them.",
                 GraphicsDraftDirty() ? game_ui::kRowConfirm : game_ui::kRowDisabled)) {
        ApplyGraphicsDraft();
    }

    int sizeIndex = -1;
    static std::array<const char*, kWindowSizes.size()> sizeLabels{};
    for (size_t i = 0; i < kWindowSizes.size(); ++i) {
        sizeLabels[i] = kWindowSizes[i].label;
        if (kWindowSizes[i].width == static_cast<int>(next.windowWidth) &&
            kWindowSizes[i].height == static_cast<int>(next.windowHeight)) {
            sizeIndex = static_cast<int>(i);
        }
    }
    char customSize[48];
    std::snprintf(customSize, sizeof customSize, "%u x %u", next.windowWidth, next.windowHeight);
    if (v.Choice("Window size", &sizeIndex, sizeLabels.data(), static_cast<int>(sizeLabels.size()), 3,
                 "Windowed mode only. Choose Apply changes to resize the window.",
                 next.mode != AURORA_DISPLAY_MODE_WINDOWED ? game_ui::kRowDisabled : 0u, nullptr, customSize) & kChanged) {
        const WindowSizeItem& size = kWindowSizes[static_cast<size_t>(sizeIndex)];
        next.windowWidth = static_cast<uint32_t>(size.width);
        next.windowHeight = static_cast<uint32_t>(size.height);
    }
    v.Toggle("VSync", &next.vsync, false,
             "Syncs presentation to the display for tear-free motion and caps the frame rate at the refresh rate. Ignored by the Uncapped frame rate and while VRR is active. Choose Apply changes after editing.",
             game_ui::kRowConfirm);

    v.Section("Variable refresh rate");
    static constexpr const char* kVrrLabels[] = {"Auto", "On", "Off"};
    v.Choice("VRR (G-SYNC / FreeSync)", &g_graphicsDraft.vrrMode, kVrrLabels, 3,
             static_cast<int>(AURORA_VRR_AUTO),
             "Lets a G-SYNC / FreeSync / Windows VRR display follow the game's frame rate instead of a fixed refresh. Select the desired policy, then use Apply changes.",
             0u, "Auto");
    const uint32_t vrrRowFlags =
        g_graphicsDraft.vrrMode == static_cast<int>(AURORA_VRR_OFF) ? game_ui::kRowDisabled : 0u;
    v.Toggle("Latency limiter", &g_graphicsDraft.vrrLimiter, true,
             "Caps presents below the refresh rate while VRR is active. Changes take effect with Apply changes.",
             vrrRowFlags, "On");
    if (v.Slider("Latency limiter margin", &g_graphicsDraft.vrrMargin, 0.0f, 10.0f, 1.0f, "%.0f FPS", 3.0f,
                 "How far below the refresh rate the limiter holds the frame rate.",
                 vrrRowFlags | (g_graphicsDraft.vrrLimiter ? 0u : game_ui::kRowDisabled), "3 FPS")) {
        g_graphicsDraft.vrrMargin = std::round(g_graphicsDraft.vrrMargin);
    }
    {
        AuroraVrrStatus vrr{};
        aurora_get_vrr_status(&vrr);
        char text[160];
        std::string line;
        if (vrr.swapchainTearing) {
            std::snprintf(text, sizeof text, "%s (tearing allowed)", vrr.presentMode);
        } else if (std::string_view(vrr.presentMode) == "Fifo") {
            std::snprintf(text, sizeof text, "Fifo (VSync)");
        } else {
            std::snprintf(text, sizeof text, "%s", vrr.presentMode);
        }
        line = text;
        if (vrr.active && vrr.limiterActive) {
            std::snprintf(text, sizeof text, ", limit %.0f FPS", vrr.limitFps);
            line += text;
        } else if (vrr.active) {
            line += ", no limit";
        }
        if (vrr.plannedFps > 0.0) {
            std::snprintf(text, sizeof text, ", target %.0f FPS", vrr.plannedFps);
            line += text;
        }
        v.Info("Present mode", line);
        if (vrr.refreshHz > 0.0) {
            std::snprintf(text, sizeof text, "%.2f Hz", vrr.refreshHz);
            v.Info("Display refresh", text);
        }
        if (!vrr.tearingSupported) {
            v.Note("This system or graphics API does not allow tearing presents, so VRR is unavailable.", game_ui::kWarn);
        } else if (vrr.active) {
            v.Info("VRR", "Active (driver decides whether the display goes variable)", game_ui::kGood);
            if (static_cast<AuroraDisplayMode>(next.mode) == AURORA_DISPLAY_MODE_WINDOWED) {
                v.Note("Windowed: G-SYNC needs \"Enable for windowed and full screen mode\" in the NVIDIA Control Panel. Borderless fullscreen is the most reliable.");
            }
        } else if (g_graphicsDraft.vrrMode == static_cast<int>(AURORA_VRR_OFF)) {
            v.Info("VRR", "Off");
        } else {
            v.Info("VRR", vrr.refreshHz > 0.0 && vrr.refreshHz <= 63.2
                              ? "Idle (display is 60 Hz)"
                              : "Idle (set a Frame rate above Original in Graphics, or choose On)");
        }
    }

    v.Section("Picture");
    static constexpr const char* kAspects[] = {"4:3 (black bars)", "16:9 (black bars)", "Fill window"};
    int aspect = !next.forceAspect ? 2 : (next.widescreen ? 1 : 0);
    const uint32_t aspectFlags = game_ui::kRowConfirm | (next.widescreen != g_widescreenAtLaunch ? game_ui::kRowRestart : 0u);
    if (v.Choice("Aspect ratio", &aspect, kAspects, 3, 2,
                 "4:3 and 16:9 keep that shape and add black bars; Fill window stretches the picture to the window. Switching the game between 4:3 and 16:9 takes effect after a restart.",
                 aspectFlags) & kChanged) {
        if (aspect == 2) {
            next.forceAspect = false;
        } else {
            next.forceAspect = true;
            next.widescreen = aspect == 1;
        }
    }
    int filter = g_graphicsDraft.scalingFilter;
    if (v.Choice("Scaling filter", &filter, SystemSettings::kScalingFilterLabels.data(),
                 static_cast<int>(SystemSettings::kScalingFilterLabels.size()), 0,
                 "How the picture is resampled to the window.") & kChanged) {
        g_graphicsDraft.scalingFilter = filter;
    }

    v.Section("On-screen info");
    v.Toggle("FPS counter", &g_graphicsDraft.showFps, true,
             "Shows the presented frame rate in the top-right corner.");
    v.Toggle("Frame-time graph", &g_graphicsDraft.showFrameTime, false,
             "Host frame time in milliseconds with a short history; spikes show stutter.");

    g_graphicsDraft.display = next;
}

struct QualityPreset {
    float resolution;
    int msaa;
    int anisotropy;
};
constexpr std::array<QualityPreset, 4> kPresets = {{{1.0f, 1, 2}, {1.0f, 1, 16}, {2.0f, 2, 16}, {4.0f, 4, 16}}};
constexpr std::array<const char*, 5> kPresetNames = {"Low", "Medium", "High", "Ultra", "Custom"};

int DetectPreset(float resolution, int msaa, int anisotropy) {
    for (size_t i = 0; i < kPresets.size(); ++i) {
        if (std::fabs(kPresets[i].resolution - resolution) < 0.001f && kPresets[i].msaa == msaa &&
            kPresets[i].anisotropy == anisotropy) {
            return static_cast<int>(i);
        }
    }
    return 4;
}

void BuildUpscalingSection(ListView& v) {
    v.Section("Upscaling");
    AuroraDlssStatus dlssStatus{};
    aurora_get_dlss_status(&dlssStatus);
    const bool dlssAvailable = dlssStatus.state == AURORA_DLSS_AVAILABLE;
    const int upscalerCount = (dlssAvailable || g_graphicsDraft.upscaler == kUpscalerDlss)
                                  ? static_cast<int>(kUpscalerLabels.size())
                                  : kUpscalerDlss;
    int upscaler = g_graphicsDraft.upscaler;
    if (v.Choice("Upscaler", &upscaler, kUpscalerLabels.data(), upscalerCount, 0,
                 "Renders the game at a lower internal resolution and upscales it to the window; the menu and on-screen text stay at full resolution. FSR 1 is AMD's spatial upscaler (edge-adaptive upscale plus sharpening): it works on any GPU and needs no motion data, but cannot add detail the way a temporal upscaler can.") & kChanged) {
        g_graphicsDraft.upscaler = upscaler;
    }
    if (!dlssAvailable && g_graphicsDraft.upscaler != kUpscalerDlss) {
        v.Info("NVIDIA DLSS", dlssStatus.state == AURORA_DLSS_CHECKING || dlssStatus.state == AURORA_DLSS_NOT_STARTED
                                  ? std::string("checking...")
                                  : (dlssStatus.reason[0] != '\0' ? std::string(dlssStatus.reason)
                                                                  : std::string("NVIDIA RTX GPU and nvngx_dlss.dll required")));
    }
    if (g_graphicsDraft.upscaler == kUpscalerFsr1) {
        int quality = g_graphicsDraft.fsrQuality;
        if (v.Choice("FSR quality", &quality, kFsrQualityLabels.data(), static_cast<int>(kFsrQualityLabels.size()),
                     kDefaultFsrQualityIndex,
                     "Ratio between output and internal resolution (per axis). Ultra Quality renders 77% of the window size, Quality 67%, Balanced 59%, Performance 50%, Ultra Performance 33%. Native turns upscaling off.") & kChanged) {
            g_graphicsDraft.fsrQuality = quality;
        }
        if (v.Slider("FSR sharpness", &g_graphicsDraft.fsrSharpness, 0.0f, 100.0f, 5.0f, "%.0f%%",
                     static_cast<float>(RuntimeConfigFile::kDefaultFsrSharpness),
                     "Strength of the RCAS sharpening pass after upscaling. 0% turns the pass off. Applied together with the other pending settings.")) {
        }
        AuroraWindowSize window = aurora_get_window_size();
        AuroraUpscaleInfo info{};
        aurora_get_upscale_info(&info);
        const bool live = info.active != AURORA_UPSCALER_OFF && info.inputWidth != 0;
        const std::string internal = live ? ResolutionText(info.inputWidth, info.inputHeight)
                                          : ResolutionText(window.fb_width, window.fb_height);
        const std::string output = live ? ResolutionText(info.outputWidth, info.outputHeight)
                                        : ResolutionText(window.native_fb_width, window.native_fb_height);
        v.Info("Internal resolution", internal);
        v.Info("Output resolution", output);
        v.Note(live ? "FSR 1 is running." : "FSR 1 is selected but not running (Native quality, or the window is not larger than the internal size).");
    }
    if (g_graphicsDraft.upscaler == kUpscalerDlss) {
        int mode = g_graphicsDraft.dlssMode;
        if (v.Choice("DLSS mode", &mode, kDlssModeLabels.data(), static_cast<int>(kDlssModeLabels.size()),
                     static_cast<int>(RuntimeConfigFile::kDefaultDlssModeIndex),
                     "NVIDIA DLSS Super Resolution. The percentage is the internal resolution relative to the window (per axis): Ultra Performance 33%, Performance 50%, Balanced 58%, Quality 67%. DLAA renders at the full window size and only improves anti-aliasing. The game has no motion-vector buffer, so DLSS works from the image itself and is less sharp in motion than in a native DLSS game.") & kChanged) {
            g_graphicsDraft.dlssMode = mode;
        }
        if (v.Slider("DLSS sharpness", &g_graphicsDraft.dlssSharpness, 0.0f, 100.0f, 5.0f, "%.0f%%",
                     static_cast<float>(RuntimeConfigFile::kDefaultDlssSharpness),
                     "Contrast-adaptive sharpening after DLSS. 0% turns it off. Applied together with the other pending settings.")) {
        }
        if (v.Toggle("Estimated motion vectors", &g_graphicsDraft.dlssMotion, false,
                     "Estimates per-pixel motion from consecutive frames and gives it to DLSS. Off sends zero motion, so DLSS rejects history on its own (softer in motion, no wrong-vector smearing). Experimental: the estimate is noisy.",
                     game_ui::kRowExperimental, "Off") & kChanged) {
        }
        AuroraWindowSize window = aurora_get_window_size();
        AuroraUpscaleInfo info{};
        aurora_get_upscale_info(&info);
        const bool live = info.active == AURORA_UPSCALER_DLSS && info.inputWidth != 0;
        v.Info("Internal resolution", live ? ResolutionText(info.inputWidth, info.inputHeight)
                                           : ResolutionText(window.fb_width, window.fb_height));
        v.Info("Output resolution", live ? ResolutionText(info.outputWidth, info.outputHeight)
                                         : ResolutionText(window.native_fb_width, window.native_fb_height));
        if (live) {
            v.Note(std::string("DLSS is running (") + dlssStatus.version + ").");
        } else if (dlssAvailable) {
            v.Note("DLSS is selected but not running on this frame (window smaller than the internal size, or DLSS rejected the size). FSR 1 is used instead.");
        } else {
            v.Note(std::string("DLSS is not available: ") +
                   (dlssStatus.state == AURORA_DLSS_CHECKING || dlssStatus.state == AURORA_DLSS_NOT_STARTED
                        ? "still checking this GPU. FSR 1 is used meanwhile."
                        : (dlssStatus.reason[0] != '\0' ? std::string(dlssStatus.reason) : std::string("NVIDIA RTX GPU and nvngx_dlss.dll required")) +
                              ". FSR 1 is used instead."));
        }
    }
}

void BuildGraphicsPage(ListView& v) {
    DisplayState next = g_graphicsDraft.display;
    const bool draftUpscalerActive =
        (g_graphicsDraft.upscaler == kUpscalerFsr1 && g_graphicsDraft.fsrQuality > 0) ||
        g_graphicsDraft.upscaler == kUpscalerDlss;

    v.Note("A green star marks the recommended value of a row.");
    v.Section("Quality");
    int preset = DetectPreset(next.resolution, g_graphicsDraft.msaa, g_graphicsDraft.anisotropy);
    if (v.Choice("Graphics preset", &preset, kPresetNames.data(), static_cast<int>(kPresetNames.size()), 1,
                 "Sets resolution scale, anti-aliasing and anisotropic filtering together. Medium is the stock setup. Anti-aliasing and filtering apply after a restart.",
                 game_ui::kRowConfirm) & kChanged) {
        if (preset < static_cast<int>(kPresets.size())) {
            const QualityPreset& p = kPresets[static_cast<size_t>(preset)];
            next.resolution = p.resolution;
            g_graphicsDraft.msaa = p.msaa;
            g_graphicsDraft.anisotropy = p.anisotropy;
        }
    }
    if (v.Action("Apply graphics changes", GraphicsDraftDirty() ? "Apply and confirm" : "Up to date",
                 "Applies all pending Display and Graphics changes together, then asks once whether to keep or revert them.",
                 GraphicsDraftDirty() ? game_ui::kRowConfirm : game_ui::kRowDisabled)) {
        ApplyGraphicsDraft();
    }

    int resolutionIndex = 1;
    static std::array<const char*, kResolutions.size()> resolutionLabels{};
    for (size_t i = 0; i < kResolutions.size(); ++i) {
        resolutionLabels[i] = kResolutions[i].label;
        if (std::fabs(kResolutions[i].scale - next.resolution) < 0.001f) resolutionIndex = static_cast<int>(i);
    }
    if (v.Choice("Resolution scale", &resolutionIndex, resolutionLabels.data(), static_cast<int>(resolutionLabels.size()), 1,
                 draftUpscalerActive
                     ? "Not used while an upscaler is on: the Upscaling section below sets the internal resolution from the window size."
                     : "Rendering scale relative to the original 640x480 picture. Native (1x) is the original look. 6x and 8x are capped to 4x while frame interpolation is on.",
                 game_ui::kRowConfirm | (draftUpscalerActive ? game_ui::kRowDisabled : 0u)) & kChanged) {
        next.resolution = kResolutions[static_cast<size_t>(resolutionIndex)].scale;
    }

    static constexpr const char* kMsaaLabels[] = {"Off", "2x", "4x", "8x"};
    int msaaIndex = 0;
    for (size_t i = 0; i < SystemSettings::kMsaaChoices.size(); ++i) {
        if (SystemSettings::kMsaaChoices[i] == g_graphicsDraft.msaa) msaaIndex = static_cast<int>(i);
    }
    if (v.Choice("Anti-aliasing (MSAA)", &msaaIndex, kMsaaLabels, 4, 0,
                 "Off is the safest. Higher values cost GPU time and are untested with every effect.",
                 game_ui::kRowRestart, "Off") & kChanged) {
        g_graphicsDraft.msaa = SystemSettings::kMsaaChoices[static_cast<size_t>(msaaIndex)];
    }
    static constexpr const char* kAnisoLabels[] = {"Off", "2x", "4x", "8x", "16x"};
    int anisoIndex = 4;
    for (size_t i = 0; i < SystemSettings::kAnisotropyChoices.size(); ++i) {
        if (SystemSettings::kAnisotropyChoices[i] == g_graphicsDraft.anisotropy) anisoIndex = static_cast<int>(i);
    }
    if (v.Choice("Anisotropic filtering", &anisoIndex, kAnisoLabels, 5, 4,
                 "Sharper textures at grazing angles.", game_ui::kRowRestart, "16x") & kChanged) {
        g_graphicsDraft.anisotropy = SystemSettings::kAnisotropyChoices[static_cast<size_t>(anisoIndex)];
    }

    BuildUpscalingSection(v);

    // The built-in transform-based interpolation; it is not AMD's optical-flow FSR 3 Frame Generation (docs/FSR-NOTES.md).
    v.Section("Frame generation");
    static std::array<const char*, kFrameRates.size()> frameRateLabels{};
    for (size_t i = 0; i < kFrameRates.size(); ++i) {
        frameRateLabels[i] = kFrameRates[i].label;
    }
    if (v.Choice("Frame rate", &next.interpolation, frameRateLabels.data(), static_cast<int>(frameRateLabels.size()), 0,
                 "How many frames are shown per game frame. The game itself (logic, physics, audio, timing) always runs at its original 60 Hz. Extra frames are interpolated from object and camera motion, so they may show artifacts and cost GPU and CPU time. Match display follows your monitor; Uncapped presents as fast as the GPU allows (VSync is ignored).",
                 game_ui::kRowConfirm | game_ui::kRowExperimental) & kChanged) {
    }
    int capIndex = static_cast<int>(kFrameRateCaps.size()) - 1;
    for (size_t i = 0; i < kFrameRateCaps.size(); ++i) {
        if (g_graphicsDraft.frameRateCap <= kFrameRateCaps[i]) {
            capIndex = static_cast<int>(i);
            break;
        }
    }
    static constexpr const char* kCapLabels[] = {"240 FPS", "360 FPS", "500 FPS", "1000 FPS"};
    if (v.Choice("Frame rate limit", &capIndex, kCapLabels, static_cast<int>(kFrameRateCaps.size()), 3,
                 "Hard upper limit for every mode above Original, so Match display and Uncapped cannot run away on a fast GPU. Fixed rates above the limit are lowered to it.",
                 IsHighFrameRateIndex(next.interpolation) ? 0u : game_ui::kRowDisabled) & kChanged) {
        g_graphicsDraft.frameRateCap = kFrameRateCaps[static_cast<size_t>(capIndex)];
    }
    v.Note("Turn VSync on (Display tab) for tear-free motion at a fixed rate or Match display; with VSync the rate cannot exceed the display refresh.");

    v.Note("NVIDIA DLSS Frame Generation is not offered: it needs control of the swap chain (owned by the renderer) and true motion vectors, which this port does not have. The interpolation above is the frame generation available.");

    v.Section("Advanced");
    v.Toggle("Disable copy filter", &g_graphicsDraft.disableCopyFilter, true,
             "Skips the console's blur on framebuffer copies for a sharper picture.", 0, "On");
    v.Toggle("Skip draws while shaders compile", &g_graphicsDraft.skipUnreadyPipelines, true,
             "Avoids stutter while pipelines build; some effects may flicker on first use.", 0, "On");
    v.Toggle("Run GX on its own thread", &g_graphicsDraft.gxWorker, true,
             "Processes graphics commands on a separate thread.", game_ui::kRowRestart, "On");
    v.Toggle("Custom textures", &g_graphicsDraft.textureReplacements, false,
             "Loads replacement textures from texture_replacements/ next to Config.toml.", game_ui::kRowRestart);
    v.Toggle("Dump unmatched textures", &g_graphicsDraft.textureDumps, false,
             "Writes every texture without a replacement to the cache folder (needs custom textures).",
             game_ui::kRowRestart | (g_graphicsDraft.textureReplacements ? 0u : game_ui::kRowDisabled));
    v.Info("Graphics API", GraphicsApiDisplayName());
    v.Note("Pick another API with graphics_api in Config.toml (restart required).");

    g_graphicsDraft.display = next;
}

void BuildAudioPage(ListView& v) {
    v.Section("Volume");
    float volume = static_cast<float>(g_audioVolumePercent);
    if (auto r = v.Slider("Master volume", &volume, 0.0f, 100.0f, 5.0f, "%.0f%%", 100.0f, "Overall output volume of the game.")) {
        g_audioVolumePercent = static_cast<int>(std::lround(volume));
        const float gain = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(gain);
        if (r & kCommit) RuntimeConfigFile::SetAudioVolume(gain);
    }
    if (v.Toggle("Mute", &g_audioMuted, false, "Silences the game without changing the volume.") & kChanged) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    if (v.Action("Mute shortcut", KeyBindingName(g_muteHotkey),
                 "Keyboard key that toggles mute at any time, also during play. Press A or click, then press the key.")) {
        g_muteCapture = true;
        g_muteDeadline = Clock::now() + std::chrono::seconds(8);
        g_muteModal = game_ui::ModalState{};
    }
    v.Section("Advanced");
    if (v.Toggle("Mix audio on a worker thread", &g_audioMixWorker, true,
                 "Runs the AX/DSP voice mix off the game thread. Turn this off if you suspect an audio problem; the mix then runs inline.",
                 0, "On") & kChanged) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix before switching.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
}

void BuildDevicesPage(ListView& v) {
    controls_menu::BuildSchemeRow(v);
    v.Section("Controller");
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        v.Info("Player 1 controller", "None detected - plug one in", game_ui::kWarn);
    } else {
        std::vector<std::string> names;
        for (uint32_t i = 0; i < controllerCount; ++i) {
            const char* name = PADGetNameForControllerIndex(i);
            names.push_back(name != nullptr ? name : "Unknown controller");
        }
        std::vector<const char*> pointers;
        for (const std::string& name : names) pointers.push_back(name.c_str());
        int assigned = PADGetIndexForPort(0);
        if (v.Choice("Player 1 controller", &assigned, pointers.data(), static_cast<int>(pointers.size()), 0,
                     "Which connected controller plays. Controllers can also be hot-swapped by pressing a button on them.", 0,
                     nullptr, "None") & kChanged) {
            PADSetPortForIndex(static_cast<uint32_t>(assigned), 0);
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
            ApplyConfiguredMappings();
        }
    }
    if (v.Toggle("Controller vibration", &g_rumbleEnabled, true, "Rumble on gamepads.") & kChanged) {
        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
        if (!g_rumbleEnabled) {
            // Stop whatever is already running: the game will not send another motor command until its own
            // state machine decides to.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
        }
    }

    v.Section("Wii Remote");
    const std::string notice = Wiimote::PlatformNotice();
    if (!notice.empty()) v.Note(notice, game_ui::kWarn);
    if (v.Toggle("Use Wii Remotes", &g_wiiRemotesEnabled, false,
                 "Lets physical Wii Remotes connect. Takes effect after restarting the game. Turn this off to use a Wii U Pro Controller as a gamepad.",
                 game_ui::kRowRestart) & kChanged) {
        g_wiiRemotesSaveFailed = !RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
        if (g_wiiRemotesSaveFailed) g_wiiRemotesEnabled = !g_wiiRemotesEnabled;
    }
    if (g_wiiRemotesSaveFailed) v.Info("Wii Remotes", "Could not save to Config.toml", game_ui::kBad);
    const Wiimote::PairingInfo pairing = Wiimote::Pairing();
    const Wiimote::SlotInfo remote = Wiimote::Slot(0);
    if (remote.attached) {
        std::string state = WiiRemoteLabel(remote);
        if (remote.battery >= 0) state += "   Battery " + std::to_string(remote.battery) + "/4";
        v.Info("Player 1 Wii Remote", state, game_ui::kGood);
        if (!remote.asleep && v.Action("Disconnect Wii Remote", "Disconnect", "Ends the connection and powers the remote off.")) {
            Wiimote::Disconnect(0);
        }
    }
    if (v.Action("Pair a Wii Remote", pairing.searching ? "Searching..." : "Pair",
                 pairing.supported ? "Searches for a minute, like pressing SYNC on the console. Press the red SYNC button (or 1 and 2) on the remote. Paired before? Just press any button on it."
                                   : "Pair the Wii Remote in your system's Bluetooth settings, then press any button on it.",
                 pairing.supported ? 0u : game_ui::kRowDisabled)) {
        Wiimote::StartPairing();
    }
    if (!pairing.status.empty()) v.Info("Pairing", pairing.status);
    if (const size_t waiting = Wiimote::WaitingRemotes(); waiting != 0) {
        v.Info("Waiting", std::to_string(waiting) + " more Wii Remote(s) connected; joins when a player leaves");
    }
    if (v.Toggle("Look for Wii Remotes automatically", &g_wiiAutoPair, true,
                 "While no Wii Remote is connected, keep searching for one in pairing mode. Searching can briefly disturb Bluetooth headphones.") & kChanged) {
        RuntimeConfigFile::SetWiiAutoPairEnabled(g_wiiAutoPair);
    }
    int sensorBar = g_wiiSensorBarAbove ? 1 : 0;
    static constexpr const char* kSensorBar[] = {"Below TV", "Above TV"};
    if (v.Choice("Sensor bar", &sensorBar, kSensorBar, 2, 0, "Where the sensor bar sits. Takes effect the next time the game starts.",
                 game_ui::kRowNextLaunch) & kChanged) {
        g_wiiSensorBarAbove = sensorBar == 1;
        RuntimeConfigFile::SetWiiSensorBarAbove(g_wiiSensorBarAbove);
    }
    float sensitivity = static_cast<float>(g_wiiIrSensitivity);
    if (v.Slider("Pointer sensitivity", &sensitivity, 1.0f, 5.0f, 1.0f, "%.0f", 3.0f,
                 "The sensor bar sensitivity from the Wii settings. Takes effect the next time the game starts.",
                 game_ui::kRowNextLaunch) & kCommit) {
        g_wiiIrSensitivity = static_cast<int>(std::lround(sensitivity));
        RuntimeConfigFile::SetWiiIrSensitivity(g_wiiIrSensitivity);
    }

    const std::vector<controller_mapping_wizard::Candidate> candidates = controller_mapping_wizard::Candidates();
    if (!candidates.empty()) {
        v.Section("Unrecognized controllers");
        v.Note("These devices have no usable gamepad mapping. Set one up by pressing each control when asked.");
        for (const auto& candidate : candidates) {
            if (v.Action(candidate.name.c_str(), candidate.incompleteMapping ? "Fix mapping" : "Set up",
                         candidate.incompleteMapping ? "SDL matched a mapping without an analog stick for this device."
                                                     : "Walks through every control of this device.")) {
                controller_mapping_wizard::Start(candidate.id);
            }
        }
    }
}

void BuildControlsPage(ListView& v) {
    const int page = g_controlsPage;
    int selected = page;
    if (v.Tabs(kControlsPageNames.data(), kControlsPageCount, &selected, "Left / right switches the controls page.") & kChanged) {
        g_controlsPage = selected;
        g_tabAnim = 0.0f;
        g_tabDir = selected > page ? 1 : -1;
    }
    if (page < controls_menu::kPageCount) {
        controls_menu::BuildPage(v, page);
    } else {
        BuildDevicesPage(v);
    }
}

#if defined(_WIN32)
void OpenUserDataFolder() {
    const std::wstring folder = RuntimeConfigFile::ApplicationDataDirectory().wstring();
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#endif

void BuildSystemPage(ListView& v) {
    SystemSettings::Values sys = SystemSettings::Get();

    v.Section("Files");
    v.Info("Config", RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ResolveConfigPath()), game_ui::kTextNormal, true);
    const std::string nand = RuntimeConfigFile::NandRoot();
    v.Info("Save data (NAND)", nand.empty() ? "default, under the user data folder" : nand, game_ui::kTextNormal, true);
    const std::string dvd = RuntimeConfigFile::DvdRoot();
    v.Info("Game files", dvd.empty() ? "not set (paths.dvd_root in Config.toml)" : dvd, dvd.empty() ? game_ui::kWarn : game_ui::kTextNormal, true);
#if defined(_WIN32)
    if (v.Action("User data folder", "Open", "Opens the folder that holds Config.toml and the save data.")) {
        OpenUserDataFolder();
    }
#endif

    v.Section("Diagnostics");
    AuroraPresentTiming timing{};
    aurora_get_present_timing(&timing);
    char line[96];
    std::snprintf(line, sizeof line, "%.1f FPS (motion %.1f)", timing.framesPerSecond, timing.effectiveFramesPerSecond);
    v.Info("Presented", line);
    v.Info("Shaders compiling", std::to_string(aurora_get_queued_pipeline_count()));
    v.Note("While this menu is open the game keeps running with its input blocked unless the game paused it.");

    v.Section("Reset and exit");
    if (v.Action("Reset all settings", "Reset...", "Restores every option of every page to its default.", game_ui::kRowDanger)) {
        game_ui::RequestConfirm("Reset all settings", "Reset every option of every page to its default?", "Reset all",
                                [] { ResetEverything(); }, true);
    }
    if (v.Action("Quit game", "Quit...", "Closes the game.", game_ui::kRowDanger)) {
        game_ui::RequestConfirm("Quit", std::string("Quit ") + kGameTitle + "?", "Quit", [] { ExitForAuroraWindowClose(); }, true);
    }
}

void BuildAboutPage(ListView& v) {
    v.Section("Sin & Punishment: Star Successor recomp");
    v.Note("A static recompilation of the original game for PC. No game data is included; GPL-3.");
    v.Section("This session");
    v.Info("Graphics API", GraphicsApiDisplayName());
    const AuroraWindowSize size = aurora_get_window_size();
    v.Info("Window", std::to_string(size.width) + " x " + std::to_string(size.height));
    v.Info("Internal resolution", std::to_string(size.fb_width) + " x " + std::to_string(size.fb_height));
    const double refresh = aurora_get_display_refresh_rate();
    if (refresh > 0.0) {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f Hz", refresh);
        v.Info("Display refresh", text);
    }
    v.Info("Config version", std::to_string(SystemSettings::kConfigVersion));
    v.Section("Tips");
    v.Note("Esc or B goes back and closes this menu. Alt+Enter toggles fullscreen. LB / RB (Q / E) switch tabs, X resets a row, Y resets a page.");
}

// ---- drawing -------------------------------------------------------------------------------------------
void DrawTabIcon(ImDrawList* dl, int tab, ImVec2 c, float s, ImU32 colour, float alpha) {
    const ImU32 col = game_ui::Fade(colour, alpha);
    const float t = std::max(1.5f, s * 0.085f);
    switch (tab) {
    case kTabDisplay: {
        dl->AddRect(ImVec2(c.x - s * 0.5f, c.y - s * 0.38f), ImVec2(c.x + s * 0.5f, c.y + s * 0.24f), col, s * 0.08f, 0, t);
        dl->AddLine(ImVec2(c.x - s * 0.2f, c.y + s * 0.42f), ImVec2(c.x + s * 0.2f, c.y + s * 0.42f), col, t);
        dl->AddLine(ImVec2(c.x, c.y + s * 0.24f), ImVec2(c.x, c.y + s * 0.42f), col, t);
        break;
    }
    case kTabGraphics: {
        const float r = s * 0.5f;
        ImVec2 pts[6];
        for (int i = 0; i < 6; ++i) {
            const float a = -1.5707963f + static_cast<float>(i) * 1.0471976f;
            pts[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
        }
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
        for (int i = 1; i < 6; i += 2) dl->AddLine(c, pts[i], col, t * 0.8f);
        break;
    }
    case kTabAudio: {
        const ImVec2 poly[5] = {ImVec2(c.x - s * 0.5f, c.y - s * 0.16f), ImVec2(c.x - s * 0.24f, c.y - s * 0.16f),
                                ImVec2(c.x + s * 0.06f, c.y - s * 0.42f), ImVec2(c.x + s * 0.06f, c.y + s * 0.42f),
                                ImVec2(c.x - s * 0.24f, c.y + s * 0.16f)};
        dl->AddConvexPolyFilled(poly, 5, col);
        dl->AddLine(ImVec2(c.x - s * 0.5f, c.y - s * 0.16f), ImVec2(c.x - s * 0.5f, c.y + s * 0.16f), col, t);
        dl->AddLine(ImVec2(c.x - s * 0.5f, c.y + s * 0.16f), ImVec2(c.x - s * 0.24f, c.y + s * 0.16f), col, t);
        dl->PathArcTo(ImVec2(c.x + s * 0.06f, c.y), s * 0.26f, -0.9f, 0.9f, 12);
        dl->PathStroke(col, 0, t);
        dl->PathArcTo(ImVec2(c.x + s * 0.06f, c.y), s * 0.46f, -0.9f, 0.9f, 14);
        dl->PathStroke(col, 0, t);
        break;
    }
    case kTabControls: {
        dl->AddRect(ImVec2(c.x - s * 0.55f, c.y - s * 0.3f), ImVec2(c.x + s * 0.55f, c.y + s * 0.34f), col, s * 0.3f, 0, t);
        dl->AddLine(ImVec2(c.x - s * 0.3f, c.y - s * 0.1f), ImVec2(c.x - s * 0.3f, c.y + s * 0.18f), col, t);
        dl->AddLine(ImVec2(c.x - s * 0.44f, c.y + s * 0.04f), ImVec2(c.x - s * 0.16f, c.y + s * 0.04f), col, t);
        dl->AddCircleFilled(ImVec2(c.x + s * 0.2f, c.y - s * 0.04f), s * 0.06f, col, 10);
        dl->AddCircleFilled(ImVec2(c.x + s * 0.36f, c.y + s * 0.1f), s * 0.06f, col, 10);
        break;
    }
    case kTabSystem: {
        dl->AddCircle(c, s * 0.26f, col, 20, t);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.7853982f;
            const ImVec2 d(std::cos(a), std::sin(a));
            dl->AddLine(ImVec2(c.x + d.x * s * 0.3f, c.y + d.y * s * 0.3f), ImVec2(c.x + d.x * s * 0.48f, c.y + d.y * s * 0.48f), col, t * 1.5f);
        }
        dl->AddCircleFilled(c, s * 0.09f, col, 10);
        break;
    }
    default: {
        dl->AddCircle(c, s * 0.48f, col, 24, t);
        dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.22f), s * 0.065f, col, 10);
        dl->AddLine(ImVec2(c.x, c.y - s * 0.06f), ImVec2(c.x, c.y + s * 0.26f), col, t * 1.4f);
        break;
    }
    }
}

std::vector<std::pair<std::string, std::string>> FooterHints(const ListView& view, game_ui::Device device) {
    const bool pad = device == game_ui::Device::Gamepad;
    std::vector<std::pair<std::string, std::string>> hints;
    const std::string a = pad ? "A" : "Enter";
    switch (view.FocusedKind()) {
    case game_ui::RowKind::Toggle: hints.push_back({a, "Toggle"}); break;
    case game_ui::RowKind::Choice: hints.push_back({"< >", "Change"}); break;
    case game_ui::RowKind::Slider: hints.push_back({"< >", "Adjust"}); break;
    case game_ui::RowKind::Action: hints.push_back({a, "Select"}); break;
    case game_ui::RowKind::Bindings:
        hints.push_back({a, "Rebind"});
        hints.push_back({"< >", "Slot"});
        break;
    case game_ui::RowKind::Tabs: hints.push_back({"< >", "Page"}); break;
    default: break;
    }
    hints.push_back({pad ? "B" : "Esc", "Back"});
    hints.push_back({pad ? "LB/RB" : "Q/E", "Tab"});
    if (pad) hints.push_back({"X", "Apply / Confirm"});
    if (view.FocusedResettable()) hints.push_back({pad ? "L3" : "Del", view.FocusedKind() == game_ui::RowKind::Bindings ? "Clear" : "Reset"});
    hints.push_back({pad ? "Y" : "Ctrl+Del", "Reset page"});
    return hints;
}

bool AnyModalActive();

void DrawMenu(game_ui::Frame& in, bool interactive) {
    if (!g_menuVisible) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float S = game_ui::Scale();
    const float a = Ease(g_anim);
    const bool live = interactive && g_menuTarget;

    if (live) {
        if (in.back) CloseMenu();
        if (in.tabPrev) SwitchTab(-1);
        if (in.tabNext) SwitchTab(1);
        if (in.resetPage) RequestResetPage();
    }
    g_tabAnim = std::min(1.0f, g_tabAnim + in.dt * 7.0f);

    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, game_ui::Fade(IM_COL32(0, 8, 22, 150), a));
    const float pw = std::min(1320.0f * S, io.DisplaySize.x * 0.94f);
    const float ph = std::min(960.0f * S, io.DisplaySize.y * 0.94f);
    const float slide = (1.0f - a) * 36.0f * S;
    const ImVec2 pmin((io.DisplaySize.x - pw) * 0.5f, (io.DisplaySize.y - ph) * 0.5f + slide);
    const ImVec2 pmax(pmin.x + pw, pmin.y + ph);
    game_ui::DrawGlassPanel(dl, pmin, pmax, a, 9.0f, 42.0f);
    auto U = [&](float u) { return u * S; };

    // Caption, page icon and title.
    game_ui::TextCentered(dl, 20.0f, ImVec2((pmin.x + pmax.x) * 0.5f, pmin.y + U(30.0f)), game_ui::kTextNormal,
                          "SIN & PUNISHMENT: STAR SUCCESSOR RECOMP", game_ui::Outline::Thin, a);
    {
        const ImVec2 bmin(pmin.x + U(52.0f), pmin.y + U(46.0f));
        const ImVec2 bmax(bmin.x + U(72.0f), bmin.y + U(72.0f));
        dl->AddRectFilled(bmin, bmax, game_ui::Fade(IM_COL32(14, 44, 84, 235), a), U(14.0f));
        dl->AddRect(bmin, bmax, game_ui::Fade(IM_COL32(255, 255, 255, 235), a), U(14.0f), 0, std::max(1.5f, U(3.0f)));
        DrawTabIcon(dl, g_tab, ImVec2((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f), U(40.0f), game_ui::kWhite, a);
    }
    std::string title = std::string("- ") + kTabNames[static_cast<size_t>(g_tab)];
    if (g_tab == kTabControls) title += ": " + std::string(kControlsPageNames[static_cast<size_t>(g_controlsPage)]);
    title += " -";
    game_ui::TextCentered(dl, 50.0f, ImVec2((pmin.x + pmax.x) * 0.5f, pmin.y + U(84.0f)), game_ui::kTitleYellow, title.c_str(),
                          game_ui::Outline::Thick, a);

    // Tab strip with the shoulder-button glyphs.
    {
        const bool pad = in.device == game_ui::Device::Gamepad;
        const char* leftGlyph = pad ? "LB" : "Q";
        const char* rightGlyph = pad ? "RB" : "E";
        const float cy = pmin.y + U(160.0f);
        const float margin = U(52.0f);
        const float glyphL = game_ui::GlyphWidth(leftGlyph, false);
        const float glyphR = game_ui::GlyphWidth(rightGlyph, false);
        game_ui::DrawGlyph(dl, ImVec2(pmin.x + margin, cy), leftGlyph, a, false);
        game_ui::DrawGlyph(dl, ImVec2(pmax.x - margin - glyphR, cy), rightGlyph, a, false);
        const float x0 = pmin.x + margin + glyphL + U(18.0f);
        const float x1 = pmax.x - margin - glyphR - U(18.0f);
        const float gap = U(10.0f);
        const float height = U(56.0f);
        std::array<float, kTabCount> full{};
        float totalFull = 0.0f;
        for (int i = 0; i < kTabCount; ++i) {
            full[static_cast<size_t>(i)] = U(36.0f) + U(14.0f) + game_ui::TextSize(26.0f, kTabNames[static_cast<size_t>(i)]).x + U(34.0f);
            totalFull += full[static_cast<size_t>(i)] + (i + 1 < kTabCount ? gap : 0.0f);
        }
        const bool allLabels = totalFull <= (x1 - x0);
        float total = 0.0f;
        std::array<float, kTabCount> widths{};
        for (int i = 0; i < kTabCount; ++i) {
            widths[static_cast<size_t>(i)] = (allLabels || i == g_tab) ? full[static_cast<size_t>(i)] : height;
            total += widths[static_cast<size_t>(i)] + (i + 1 < kTabCount ? gap : 0.0f);
        }
        float x = x0 + std::max(0.0f, (x1 - x0 - total) * 0.5f);
        for (int i = 0; i < kTabCount; ++i) {
            const float w = widths[static_cast<size_t>(i)];
            const ImVec2 mn(x, cy - height * 0.5f);
            const ImVec2 mx(x + w, cy + height * 0.5f);
            const bool sel = i == g_tab;
            const bool hot = live && in.mouseValid && game_ui::PointIn(in.mouse, mn, mx);
            game_ui::DrawPill(dl, mn, mx,
                              sel ? IM_COL32(18, 58, 112, 238) : (hot ? IM_COL32(255, 255, 255, 80) : IM_COL32(255, 255, 255, 36)),
                              sel ? IM_COL32(255, 255, 255, 250) : IM_COL32(255, 255, 255, 120), a, sel ? std::max(2.0f, U(3.0f)) : 1.5f);
            const bool showLabel = allLabels || sel;
            const ImU32 ink = sel ? game_ui::kWhite : game_ui::kTextNormal;
            const float iconX = showLabel ? mn.x + U(18.0f) + U(18.0f) : (mn.x + mx.x) * 0.5f;
            DrawTabIcon(dl, i, ImVec2(iconX, cy), U(30.0f), ink, a);
            if (showLabel) {
                const ImVec2 ts = game_ui::TextSize(26.0f, kTabNames[static_cast<size_t>(i)]);
                game_ui::Text(dl, 26.0f, ImVec2(mn.x + U(18.0f) + U(36.0f) + U(10.0f), cy - ts.y * 0.5f), ink,
                              kTabNames[static_cast<size_t>(i)], game_ui::Outline::Thick, a);
            }
            if (hot && in.mouseClicked) SelectTab(i);
            x += w + gap;
        }
    }

    // Content list.
    const ImVec2 cmin(pmin.x + U(8.0f), pmin.y + U(206.0f));
    const ImVec2 cmax(pmax.x - U(8.0f), pmax.y - U(232.0f));
    const float ease = Ease(g_tabAnim);
    game_ui::ListView v(StateForCurrentPage(), in, dl, cmin, cmax, live, a * ease, (1.0f - ease) * static_cast<float>(g_tabDir) * U(46.0f));
    switch (g_tab) {
    case kTabDisplay: BuildDisplayPage(v); break;
    case kTabGraphics: BuildGraphicsPage(v); break;
    case kTabAudio: BuildAudioPage(v); break;
    case kTabControls: BuildControlsPage(v); break;
    case kTabSystem: BuildSystemPage(v); break;
    default: BuildAboutPage(v); break;
    }
    v.End();

    if (live && in.apply && !AnyModalActive()) {
        // Consume this edge so opening the display prompt cannot also accept it.
        in.apply = false;
        if (g_tab == kTabDisplay || g_tab == kTabGraphics) {
            ApplyGraphicsDraft();
        } else {
            // These pages apply edits live; explicitly flush their settings.
            AimSettings::Save();
            SystemSettings::Save();
        }
    }

    // Help line for the highlighted row.
    {
        const ImVec2 hmin(pmin.x + U(48.0f), pmax.y - U(222.0f));
        const ImVec2 hmax(pmax.x - U(48.0f), pmax.y - U(108.0f));
        dl->AddRectFilled(hmin, hmax, game_ui::Fade(IM_COL32(8, 30, 62, 120), a), U(16.0f));
        dl->AddRect(hmin, hmax, game_ui::Fade(IM_COL32(255, 255, 255, 70), a), U(16.0f), 0, 1.0f);
        const std::string& help = v.FocusedHelp();
        dl->PushClipRect(hmin, hmax, true);
        const float wrap = hmax.x - hmin.x - U(40.0f);
        float helpSize = 23.0f;
        const float availableHeight = hmax.y - hmin.y - U(18.0f);
        while (helpSize > 16.0f && game_ui::WrappedHeight(helpSize, wrap, help) > availableHeight) {
            helpSize -= 1.0f;
        }
        const float textH = game_ui::WrappedHeight(helpSize, wrap, help);
        game_ui::DrawWrapped(dl, helpSize, ImVec2(hmin.x + U(20.0f), hmin.y + (hmax.y - hmin.y - textH) * 0.5f), wrap, game_ui::kWhite,
                             help, game_ui::Outline::Thin, a);
        dl->PopClipRect();
    }

    // Footer: context-sensitive button hints and the restart badge.
    {
        const float cy = pmax.y - U(62.0f);
        const float left = pmin.x + U(52.0f);
        game_ui::DrawHints(dl, ImVec2(left, cy), FooterHints(v, in.device), a, 0);
        if (RestartPending()) {
            const float w = game_ui::BadgeWidth("RESTART REQUIRED");
            game_ui::DrawBadge(dl, ImVec2(pmax.x - U(52.0f) - w, cy - U(13.0f)), "RESTART REQUIRED", game_ui::Badge::Restart, a);
        }
    }
}

// The Keep / Revert countdown, the mute-key capture and the controls dialogs. Only one is shown at a time.
bool AnyModalActive() {
    return controls_menu::Capturing() || controller_mapping_wizard::IsActive() || g_muteCapture || g_revertActive ||
           game_ui::ConfirmOpen();
}

void DrawRevertPrompt(game_ui::Frame& in) {
    const float remaining = std::chrono::duration<float>(g_revertDeadline - Clock::now()).count();
    if (remaining <= 0.0f) {
        RevertDisplay();
        g_revertModal = game_ui::ModalState{};
        return;
    }
    if (in.apply) {
        in.apply = false;
        KeepDisplay();
        g_revertModal = game_ui::ModalState{};
        return;
    }
    game_ui::ModalSpec spec;
    spec.title = "Keep display settings?";
    spec.lines = {"The previous settings return in " + std::to_string(static_cast<int>(std::ceil(remaining))) +
                  " seconds unless you keep these.", AimSettings::Get().playStationPrompts
                      ? "Press Square to confirm and keep these settings."
                      : "Press X to confirm and keep these settings."};
    spec.buttons = {"Keep", "Revert"};
    spec.defaultButton = 0;
    spec.cancelButton = 1;
    spec.progress = remaining / std::chrono::duration<float>(kRevertTime).count();
    const int result = game_ui::DrawModal(g_revertModal, in, spec);
    if (result == 0) KeepDisplay();
    else if (result == 1) RevertDisplay();
}

void DrawMuteCapture(game_ui::Frame& in) {
    const float remaining = std::chrono::duration<float>(g_muteDeadline - Clock::now()).count();
    game_ui::ModalSpec spec;
    spec.title = "Mute shortcut";
    spec.lines = {"Press the keyboard key that should toggle mute.", "Esc cancels."};
    spec.buttons = {"Clear shortcut", "Cancel"};
    spec.defaultButton = 1;
    spec.cancelButton = 1;
    spec.progress = std::clamp(remaining / 8.0f, 0.0f, 1.0f);
    const int result = game_ui::DrawModal(g_muteModal, in, spec);
    if (result == 0) {
        g_muteHotkey = PAD_KEY_INVALID;
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_muteCapture = false;
    } else if (result == 1 || remaining <= 0.0f) {
        g_muteCapture = false;
    }
    if (!g_muteCapture) g_muteModal = game_ui::ModalState{};
}

void DrawModals(game_ui::Frame& in) {
    if (controls_menu::Capturing()) {
        controls_menu::DrawModals(in);
    } else if (controller_mapping_wizard::IsActive()) {
        controller_mapping_wizard::Draw(in);
    } else if (g_muteCapture) {
        DrawMuteCapture(in);
    } else if (g_revertActive) {
        DrawRevertPrompt(in);
    } else if (game_ui::ConfirmOpen()) {
        game_ui::DrawConfirm(in);
    }
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    const FrameRateItem& frameRate = kFrameRates[static_cast<size_t>(g_frameInterpolationMode)];
    const uint32_t targetFps = frameRate.kind == RuntimeConfigFile::FrameRateKind::Fixed ? frameRate.fps : 0u;
    const double refreshRate = aurora_get_display_refresh_rate();
    const bool interpolationInactive = IsHighFrameRateMode() && !aurora_display_shows_interpolation();
    AuroraVrrStatus vrrStatus{};
    aurora_get_vrr_status(&vrrStatus);
    const bool unevenPacing =
        !g_vsync && !vrrStatus.limiterActive && !interpolationInactive && refreshRate > 0.0 && targetFps > refreshRate + 1.0;
    const bool showFrameTime = SystemSettings::Get().showFrameTime;
    static std::array<float, 120> s_frameTimes{};
    static size_t s_frameTimeIndex = 0;
    s_frameTimes[s_frameTimeIndex] = ImGui::GetIO().DeltaTime * 1000.0f;
    s_frameTimeIndex = (s_frameTimeIndex + 1) % s_frameTimes.size();
    if (!g_showFps && !showFrameTime && !unevenPacing && !interpolationInactive) {
        return;
    }

    std::vector<std::pair<std::string, ImU32>> lines;
    char text[160];
    if (g_showFps) {
        if (presentTiming.sampleCount == 0) {
            lines.push_back({"FPS --", game_ui::kWhite});
        } else {
            // Present timing includes the additional frames produced by interpolation, so this remains the
            // actual displayed FPS.
            std::snprintf(text, sizeof text, "FPS %.1f", presentTiming.framesPerSecond);
            lines.push_back({text, game_ui::kWhite});
            // Replay-unsafe frames hold the presented cadence with duplicated slots, so the counter alone reads
            // 180 while the motion on screen is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond < presentTiming.framesPerSecond * 0.95) {
                std::snprintf(text, sizeof text, "Motion %.1f", presentTiming.effectiveFramesPerSecond);
                lines.push_back({text, game_ui::kWarn});
            }
        }
    }
    if (showFrameTime) {
        const float latest = s_frameTimes[(s_frameTimeIndex + s_frameTimes.size() - 1) % s_frameTimes.size()];
        std::snprintf(text, sizeof text, "Frame %.2f ms", latest);
        lines.push_back({text, game_ui::kWhite});
    }
    if (interpolationInactive) {
        std::snprintf(text, sizeof text, "%s has no effect on a %.0f Hz display.", frameRate.label, refreshRate);
        lines.push_back({text, game_ui::kWarn});
    }
    if (unevenPacing) {
        std::snprintf(text, sizeof text, "%u FPS on a %.0f Hz display without VSync is not smooth.", targetFps, refreshRate);
        lines.push_back({text, game_ui::kWarn});
        lines.push_back({"Use Match display or enable VSync.", game_ui::kWarn});
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float margin = game_ui::Px(14.0f);
    const ImVec2 anchor(io.DisplaySize.x - margin, margin);
    const float used = game_ui::DrawHudBox(dl, anchor, lines);
    if (showFrameTime) {
        const float w = game_ui::Px(220.0f);
        const float h = game_ui::Px(56.0f);
        const ImVec2 mn(anchor.x - w, anchor.y + used + game_ui::Px(6.0f));
        const ImVec2 mx(anchor.x, mn.y + h);
        dl->AddRectFilled(mn, mx, IM_COL32(10, 36, 72, 175), game_ui::Px(12.0f));
        dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 120), game_ui::Px(12.0f), 0, 1.0f);
        const float pad = game_ui::Px(8.0f);
        const float step = (w - pad * 2.0f) / static_cast<float>(s_frameTimes.size() - 1);
        const float y16 = mx.y - pad - (h - pad * 2.0f) * (16.7f / 33.3f);
        dl->AddLine(ImVec2(mn.x + pad, y16), ImVec2(mx.x - pad, y16), IM_COL32(255, 255, 255, 60), 1.0f);
        ImVec2 previous{};
        for (size_t i = 0; i < s_frameTimes.size(); ++i) {
            const float ms = s_frameTimes[(s_frameTimeIndex + i) % s_frameTimes.size()];
            const float t = std::clamp(ms / 33.3f, 0.0f, 1.0f);
            const ImVec2 p(mn.x + pad + step * static_cast<float>(i), mx.y - pad - (h - pad * 2.0f) * t);
            if (i > 0) dl->AddLine(previous, p, ms > 20.0f ? IM_COL32(255, 150, 120, 255) : IM_COL32(150, 240, 150, 255), 1.5f);
            previous = p;
        }
    }
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }
    char text[64];
    std::snprintf(text, sizeof text, "%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    const float margin = game_ui::Px(14.0f);
    const float width = game_ui::TextSize(22.0f, text).x + game_ui::Px(24.0f);
    game_ui::DrawHudBox(ImGui::GetForegroundDrawList(), ImVec2(margin + width, margin), {{text, game_ui::kWhite}});
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
    const float cx = io.DisplaySize.x * 0.5f;
    const float cy = io.DisplaySize.y * 0.5f;
    game_ui::TextCentered(dl, 76.0f, ImVec2(cx, cy - game_ui::Px(46.0f)), game_ui::kTitleYellow, "Sin & Punishment", game_ui::Outline::Thick);
    game_ui::TextCentered(dl, 40.0f, ImVec2(cx, cy + game_ui::Px(24.0f)), game_ui::kWhite, "Star Successor recomp", game_ui::Outline::Thick);
    float y = cy + game_ui::Px(96.0f);
    if (g_bootShaderNotice && !g_bootShadersReady.load(std::memory_order_relaxed)) {
        char line[96];
        std::snprintf(line, sizeof(line), "Compiling shaders, please hold on: %u remaining", aurora_get_queued_pipeline_count());
        game_ui::TextCentered(dl, 26.0f, ImVec2(cx, y), game_ui::kTextNormal, line, game_ui::Outline::Thin);
        y += game_ui::Px(40.0f);
    }
    const bool waitForButton = SystemSettings::Get().startupCover == SystemSettings::StartupCover::Button &&
                               !g_strapInputAccepted.load(std::memory_order_relaxed);
    if (waitForButton) {
        game_ui::TextCentered(dl, 26.0f, ImVec2(cx, y), game_ui::kTextNormal, "Press any button to start", game_ui::Outline::Thin);
    }
}

void UpdateBootShaderState() {
    if (g_bootShadersReady.load(std::memory_order_relaxed)) {
        return;
    }
    const auto now = Clock::now();
    if (g_bootShaderWaitStart == Clock::time_point{}) {
        g_bootShaderWaitStart = now;
    }
    const uint32_t queued = aurora_get_queued_pipeline_count();
    g_bootShaderNotice |= queued > kBootShaderNoticeThreshold;
    if (queued == 0 || now - g_bootShaderWaitStart > kBootShaderWaitLimit) {
        g_bootShadersReady.store(true, std::memory_order_release);
    }
}

// Steam's desktop layout maps B to Escape on Valve controllers.
bool ValveControllerConnected() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    bool found = false;
    for (int i = 0; ids != nullptr && i < count && !found; ++i) {
        found = SDL_GetJoystickVendorForID(ids[i]) == 0x28DE && SDL_GetJoystickProductForID(ids[i]) != 0x11FF;
    }
    SDL_free(ids);
    return found;
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
    const bool shouldHide =
        !g_menuVisible && !AnyModalActive() && Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the menu, so the active mode is
// compared against the last persisted one every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

void ApplyInputBlockState() {
    const bool blocked = controller_mapping_wizard::IsActive() || controls_menu::Capturing() || g_muteCapture ||
                         g_revertActive || game_ui::ConfirmOpen() || g_menuVisible || StartupScreenVisible();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}

// Back + Start on gamepad 0 opens / closes the menu (developer fallback; the game hook is the normal entry).
void PollGamepadMenuChord() {
    static bool wasDown = false;
    SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
    const bool down = pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
                      SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    if (down && !wasDown && !AnyModalActive()) {
        ToggleDeveloperMenu();
        ApplyInputBlockState();
    }
    wasDown = down;
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    AimInput::Install();
    AimSettings::UpgradeLayoutConfig();
    game_ui::InstallFont();
    aurora_set_resampler(static_cast<AuroraSampler>(SystemSettings::Get().scalingFilter));
    SystemSettings::EnsureVersionStamp();
    PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    LimitResolutionForFrameRate();
    ApplyUpscaler();
    ApplyFrameRate(g_frameInterpolationMode);
    ApplyVrr();
    if (const char* launcherMode = std::getenv("RECOMPLAUNCHER_MODE");
        launcherMode && std::string_view(launcherMode) == "bigpicture") {
        g_displayMode = static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    g_bootShadersReady.store(false, std::memory_order_relaxed);
    g_bootShaderNotice = false;
    g_bootShaderWaitStart = {};
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

void HandleEvents(const AuroraEvent* events) noexcept {
    // Events are dispatched between frames, the only time the font atlas may change.
    game_ui::InstallFont();
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        AimInput::HandleSdlEvent(ev->sdl);
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (SystemSettings::Get().startupCover == SystemSettings::StartupCover::Button &&
            (ev->sdl.type == SDL_EVENT_KEY_DOWN || ev->sdl.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             ev->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev->sdl.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) &&
            !g_menuVisible) {
            NotifyStrapInputAccepted();
        }
        const bool bindCapture = controls_menu::Capturing();
        if (bindCapture) {
            if (IsMouseActivity(ev->sdl)) g_lastMouseActivity = Clock::now();
            continue;
        }
        if (g_muteCapture) {
            if (ev->sdl.type == SDL_EVENT_KEY_DOWN && !ev->sdl.key.repeat) {
                const SDL_Scancode code = ev->sdl.key.scancode;
                if (code == SDL_SCANCODE_ESCAPE) {
                    g_muteCapture = false;
                } else if (code == SDL_SCANCODE_BACKSPACE || code == SDL_SCANCODE_DELETE) {
                    g_muteHotkey = PAD_KEY_INVALID;
                    RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
                    g_muteCapture = false;
                } else if (code != SDL_SCANCODE_UNKNOWN) {
                    g_muteHotkey = static_cast<int32_t>(code);
                    RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
                    g_muteCapture = false;
                }
            }
            if (IsMouseActivity(ev->sdl)) g_lastMouseActivity = Clock::now();
            continue;
        }
        if (IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            if (!AnyModalActive()) {
                ToggleDeveloperMenu();
                ApplyInputBlockState();
            }
        }
        if (g_muteHotkey != PAD_KEY_INVALID && IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        // While the menu or a dialog is open Esc is a normal "back" input handled in Draw(); with nothing
        // open it asks whether to quit.
        if (IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE) && !g_menuVisible && !AnyModalActive() && !ValveControllerConnected() &&
            !AimSettings::KeyBound(AimSettings::Get(),SDL_SCANCODE_ESCAPE)) {
            game_ui::RequestConfirm("Quit", std::string("Quit ") + kGameTitle + "?", "Quit", [] { ExitForAuroraWindowClose(); }, true);
            ApplyInputBlockState();
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    Wiimote::Shutdown();
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

// QA hook: SIN_UI_FUZZ=<seed>,<interval_ms>,<key list e.g. DOWN+UP+LEFT+RIGHT+ENTER+TAB+DEL+F10> pushes random key presses
// into the SDL queue once SIN_UI_FUZZ_START seconds have passed (logs each key to the console for replay).
void InjectFuzzKeys() {
    struct Fuzz {
        bool enabled = false;
        uint32_t rng = 1;
        int intervalMs = 150;
        std::vector<SDL_Scancode> keys;
        std::vector<std::string> names;
        Clock::time_point next{};
        Clock::time_point start{};
        int count = 0;
    };
    static Fuzz fuzz = [] {
        Fuzz f;
        const char* env = std::getenv("SIN_UI_FUZZ");
        if (!env || !*env) return f;
        const std::string spec = env;
        const size_t a = spec.find(','), b = spec.find(',', a == std::string::npos ? 0 : a + 1);
        f.rng = static_cast<uint32_t>(std::atoi(spec.c_str())) * 2654435761u + 12345u;
        if (a != std::string::npos) f.intervalMs = std::max(20, std::atoi(spec.c_str() + a + 1));
        const std::string list = b == std::string::npos ? "DOWN+UP+LEFT+RIGHT+ENTER+TAB+DOWN+RIGHT" : spec.substr(b + 1);
        static const std::pair<const char*, SDL_Scancode> table[] = {
            {"DOWN", SDL_SCANCODE_DOWN}, {"UP", SDL_SCANCODE_UP}, {"LEFT", SDL_SCANCODE_LEFT}, {"RIGHT", SDL_SCANCODE_RIGHT},
            {"ENTER", SDL_SCANCODE_RETURN}, {"TAB", SDL_SCANCODE_TAB}, {"ESC", SDL_SCANCODE_ESCAPE}, {"DEL", SDL_SCANCODE_DELETE},
            {"Q", SDL_SCANCODE_Q}, {"E", SDL_SCANCODE_E}, {"PGDN", SDL_SCANCODE_PAGEDOWN}, {"PGUP", SDL_SCANCODE_PAGEUP},
            {"F10", SDL_SCANCODE_F10}, {"SPACE", SDL_SCANCODE_SPACE}, {"BACKSPACE", SDL_SCANCODE_BACKSPACE}};
        for (size_t at = 0; at <= list.size();) {
            const size_t plus = list.find('+', at);
            const std::string name = list.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
            for (const auto& entry : table) {
                if (name == entry.first) {
                    f.keys.push_back(entry.second);
                    f.names.push_back(name);
                }
            }
            if (plus == std::string::npos) break;
            at = plus + 1;
        }
        const char* startEnv = std::getenv("SIN_UI_FUZZ_START");
        f.start = Clock::now() + std::chrono::milliseconds(static_cast<int>((startEnv ? std::atof(startEnv) : 0.0) * 1000.0));
        f.next = f.start;
        f.enabled = !f.keys.empty();
        return f;
    }();
    if (!fuzz.enabled || Clock::now() < fuzz.next) return;
    fuzz.next = Clock::now() + std::chrono::milliseconds(fuzz.intervalMs);
    fuzz.rng = fuzz.rng * 1664525u + 1013904223u;
    const size_t pick = (fuzz.rng >> 8) % fuzz.keys.size();
    const SDL_Scancode code = fuzz.keys[pick];
    SDL_Event ev{};
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.scancode = code;
    ev.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
    ev.key.down = true;
    ev.key.windowID = SDL_GetWindowID(aurora_get_window());
    SDL_PushEvent(&ev);
    ev.type = SDL_EVENT_KEY_UP;
    ev.key.down = false;
    SDL_PushEvent(&ev);
    std::fprintf(stderr, "[ui-fuzz] #%d %s\n", ++fuzz.count, fuzz.names[pick].c_str());
}

void Draw() noexcept {
    // ImGui is only touched on this thread; the render worker replays a frozen copy.
    static int s_lastFrame = -1;
    const int frame = ImGui::GetFrameCount();
    if (frame == s_lastFrame) {
        return;
    }
    s_lastFrame = frame;
    InjectFuzzKeys();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    PollGamepadMenuChord();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    UpdateBootShaderState();
    game_ui::Frame& in = game_ui::PollInput();
    UpdateMenuState(in.dt);
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    DrawFpsOverlay();
    controls_menu::DrawHud();
    const bool modal = AnyModalActive();
    DrawMenu(in, !modal);
    DrawModals(in);
    ApplyInputBlockState();
    AimInput::UpdateWindowState(InputBindings::InputBlocked());
    DrawStartupScreen();
}

bool StartupScreenVisible() noexcept {
    // Show the game's output immediately; no host splash or startup input gate.
    return false;
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept {
    ++g_presentedFrame;
    // The startup cover was built around the donor game's wrist-strap scene, which signalled
    // NotifyStrapInputAccepted(). Sin & Punishment has no such hook (its own strap screen is part of
    // the game and must stay visible), so release the cover once the game has been presenting frames
    // for about two seconds; boot-shader readiness still gates it separately.
    constexpr uint64_t kAutoDismissFrame = 120;
    switch (SystemSettings::Get().startupCover) {
    case SystemSettings::StartupCover::Auto:
        if (g_presentedFrame == kAutoDismissFrame) {
            NotifyStrapInputAccepted();
        }
        break;
    case SystemSettings::StartupCover::Skip: {
        bool expected = false;
        if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            g_startupDismissFrame.store(0, std::memory_order_release);
        }
        break;
    }
    case SystemSettings::StartupCover::Button:
        break;
    }
}

void OpenFromGame() noexcept {
    g_gameOpenFlag.store(true, std::memory_order_release);
    g_request.store(1, std::memory_order_release);
}

void CloseFromGame() noexcept { g_request.store(2, std::memory_order_release); }

bool IsOpenFromGame() noexcept { return g_gameOpenFlag.load(std::memory_order_acquire); }

void SetPauseCallback(void (*callback)(bool paused)) noexcept {
    g_pauseCallback.store(callback, std::memory_order_release);
}
} // namespace settings_overlay

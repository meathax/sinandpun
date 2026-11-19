#include "settings_overlay.h"
#include "gx_worker.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "aim_input.h"
#include "controls_menu.h"
#include "input_bindings.h"
#include "motion_trick.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
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

bool g_topBarVisible = false;
bool g_exitPromptOpen = false;
bool g_restartPromptOpen = false;
bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    default:
        return 0;
    }
}();
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
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
bool g_forceAspect = RuntimeConfigFile::ForceAspectEnabled();
bool g_widescreen = RuntimeConfigFile::WidescreenEnabled(true);
#if defined(__APPLE__)
bool g_metalFxSpatialUpscaling = RuntimeConfigFile::MetalFxSpatialUpscaling(false);
#endif
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
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

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

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

constexpr std::array<uint32_t, 3> kFrameInterpolationTargetFps{0, 120, 180};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
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

// Display options that can leave the user without a usable picture are applied live, then kept or
// reverted: the prompt auto-reverts after kRevertSeconds like an OS display-settings dialog.
struct DisplayState {
    int mode;
    float resolution;
    int interpolation;
    bool vsync;
    bool forceAspect;

    bool operator==(const DisplayState& o) const {
        return mode == o.mode && resolution == o.resolution && interpolation == o.interpolation &&
               vsync == o.vsync && forceAspect == o.forceAspect;
    }
};

DisplayState CurrentDisplay() {
    return {g_displayMode, g_resolutionScale, g_frameInterpolationMode, g_vsync, g_forceAspect};
}

DisplayState g_applied = CurrentDisplay();
DisplayState g_revertBase{};
bool g_revertActive = false;
Clock::time_point g_revertDeadline{};
constexpr auto kRevertTime = std::chrono::seconds(10);

DisplayState NormalizeDisplay(DisplayState s) {
    if (kFrameInterpolationTargetFps[static_cast<size_t>(s.interpolation)] > 60 && IsHighResolutionScale(s.resolution)) {
        s.resolution = 4.0f;
    }
    return s;
}

void ApplyDisplay(const DisplayState& next) {
    if (next.mode != g_applied.mode) {
        aurora_set_display_mode(static_cast<AuroraDisplayMode>(next.mode));
    }
    if (next.interpolation != g_applied.interpolation) {
        GX_SYNC_INLINE();
        aurora_set_frame_interpolation_fps(kFrameInterpolationTargetFps[static_cast<size_t>(next.interpolation)]);
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
        SetMkwForceAspect(next.forceAspect);
    }
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    g_resolutionScale = next.resolution;
    g_frameInterpolationMode = next.interpolation;
    g_vsync = next.vsync;
    g_forceAspect = next.forceAspect;
    g_applied = CurrentDisplay();
}

void PersistDisplay(const DisplayState& s) {
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(s.mode)]));
    RuntimeConfigFile::SetResolutionMultiplier(s.resolution);
    RuntimeConfigFile::SetFrameInterpolationFps(kFrameInterpolationTargetFps[static_cast<size_t>(s.interpolation)]);
    RuntimeConfigFile::SetVsync(s.vsync);
    RuntimeConfigFile::SetForceAspect(s.forceAspect);
}

void ChangeDisplay(DisplayState next) {
    const DisplayState before = CurrentDisplay();
    next = NormalizeDisplay(next);
    if (next == before) {
        return;
    }
    if (!g_revertActive) {
        g_revertBase = before;
        g_revertActive = true;
    }
    g_revertDeadline = Clock::now() + kRevertTime;
    ApplyDisplay(next);
}

void KeepDisplay() {
    PersistDisplay(CurrentDisplay());
    g_revertActive = false;
}

void RevertDisplay() {
    g_revertActive = false;
    ApplyDisplay(g_revertBase);
}

// Defaults are applied and saved at once: windowed, native resolution, no interpolation.
void ResetDisplayDefaults() {
    g_revertActive = false;
    ApplyDisplay(DisplayState{static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED), 1.0f, 0, false, false});
    PersistDisplay(CurrentDisplay());
}

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

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
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
        // The [controller] bindings are positional and describe whatever pad the
        // user set them up with (usually an Xbox layout: a = south), so they are
        // per-port overrides over a shared fallback. A Wii U Pro Controller has a
        // fixed, known layout (A on the east position) that aurora already maps
        // by name; applying configured bindings on top swaps A/B and X/Y.
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
const bool g_wiiRemotesAtLaunch = g_wiiRemotesEnabled;
bool g_wiiRemotesSaveFailed = false;
bool g_wiiAutoPair = RuntimeConfigFile::WiiAutoPairEnabled(true);
bool g_wiiSensorBarAbove = RuntimeConfigFile::WiiSensorBarAbove(false);
int g_wiiIrSensitivity = RuntimeConfigFile::WiiIrSensitivity(3);


constexpr float kControllerMenuWidth = 460.0f;

void PushMenuTextWrap() {
    ImGui::PushTextWrapPos(0.0f);
}

std::string WiiRemoteLabel(const Wiimote::SlotInfo& info) {
    std::string label = info.model;
    if (!info.extension.empty()) {
        label += " + " + info.extension;
    }
    return label;
}

// Pairing, the console's Wii Remote settings and the driver switch; applies to every remote.
void DrawWiiRemoteSetup() {
    if (!ImGui::CollapsingHeader("Wii Remote setup (Bluetooth)")) {
        return;
    }
    ImGui::Indent();
    PushMenuTextWrap();
    const std::string notice = Wiimote::PlatformNotice();
    if (!notice.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", notice.c_str());
    }
    const Wiimote::PairingInfo pairing = Wiimote::Pairing();
    if (pairing.supported) {
        ImGui::TextUnformatted("Press the red SYNC button (or 1 and 2) on the Wii Remote, like on a Wii.");
        ImGui::TextDisabled("Paired before? Just press any button on it.");
    } else if (g_wiiRemotesEnabled) {
        ImGui::TextUnformatted("Pair the Wii Remote in your system's Bluetooth settings, then press any button on it.");
    }
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!pairing.supported);
    if (ImGui::Button("Pair a Wii Remote")) {
        Wiimote::StartPairing();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Searches for a minute, like pressing SYNC on the console.");
    }
    ImGui::SameLine();
    if (pairing.searching) {
        ImGui::TextDisabled("Searching...");
    } else if (!pairing.status.empty()) {
        ImGui::TextDisabled("%s", pairing.status.c_str());
    }
    if (const size_t waiting = Wiimote::WaitingRemotes(); waiting != 0) {
        ImGui::TextDisabled("%zu more Wii Remote%s connected; joins when a player leaves.", waiting,
                            waiting == 1 ? "" : "s");
    }
    if (ImGui::Checkbox("Look for Wii Remotes automatically", &g_wiiAutoPair)) {
        RuntimeConfigFile::SetWiiAutoPairEnabled(g_wiiAutoPair);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii Remote is connected, keep searching for one in pairing mode.\n"
                          "Searching can briefly disturb Bluetooth headphones.");
    }
    ImGui::SetNextItemWidth(160.0f);
    int sensorBar = g_wiiSensorBarAbove ? 1 : 0;
    if (ImGui::Combo("Sensor bar", &sensorBar, "Below TV\0Above TV\0")) {
        g_wiiSensorBarAbove = sensorBar == 1;
        RuntimeConfigFile::SetWiiSensorBarAbove(g_wiiSensorBarAbove);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect the next time the game starts.");
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Pointer sensitivity", &g_wiiIrSensitivity, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        RuntimeConfigFile::SetWiiIrSensitivity(g_wiiIrSensitivity);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The sensor bar sensitivity from the Wii settings.\nTakes effect the next time the game starts.");
    }
    if (ImGui::Checkbox("Use Wii Remotes", &g_wiiRemotesEnabled)) {
        g_wiiRemotesSaveFailed = !RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
        if (g_wiiRemotesSaveFailed) g_wiiRemotesEnabled = !g_wiiRemotesEnabled;
        g_restartPromptOpen = g_wiiRemotesEnabled != g_wiiRemotesAtLaunch && !g_wiiRemotesSaveFailed;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect after restarting the game. Turn this off to use a Wii U Pro Controller as a gamepad.");
    }
    if (g_wiiRemotesSaveFailed) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Could not save this setting to Config.toml.");
    }
    if (g_wiiRemotesEnabled != g_wiiRemotesAtLaunch) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Restart the game to apply this change.");
    }
    ImGui::Unindent();
}

// The Wii Remote the game has on this player's channel.
void DrawWiiRemoteDetails(uint32_t port, const Wiimote::SlotInfo& info) {
    ImGui::Text("%s", WiiRemoteLabel(info).c_str());
    if (info.battery >= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("Battery %d/4", info.battery);
    }
    PushMenuTextWrap();
    if (info.asleep) {
        ImGui::TextUnformatted("Disconnected by the game. Press any button on it to reconnect.");
    } else if (info.link == Bluetooth::LinkState::Ready) {
        ImGui::TextUnformatted("The game reads it like a real Wii Remote. Nothing to set up.");
    } else {
        ImGui::TextUnformatted("Connecting to the game...");
    }
    ImGui::PopTextWrapPos();
    if (!info.asleep && ImGui::Button("Disconnect")) {
        Wiimote::Disconnect(port);
    }
}

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target)
                RuntimeConfigFile::SetPortControllerButton(capture.port, i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImGui::Text("Rebind: %s", g_rebind.label.c_str());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a keyboard key."
                : "Press a keyboard key or click a mouse button.");
        ImGui::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImGui::TextUnformatted("Escape can be bound. F10 is reserved for settings.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImGui::Button("Clear mapping");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_rebind.active = false;
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        if (g_rebind.active && (clear || remaining <= 0.0f)) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
                if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10) CompleteRebind(i);
                g_rebind.keys[i] = keys[i];
            }
            const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
            for (int i = 1; i <= 5 && g_rebind.active; ++i)
                if (!overControl && g_rebind.kind != RebindKind::MuteHotkey &&
                    (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
            g_rebind.mouse = mouse;
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
            auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
            if (pad != nullptr) {
                for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                    const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                    if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                    g_rebind.buttons[i] = pressed;
                }
                for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
                    const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
                    if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
                    if (g_rebind.axesReady[i] && std::abs(value) >= 16384)
                        CompleteRebind(PADEncodeAxisButton(i, value < 0));
                }
            }
        }
    }
    if (!g_rebind.active) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target,
                    float width = 220.0f) {
    const std::string caption = std::string(KeyBindingName(scancode)) + "##binding";
    if (ImGui::Button(caption.c_str(), ImVec2(width, 0.0f))) BeginRebind(kind, target, label);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);

}

void ApplyKeyboardPreset(uint32_t port) {
    const std::array<int, PAD_BUTTON_COUNT> keys = {
        PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
        SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    };
    for (size_t i = 0; i < keys.size(); ++i)
        PADSetKeyButtonBinding(port, {keys[i], kControllerButtons[i].padButton});
    const std::array<int, PAD_AXIS_COUNT> axes = {
        SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
        SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
        SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
    };
    uint32_t axisCount = 0;
    auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i)
        PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
    PADSerializeMappings();
}

bool KeyboardActive(uint32_t port) {
    uint32_t count = 0;
    return PADGetKeyButtonBindings(port, &count) != nullptr;
}

void SetKeyboardActive(uint32_t port, bool active) {
    if (KeyboardActive(port) == active) return;
    PADSetKeyboardActive(port, active);
    PADSerializeMappings();
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    if (active && buttons != nullptr && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        })) {
        ApplyKeyboardPreset(port);
    }
}

void DrawKeyboardSettings(uint32_t port) {
    PushMenuTextWrap();
    ImGui::TextUnformatted("Default: WASD to steer, mouse left to accelerate, Space to brake.");
    ImGui::PopTextWrapPos();
    if (!ImGui::CollapsingHeader("Customize keys")) return;
    ImGui::Indent();
    if (ImGui::Button("Reset to WASD + mouse")) ApplyKeyboardPreset(port);
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    ImGui::SeparatorText("Buttons");
    for (uint32_t i = 0; buttons != nullptr && i < count; ++i) {
        ImGui::PushID(static_cast<int>(i));
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), buttons[i].scancode, RebindKind::KeyboardButton,
                       buttons[i].padButton);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Sticks and triggers");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; axes != nullptr && i < axisCount; ++i) {
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = std::string(PADGetAxisName(axes[i].padAxis)) + " " +
                                  (direction != nullptr ? direction : "");
        DrawKeyBinding(label.c_str(), axes[i].scancode, RebindKind::KeyboardAxis, axes[i].padAxis);
        ImGui::PopID();
    }
    ImGui::Unindent();
}

int ExpressionResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void DrawExpressionSettings() {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0f);
    ImGui::TextDisabled(
        "Optional. An expression overrides nothing: its result is combined with the "
        "button mapping above. Operators ! & | ^ and functions if, min, max, clamp, "
        "timer, toggle, hold, tap, pulse, smooth, deadzone behave as they do in Dolphin.");
    ImGui::PopTextWrapPos();

    static std::array<std::string, InputBindings::kControls.size()> errors;
    static std::array<std::string, InputBindings::kControls.size()> buffers;
    static std::string importStatus;
    static int loadedPort = -1;
    static bool reloadBuffers = true;
    const auto port = static_cast<uint32_t>(g_controllerPort);

    if (loadedPort != g_controllerPort || reloadBuffers) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i] = InputBindings::GetExpression(port, i);
        }
        errors.fill(std::string());
        loadedPort = g_controllerPort;
        reloadBuffers = false;
    }

    if (ImGui::Button("Import from Dolphin")) {
        const std::string path = InputBindings::DefaultDolphinConfigPath();
        std::string summary;
        std::string error;
        if (InputBindings::ImportDolphinConfig(path, g_controllerPort + 1, port, summary, error) < 0) {
            importStatus = error;
        } else {
            importStatus = summary;
            errors.fill(std::string());
            reloadBuffers = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini,\n"
                          "or GCPadNew.ini next to the executable.", g_controllerPort + 1);
    }
    if (!importStatus.empty()) {
        ImGui::TextDisabled("%s", importStatus.c_str());
    }

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText(InputBindings::kControls[i].label, text.data(), text.capacity() + 1,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                             ExpressionResizeCallback, &text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        if (InputBindings::IsActive(port, i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "active");
        }
        if (!errors[i].empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", errors[i].c_str());
        }
        ImGui::PopID();
    }
}

void DrawRumbleSettings() {
    ImGui::Separator();
    if (ImGui::Checkbox("Controller vibration", &g_rumbleEnabled)) {
        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
        if (!g_rumbleEnabled) {
            // Stop whatever is already running: the game will not send another
            // motor command until its own state machine decides to.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies to every port.");
    }
}

const char* TrickDirectionLabel(MotionTrick::Direction direction) {
    switch (direction) {
    case MotionTrick::Direction::Up: return "up";
    case MotionTrick::Direction::Down: return "down";
    case MotionTrick::Direction::Left: return "left";
    case MotionTrick::Direction::Right: return "right";
    default: return "";
    }
}

void DrawMotionTrickSettings(uint32_t port) {
    const MotionTrick::PortStatus status = MotionTrick::Status(port);
    const bool hasMotion = MotionTrick::PortHasMotion(port) && !status.sensorsUnavailable;
    bool enabled = MotionTrick::Enabled();
    ImGui::BeginDisabled(!hasMotion && !enabled);
    if (ImGui::Checkbox("Shake to trick", &enabled)) {
        MotionTrick::SetEnabled(enabled);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(hasMotion
            ? "Flick the controller to trick off ramps, or to wheelie on a bike, like shaking a Wii Remote.\n"
              "The flick direction picks the trick. The D-pad keeps working. Applies to every player."
            : "Steam Input can hide a controller's motion sensor; turn it off for this game to use it here.");
    }
    if (!hasMotion) {
        ImGui::SameLine();
        ImGui::TextDisabled("(no motion sensor)");
        return;
    }
    if (!enabled) {
        return;
    }

    ImGui::Indent();
    int sensitivity = MotionTrick::Sensitivity();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Sensitivity##motion", &sensitivity, MotionTrick::kMinSensitivity,
                         MotionTrick::kMaxSensitivity, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        MotionTrick::SetSensitivity(sensitivity);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Higher reacts to smaller flicks. Lower it if you trick or wheelie by accident while steering.");
    }

    constexpr float kMeterScale = 1.5f;
    const ImVec2 meterPos = ImGui::GetCursorScreenPos();
    const ImVec2 meterSize(160.0f, ImGui::GetFrameHeight() * 0.6f);
    ImGui::ProgressBar(std::min(status.intensity / kMeterScale, 1.0f), meterSize, "");
    const float thresholdX = meterPos.x + meterSize.x / kMeterScale;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(thresholdX, meterPos.y), ImVec2(thresholdX, meterPos.y + meterSize.y),
                                        ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
    ImGui::SameLine();
    const bool recent = status.lastTrick != MotionTrick::Direction::None &&
                        MotionTrick::NowUs() - status.lastTrickUs < 700'000;
    if (recent) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Trick! (%s)", TrickDirectionLabel(status.lastTrick));
    } else {
        ImGui::TextDisabled("Flick to test");
    }
    ImGui::Unindent();
}

void DrawGamepadMapping(uint32_t port) {
    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(port, &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings = PADGetAltButtonMappings(port, &altMappingCount);

    const auto writeBinding = [port](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetPortControllerButton(port, index, value);
    };

    // Which rows show the second-binding button without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != static_cast<int>(port)) {
        altRowExpandedPort = static_cast<int>(port);
        altRowExpanded.fill(false);
    }

    ImGui::TextUnformatted("Layout:");
    ImGui::SameLine();
    if (ImGui::SmallButton("GameCube")) {
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetPortControllerButton(port, i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetPortControllerButton(port, i, preset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };
    ImGui::SameLine();
    if (ImGui::SmallButton("Classic Controller Pro")) {
        applyPreset(kClassicProPreset);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("PlayStation")) {
        applyPreset(kPlayStationPreset);
    }

    ImGui::TextDisabled("Click a binding, then press the input to use. Triggers: LT/L2, RT/R2.");
    ImGui::TextDisabled("Bindings are saved for this port; ports never rebound use the shared mapping.");
    const float bindingWidth = ImGui::CalcTextSize("Right shoulder (RB / R1)").x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float bindingX = ImGui::GetCursorPosX() + ImGui::CalcTextSize("D-pad Right").x +
                           ImGui::GetStyle().ItemSpacing.x * 2.0f;
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            ImGui::SetNextItemWidth(bindingWidth);
            if (ImGui::SliderInt(secondary ? "##altThreshold" : "##primaryThreshold", &threshold,
                                 1, 100, "Threshold: %d%%", ImGuiSliderFlags_AlwaysClamp)) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(port, updated);
                else PADSetButtonMapping(port, updated);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                writeBinding(i, mappingIt->nativeButton,
                             altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::SameLine(bindingX);
        ImGui::BeginGroup();
        const std::string primaryCaption = std::string(current.label) + "##primary";
        if (ImGui::Button(primaryCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        }
        drawThreshold(mappingIt, false);
        ImGui::EndGroup();
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                ImGui::BeginGroup();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                const std::string altCaption = std::string(altLabel) + "##alt";
                if (ImGui::Button(altCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                drawThreshold(altIt, true);
                ImGui::EndGroup();
            }
        }
        ImGui::PopID();
    }
}

std::string ControllerLabel(uint32_t index) {
    const char* name = PADGetNameForControllerIndex(index);
    return name != nullptr ? name : "Unknown controller";
}

// One row per player: the device combo replaces separate assign/unassign/keyboard controls.
void DrawPlayerRow(uint32_t port) {
    const bool keyboard = KeyboardActive(port);
    const int32_t assigned = PADGetIndexForPort(port);
    std::string preview = "None";
    if (keyboard) {
        preview = "Keyboard and mouse";
    } else if (assigned >= 0) {
        preview = ControllerLabel(static_cast<uint32_t>(assigned));
    } else if (const Wiimote::SlotInfo remote = Wiimote::Slot(port); remote.attached) {
        preview = WiiRemoteLabel(remote);
    }

    ImGui::PushID(static_cast<int>(port));
    const std::string label = "Player " + std::to_string(port + 1);
    ImGui::AlignTextToFramePadding();
    if (ImGui::RadioButton(label.c_str(), g_controllerPort == static_cast<int>(port))) {
        g_controllerPort = static_cast<int>(port);
    }
    ImGui::SameLine(ImGui::CalcTextSize("Player 0").x + ImGui::GetFrameHeight() + 16.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##device", preview.c_str())) {
        g_controllerPort = static_cast<int>(port);
        if (ImGui::Selectable("None", !keyboard && assigned < 0)) {
            SetKeyboardActive(port, false);
            PADClearPort(port);
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        const uint32_t controllerCount = PADCount();
        for (uint32_t index = 0; index < controllerCount; ++index) {
            std::string item = ControllerLabel(index);
            for (uint32_t other = 0; other < PAD_MAX_CONTROLLERS; ++other) {
                if (other != port && PADGetIndexForPort(other) == static_cast<s32>(index)) {
                    item += " (Player " + std::to_string(other + 1) + ")";
                    break;
                }
            }
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(item.c_str(), !keyboard && assigned == static_cast<int32_t>(index))) {
                SetKeyboardActive(port, false);
                PADSetPortForIndex(index, port);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        if (ImGui::Selectable("Keyboard and mouse", keyboard)) {
            SetKeyboardActive(port, true);
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void DrawControllerSettings() {
    ImGui::Dummy(ImVec2(kControllerMenuWidth, 0.0f));
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        DrawPlayerRow(port);
    }
    bool anyRemote = false;
    for (uint32_t player = 0; player < PAD_MAX_CONTROLLERS; ++player) {
        anyRemote |= Wiimote::Slot(player).attached;
    }
    if (PADCount() == 0 && !anyRemote) {
        ImGui::TextDisabled("No controller detected. Plug one in, or pair a Wii Remote below.");
    }

    const uint32_t port = static_cast<uint32_t>(g_controllerPort);
    ImGui::SeparatorText(("Player " + std::to_string(port + 1)).c_str());
    const Wiimote::SlotInfo remote = Wiimote::Slot(port);
    if (remote.attached) {
        DrawWiiRemoteDetails(port, remote);
        if (KeyboardActive(port) || PADGetIndexForPort(port) >= 0) {
            ImGui::Spacing();
        }
    }
    if (KeyboardActive(port)) {
        DrawKeyboardSettings(port);
    } else if (PADGetIndexForPort(port) < 0) {
        if (!remote.attached) {
            ImGui::TextDisabled("No device. Pick one in the list above.");
        }
    } else {
        ImGui::TextUnformatted("Ready to play.");
        DrawMotionTrickSettings(port);
        if (ImGui::CollapsingHeader("Customize buttons")) {
            ImGui::Indent();
            DrawGamepadMapping(port);
            if (ImGui::TreeNode("Advanced: Dolphin expressions")) {
                DrawExpressionSettings();
                ImGui::TreePop();
            }
            ImGui::Unindent();
        }
    }

    ImGui::Spacing();
    controller_mapping_wizard::DrawSetupList();
    DrawWiiRemoteSetup();
    DrawRumbleSettings();
}

void DrawAudioSettings() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    if (ImGui::SliderInt("Music", &g_musicVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_musicVolumePercent) / 100.0f;
        MusicAttenuation::SetMusicVolume(volume);
        RuntimeConfigFile::SetMusicVolume(volume);
    }
    if (ImGui::SliderInt("Sound Effects", &g_soundEffectsVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_soundEffectsVolumePercent) / 100.0f;
        MusicAttenuation::SetSoundEffectsVolume(volume);
        RuntimeConfigFile::SetSoundEffectsVolume(volume);
    }
    if (ImGui::SliderInt("UI", &g_uiVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_uiVolumePercent) / 100.0f;
        MusicAttenuation::SetUiVolume(volume);
        RuntimeConfigFile::SetUiVolume(volume);
    }
    if (ImGui::SliderInt("Voices", &g_voicesVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_voicesVolumePercent) / 100.0f;
        MusicAttenuation::SetVoicesVolume(volume);
        RuntimeConfigFile::SetVoicesVolume(volume);
    }
    const float labelColumn = ImGui::GetCursorPosX() + ImGui::CalcItemWidth();
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::SameLine();
    DrawKeyBinding("Mute shortcut", g_muteHotkey, RebindKind::MuteHotkey, 0,
                   std::max(60.0f, labelColumn - ImGui::GetCursorPosX()));
    ImGui::Separator();
    if (ImGui::Checkbox("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Mute game music while external media is playing",
                        &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
#if defined(__APPLE__)
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Detects other apps with active audio output on macOS 14.2 or later. "
            "Apps that keep an output stream running silently may keep game music muted.");
    }
#endif
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            ImGui::TextDisabled("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            ImGui::TextDisabled("Checking external audio...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
#if defined(__APPLE__)
            ImGui::TextDisabled("External audio detection unavailable (requires macOS 14.2 or later).");
#else
            ImGui::TextDisabled("Media controls are unavailable.");
#endif
        } else {
            ImGui::TextDisabled("No external media is currently playing.");
        }
    }
}

void Hint(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

void DrawGraphicsPage() {
    struct EffectFlag {
        const char* label;
        uint32_t flag;
    };
    static constexpr std::array<EffectFlag, 1> kEffectFlags = {{
        {"Disable bloom", 0x10u},
    }};

    for (const auto& effect : kEffectFlags) {
        bool disabled = (g_disabledPostProcessingPaths & effect.flag) != 0;
        if (ImGui::Checkbox(effect.label, &disabled)) {
            if (disabled) {
                g_disabledPostProcessingPaths |= effect.flag;
            } else {
                g_disabledPostProcessingPaths &= ~effect.flag;
            }
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
        }
    }
    Hint("Applied when the next scene renderer is created.");
    ImGui::Separator();
#if defined(__APPLE__)
    const bool metalFxSupported = aurora_is_metalfx_spatial_supported();
    ImGui::BeginDisabled(!metalFxSupported);
    if (ImGui::Checkbox("MetalFX spatial upscaling", &g_metalFxSpatialUpscaling)) {
        aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
        RuntimeConfigFile::SetMetalFxSpatialUpscaling(g_metalFxSpatialUpscaling);
    }
    ImGui::EndDisabled();
    switch (aurora_get_metalfx_status()) {
    case AURORA_METALFX_ACTIVE:
        ImGui::TextDisabled("Active: upscaling the game image before the overlay.");
        break;
    case AURORA_METALFX_NOT_UPSCALING:
        ImGui::TextDisabled("Choose a lower internal resolution to use MetalFX.");
        break;
    case AURORA_METALFX_UNSUPPORTED:
        ImGui::TextDisabled("Requires macOS 13+, Metal, and a MetalFX-capable GPU.");
        break;
    case AURORA_METALFX_ERROR:
        ImGui::TextDisabled("Unavailable after a renderer error; toggle off and on to retry.");
        break;
    case AURORA_METALFX_DISABLED:
        if (metalFxSupported) {
            ImGui::TextDisabled("Render below output resolution for sharper lower-cost output.");
        } else {
            ImGui::TextDisabled("MetalFX spatial upscaling is unavailable on this device.");
        }
        break;
    }
#endif
    if (ImGui::Checkbox("Disable copy filter (recommended)", &g_disableCopyFilter)) {
        GX_SYNC_INLINE();
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImGui::Checkbox("Skip draws while shaders compile (recommended)", &g_skipUnreadyPipelines)) {
        GX_SYNC_INLINE();
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    Hint("Skipping avoids stutter while pipelines build; some effects may flicker on first use.");
    ImGui::Separator();

    SystemSettings::Values sys = SystemSettings::Get();
    bool sysChanged = false;
    auto choiceCombo = [&](const char* label, int& value, const int* choices, size_t count) {
        const std::string preview = value == 1 ? std::string("Off") : std::to_string(value) + "x";
        if (ImGui::BeginCombo(label, preview.c_str())) {
            for (size_t i = 0; i < count; ++i) {
                const std::string item = choices[i] == 1 ? std::string("Off") : std::to_string(choices[i]) + "x";
                if (ImGui::Selectable(item.c_str(), value == choices[i])) {
                    value = choices[i];
                    sysChanged = true;
                }
            }
            ImGui::EndCombo();
        }
    };
    choiceCombo("Anti-aliasing, MSAA (restart required)", sys.msaa, SystemSettings::kMsaaChoices.data(),
                SystemSettings::kMsaaChoices.size());
    Hint("Off is the safest. Higher values cost GPU time and are untested with every effect.");
    choiceCombo("Anisotropic filtering (restart required, 16x recommended)", sys.anisotropy,
                SystemSettings::kAnisotropyChoices.data(), SystemSettings::kAnisotropyChoices.size());
    if (sysChanged) {
        SystemSettings::Set(sys);
        SystemSettings::Save();
    }
    ImGui::Separator();

    auto restartToggle = [](const char* label, bool value, const char* section, const char* key,
                            std::optional<bool>& slot) {
        if (ImGui::Checkbox(label, &value)) {
            slot = value;
            RuntimeConfigFile::WriteSetting(section, key, value ? "true" : "false");
        }
    };
    RuntimeUserConfig& userConfig = RuntimeConfigFile::Mutable();
    restartToggle("Custom textures (restart required)", RuntimeConfigFile::TextureReplacements(false), "video",
                  "texture_replacements", userConfig.textureReplacements);
    restartToggle("Dump unmatched textures (restart required)", RuntimeConfigFile::TextureDumps(false), "video",
                  "texture_dumps", userConfig.textureDumps);
    Hint("Custom textures are read from texture_replacements/ next to Config.toml.");
    restartToggle("Run GX on its own thread (restart required, recommended)", RuntimeConfigFile::GxWorkerEnabled(true),
                  "video", "gx_worker", userConfig.gxWorker);
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", GraphicsApiDisplayName());
    Hint("Pick another API with graphics_api in Config.toml (restart required).");
}

void DrawDisplayPage() {
    Hint("Options marked (confirm) are applied at once and revert after 10 seconds unless you keep them.");
    if (!g_revertActive) {
        g_displayMode = static_cast<int>(aurora_get_display_mode());
    }
    DisplayState next = CurrentDisplay();
    bool changed = false;

    static constexpr const char* kDisplayModes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
    changed |= ImGui::Combo("Display mode (confirm)", &next.mode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)));
    if (next.mode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        Hint("Requests the closest native-resolution display mode to the output frame rate.");
    }

    struct WindowSize {
        const char* label;
        int width;
        int height;
    };
    static constexpr std::array<WindowSize, 7> kWindowSizes = {{
        {"854 x 480", 854, 480}, {"1280 x 720", 1280, 720}, {"1600 x 900", 1600, 900}, {"1920 x 1080", 1920, 1080},
        {"2560 x 1440", 2560, 1440}, {"960 x 720 (4:3)", 960, 720}, {"1440 x 1080 (4:3)", 1440, 1080},
    }};
    const AuroraWindowSize windowSize = aurora_get_window_size();
    char windowLabel[48];
    std::snprintf(windowLabel, sizeof windowLabel, "%u x %u", windowSize.width, windowSize.height);
    ImGui::BeginDisabled(next.mode != AURORA_DISPLAY_MODE_WINDOWED);
    if (ImGui::BeginCombo("Window size", windowLabel)) {
        for (const WindowSize& size : kWindowSizes) {
            if (ImGui::Selectable(size.label)) {
                SDL_SetWindowSize(aurora_get_window(), size.width, size.height);
                RuntimeConfigFile::SetWindowSize(static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Hint("Windowed only. You can also drag the window edges.");

    changed |= ImGui::Checkbox("VSync (confirm)", &next.vsync);
    Hint("Needed for smooth motion with frame interpolation.");

    ImGui::Separator();
    if (ImGui::BeginCombo("Aspect ratio (restart required)", g_widescreen ? "16:9 widescreen" : "4:3 original")) {
        if (ImGui::Selectable("16:9 widescreen", g_widescreen)) {
            g_widescreen = true;
            RuntimeConfigFile::SetWidescreen(true);
        }
        if (ImGui::Selectable("4:3 original", !g_widescreen)) {
            g_widescreen = false;
            RuntimeConfigFile::SetWidescreen(false);
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::Checkbox("Keep aspect ratio with black bars (confirm)", &next.forceAspect);
    Hint("Off stretches the picture to the window. On letterboxes it; the mouse pointer follows the picture.");

    ImGui::Separator();
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [&](const ResolutionItem& item) {
        return std::fabs(item.scale - next.resolution) < 0.001f;
    });
    if (ImGui::BeginCombo("Internal resolution (confirm)", resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom")) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - next.resolution) < 0.001f;
            const bool disabled = kFrameInterpolationTargetFps[static_cast<size_t>(next.interpolation)] > 60 &&
                                  IsHighResolutionScale(resolution.scale);
            ImGui::BeginDisabled(disabled);
            if (ImGui::Selectable(resolution.label, selected)) {
                next.resolution = resolution.scale;
                changed = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    Hint("Rendering scale relative to the original 640x480 picture. Native (1x) is the original look.");

    static constexpr std::array<const char*, 3> kInterpolation = {"Off", "120 FPS", "180 FPS"};
    if (ImGui::BeginCombo("Frame interpolation, experimental (confirm)", kInterpolation[static_cast<size_t>(next.interpolation)])) {
        for (int mode = 0; mode < static_cast<int>(kInterpolation.size()); ++mode) {
            if (ImGui::Selectable(kInterpolation[static_cast<size_t>(mode)], next.interpolation == mode)) {
                next.interpolation = mode;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    Hint("Generates extra frames on high-refresh displays. Game logic stays at its original rate. May show artifacts.");

    SystemSettings::Values sys = SystemSettings::Get();
    int filter = sys.scalingFilter;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("Scaling filter", &filter, SystemSettings::kScalingFilterLabels.data(),
                     static_cast<int>(SystemSettings::kScalingFilterLabels.size()))) {
        sys.scalingFilter = filter;
        SystemSettings::Set(sys);
        SystemSettings::Save();
        aurora_set_resampler(static_cast<AuroraSampler>(filter));
    }
    Hint("How the picture is resampled to the window.");

    if (changed) {
        ChangeDisplay(next);
    }
}

void DrawSystemPage() {
    if (ImGui::Checkbox("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    SystemSettings::Values sys = SystemSettings::Get();
    bool changed = false;
    changed |= ImGui::Checkbox("Show frame-time graph", &sys.showFrameTime);
    Hint("Host frame time in milliseconds with a short history; spikes show stutter.");

    ImGui::Separator();
    int cover = static_cast<int>(sys.startupCover);
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::Combo("Startup screen (next launch)", &cover, SystemSettings::kStartupCoverLabels.data(),
                     static_cast<int>(SystemSettings::kStartupCoverLabels.size()))) {
        sys.startupCover = static_cast<SystemSettings::StartupCover>(cover);
        changed = true;
    }
    Hint("The black cover hides the first seconds while shaders build. It always waits for shaders.");
    if (changed) {
        SystemSettings::Set(sys);
        SystemSettings::Save();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Files");
    const std::string configPath = RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ResolveConfigPath());
    ImGui::TextWrapped("Config: %s", configPath.c_str());
    const std::string nand = RuntimeConfigFile::NandRoot();
    ImGui::TextWrapped("Save data (NAND): %s", nand.empty() ? "default, under the user data folder" : nand.c_str());
    const std::string dvd = RuntimeConfigFile::DvdRoot();
    ImGui::TextWrapped("Game files (DVD root): %s", dvd.empty() ? "not set (paths.dvd_root in Config.toml)" : dvd.c_str());
    Hint("Paths are edited in Config.toml.");
#if defined(_WIN32)
    if (ImGui::Button("Open user data folder")) {
        const std::wstring folder = RuntimeConfigFile::ApplicationDataDirectory().wstring();
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
#endif

    ImGui::Separator();
    AuroraPresentTiming timing{};
    aurora_get_present_timing(&timing);
    ImGui::Text("Presented: %.1f FPS (motion %.1f)", timing.framesPerSecond, timing.effectiveFramesPerSecond);
    ImGui::Text("Shaders compiling: %u", aurora_get_queued_pipeline_count());
    Hint("While this menu is open the game keeps running with its input blocked (there is no safe emulation pause).");
}

void DrawAboutPage() {
    ImGui::TextUnformatted("Sin & Punishment: Star Successor - PC port");
    ImGui::TextDisabled("Static recompilation of the original game; no game data is included. GPL-3.");
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", GraphicsApiDisplayName());
    const AuroraWindowSize size = aurora_get_window_size();
    ImGui::Text("Window: %u x %u   Internal: %u x %u", size.width, size.height, size.fb_width, size.fb_height);
    const double refresh = aurora_get_display_refresh_rate();
    if (refresh > 0.0) {
        ImGui::Text("Display refresh: %.1f Hz", refresh);
    }
    ImGui::Text("Config version: %d", SystemSettings::kConfigVersion);
    ImGui::Separator();
    Hint("Menu: F10 / F1, or Back + Start on a gamepad. Esc / B goes back. Alt+Enter toggles fullscreen.");
}

void ResetGraphicsDefaults() {
    g_disabledPostProcessingPaths = 0;
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(0);
    RuntimeConfigFile::SetDisabledPostProcessingPaths(0);
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

void ResetSystemDefaults() {
    SystemSettings::Values sys = SystemSettings::Get();
    sys.startupCover = SystemSettings::StartupCover::Auto;
    sys.showFrameTime = false;
    SystemSettings::Set(sys);
    SystemSettings::Save();
    g_showFps = true;
    RuntimeConfigFile::SetShowFps(true);
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    const double refreshRate = aurora_get_display_refresh_rate();
    const bool interpolationInactive = targetFps > 60 && !aurora_display_shows_interpolation();
    const bool unevenPacing =
        !g_vsync && !interpolationInactive && refreshRate > 0.0 && targetFps > refreshRate + 1.0;
    const bool showFrameTime = SystemSettings::Get().showFrameTime;
    if (!g_showFps && !showFrameTime && !unevenPacing && !interpolationInactive) {
        return;
    }
    static std::array<float, 120> s_frameTimes{};
    static size_t s_frameTimeIndex = 0;
    s_frameTimes[s_frameTimeIndex] = ImGui::GetIO().DeltaTime * 1000.0f;
    s_frameTimeIndex = (s_frameTimeIndex + 1) % s_frameTimes.size();

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (g_showFps && presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else if (g_showFps) {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
        if (showFrameTime) {
            const float latest = s_frameTimes[(s_frameTimeIndex + s_frameTimes.size() - 1) % s_frameTimes.size()];
            ImGui::Text("Frame: %.2f ms", latest);
            ImGui::PlotLines("##frametime", s_frameTimes.data(), static_cast<int>(s_frameTimes.size()),
                             static_cast<int>(s_frameTimeIndex), nullptr, 0.0f, 33.3f, ImVec2(180.0f, 40.0f));
        }
        if (interpolationInactive) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                               "%u FPS interpolation has no effect on a %.0f Hz display.", targetFps,
                               refreshRate);
        }
        if (unevenPacing) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                               "%u FPS interpolation on a %.0f Hz display without VSync is not smooth.\n"
                               "Enable VSync for smooth motion.",
                               targetFps, refreshRate);
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Wiicompiled Startup", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.25f);
        constexpr const char* kTitle = "WiiCompiled";
        const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
        const float titleX = std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f);
        const float startY = std::max(0.0f, (viewport->Size.y - titleSize.y) * 0.5f);
        ImGui::SetCursorPos(ImVec2(titleX, startY));
        ImGui::TextUnformatted(kTitle);
        float nextY = startY + titleSize.y + 12.0f;
        if (g_bootShaderNotice && !g_bootShadersReady.load(std::memory_order_relaxed)) {
            ImGui::SetWindowFontScale(0.9f);
            char line[96];
            std::snprintf(line, sizeof(line), "Compiling shaders, please hold on: %u remaining",
                          aurora_get_queued_pipeline_count());
            const float lineX = std::max(0.0f, (viewport->Size.x - ImGui::CalcTextSize(line).x) * 0.5f);
            ImGui::SetCursorPos(ImVec2(lineX, startY + titleSize.y * 1.8f));
            ImGui::TextUnformatted(line);
            nextY = ImGui::GetCursorPosY() + 4.0f;
        }
        const bool waitForButton = SystemSettings::Get().startupCover == SystemSettings::StartupCover::Button &&
                                   !g_strapInputAccepted.load(std::memory_order_relaxed);
        const char* kHint = waitForButton ? "Press any button to start   |   F10 or Back + Start: settings"
                                          : "F10 or Back + Start: settings";
        ImGui::SetWindowFontScale(0.8f);
        const ImVec2 hintSize = ImGui::CalcTextSize(kHint);
        ImGui::SetCursorPos(ImVec2(std::max(0.0f, (viewport->Size.x - hintSize.x) * 0.5f), nextY));
        ImGui::TextUnformatted(kHint);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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

void DrawExitPrompt() {
    constexpr const char* kTitle = "Exit";
    if (g_exitPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Quit the game?");
    if (ImGui::Button("Exit", ImVec2(120.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) g_exitPromptOpen = false;
    if (!g_exitPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawRestartPrompt() {
    constexpr const char* kTitle = "Restart required";
    if (g_restartPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_restartPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(g_wiiRemotesEnabled ? "Wii Remotes will be used after you restart the game."
                                               : "Wii Remotes will stop being used after you restart the game.");
    if (ImGui::Button("Exit now", ImVec2(140.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImGui::Button("Restart later", ImVec2(140.0f, 0.0f))) g_restartPromptOpen = false;
    if (!g_restartPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

int g_tab = 0;
int g_tabRequest = -1;
bool g_popupWasOpen = false;

void ResetAudioDefaults() {
    g_audioVolumePercent = g_musicVolumePercent = g_soundEffectsVolumePercent = 100;
    g_uiVolumePercent = g_voicesVolumePercent = 100;
    g_audioMuted = false;
    g_audioMixWorker = true;
    g_attenuateMusicWhenMediaPlays = false;
    g_muteHotkey = SDL_SCANCODE_BACKSLASH;
    AudioBackend::Instance().SetMasterVolume(1.0f);
    AudioBackend::Instance().SetMuted(false);
    MusicAttenuation::SetMusicVolume(1.0f);
    MusicAttenuation::SetSoundEffectsVolume(1.0f);
    MusicAttenuation::SetUiVolume(1.0f);
    MusicAttenuation::SetVoicesVolume(1.0f);
    MusicAttenuation::SetEnabled(false);
    AxDspHle::SetMixWorkerEnabled(true);
    RuntimeConfigFile::SetAudioVolume(1.0f);
    RuntimeConfigFile::SetMusicVolume(1.0f);
    RuntimeConfigFile::SetSoundEffectsVolume(1.0f);
    RuntimeConfigFile::SetUiVolume(1.0f);
    RuntimeConfigFile::SetVoicesVolume(1.0f);
    RuntimeConfigFile::SetAudioMuted(false);
    RuntimeConfigFile::SetAudioMixWorker(true);
    RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(false);
    RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
}

void ResetDisplayPageDefaults() {
    ResetDisplayDefaults();
    g_widescreen = true;
    RuntimeConfigFile::SetWidescreen(true);
    SystemSettings::Values sys = SystemSettings::Get();
    sys.scalingFilter = 0;
    SystemSettings::Set(sys);
    SystemSettings::Save();
    aurora_set_resampler(SAMPLER_BILINEAR);
}

void ResetCurrentPage() {
    switch (g_tab) {
    case 0: ResetDisplayPageDefaults(); break;
    case 1: ResetGraphicsDefaults(); break;
    case 2: ResetAudioDefaults(); break;
    case 3: controls_menu::ResetAll(); break;
    case 4: ResetSystemDefaults(); break;
    default: break;
    }
}

void ResetEverything() {
    ResetDisplayPageDefaults();
    ResetGraphicsDefaults();
    ResetAudioDefaults();
    controls_menu::ResetAll();
    ResetSystemDefaults();
}

// Gamepad shortcuts while the menu is open: LB/RB switch tabs, B leaves the menu when nothing else is
// using it (ImGui itself consumes B to close combos and edit modes).
void PollMenuGamepad() {
    static bool s_lb = false, s_rb = false, s_b = false;
    SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
    auto edge = [&](SDL_GamepadButton button, bool& was) {
        const bool down = pad != nullptr && SDL_GetGamepadButton(pad, button);
        const bool pressed = down && !was;
        was = down;
        return pressed;
    };
    const bool previous = edge(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, s_lb);
    const bool next = edge(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, s_rb);
    const bool back = edge(SDL_GAMEPAD_BUTTON_EAST, s_b);
    if (!g_topBarVisible || controls_menu::Capturing() || g_revertActive) {
        return;
    }
    if (previous) g_tabRequest = (g_tab + 5) % 6;
    if (next) g_tabRequest = (g_tab + 1) % 6;
    if (back && !g_popupWasOpen) {
        SetTopBarVisible(false);
    }
}

void DrawRevertPrompt() {
    if (!g_revertActive) {
        return;
    }
    const float remaining = std::chrono::duration<float>(g_revertDeadline - Clock::now()).count();
    if (remaining <= 0.0f) {
        RevertDisplay();
        return;
    }
    constexpr const char* kTitle = "Keep display settings?";
    if (!ImGui::IsPopupOpen(kTitle)) {
        ImGui::OpenPopup(kTitle);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }
    ImGui::Text("The previous settings return in %d seconds unless you keep these.",
                static_cast<int>(std::ceil(remaining)));
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    bool revert = ImGui::Button("Revert", ImVec2(150.0f, 0.0f));
    ImGui::SameLine();
    const bool keep = ImGui::Button("Keep", ImVec2(150.0f, 0.0f));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        revert = true;
    }
    if (keep) {
        KeepDisplay();
    } else if (revert) {
        RevertDisplay();
    }
    if (!g_revertActive) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawTopBar() {
    if (!g_topBarVisible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 110));
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.8f, viewport->Size.y * 0.84f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("WiiCompiled settings", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::Button("Close")) {
            SetTopBarVisible(false);
        }
        ImGui::SameLine();
        if (controls_menu::ResetConfirmButton("Reset this page", "resetpage")) {
            ResetCurrentPage();
        }
        ImGui::SameLine();
        if (controls_menu::ResetConfirmButton("Reset ALL settings", "resetall")) {
            ResetEverything();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit game...")) {
            g_exitPromptOpen = true;
        }
        Hint("LB / RB: change tab   D-pad: move   A: select   B: back   Back + Start, F10, F1 or Esc: close");
        ImGui::Separator();

        static constexpr std::array<const char*, 6> kTabs = {"Display", "Graphics", "Audio", "Controls", "System", "About"};
        if (ImGui::BeginTabBar("##settingsTabs")) {
            for (int tab = 0; tab < static_cast<int>(kTabs.size()); ++tab) {
                const ImGuiTabItemFlags flags = g_tabRequest == tab ? ImGuiTabItemFlags_SetSelected : 0;
                if (!ImGui::BeginTabItem(kTabs[static_cast<size_t>(tab)], nullptr, flags)) {
                    continue;
                }
                g_tab = tab;
                switch (tab) {
                case 0: DrawDisplayPage(); break;
                case 1: DrawGraphicsPage(); break;
                case 2:
                    DrawAudioSettings();
                    DrawRebindPrompt();
                    break;
                case 3:
                    controls_menu::Draw(&DrawControllerSettings);
                    DrawRebindPrompt();
                    break;
                case 4: DrawSystemPage(); break;
                default: DrawAboutPage(); break;
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        g_tabRequest = -1;
    }
    ImGui::End();
    g_popupWasOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                     ImGui::IsAnyItemActive();
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
        !g_topBarVisible && Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
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

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

void ApplyInputBlockState() {
    const bool blocked = controller_mapping_wizard::IsActive() || g_rebind.active || g_revertActive ||
                         g_exitPromptOpen || g_restartPromptOpen || g_topBarVisible || StartupScreenVisible();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    AimInput::Install();
    aurora_set_resampler(static_cast<AuroraSampler>(SystemSettings::Get().scalingFilter));
    SystemSettings::EnsureVersionStamp();
    PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
#if defined(__APPLE__)
    aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
#endif
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
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
            !g_topBarVisible) {
            NotifyStrapInputAccepted();
        }
        const bool bindCapture = controls_menu::Capturing();
        if (bindCapture) {
            if (IsMouseActivity(ev->sdl)) g_lastMouseActivity = Clock::now();
            continue;
        }
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        if (!g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_F10) || IsToggleKey(ev->sdl, SDL_SCANCODE_F1))) {
            SetTopBarVisible(!g_topBarVisible);
            ApplyInputBlockState();
        }
        if (!g_rebind.active && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE)) {
            if (g_revertActive) {
                RevertDisplay();
            } else if (g_restartPromptOpen) {
                g_restartPromptOpen = false;
            } else if (g_exitPromptOpen) {
                g_exitPromptOpen = false;
            } else if (g_topBarVisible) {
                SetTopBarVisible(false);
            } else if (!ValveControllerConnected()) {
                g_exitPromptOpen = true;
            }
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

// Back + Start on gamepad 0 opens/closes the menu so a gamepad-only user can reach (and leave) it.
static void PollGamepadMenuChord() {
    static bool wasDown = false;
    SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
    const bool down = pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
                      SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    if (down && !wasDown) {
        SetTopBarVisible(!g_topBarVisible);
        ApplyInputBlockState();
    }
    wasDown = down;
}

// ImGui navigates with the gamepad only while the menu is open.
static void UpdateMenuNavigation() {
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiConfigFlags navFlags = ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    if (g_topBarVisible || g_revertActive) {
        io.ConfigFlags |= navFlags;
    } else {
        io.ConfigFlags &= ~navFlags;
    }
}

void Draw() noexcept {
    // ImGui is only touched on this thread; the render worker replays a frozen copy.
    PollGamepadMenuChord();
    PollMenuGamepad();
    UpdateMenuNavigation();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    UpdateBootShaderState();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    DrawFpsOverlay();
    DrawTopBar();
    DrawExitPrompt();
    DrawRestartPrompt();
    DrawRevertPrompt();
    controller_mapping_wizard::Draw();
    ApplyInputBlockState();
    AimInput::UpdateWindowState(InputBindings::InputBlocked());
    DrawStartupScreen();
}

bool StartupScreenVisible() noexcept {
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           !g_bootShadersReady.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
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
    // The startup cover was built around Mario Kart Wii's wrist-strap scene, which signalled
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
} // namespace settings_overlay

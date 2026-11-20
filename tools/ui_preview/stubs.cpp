// Link stubs so settings_overlay.cpp can run standalone in the offline UI preview. Behaviour is deliberately
// minimal: no controller, no audio, a fixed fake window, a few fake Wii Remote / display values.
#include "aim_input.h"
#include "audio_backend.h"
#include "gx_worker.h"
#include "input_bindings.h"
#include "steam_input_layout.h"
#include "wiimote_host.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <cstdint>
#include <cstdio>

extern "C" void PAD_HLE_SetRumbleEnabled(bool) {}
namespace AxDspHle { void SetMixWorkerEnabled(bool) {} }
void SetForceAspect(bool) {}

namespace AimInput {
void Install() {}
void HandleSdlEvent(const SDL_Event&) {}
InputMap::RawSample Sample() { return {}; }
MouseSample MouseNow() { return {}; }
DualAnalog::Rect PictureRect(const MouseSample&) { return {}; }
InputMap::ActiveDevice LastDevice() { return InputMap::ActiveDevice::None; }
InputMap::Scheme EffectiveScheme() { return InputMap::Scheme::Pointer; }
SchemeNotice LatestSwitch() { return {}; }
void UpdateWindowState(bool) {}
void BeginCapture(InputMap::Profile) {}
CaptureResult PollCapture(bool) { return {}; }
void CancelCapture() {}
bool Capturing() { return false; }
} // namespace AimInput

AudioBackend& AudioBackend::Instance() {
    static AudioBackend backend;
    return backend;
}
void AudioBackend::SetMasterVolume(float) {}
void AudioBackend::SetMuted(bool) {}

namespace GxWorker {
void BeginInline() {}
void EndInline() {}
} // namespace GxWorker

namespace InputBindings {
static bool g_blocked = false;
void Reload() noexcept {}
void SetInputBlocked(bool blocked) noexcept { g_blocked = blocked; }
bool InputBlocked() noexcept { return g_blocked; }
} // namespace InputBindings

namespace SteamInputLayout { void Release() noexcept {} }

namespace Wiimote {
void Shutdown() {}
SlotInfo Slot(size_t) { return {}; }
PairingInfo Pairing() { PairingInfo p; p.supported = true; return p; }
void StartPairing() {}
void Disconnect(size_t) {}
std::string PlatformNotice() { return {}; }
size_t WaitingRemotes() { return 0; }
} // namespace Wiimote

AuroraWindowSize aurora_get_window_size() {
    AuroraWindowSize s{};
    s.width = 1920;
    s.height = 1080;
    s.fb_width = 1920;
    s.fb_height = 1080;
    s.native_fb_width = 1920;
    s.native_fb_height = 1080;
    s.scale = 1.0f;
    return s;
}
AuroraDisplayMode aurora_get_display_mode() { return AURORA_DISPLAY_MODE_WINDOWED; }
double aurora_get_display_refresh_rate() { return 60.0; }
bool aurora_display_shows_interpolation() { return false; }
void aurora_set_resampler(AuroraSampler) {}
SDL_Window* aurora_get_window() { return nullptr; }
AuroraBackend aurora_get_backend() { return BACKEND_D3D12; }
void aurora_set_display_mode(AuroraDisplayMode) {}
void aurora_get_present_timing(AuroraPresentTiming* t) {
    *t = AuroraPresentTiming{};
    t->framesPerSecond = 59.8;
    t->effectiveFramesPerSecond = 59.8;
    t->sampleCount = 10;
}
void aurora_set_vrr(AuroraVrrMode, uint32_t, bool) {}
void aurora_get_vrr_status(AuroraVrrStatus* s) {
    *s = AuroraVrrStatus{};
    s->tearingSupported = true;
    s->refreshHz = 144.0;
    s->presentMode = "Fifo";
}
void aurora_set_frame_interpolation_fps(uint32_t) {}
void aurora_set_frame_rate(AuroraFrameRateMode, uint32_t, uint32_t) {}
void aurora_get_frame_rate(AuroraFrameRateMode* mode, uint32_t* fixedFps, uint32_t* capFps) {
    if (mode) *mode = AURORA_FRAME_RATE_OFF;
    if (fixedFps) *fixedFps = 0;
    if (capFps) *capFps = 1000;
}
void aurora_set_upscaler(AuroraUpscaler, float, float) {}
void aurora_get_upscale_info(AuroraUpscaleInfo* info) {
    *info = AuroraUpscaleInfo{};
    info->active = AURORA_UPSCALER_FSR1;
    info->inputWidth = 1280;
    info->inputHeight = 720;
    info->outputWidth = 1920;
    info->outputHeight = 1080;
}
// The UI preview pretends to be an RTX machine so the NVIDIA DLSS rows of the Upscaling section can be inspected.
void aurora_get_dlss_status(AuroraDlssStatus* status) {
    *status = AuroraDlssStatus{};
    status->state = AURORA_DLSS_AVAILABLE;
    std::snprintf(status->version, sizeof status->version, "%s", "Streamline 2.14.16 / DLSS 310.9.16");
}
void aurora_set_dlss_motion_vectors(bool) {}
void aurora_set_skip_unready_pipelines(bool) {}
uint32_t aurora_get_queued_pipeline_count() { return 0; }
void aurora_set_disable_copy_filter(bool) {}
void aurora_enable_vsync(bool) {}
void VISetFrameBufferScale(float) {}

u32 PADCount() { return 0; }
s32 PADGetIndexForPort(u32) { return -1; }
const char* PADGetNameForControllerIndex(u32) { return "Test controller"; }
struct SDL_Gamepad* PADGetSDLGamepadForIndex(u32) { return nullptr; }
void PADBlockInput(bool) {}
void PADControlAllMotors(const u32*) {}
void PADSetButtonMapping(u32, PADButtonMapping) {}
void PADSetAltButtonMapping(u32, PADButtonMapping) {}
PADButtonMapping* PADGetButtonMappings(u32, u32*) { return nullptr; }
void PADSetPortForIndex(u32, u32) {}

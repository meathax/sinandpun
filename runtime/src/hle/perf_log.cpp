// Debug aid: SIN_PERF_LOG=1 logs once per second (after a 20 s boot skip):
//   fps / guest  - guest frames per second (EFB->XFB copies; 60.0 when the game runs at full speed)
//   present      - frames actually presented per second (interpolated frames included)
//   ratio        - present / guest, the frame-rate multiplier
//   speed        - guest / 60: game speed relative to the original (1.000 = original speed)
//   vi_time      - guest time (VI retraces / 60) per wall second; stays 1.000 at any present rate
//   mode / plan / slots - frame-rate mode, planned present Hz and interpolated slots per guest frame
// A second [perf-present] line per second reports the swapchain present mode (Immediate = tearing allowed),
// VRR state (mode, active, limiter, limit Hz), planned target, display refresh and the present-to-present
// interval distribution (p50/p99/max/jitter over the last second): evenly paced VRR shows p50 ~= p99 ~= 1000/limit.
// plus process CPU use (percent of one core) and working set. Used for the performance pass (docs/STATUS.md).
// A [perf-upscale] line reports the upscaler (requested/active), its scale factor and sharpness and the real
// internal (game render target) and output (presented) resolutions, so a log proves the render scale.
// SIN_SCREENSHOT=<file.png> (optional SIN_SCREENSHOT_DELAY=<seconds>, default 30) saves the final upscaled game
// image once, before the overlay is drawn; independent of SIN_PERF_LOG.

#include "perf_pacing.h"
#include "runtime_log.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

extern int g_gxFrameCount;  // hle/gx: number of EFB->XFB copies presented
uint32_t VI_HLE_GetRetraceCount();  // hle/vi.cpp

// Per-guest-frame timing (SIN_PERF_LOG): the guest thread records, for every GXCopyDisp, the wall time since
// the previous one (pacing included, so ~16.67 ms) and the CPU cycles the guest thread burned between the end
// of the previous pace wait and this copy (the real per-frame guest CPU cost; blocked time is not counted).
// The perf thread turns the last second into a [perf-gf] line (p50/p99/p99.9/max).
namespace {
constexpr uint32_t kGfRing = 1024;
struct GfSample {
    float wallMs;
    float mcycles;
};
GfSample g_gfRing[kGfRing];
std::atomic<uint32_t> g_gfWrite{0};
bool g_gfEnabled = [] {
    const char* v = std::getenv("SIN_PERF_LOG");
    return v && *v && *v != '0';
}();
}  // namespace

#if defined(_WIN32)
namespace {
uint64_t GfThreadCycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}
double GfNowMs() {
    static const double invFreq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * invFreq;
}
uint64_t g_gfCycleMark = 0;
double g_gfLastMs = 0.0;
}  // namespace
#endif

// Called by GXCopyDisp (guest thread) right before the pace wait.
void PerfLog_GuestFrameCopy() {
#if defined(_WIN32)
    if (!g_gfEnabled) {
        return;
    }
    const uint64_t cyc = GfThreadCycles();
    const double ms = GfNowMs();
    if (g_gfLastMs != 0.0 && g_gfCycleMark != 0) {
        const uint32_t i = g_gfWrite.load(std::memory_order_relaxed);
        g_gfRing[i % kGfRing] = {static_cast<float>(ms - g_gfLastMs), static_cast<float>((cyc - g_gfCycleMark) * 1e-6)};
        g_gfWrite.store(i + 1, std::memory_order_release);
    }
    g_gfLastMs = ms;
#endif
}

// Called by GXCopyDisp (guest thread) right after the pace wait returned.
void PerfLog_GuestFrameResume() {
#if defined(_WIN32)
    if (g_gfEnabled) {
        g_gfCycleMark = GfThreadCycles();
    }
#endif
}

namespace {

const char* UpscalerName(AuroraUpscaler upscaler) {
    switch (upscaler) {
    case AURORA_UPSCALER_FSR1: return "fsr1";
    case AURORA_UPSCALER_FSR3: return "fsr3";
    case AURORA_UPSCALER_DLSS: return "dlss";
    default: return "off";
    }
}

struct PerfLog {
    PerfLog() {
        if (const char* shot = std::getenv("SIN_SCREENSHOT"); shot && *shot) {
            const char* delayText = std::getenv("SIN_SCREENSHOT_DELAY");
            const int delay = delayText ? std::max(1, std::atoi(delayText)) : 30;
            std::thread([path = std::string(shot), delay] {
                std::this_thread::sleep_for(std::chrono::seconds(delay));
                aurora_capture_present_image(path.c_str());
                RT_LOGF(RT_TAG_RUNTIME, "[screenshot] requested final image capture -> %s\n", path.c_str());
            }).detach();
        }
        const char* v = std::getenv("SIN_PERF_LOG");
        if (!v || !*v || *v == '0') {
            return;
        }
        std::thread([] {
#if defined(_WIN32)
            using Clock = std::chrono::steady_clock;
            auto lastTime = Clock::now();
            int lastFrames = g_gxFrameCount;
            const auto cpuNow = [] {
                FILETIME c, e, k, u;
                GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
                const auto to100ns = [](const FILETIME& f) {
                    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
                };
                return to100ns(k) + to100ns(u);
            };
            uint64_t lastCpu = cpuNow();
            std::this_thread::sleep_for(std::chrono::seconds(20));  // skip boot
            lastTime = Clock::now();
            lastFrames = g_gxFrameCount;
            lastCpu = cpuNow();
            double presentMaxRun = 0.0;
            AuroraPresentTiming baseline{};
            aurora_get_present_timing(&baseline);
            uint64_t lastPresents = baseline.totalPresentCount;
            uint32_t lastRetraces = VI_HLE_GetRetraceCount();
            uint32_t gfRead = g_gfWrite.load(std::memory_order_acquire);
            DWORD lastPageFaults = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const auto now = Clock::now();
                const double dt = std::chrono::duration<double>(now - lastTime).count();
                const int frames = g_gxFrameCount;
                const uint64_t cpu = cpuNow();
                PROCESS_MEMORY_COUNTERS pmc{};
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
                const double cpuPct = static_cast<double>(cpu - lastCpu) / 1e5 / dt;  // 100ns units -> % of one core
                AuroraPresentTiming timing{};
                aurora_get_present_timing(&timing);
                AuroraFrameInterpolationDiagnostics interp{};
                aurora_get_frame_interpolation_diagnostics(&interp);
                const uint32_t retraces = VI_HLE_GetRetraceCount();
                const double guestFps = (frames - lastFrames) / dt;
                const double presentFps = static_cast<double>(timing.totalPresentCount - lastPresents) / dt;
                // Guest time elapsed per wall second: VI retraces are the 60 Hz guest clock. The speed figure
                // is the produced guest frames over the 60 Hz expectation, so a render-bound slowdown shows
                // here even though the retrace clock itself keeps ticking.
                const double viRatio = static_cast<double>(retraces - lastRetraces) / 60.0 / dt;
                RT_LOGF(RT_TAG_RUNTIME,
                        "[perf] fps=%.1f guest=%.1f present=%.1f ratio=%.2f speed=%.3f vi_time=%.3f mode=%u plan=%.1f "
                        "slots=%u cpu=%.0f%% (of one core) ws=%zuMB\n",
                        guestFps, guestFps, presentFps, guestFps > 0.0 ? presentFps / guestFps : 0.0, guestFps / 60.0,
                        viRatio, interp.frameRateMode, static_cast<double>(interp.plannedFps), interp.activeSamples,
                        cpuPct, static_cast<size_t>(pmc.WorkingSetSize >> 20));
                AuroraVrrStatus vrr{};
                aurora_get_vrr_status(&vrr);
                RT_LOGF(RT_TAG_RUNTIME,
                        "[perf-present] present_mode=%s tearing_supported=%d vrr=%s vrr_active=%d limiter=%d "
                        "limit=%.1f target=%.1f refresh=%.2f p50=%.2fms p99=%.2fms max=%.2fms jitter=%.2fms n=%u\n",
                        vrr.presentMode, vrr.tearingSupported ? 1 : 0,
                        vrr.mode == AURORA_VRR_ON ? "on" : (vrr.mode == AURORA_VRR_OFF ? "off" : "auto"),
                        vrr.active ? 1 : 0, vrr.limiterActive ? 1 : 0, vrr.limitFps, vrr.plannedFps, vrr.refreshHz,
                        timing.p50FrameTimeMs, timing.p99FrameTimeMs, timing.maxFrameTimeMs, timing.jitterMs,
                        timing.sampleCount);
                AuroraUpscaleInfo up{};
                aurora_get_upscale_info(&up);
                RT_LOGF(RT_TAG_RUNTIME,
                        "[perf-upscale] upscaler=%s active=%s factor=%.2f sharpness=%.0f%% sharpened=%d "
                        "internal=%ux%u output=%ux%u scale=%.3f\n",
                        UpscalerName(up.requested), UpscalerName(up.active), static_cast<double>(up.scaleFactor),
                        static_cast<double>(up.sharpness) * 100.0, up.sharpened ? 1 : 0, up.inputWidth, up.inputHeight,
                        up.outputWidth, up.outputHeight,
                        up.inputHeight ? static_cast<double>(up.outputHeight) / up.inputHeight : 0.0);
                {
                    const uint32_t wr = g_gfWrite.load(std::memory_order_acquire);
                    const uint32_t n = std::min<uint32_t>(wr - gfRead, kGfRing);
                    if (n > 0) {
                        std::vector<float> wall, cyc;
                        for (uint32_t k = wr - n; k != wr; ++k) {
                            wall.push_back(g_gfRing[k % kGfRing].wallMs);
                            cyc.push_back(g_gfRing[k % kGfRing].mcycles);
                        }
                        gfRead = wr;
                        std::sort(wall.begin(), wall.end());
                        std::sort(cyc.begin(), cyc.end());
                        const auto q = [n](const std::vector<float>& v, double p) {
                            return v[std::min<size_t>(n - 1, static_cast<size_t>(p * n))];
                        };
                        RT_LOGF(RT_TAG_RUNTIME,
                                "[perf-gf] n=%u wall_ms p50=%.2f p99=%.2f p99.9=%.2f max=%.2f | cpu_Mcyc p50=%.1f "
                                "p99=%.1f p99.9=%.1f max=%.1f | pagefaults=%lu\n",
                                n, q(wall, 0.5), q(wall, 0.99), q(wall, 0.999), wall.back(), q(cyc, 0.5), q(cyc, 0.99),
                                q(cyc, 0.999), cyc.back(), static_cast<unsigned long>(pmc.PageFaultCount - lastPageFaults));
                    }
                }
                {
                    PerfPacing::Summary w, t;
                    PerfPacing::TakeSummary(w, t);
                    RT_LOGF(RT_TAG_RUNTIME,
                            "[perf-pacing] t=%.0f guest_interval n=%u p50=%.2f p99=%.2f p99.9=%.2f max=%.2f over25=%u | "
                            "run n=%u p50=%.2f p99=%.2f p99.9=%.2f max=%.2f over25=%u | present_max_run=%.2f\n",
                            PerfPacing::SecondsSinceStart(), w.n, w.p50, w.p99, w.p999, w.max, w.over25, t.n, t.p50, t.p99, t.p999, t.max, t.over25,
                            presentMaxRun = std::max(presentMaxRun, static_cast<double>(timing.maxFrameTimeMs)));
                }                lastPageFaults = pmc.PageFaultCount;
                lastTime = now;
                lastFrames = frames;
                lastCpu = cpu;
                lastPresents = timing.totalPresentCount;
                lastRetraces = retraces;
            }
#endif
        }).detach();
    }
} g_perfLog;

}  // namespace

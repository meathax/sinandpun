// Debug aid: SIN_PERF_LOG=1 logs once per second: presented frames per second, process CPU use (percent of one
// core) and working set. Used for the performance pass (docs/STATUS.md).

#include "runtime_log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

extern int g_gxFrameCount;  // hle/gx: number of EFB->XFB copies presented

namespace {

struct PerfLog {
    PerfLog() {
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
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const auto now = Clock::now();
                const double dt = std::chrono::duration<double>(now - lastTime).count();
                const int frames = g_gxFrameCount;
                const uint64_t cpu = cpuNow();
                PROCESS_MEMORY_COUNTERS pmc{};
                GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
                const double cpuPct = static_cast<double>(cpu - lastCpu) / 1e5 / dt;  // 100ns units -> % of one core
                RT_LOGF(RT_TAG_RUNTIME, "[perf] fps=%.1f cpu=%.0f%% (of one core) ws=%zuMB\n", (frames - lastFrames) / dt,
                        cpuPct, static_cast<size_t>(pmc.WorkingSetSize >> 20));
                lastTime = now;
                lastFrames = frames;
                lastCpu = cpu;
            }
#endif
        }).detach();
    }
} g_perfLog;

}  // namespace

#include "perf_sched.h"

#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace PerfSched {
namespace {

unsigned Mask() {
    static const unsigned mask = [] {
        const char* v = std::getenv("SIN_SCHED");
        if (v && *v) {
            return static_cast<unsigned>(std::strtoul(v, nullptr, 0));
        }
        return static_cast<unsigned>(kProcessPriority | kPowerThrottle | kMmcss | kThreadPriority);
    }();
    return mask;
}

#if defined(_WIN32)
using AvSetMmThreadCharacteristicsW_t = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);

// Logical processors of the highest EfficiencyClass (P-cores on hybrid CPUs), optionally one per core.
std::vector<ULONG> PerformanceCpus(bool onePerCore) {
    std::vector<ULONG> cpus;
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (size == 0) {
        return cpus;
    }
    std::vector<uint8_t> buffer(size);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                                          &size)) {
        return cpus;
    }
    BYTE best = 0;
    for (size_t off = 0; off < size;) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + off);
        best = std::max(best, info->Processor.EfficiencyClass);
        off += info->Size;
    }
    for (size_t off = 0; off < size;) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + off);
        if (info->Processor.EfficiencyClass == best && info->Processor.GroupCount > 0) {
            const GROUP_AFFINITY& g = info->Processor.GroupMask[0];
            for (unsigned bit = 0; bit < 64; ++bit) {
                if (g.Mask & (KAFFINITY{1} << bit)) {
                    cpus.push_back(bit);
                    if (onePerCore) {
                        break;
                    }
                }
            }
        }
        off += info->Size;
    }
    return cpus;
}
#endif

}  // namespace

void InitProcess() {
#if defined(_WIN32)
    const unsigned mask = Mask();
    if (mask == 0) {
        return;
    }
    if (mask & kProcessPriority) {
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    }
    if (mask & kPowerThrottle) {
        // PROCESS_POWER_THROTTLING_STATE, declared here because the MinGW headers lack it.
        struct {
            ULONG Version;
            ULONG ControlMask;
            ULONG StateMask;
        } state{1, 0x1 /*EXECUTION_SPEED*/ | 0x4 /*IGNORE_TIMER_RESOLUTION*/, 0};  // controlled by us, and off
        using SetProcessInformation_t = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
        if (auto fn = reinterpret_cast<SetProcessInformation_t>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"))) {
            fn(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &state, sizeof state);
        }
    }
    if (mask & kPCoresOnly) {
        const auto cpus = PerformanceCpus((mask & kNoSmt) != 0);
        if (!cpus.empty()) {
            GROUP_AFFINITY group{};
            for (ULONG cpu : cpus) {
                group.Mask |= KAFFINITY{1} << cpu;
            }
            // Windows 11+ only: a static import stops the exe loading at all on Windows 10,
            // so resolve it at runtime and skip P-core pinning where it is absent.
            using SetProcessDefaultCpuSetMasks_t = BOOL(WINAPI*)(HANDLE, PGROUP_AFFINITY, USHORT);
            if (auto fn = reinterpret_cast<SetProcessDefaultCpuSetMasks_t>(reinterpret_cast<void*>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessDefaultCpuSetMasks")))) {
                fn(GetCurrentProcess(), &group, 1);
            } else {
                RT_LOGF(RT_TAG_RUNTIME, "[sched] SetProcessDefaultCpuSetMasks unavailable (pre-Windows 11)\n");
            }
        }
        RT_LOGF(RT_TAG_RUNTIME, "[sched] mask=0x%x P-core logical cpus=%zu\n", mask, cpus.size());
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "[sched] mask=0x%x\n", mask);
    }
#endif
}

void TuneThread(const char* role) {
#if defined(_WIN32)
    const unsigned mask = Mask();
    if (mask == 0) {
        return;
    }
    if (mask & kThreadPriority) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
    if (mask & kMmcss) {
        static const auto fn = reinterpret_cast<AvSetMmThreadCharacteristicsW_t>(
            [] {
                HMODULE avrt = LoadLibraryW(L"avrt.dll");
                return avrt ? reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) : nullptr;
            }());
        if (fn) {
            DWORD task = 0;
            if (fn(L"Games", &task) == nullptr) {
                RT_LOGF(RT_TAG_RUNTIME, "[sched] MMCSS failed for %s (error %lu)\n", role, GetLastError());
            }
        }
    }
#else
    (void)role;
#endif
}

}  // namespace PerfSched

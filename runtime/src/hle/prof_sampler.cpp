// Debug aid: SIN_PROF_SAMPLER=1 starts a sampling profiler thread (Windows only).
// Every ~1 ms it suspends each running thread of the process, reads RIP and histograms it as an exe RVA
// (16-byte buckets). A thread counts as "running" only if its cycle counter advanced since the previous
// sample, so blocked threads do not dilute the picture. Every 10 s (after a 20 s boot skip, SIN_PROF_SKIP
// overrides) the cumulative histogram is written to prof_samples.txt in the working directory:
//   tid <id> total <n>            one block per thread, then lines "<count> <rva hex>"
// Resolve RVAs to symbols offline with tools/prof_resolve.py (llvm-nm on WiiCompiled.exe).

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" int __llvm_profile_write_file(void) __attribute__((weak));

namespace {

struct ThreadState {
    HANDLE handle = nullptr;
    uint64_t lastCycles = 0;
    uint64_t running = 0;
    std::unordered_map<uint32_t, uint32_t> rva;  // 16-byte bucket -> samples
};

void RefreshThreads(std::map<DWORD, ThreadState>& threads, DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self ||
                threads.count(te.th32ThreadID)) {
                continue;
            }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                  te.th32ThreadID);
            if (h) {
                threads[te.th32ThreadID].handle = h;
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void Dump(const std::map<DWORD, ThreadState>& threads, uint64_t rounds) {
    FILE* f = std::fopen("prof_samples.txt", "w");
    if (!f) {
        return;
    }
    std::fprintf(f, "rounds %llu\n", static_cast<unsigned long long>(rounds));
    for (const auto& [tid, st] : threads) {
        if (st.running < 20) {
            continue;
        }
        std::vector<std::pair<uint32_t, uint32_t>> rows;
        for (const auto& kv : st.rva) {
            rows.emplace_back(kv.second, kv.first);
        }
        std::sort(rows.rbegin(), rows.rend());
        std::fprintf(f, "tid %lu total %llu\n", static_cast<unsigned long>(tid),
                     static_cast<unsigned long long>(st.running));
        for (size_t i = 0; i < rows.size() && i < 4000; ++i) {
            std::fprintf(f, "%u %X\n", rows[i].first, rows[i].second << 4);
        }
    }
    std::fclose(f);
}

// SIN_PGO_DUMP_SECS=<n>: in a -DMKW_PGO_GEN=ON build, write the .profraw every n seconds (the game exits via
// _Exit/kill, so the atexit writer never runs).
void StartPgoDump() {
    const char* v = std::getenv("SIN_PGO_DUMP_SECS");
    if (!v || !*v || !__llvm_profile_write_file) {
        return;
    }
    const int secs = std::max(5, std::atoi(v));
    std::thread([secs] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(secs));
            __llvm_profile_write_file();
        }
    }).detach();
}

struct ProfSampler {
    ~ProfSampler() = default;
    ProfSampler() {
        StartPgoDump();
        const char* v = std::getenv("SIN_PROF_SAMPLER");
        if (!v || !*v || *v == '0') {
            return;
        }
        const char* skipText = std::getenv("SIN_PROF_SKIP");
        const int skip = skipText ? std::atoi(skipText) : 20;
        std::thread([skip] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            std::this_thread::sleep_for(std::chrono::seconds(skip));
            const DWORD self = GetCurrentThreadId();
            const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            std::map<DWORD, ThreadState> threads;
            uint64_t rounds = 0;
            auto lastRefresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            auto lastDump = std::chrono::steady_clock::now();
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastRefresh > std::chrono::seconds(2)) {
                    RefreshThreads(threads, self);
                    lastRefresh = now;
                }
                for (auto& [tid, st] : threads) {
                    (void)tid;
                    ULONG64 cycles = 0;
                    if (!QueryThreadCycleTime(st.handle, &cycles)) {
                        continue;
                    }
                    const bool active = cycles - st.lastCycles > 400000;  // suspend/resume itself costs a few 10k cycles
                    st.lastCycles = cycles;
                    if (!active) {
                        continue;
                    }
                    if (SuspendThread(st.handle) == static_cast<DWORD>(-1)) {
                        continue;
                    }
                    CONTEXT cx{};
                    cx.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(st.handle, &cx)) {
                        ++st.running;
                        const uintptr_t rip = static_cast<uintptr_t>(cx.Rip);
                        const uintptr_t rel = rip - exeBase;
                        ++st.rva[rel < 0x10000000u ? static_cast<uint32_t>(rel >> 4) : 0xFFFFFFFu];
                    }
                    ResumeThread(st.handle);
                }
                ++rounds;
                if (now - lastDump > std::chrono::seconds(10)) {
                    Dump(threads, rounds);
                    lastDump = now;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }).detach();
    }
} g_profSampler;

}  // namespace
#endif

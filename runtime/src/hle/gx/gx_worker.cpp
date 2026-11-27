#include "gx_worker.h"

#include "perf_pacing.h"
#include "perf_stats.h"
#include "perf_sched.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

void GxWorkerReplayFifoWrite(uint32_t value, uint32_t sizeBytes);
void GxWorkerReplayFifoBurst(const uint8_t* data, uint32_t sizeBytes);

namespace GxWorker {
namespace detail {
std::atomic<bool> g_active{false};
constinit thread_local bool t_onWorker = false;
constinit thread_local int t_inlineDepth = 0;
bool g_fifoPending = false;
} // namespace detail

namespace {
struct Header {
    void (*run)(void* payload);
    uint32_t size;
    uint32_t reserved;
};
constexpr size_t kHeaderSize = 16;
static_assert(sizeof(Header) == kHeaderSize);

constexpr size_t kCapacity = size_t{8} << 20;
// How long the worker keeps polling before it sleeps; recording arrives in bursts and each wake-up
// costs the game thread a lock and notify.
constexpr auto kIdleSpin = std::chrono::microseconds{50};
constexpr size_t kFifoFlushBytes = 8 * 1024;
constexpr uint8_t kFifoBurstTag = 0xB0;

uint8_t* g_ring = nullptr;
uint64_t g_write = 0;
std::atomic<uint64_t> g_head{0};
std::atomic<uint64_t> g_tail{0};
std::atomic<bool> g_consumerWaiting{false};
std::atomic<bool> g_producerWaiting{false};
std::mutex g_headMutex;
std::condition_variable g_headCv;
std::mutex g_tailMutex;
std::condition_variable g_tailCv;
void (*g_waitCallback)() = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_abandon{false};
std::atomic<bool> g_exited{false};
std::thread* g_thread = nullptr;
std::vector<uint8_t> g_fifoBytes;

constexpr size_t AlignUp(size_t value) { return (value + detail::kAlign - 1) & ~(detail::kAlign - 1); }

inline void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}

// libc++ on Windows truncates sub-millisecond condvar timeouts to zero (a spin), so the bounded
// wait goes straight to the OS there.
void WaitTailBriefly(std::unique_lock<std::mutex>& lock) {
#if defined(_WIN32) && defined(_LIBCPP_VERSION)
    SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(g_tailCv.native_handle()),
                              reinterpret_cast<PSRWLOCK>(lock.mutex()->native_handle()), 1, 0);
#else
    g_tailCv.wait_for(lock, std::chrono::milliseconds{1});
#endif
}

// Producer-only: the last write position handed to the worker. Publishing every record kept the
// head cache line bouncing between the game thread and the polling worker.
uint64_t g_published = 0;
constexpr uint64_t kPublishBytes = 2048;

void Publish() {
    if (g_published == g_write) {
        return;
    }
    g_published = g_write;
    g_head.store(g_write, std::memory_order_seq_cst);
    if (g_consumerWaiting.load(std::memory_order_seq_cst)) {
        std::lock_guard lock{g_headMutex};
        g_headCv.notify_one();
    }
}

void WaitForTail(uint64_t minTail, bool service) {
    if (g_tail.load(std::memory_order_acquire) >= minTail) {
        return;
    }
    Publish();
    {
        PerfStats::WaitScope wait;
        for (int spin = 0; spin < 256 && g_tail.load(std::memory_order_acquire) < minTail; ++spin) {
            CpuRelax();
        }
    }
    while (g_tail.load(std::memory_order_acquire) < minTail) {
        {
            PerfStats::WaitScope wait;
            std::unique_lock lock{g_tailMutex};
            g_producerWaiting.store(true, std::memory_order_seq_cst);
            if (g_tail.load(std::memory_order_seq_cst) < minTail) {
                WaitTailBriefly(lock);
            }
            g_producerWaiting.store(false, std::memory_order_relaxed);
        }
        if (service && g_waitCallback != nullptr && g_tail.load(std::memory_order_acquire) < minTail) {
            g_waitCallback();
        }
    }
}

// Producer-only copy of a tail already observed; avoids reading the worker-written line per record.
uint64_t g_tailSeen = 0;

void WaitForSpace(size_t bytes) {
    if (g_write + bytes > kCapacity) {
        const uint64_t minTail = g_write + bytes - kCapacity;
        if (g_tailSeen >= minTail) {
            return;
        }
        g_tailSeen = g_tail.load(std::memory_order_acquire);
        if (g_tailSeen < minTail) {
            WaitForTail(minTail, false);
        }
    }
}

void ReplayFifo(void* payload) {
    uint32_t size = 0;
    std::memcpy(&size, payload, sizeof(size));
    const uint8_t* data = static_cast<const uint8_t*>(payload) + sizeof(size);
    const uint8_t* end = data + size;
    while (data < end) {
        const uint8_t tag = *data++;
        if (tag == kFifoBurstTag) {
            uint32_t length = 0;
            std::memcpy(&length, data, sizeof(length));
            data += sizeof(length);
            try {
                GxWorkerReplayFifoBurst(data, length);
            } catch (...) {
                RT_LOGF(RT_TAG_GX, "GX worker FIFO burst failed\n");
            }
            data += length;
        } else {
            uint32_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            data += sizeof(value);
            try {
                GxWorkerReplayFifoWrite(value, tag);
            } catch (...) {
                RT_LOGF(RT_TAG_GX, "GX worker FIFO write failed\n");
            }
        }
    }
}

// SIN_PERF_LOG: game-thread time spent waiting in Sync and the worker thread's CPU cycles (PerfTake).
std::atomic<uint64_t> g_perfSyncNs{0};
std::atomic<uint64_t> g_perfSyncCount{0};
#if defined(_WIN32)
std::atomic<HANDLE> g_perfWorkerHandle{nullptr};
#endif

void WorkerMain() {
    detail::t_onWorker = true;
#if defined(_WIN32)
    {
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_QUERY_INFORMATION,
                        FALSE, 0);
        g_perfWorkerHandle.store(h, std::memory_order_release);
    }
#endif
    PerfSched::TuneThread("gx");
    struct SignalExit {
        ~SignalExit() {
            std::lock_guard lock{g_tailMutex};
            g_exited.store(true, std::memory_order_release);
            g_tailCv.notify_all();
        }
    } signalExit;
    AuroraSetGxProducerThread();
#if defined(_WIN32)
    // Windows 10 1607+; resolved at runtime so older builds still load the exe.
    using SetThreadDescription_t = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto fn = reinterpret_cast<SetThreadDescription_t>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))) {
        fn(GetCurrentThread(), L"GX worker");
    }
#endif
    uint64_t tail = g_tail.load(std::memory_order_relaxed);
    for (;;) {
        uint64_t head = g_head.load(std::memory_order_acquire);
        if (head == tail) {
            const auto spinUntil = std::chrono::steady_clock::now() + kIdleSpin;
            while (head == tail) {
                for (int spin = 0; spin < 64 && head == tail; ++spin) {
                    CpuRelax();
                    head = g_head.load(std::memory_order_acquire);
                }
                if (head == tail && std::chrono::steady_clock::now() >= spinUntil) {
                    break;
                }
            }
        }
        if (head == tail) {
            if (g_stop.load(std::memory_order_acquire)) {
                return;
            }
            {
                std::unique_lock lock{g_headMutex};
                g_consumerWaiting.store(true, std::memory_order_seq_cst);
                while (g_head.load(std::memory_order_seq_cst) == tail && !g_stop.load(std::memory_order_acquire)) {
                    g_headCv.wait(lock);
                }
                g_consumerWaiting.store(false, std::memory_order_relaxed);
            }
            continue;
        }
        while (tail != head) {
            if (g_abandon.load(std::memory_order_acquire)) {
                return;
            }
            uint8_t* record = g_ring + tail % kCapacity;
            const Header* header = reinterpret_cast<const Header*>(record);
            if (header->run != nullptr) {
                try {
                    static const bool slowLog = [] {
                        const char* v = std::getenv("SIN_GX_SLOW_LOG");
                        return v && *v && *v != '0';
                    }();
                    const auto slowStart = slowLog ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    header->run(record + kHeaderSize);
                    if (slowLog) {
                        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - slowStart).count();
                        if (ms > 8.0) {
#if defined(_WIN32)
                            const auto rva = reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(header->run)) -
                                             reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
#else
                            const auto rva = reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(header->run));
#endif
                            RT_LOGF(RT_TAG_GX, "[gx-slow] record fn=0x%llx took %.1fms\n", static_cast<unsigned long long>(rva), ms);
                        }
                    }
                } catch (const std::exception& error) {
                    RT_LOGF(RT_TAG_GX, "GX worker command failed: %s\n", error.what());
                } catch (...) {
                    RT_LOGF(RT_TAG_GX, "GX worker command failed\n");
                }
            }
            tail += header->size;
            g_tail.store(tail, std::memory_order_seq_cst);
            if (g_producerWaiting.load(std::memory_order_seq_cst)) {
                std::lock_guard lock{g_tailMutex};
                g_tailCv.notify_all();
            }
        }
    }
}
} // namespace

namespace detail {
void* Reserve(size_t payloadSize, void (*run)(void*)) {
    const size_t total = kHeaderSize + AlignUp(payloadSize);
    if (total > kCapacity) {
        RT_LOGF(RT_TAG_GX, "GX worker command of %zu bytes exceeds the ring\n", total);
        std::abort();
    }
    size_t offset = static_cast<size_t>(g_write % kCapacity);
    const size_t remaining = kCapacity - offset;
    if (total > remaining) {
        WaitForSpace(remaining);
        auto* wrap = reinterpret_cast<Header*>(g_ring + offset);
        wrap->run = nullptr;
        wrap->size = static_cast<uint32_t>(remaining);
        g_write += remaining;
        // Publish the skip so the worker can drain past it while we wait for the front.
        Commit();
        offset = 0;
    }
    WaitForSpace(total);
    auto* header = reinterpret_cast<Header*>(g_ring + offset);
    header->run = run;
    header->size = static_cast<uint32_t>(total);
    g_write += total;
    return g_ring + offset + kHeaderSize;
}

// Anything that waits on the worker publishes first (WaitForTail), so batching here only delays it.
void Commit() {
    if (g_write - g_published >= kPublishBytes) {
        Publish();
    }
}

void FlushFifo() {
    g_fifoPending = false;
    if (g_fifoBytes.empty()) {
        return;
    }
    const uint32_t size = static_cast<uint32_t>(g_fifoBytes.size());
    void* payload = Reserve(sizeof(size) + size, &ReplayFifo);
    std::memcpy(payload, &size, sizeof(size));
    std::memcpy(static_cast<uint8_t*>(payload) + sizeof(size), g_fifoBytes.data(), size);
    g_fifoBytes.clear();
    Commit();
}
} // namespace detail

void FifoWrite(uint32_t value, uint32_t sizeBytes) {
    const size_t at = g_fifoBytes.size();
    g_fifoBytes.resize(at + 1 + sizeof(value));
    g_fifoBytes[at] = static_cast<uint8_t>(sizeBytes);
    std::memcpy(g_fifoBytes.data() + at + 1, &value, sizeof(value));
    detail::g_fifoPending = true;
    if (g_fifoBytes.size() >= kFifoFlushBytes) {
        detail::FlushFifo();
    }
}

void FifoBurst(const uint8_t* data, uint32_t sizeBytes) {
    const size_t at = g_fifoBytes.size();
    g_fifoBytes.resize(at + 1 + sizeof(sizeBytes) + sizeBytes);
    g_fifoBytes[at] = kFifoBurstTag;
    std::memcpy(g_fifoBytes.data() + at + 1, &sizeBytes, sizeof(sizeBytes));
    std::memcpy(g_fifoBytes.data() + at + 1 + sizeof(sizeBytes), data, sizeBytes);
    detail::g_fifoPending = true;
    if (g_fifoBytes.size() >= kFifoFlushBytes) {
        detail::FlushFifo();
    }
}

// SIN_GX_SYNC_SITES=1 (diagnostic): histogram of Sync callers (exe RVAs), logged every 10 s.
namespace {
thread_local void* t_syncSite = nullptr;
const bool g_syncSites = [] {
    const char* v = std::getenv("SIN_GX_SYNC_SITES");
    return v && *v && *v != '0';
}();
std::mutex g_siteMutex;
std::vector<std::pair<uintptr_t, std::pair<uint64_t, uint64_t>>> g_sites;  // rva -> (count, ns)
std::chrono::steady_clock::time_point g_siteDump{};
void NoteSyncSite(void* site, uint64_t ns) {
#if defined(_WIN32)
    const uintptr_t rva = reinterpret_cast<uintptr_t>(site) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
#else
    const uintptr_t rva = reinterpret_cast<uintptr_t>(site);
#endif
    std::lock_guard lock{g_siteMutex};
    auto it = std::find_if(g_sites.begin(), g_sites.end(), [&](const auto& e) { return e.first == rva; });
    if (it == g_sites.end()) {
        g_sites.push_back({rva, {0, 0}});
        it = g_sites.end() - 1;
    }
    it->second.first++;
    it->second.second += ns;
    const auto now = std::chrono::steady_clock::now();
    if (g_siteDump == std::chrono::steady_clock::time_point{}) g_siteDump = now;
    if (now - g_siteDump >= std::chrono::seconds(10)) {
        g_siteDump = now;
        std::sort(g_sites.begin(), g_sites.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        std::string line = "[gx-sync-sites]";
        for (size_t i = 0; i < g_sites.size() && i < 12; ++i) {
            char buf[96];
            std::snprintf(buf, sizeof buf, " 0x%llx:%llu/%.0fms", static_cast<unsigned long long>(g_sites[i].first),
                          static_cast<unsigned long long>(g_sites[i].second.first), g_sites[i].second.second / 1e6);
            line += buf;
        }
        RT_LOGF(RT_TAG_GX, "%s\n", line.c_str());
        g_sites.clear();
    }
}
}  // namespace

void Kick() {
    if (!Active() || detail::t_onWorker) {
        return;
    }
    if (detail::g_fifoPending) {
        detail::FlushFifo();
    }
    Publish();
}

void PerfTake(uint64_t& syncNs, uint64_t& syncs, uint64_t& workerCycles) {
    syncNs = g_perfSyncNs.exchange(0, std::memory_order_relaxed);
    syncs = g_perfSyncCount.exchange(0, std::memory_order_relaxed);
    workerCycles = 0;
#if defined(_WIN32)
    if (HANDLE h = g_perfWorkerHandle.load(std::memory_order_acquire)) {
        ULONG64 c = 0;
        QueryThreadCycleTime(h, &c);
        workerCycles = c;
    }
#endif
}

void Sync() {
    if (!Active() || detail::t_onWorker) {
        return;
    }
    const auto syncStart = std::chrono::steady_clock::now();
    void* site = t_syncSite ? t_syncSite : __builtin_return_address(0);
    t_syncSite = nullptr;
    struct SyncTimer {
        std::chrono::steady_clock::time_point start;
        void* site;
        ~SyncTimer() {
            const auto ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            g_perfSyncNs.fetch_add(ns, std::memory_order_relaxed);
            g_perfSyncCount.fetch_add(1, std::memory_order_relaxed);
            if (g_syncSites) NoteSyncSite(site, ns);
        }
    } syncTimer{syncStart, site};
    PerfPacing::PhaseScope syncPhase(PerfPacing::kPhaseGxSync);
    // Guest callbacks serviced while waiting can record more work.
    do {
        if (detail::g_fifoPending) {
            detail::FlushFifo();
        }
        WaitForTail(g_write, true);
    } while (detail::g_fifoPending || g_tail.load(std::memory_order_acquire) != g_write);
}

void SetWaitCallback(void (*callback)()) { g_waitCallback = callback; }

void BeginInline() {
    if (detail::t_inlineDepth == 0 && Active() && !detail::t_onWorker) {
        t_syncSite = __builtin_return_address(0);
        Sync();
    }
    ++detail::t_inlineDepth;
}

void EndInline() { --detail::t_inlineDepth; }

void Start() {
    if (Active()) {
        return;
    }
    if (!RuntimeConfigFile::GxWorkerEnabled(true)) {
        RT_LOGF(RT_TAG_GX, "GX worker disabled\n");
        return;
    }
    if (g_ring == nullptr) {
        g_ring = static_cast<uint8_t*>(::operator new(kCapacity, std::align_val_t{64}));
    }
    g_fifoBytes.reserve(kFifoFlushBytes + 4096);
    g_stop.store(false, std::memory_order_relaxed);
    g_abandon.store(false, std::memory_order_relaxed);
    g_exited.store(false, std::memory_order_relaxed);
    g_thread = new std::thread(WorkerMain);
    detail::g_active.store(true, std::memory_order_release);
    RT_LOGF(RT_TAG_GX, "GX worker started\n");
}

void Stop() {
    if (!Active()) {
        return;
    }
    // Guest state may already be torn down; don't service it while draining.
    g_waitCallback = nullptr;
    Sync();
    detail::g_active.store(false, std::memory_order_release);
    g_stop.store(true, std::memory_order_release);
    Submit([] {});
    Publish();
    if (g_thread != nullptr) {
        g_thread->join();
        delete g_thread;
        g_thread = nullptr;
    }
}

void Abandon() {
    if (!Active() || detail::t_onWorker) {
        return;
    }
    detail::g_active.store(false, std::memory_order_release);
    g_abandon.store(true, std::memory_order_release);
    g_stop.store(true, std::memory_order_release);
    {
        std::lock_guard lock{g_headMutex};
        g_headCv.notify_one();
    }
    bool exited = false;
    {
        std::unique_lock lock{g_tailMutex};
        exited = g_tailCv.wait_for(lock, std::chrono::seconds{2},
                                   [] { return g_exited.load(std::memory_order_acquire); });
    }
    if (g_thread != nullptr) {
        if (exited) {
            g_thread->join();
            delete g_thread;
        } else {
            g_thread->detach();
        }
        g_thread = nullptr;
    }
}

} // namespace GxWorker

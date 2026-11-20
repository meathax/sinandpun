#include "perf_pacing.h"

#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace PerfPacing {
namespace {

using Clock = std::chrono::steady_clock;

struct State {
    std::array<std::atomic<int64_t>, kPhaseCount> phaseNs{};  // since the previous guest frame
    std::atomic<uint32_t> dvdReads{0};
    std::atomic<uint64_t> dvdBytes{0};
    std::atomic<int64_t> dvdMaxNs{0};
    Clock::time_point last{};
    bool haveLast = false;
    bool logSpikes = false;
    std::mutex mutex;  // guards the sample vectors (guest thread writes, perf thread reads once a second)
    std::vector<float> window;
    static constexpr size_t kBins = 4000;  // 0.1 ms bins up to 400 ms
    std::array<uint32_t, kBins + 1> hist{};
    uint32_t totalN = 0;
    uint32_t totalOver25 = 0;
    double totalMax = 0;
    uint32_t windowOver25 = 0;
    uint64_t lastFaults = 0;
    std::atomic<uint64_t> totalDvdReads{0}, totalDvdBytes{0}, totalDvdNs{0};
    bool linkPending = false;
    Clock::time_point linkAt{};
    double linkMs = 0;
    uint64_t linkReads = 0, linkBytes = 0, linkNs = 0;
};

Clock::time_point ProcessStart() {
    static const Clock::time_point start = Clock::now();
    return start;
}

State& S() {
    static State s;
    return s;
}

bool ComputeEnabled() {
    const auto on = [](const char* n) {
        const char* v = std::getenv(n);
        return v && *v && *v != '0';
    };
    (void)ProcessStart();
    S().logSpikes = on("SIN_PERF_SPIKES");
    return on("SIN_PERF_LOG") || S().logSpikes;
}

double HistPercentile(const std::array<uint32_t, State::kBins + 1>& h, uint32_t n, double q) {
    if (n == 0) {
        return 0;
    }
    const uint64_t target = static_cast<uint64_t>(q * n + 0.9999999);
    uint64_t acc = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        acc += h[i];
        if (acc >= std::max<uint64_t>(target, 1)) {
            return (i + 0.5) * 0.1;
        }
    }
    return State::kBins * 0.1;
}

double Pct(std::vector<float>& v, double q) {
    if (v.empty()) {
        return 0;
    }
    const size_t idx = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

}  // namespace

bool Enabled() noexcept {
    static const bool enabled = ComputeEnabled();
    return enabled;
}

void AddPhase(Phase phase, Clock::duration d) noexcept {
    S().phaseNs[phase].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(),
                                 std::memory_order_relaxed);
}

void NoteDvdRead(Clock::duration d, uint32_t bytes) noexcept {
    if (!Enabled()) {
        return;
    }
    auto& s = S();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    s.phaseNs[kPhaseDvd].fetch_add(ns, std::memory_order_relaxed);
    s.dvdReads.fetch_add(1, std::memory_order_relaxed);
    s.totalDvdReads.fetch_add(1, std::memory_order_relaxed);
    s.totalDvdBytes.fetch_add(bytes, std::memory_order_relaxed);
    s.totalDvdNs.fetch_add(ns, std::memory_order_relaxed);
    s.dvdBytes.fetch_add(bytes, std::memory_order_relaxed);
    int64_t prev = s.dvdMaxNs.load(std::memory_order_relaxed);
    while (ns > prev && !s.dvdMaxNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
}

void OnGuestFrame() noexcept {
    if (!Enabled()) {
        return;
    }
    auto& s = S();
    const auto now = Clock::now();
    if (!s.haveLast) {
        s.haveLast = true;
        s.last = now;
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(now - s.last).count();
    s.last = now;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.window.push_back(static_cast<float>(ms));
        s.hist[std::min(State::kBins, static_cast<size_t>(ms * 10.0))]++;
        ++s.totalN;
        s.totalMax = std::max(s.totalMax, ms);
        if (ms > 25.0) {
            ++s.totalOver25;
            ++s.windowOver25;
        }
    }
    std::array<double, kPhaseCount> ph{};
    for (int i = 0; i < kPhaseCount; ++i) {
        ph[i] = static_cast<double>(s.phaseNs[i].exchange(0, std::memory_order_relaxed)) / 1e6;
    }
    const uint32_t reads = s.dvdReads.exchange(0, std::memory_order_relaxed);
    const uint64_t bytes = s.dvdBytes.exchange(0, std::memory_order_relaxed);
    const double dvdMax = static_cast<double>(s.dvdMaxNs.exchange(0, std::memory_order_relaxed)) / 1e6;
    if (s.linkPending && now - s.linkAt >= std::chrono::seconds(10)) {
        s.linkPending = false;
        RT_LOGF(RT_TAG_RUNTIME,
                "[perf-load] link+10s: dvd reads=%llu bytes=%lluKB host_read=%.1fms (link cost %.2f ms)\n",
                static_cast<unsigned long long>(s.totalDvdReads.load() - s.linkReads),
                static_cast<unsigned long long>((s.totalDvdBytes.load() - s.linkBytes) >> 10),
                static_cast<double>(s.totalDvdNs.load() - s.linkNs) / 1e6, s.linkMs);
    }
    if (s.logSpikes && ms > 25.0) {
        uint64_t faults = 0;
        size_t wsMb = 0;
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS pmc{};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        faults = pmc.PageFaultCount;
        wsMb = static_cast<size_t>(pmc.WorkingSetSize >> 20);
#endif
        const double other = ms - ph[kPhaseEndFrame] - ph[kPhasePace] - ph[kPhasePrewarm] - ph[kPhaseDvd] - ph[kPhaseRso];
        const char* cause = "guest/other";
        if (ph[kPhaseDvd] > 8.0) cause = "dvd";
        else if (ph[kPhaseRso] > 8.0) cause = "rso-link";
        else if (ph[kPhaseEndFrame] > 8.0) cause = "end-frame(render worker backpressure)";
        else if (ph[kPhasePrewarm] > 8.0) cause = "prewarm/events";
        else if (ph[kPhasePace] > 17.5) cause = "pace(oversleep)";
        RT_LOGF(RT_TAG_RUNTIME,
                "[spike] t=%.1f interval=%.1fms endframe=%.1f pace=%.1f prewarm=%.1f dvd=%.1f(%u reads %lluKB max %.1f) rso=%.1f "
                "other=%.1f faults+=%llu ws=%zuMB cause=%s\n",
                std::chrono::duration<double>(now - ProcessStart()).count(), ms, ph[kPhaseEndFrame], ph[kPhasePace], ph[kPhasePrewarm], ph[kPhaseDvd], reads,
                static_cast<unsigned long long>(bytes >> 10), dvdMax, ph[kPhaseRso], other,
                static_cast<unsigned long long>(faults - s.lastFaults), wsMb, cause);
        s.lastFaults = faults;
    }
}

double SecondsSinceStart() noexcept {
    return std::chrono::duration<double>(Clock::now() - ProcessStart()).count();
}

void NoteStageLink(double linkMs) noexcept {
    if (!Enabled()) {
        return;
    }
    auto& s = S();
    s.linkPending = true;
    s.linkAt = Clock::now();
    s.linkMs = linkMs;
    s.linkReads = s.totalDvdReads.load();
    s.linkBytes = s.totalDvdBytes.load();
    s.linkNs = s.totalDvdNs.load();
    RT_LOGF(RT_TAG_RUNTIME, "[perf-load] t=%.1f stage link cost %.2f ms\n", SecondsSinceStart(), linkMs);
}

void TakeSummary(Summary& window, Summary& total) noexcept {
    auto& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    window = Summary{};
    window.n = static_cast<uint32_t>(s.window.size());
    window.over25 = s.windowOver25;
    if (!s.window.empty()) {
        window.max = *std::max_element(s.window.begin(), s.window.end());
        window.p50 = Pct(s.window, 0.50);
        window.p99 = Pct(s.window, 0.99);
        window.p999 = Pct(s.window, 0.999);
    }
    s.window.clear();
    s.windowOver25 = 0;
    total = Summary{};
    total.n = s.totalN;
    total.over25 = s.totalOver25;
    total.max = s.totalMax;
    total.p50 = HistPercentile(s.hist, s.totalN, 0.50);
    total.p99 = HistPercentile(s.hist, s.totalN, 0.99);
    total.p999 = HistPercentile(s.hist, s.totalN, 0.999);
}

}  // namespace PerfPacing

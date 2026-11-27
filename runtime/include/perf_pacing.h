#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// Frame-pacing diagnostics (SIN_PERF_LOG=1 prints a [perf-pacing] line per second, SIN_PERF_SPIKES=1 logs
// a [spike] line for every guest frame interval above 25 ms with the phases that filled it). Everything is
// a no-op unless one of those variables is set: Enabled() is a cached bool.
namespace PerfPacing {

enum Phase : int {
    kPhaseEndFrame = 0,  // EndAuroraFrame (seal + hand to the render worker)
    kPhasePace,          // PaceToRetraceBoundary (intentional wait)
    kPhasePrewarm,       // UpdateAuroraAndProcessEvents + BeginAuroraFrame
    kPhaseDvd,           // host disc reads on the guest thread
    kPhaseRso,           // stage module link
    kPhaseAlarm,         // alarm/audio service inside the pace wait
    kPhaseViWait,        // guest VIWaitForRetrace (the game's own retrace wait)
    kPhaseGxSync,        // game thread waiting for the GX worker to drain (GxWorker::Sync)
    kPhaseCount
};

bool Enabled() noexcept;
void AddPhase(Phase phase, std::chrono::steady_clock::duration d) noexcept;
void NoteDvdRead(std::chrono::steady_clock::duration d, uint32_t bytes) noexcept;
// Call once per guest frame, after pacing. Records the interval and emits the spike line.
void OnGuestFrame() noexcept;
// Seconds since the first PerfPacing use (about process start); timestamps for [spike]/[perf-pacing] lines.
double SecondsSinceStart() noexcept;
// Stage module link: starts a 10 s window whose DVD totals are logged as [perf-load].
void NoteStageLink(double linkMs) noexcept;

struct Summary {
    uint32_t n = 0;
    double p50 = 0, p99 = 0, p999 = 0, max = 0;  // ms
    uint32_t over25 = 0;
};
// Interval statistics since the previous call (window) and since start (total); used by perf_log.cpp.
void TakeSummary(Summary& window, Summary& total) noexcept;

class PhaseScope {
public:
    explicit PhaseScope(Phase p) noexcept
        : m_phase(p), m_start(Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~PhaseScope() {
        if (m_start != std::chrono::steady_clock::time_point{}) {
            AddPhase(m_phase, std::chrono::steady_clock::now() - m_start);
        }
    }
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;

private:
    Phase m_phase;
    std::chrono::steady_clock::time_point m_start;
};

}  // namespace PerfPacing

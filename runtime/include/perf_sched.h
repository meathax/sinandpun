#pragma once

// Windows scheduling for the latency-critical threads (guest, GX worker, audio mixer).
// SIN_SCHED is a bit mask (default: PerfSched::kDefaultMask; 0 disables everything, for A/B runs):
//   1 process priority class ABOVE_NORMAL      2 power-throttling (EcoQoS) opt-out + honour timer resolution
//   4 MMCSS "Games" task on the hot threads     8 keep the process off E-cores (default CPU sets)
//  16 thread priority ABOVE_NORMAL on the hot threads   32 additionally one logical CPU per P-core (no SMT siblings)
namespace PerfSched {

enum Bits : unsigned {
    kProcessPriority = 1,
    kPowerThrottle = 2,
    kMmcss = 4,
    kPCoresOnly = 8,
    kThreadPriority = 16,
    kNoSmt = 32,
};

void InitProcess();                 // once, early in RuntimeMain
void TuneThread(const char* role);  // on the thread itself: "guest", "gx", "audio"

}  // namespace PerfSched

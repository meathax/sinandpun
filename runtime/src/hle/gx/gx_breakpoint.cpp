// GX command-processor breakpoint emulation.
//
// Sin & Punishment paces its main loop on the GP breakpoint interrupt: at the
// end of a frame it calls GXEnableBreakPt(<fifo write pointer>); when the GP
// reaches that point the breakpoint callback registered with
// GXSetBreakPtCallback (0x801D58B0, stored at r13-20916 = 0x804B254C) runs in
// interrupt context. That callback (fn_8003EB9C) sets the "frame done" flag the
// loop at 0x8003EC8C polls, then calls VISetNextFrameBuffer and VIFlush.
// Without the interrupt the body of the frame loop never runs a second time.
//
// The GX HLE executes draw commands as they are issued, so the GP has always
// "reached" the breakpoint by the time the game arms it. The callback is
// therefore delivered at the next VI retrace service point (see vi.cpp,
// AdvanceRetrace), which is when the original GP would have drained the FIFO.

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <atomic>

namespace {
// SDA-relative SDK globals (r13 = 0x804B7700), verified by reading the guest
// GXSetBreakPtCallback / GXEnableBreakPt bodies in main.dol.
constexpr uint32_t kBreakPtCallbackAddr = 0x804B254C; // r13 - 20916
constexpr uint32_t kBreakPtAddrAddr = 0x804B2548;     // r13 - 20920

std::atomic<bool> g_breakPtArmed{false};
} // namespace

// GXEnableBreakPt (0x801D5900): the real function programs CP registers over
// MMIO, which the runtime ignores. Keep the guest-visible bookkeeping and arm
// the emulated interrupt.
extern "C" void GXEnableBreakPt_HLE_801d5900(CpuContext* ctx) {
    try {
        Memory::Write32(kBreakPtAddrAddr, ctx->gpr[3]);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_GX, "GXEnableBreakPt", e);
    }
    g_breakPtArmed.store(true, std::memory_order_release);
    static std::atomic<int> s_log{0};
    if (s_log.fetch_add(1) < 3) {
        RT_LOGF(RT_TAG_GX, "GXEnableBreakPt(0x%08X): breakpoint armed\n", ctx->gpr[3]);
    }
}
PPC_NATIVE_OVERRIDE_VOID(801D5900, GXEnableBreakPt_HLE_801d5900, (CpuContext* ctx), (ctx));

// Returns true (and the guest callback address, possibly 0) once per arming.
bool GX_HLE_TakeBreakPoint(uint32_t* callback) {
    if (!g_breakPtArmed.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    try {
        *callback = Memory::Read32(kBreakPtCallbackAddr);
    } catch (const Memory::AccessViolation&) {
        *callback = 0;
    }
    return true;
}

// The game's OS idle function as a host-owned coroutine.
//
// Sin & Punishment calls OSSetIdleFunction(0x800380D8, 0, stack, 0x1000) from main. The SDK turns
// that into the priority-31 idle thread (0x8042A9B8). fn_800380D8 is the asset loader: a busy-wait
// loop over request flags that dispatches file-load work. On hardware it runs whenever no other
// thread is runnable and is preempted by interrupts.
//
// The scheduler HLE never schedules the SDK idle thread (the host idle loop replaces it), and the
// loader loop has no calls the runtime could preempt it at. So:
//   * the translator inserts GuestLoopPoll(ctx) at every basic-block entry of that function
//     (project manifest: translation.loop_poll_functions), and
//   * the scheduler's idle loop resumes the function on its own fiber for a bounded burst each
//     iteration; GuestLoopPoll hands control back to the host loop.
// This keeps the original code unmodified and preserves "runs only when nothing else can".

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "os_internal.h"
#include "runtime_log.h"

#include <cstdint>
#include <exception>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// SDK IdleThread (OSInit/OSSetIdleFunction). Context layout: gpr[N] at N*4, r1 at +4, r2 at +8,
// r13 at +0x34, SRR0 (entry) at +0x198.
const uint32_t kIdleThreadAddr = MKW_GADDR(8042A9B8);
// The SDK's own default idle loop lives in the SDK address range; only a game function counts.
constexpr uint32_t kGameCodeLo = 0x80003100u;
constexpr uint32_t kGameCodeHi = 0x80150000u;

#if defined(_WIN32)
struct IdleCoroutine {
    void* fiber = nullptr;
    void* hostFiber = nullptr;
    CpuContext* cpu = nullptr;
    CpuContext idleRegs{};
    CpuContext hostRegs{};
    bool dead = false;
    bool inside = false;
    uint32_t entry = 0;
    uint32_t arg = 0;
    uint32_t sp = 0;
};

IdleCoroutine g_idle;

void CALLBACK IdleFiberProc(void*) {
    IdleCoroutine& co = g_idle;
    try {
        CpuContextScope scope(co.cpu);
        InvokeIndirectCpu(co.entry, co.cpu);
        RT_LOG(RT_TAG_OS) << "idle function returned (unexpected); idle coroutine stopped" << std::endl;
    } catch (const std::exception& e) {
        RT_LOG(RT_TAG_OS) << "idle function threw: " << e.what() << "; idle coroutine stopped" << std::endl;
    } catch (...) {
        RT_LOG(RT_TAG_OS) << "idle function threw; idle coroutine stopped" << std::endl;
    }
    co.dead = true;
    for (;;) {
        SwitchToFiber(co.hostFiber);
    }
}

bool PrepareIdle(CpuContext* cpu) {
    if (g_idle.dead) {
        return false;
    }
    if (g_idle.fiber) {
        return true;
    }
    uint32_t entry = 0;
    try {
        entry = ::Memory::Read32(kIdleThreadAddr + 0x198u);
        g_idle.sp = ::Memory::Read32(kIdleThreadAddr + 0x04u);
        g_idle.arg = ::Memory::Read32(kIdleThreadAddr + 0x0Cu);
    } catch (const ::Memory::AccessViolation&) {
        return false;
    }
    if (entry < kGameCodeLo || entry >= kGameCodeHi || g_idle.sp == 0) {
        return false; // OSSetIdleFunction not called yet (or only the SDK default idle loop).
    }
    g_idle.entry = entry;
    g_idle.fiber = CreateFiberEx(0, 1024 * 1024, 0, IdleFiberProc, nullptr);
    if (!g_idle.fiber) {
        RT_LOG(RT_TAG_OS) << "idle coroutine: CreateFiberEx failed" << std::endl;
        g_idle.dead = true;
        return false;
    }
    g_idle.idleRegs = *cpu;
    g_idle.idleRegs.gpr[1] = g_idle.sp;
    g_idle.idleRegs.gpr[2] = ::Memory::Read32(kIdleThreadAddr + 0x08u);
    g_idle.idleRegs.gpr[13] = ::Memory::Read32(kIdleThreadAddr + 0x34u);
    g_idle.idleRegs.gpr[3] = g_idle.arg;
    g_idle.idleRegs.lr = 0;
    g_idle.idleRegs.pc = entry;
    g_idle.idleRegs.srr0 = entry;
    RT_LOG(RT_TAG_OS) << "idle coroutine: entry=0x" << std::hex << entry << " sp=0x" << g_idle.sp
                      << " arg=0x" << g_idle.arg << std::dec << std::endl;
    return true;
}
#endif

} // namespace

// Runs the game's idle function for up to `maxResumes` poll intervals (or until a thread other than
// the idle thread becomes runnable). Called from the scheduler's idle loop.
extern "C" void OS_HLE_RunIdleSlice(CpuContext* cpu, int maxResumes) {
#if defined(_WIN32)
    if (!cpu || g_idle.dead || g_idle.inside) {
        return;
    }
    if (!PrepareIdle(cpu)) {
        return;
    }
    g_idle.cpu = cpu;
    g_idle.hostFiber = GetCurrentFiber();
    g_idle.hostRegs = *cpu;
    for (int i = 0; i < maxResumes && !g_idle.dead; ++i) {
        *cpu = g_idle.idleRegs;
        g_idle.inside = true;
        SwitchToFiber(g_idle.fiber);
        g_idle.inside = false;
        if ((::Memory::Read32(kSchedulerPendingFlagAddr) & ~1u) != 0) {
            break;
        }
    }
    *cpu = g_idle.hostRegs;
    MkwApplyHostNiMode(cpu->fpscr);
#else
    (void)cpu;
    (void)maxResumes;
#endif
}

// Emitted by the translator at every basic-block entry of functions listed in
// translation.loop_poll_functions. Outside the idle coroutine it is a no-op.
void GuestLoopPoll(CpuContext* ctx) {
#if defined(_WIN32)
    if (!g_idle.inside || ctx == nullptr) {
        return;
    }
    g_idle.idleRegs = *ctx;
    SwitchToFiber(g_idle.hostFiber);
    // Resumed: OS_HLE_RunIdleSlice already loaded idleRegs into the shared context.
#else
    (void)ctx;
#endif
}

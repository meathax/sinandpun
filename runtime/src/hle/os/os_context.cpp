// Guest OSContext bookkeeping and the OSLoadContext context switch.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "os_internal.h"

extern "C" uint32_t OS__GetCurrentThread_801a98b0_hle()
{
    try {
        return ::Memory::Read32(kOSRunningContextAddr);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__GetCurrentThread_801a98b0", e);
        return 0;
    }
}

extern "C" void OS__ClearContext_801a2098(uint32_t contextAddr)
{
    if (contextAddr == 0) {
        return;
    }

    try {
        ::Memory::Write16(contextAddr + 0x1a0u, 0);
        ::Memory::Write16(contextAddr + 0x1a2u, 0);

        const uint32_t exceptionCtx = ::Memory::Read32(kOSExceptionContextAddr);
        if (exceptionCtx == contextAddr) {
            ::Memory::Write32(kOSExceptionContextAddr, 0);
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__ClearContext_801a2098", e);
    }
}

extern "C" void OS__SetCurrentContext_801a1e70(uint32_t contextAddr)
{
    try {
        if (contextAddr == 0) {
            ::Memory::Write32(kOSPhysicalContextAddr, 0);
            ::Memory::Write32(kOSCurrentContextAddr, 0);
            return;
        }

        const uint32_t physicalMask = contextAddr & 0x3fffffffu;
        ::Memory::Write32(kOSPhysicalContextAddr, physicalMask);
        ::Memory::Write32(kOSCurrentContextAddr, contextAddr);

        const uint32_t exceptionCtx = ::Memory::Read32(kOSExceptionContextAddr);
        uint32_t flags = ::Memory::Read32(contextAddr + 0x19cu);
        if (exceptionCtx == contextAddr) {
            flags |= 0x2000u;
        } else {
            flags &= ~0x2000u;
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        ::Memory::Write32(contextAddr + 0x19cu, flags);

        uint16_t modeFlags = ::Memory::Read16(contextAddr + 0x1a2u);
        if (g_interrupts_enabled.load()) {
            modeFlags |= 0x0002u;
        } else {
            modeFlags &= static_cast<uint16_t>(~0x0002u);
        }
        ::Memory::Write16(contextAddr + 0x1a2u, modeFlags);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__SetCurrentContext_801a1e70", e);
    }
}

PPC_NATIVE_OVERRIDE_VOID(801A89D0, OS__ClearContext_801a2098, (uint32_t contextAddr), (contextAddr));
PPC_NATIVE_OVERRIDE_VOID(801A8790, OS__SetCurrentContext_801a1e70, (uint32_t contextAddr), (contextAddr));
// SIN-PENDING(unresolved raw registration) REGISTER_NATIVE_FUNCTION(0x801A98B0, OS__GetCurrentThread_801a98b0_hle);

// OSLoadContext (0x801A1F58): restores CPU state from a guest OSContext and jumps to SRR0 by hand,
// since the real function's privileged mtspr/rfi can't be translated.

extern "C" void OS__LoadContext_801a1f58(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t guestContextAddr = cpu->gpr[3];

    RT_LOG(RT_TAG_OS) << "OS__LoadContext_801a1f58 called: ctx=0x" << std::hex << guestContextAddr << std::dec << std::endl;

    if (guestContextAddr == 0) {
        RT_LOG(RT_TAG_OS) << "OS__LoadContext: FATAL - null context address!" << std::endl;
        std::fflush(stderr);
        std::abort();
    }

    try {
        // Validate SRR0 (the jump target, at 0x198) before mutating any other state.
        uint32_t srr0 = ::Memory::Read32(guestContextAddr + 0x198u);

        // Validate the jump target exists in our translated function registry
        if (srr0 == 0) {
            // An empty context (raw .bss, never saved into) means "nothing to
            // resume": the scheduler picked the default thread before anyone
            // saved a context on it. Stay on the current execution instead of
            // aborting - the caller's spin continues, which is exactly what a
            // real OS does when the only runnable "thread" is the caller.
            RT_LOG(RT_TAG_OS) << "OS__LoadContext: context 0x" << std::hex << guestContextAddr
                      << " has SRR0=0 (never saved); staying on current execution."
                      << std::dec << std::endl;
            if (g_decrementerCallbackDepth > 0) {
                // Inside the simulated decrementer exception: OSLoadContext is
                // the handler's rfi back to the interrupted code. Unwind the
                // callback instead of falling into its post-call bytes.
                throw DecrementerExceptionReturn{};
            }
            return;
        }

        // ---- Load GPRs ----
        for (int i = 0; i < 32; ++i) {
            cpu->gpr[i] = ::Memory::Read32(guestContextAddr + static_cast<uint32_t>(i * 4));
        }

        // ---- Load Special Registers ----
        cpu->cr  = ::Memory::Read32(guestContextAddr + 0x80u);
        cpu->lr  = ::Memory::Read32(guestContextAddr + 0x84u);
        cpu->ctr = ::Memory::Read32(guestContextAddr + 0x88u);
        cpu->xer = ::Memory::Read32(guestContextAddr + 0x8Cu);

        // ---- Load GQRs (Graphics Quantization Registers) ----
        // GQR0 is always 0 (not saved/restored)
        cpu->gqr[0] = 0;
        cpu->gqr[1] = ::Memory::Read32(guestContextAddr + 0x1A8u);
        cpu->gqr[2] = ::Memory::Read32(guestContextAddr + 0x1ACu);
        cpu->gqr[3] = ::Memory::Read32(guestContextAddr + 0x1B0u);
        cpu->gqr[4] = ::Memory::Read32(guestContextAddr + 0x1B4u);
        cpu->gqr[5] = ::Memory::Read32(guestContextAddr + 0x1B8u);
        cpu->gqr[6] = ::Memory::Read32(guestContextAddr + 0x1BCu);
        cpu->gqr[7] = ::Memory::Read32(guestContextAddr + 0x1C0u);

        // ---- Clear exception context flag if set ----
        uint16_t modeFlags = ::Memory::Read16(guestContextAddr + 0x1A2u);
        if ((modeFlags & 0x0002u) != 0) {
            modeFlags &= ~0x0002u;
            ::Memory::Write16(guestContextAddr + 0x1A2u, modeFlags);
        }

        // ---- Update PC and SRR registers ----
        cpu->srr0 = srr0;
        cpu->srr1 = ::Memory::Read32(guestContextAddr + 0x19Cu);
        cpu->pc = srr0;

        RT_LOG(RT_TAG_OS) << "OS__LoadContext: jumping to target=0x" << std::hex << srr0 
                  << " SP=0x" << cpu->gpr[1] << std::dec << std::endl;

        // Equivalent to 'rfi': jump to SRR0 with MSR from SRR1.
        InvokeIndirectJump(srr0, cpu);

        // If we get here, the target returned normally (unexpected for a
        // context switch, but the recomp's dispatch makes it survivable).
        RT_LOG(RT_TAG_OS) << "OS__LoadContext: WARNING - target returned, this is unexpected!" << std::endl;

    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__LoadContext", e);
        std::fflush(stderr);
        std::abort();
    }

    // A normal return from here means "stay on current execution": the
    // SRR0=0 path above and a returned target both resume the caller, which
    // is the cooperative continuation the scheduler expects.
    return;
}

PPC_NATIVE_OVERRIDE_VOID(801A8880, OS__LoadContext_801a1f58, (CpuContext* ctx), (ctx));

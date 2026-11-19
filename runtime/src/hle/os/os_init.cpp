// Boot/init hooks, OSFatal, and assorted hardware-init stubs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "abi_bridge.h"

// os_scheduler.cpp exports these (extern "C"); no shared header exists.
extern "C" void SelectThread_801a9c08(CpuContext* ctx);
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"
#include "system_bridge.h"
#include "guest_function_imports.h"

extern "C" void OSInitAlarm_RecompModLateInit_801a961c(CpuContext* ctx) {
    MKW_GUEST_FUNC(801A961C)(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x801A961C, OSInitAlarm_RecompModLateInit_801a961c, "OSInitAlarm_RecompModLateInit_801a961c");

extern "C" void StaticRProlog_RecompModInit_8055531c(CpuContext* ctx) {
    RecompMod::RunMemoryInitializers();
    MKW_GUEST_FUNC(8055531C)(ctx);
    RecompMod::RunPostRelInitializers();
}

REGISTER_NATIVE_FUNCTION_AS(0x8055531C, StaticRProlog_RecompModInit_8055531c, "StaticRProlog_RecompModInit_8055531c");

namespace {
std::string ReadGuestCStringLimited(uint32_t address, size_t limit = 4096) {
    if (address == 0) {
        return "<null>";
    }

    std::string text;
    text.reserve(128);
    for (size_t i = 0; i < limit; ++i) {
        const uint8_t ch = Memory::Read8(address + static_cast<uint32_t>(i));
        if (ch == 0) {
            return text;
        }
        text.push_back(static_cast<char>(ch));
    }
    text += "<unterminated>";
    return text;
}
}

extern "C" void OSFatal_HLE_801a4ec4(CpuContext* ctx) {
    const uint32_t fg = ctx ? ctx->gpr[3] : 0;
    const uint32_t bg = ctx ? ctx->gpr[4] : 0;
    const uint32_t messagePtr = ctx ? ctx->gpr[5] : 0;
    RT_LOG(RT_TAG_OS) << "OS::Fatal called fg=0x" << std::hex << fg
              << " bg=0x" << bg
              << " message=0x" << messagePtr
              << std::dec << " '" << ReadGuestCStringLimited(messagePtr) << "'" << std::endl;
    if (ctx) {
        SystemBridge::DumpCpuState(ctx);
    }
    const std::string guestMessage = ReadGuestCStringLimited(messagePtr);
    const std::string details =
        guestMessage.empty() ? std::string("OS::Fatal was called without a message.") : guestMessage;
    // MarkFatalErrorReported below suppresses the atexit reporter, so this path
    // has to write its own artifacts or the run folder gets nothing.
    RuntimeCrash::WriteCrashArtifacts("osfatal", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system reported a fatal error", details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

REGISTER_NATIVE_FUNCTION_AS(0x801A4EC4, OSFatal_HLE_801a4ec4, "OSFatal_HLE_801a4ec4");

extern "C" void GKI_delay_HLE_801301b4()
{
    // The guest's GKI_delay is empty: BTA_Init spins on it while the Bluetooth
    // module answers through IPC interrupts and the 2 ms BTU alarm. Service both
    // here and let the wall clock advance.
    OS_HLE_ProcessAlarms(32);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

SIN_DISABLED_NATIVE_OVERRIDE_VOID(801301B4, GKI_delay_HLE_801301b4, (), ());

// Serial Interface (SI) - GameCube controller ports; stubbed since we don't emulate the MMIO.

// SIInit (0x801b2de0): skips MMIO setup at 0xCD006434 and controller detection.
extern "C" void SIInit_801b2de0()
{
    RT_LOG(RT_TAG_OS) << "SIInit_801b2de0 called: skipping MMIO register setup and controller detection" << std::endl;
}

// SISetSamplingRate (R2VE01 0x801B8830): ignored, we don't emulate SI polling
// timing. The translated body would otherwise reach VIGetTvFormat and the VI
// TV-mode MMIO register (crash artifact base_1795063862).
extern "C" void HLE_SISetSamplingRate_801b8830(uint32_t msec)
{
    RT_LOG(RT_TAG_OS) << "HLE_SISetSamplingRate_801b8830 called: msec=" << msec << ": Stubbed success." << std::endl;
}

// Video Interface (VI) - TV output.

// VIGetTvFormat (R2VE01 0x801BC440): CRITICAL, must report the game's own TV
// format or it misbehaves/panics. The Feb 2009 SDK build Sin links reads the
// VI TV-mode MMIO register (0xCC00206C) directly, so the translated body must
// never run; the donor's global-based equivalent never needed this native.
extern "C" uint32_t HLE_VIGetTvFormat_801bc440()
{
    // VI_NTSC = 0, VI_PAL = 1, VI_MPAL = 2
    RT_LOG(RT_TAG_OS) << "HLE_VIGetTvFormat_801bc440 called: returning " << ::GuestRegion::CurrentFacts().viTvFormat
                      << std::endl;
    return ::GuestRegion::CurrentFacts().viTvFormat;
}
PPC_NATIVE_OVERRIDE(801BC440, HLE_VIGetTvFormat_801bc440, uint32_t, (), ());

// OSRegisterShutdownFunction (R2VE01 0x801AE130): the game's OSInit registers
// the NWC24 shutdown hook and OSFatals when the call fails ("Failed to
// register network shutdown function"). The host owns shutdown, so accept
// every registration and report success.
extern "C" uint32_t HLE_OSRegisterShutdownFunction_801ae130(uint32_t info, uint32_t priority)
{
    RT_LOG(RT_TAG_OS) << "HLE_OSRegisterShutdownFunction_801ae130 called: info=0x" << std::hex << info
                      << " priority=" << std::dec << priority << ": Stubbed success." << std::endl;
    return 1;
}
PPC_NATIVE_OVERRIDE(801AE130, HLE_OSRegisterShutdownFunction_801ae130, uint32_t, (uint32_t info, uint32_t priority), (info, priority));

// __OSInitNet (R2VE01 0x801B26B0): OSInit's network-system init. The
// translated body registers the NWC24 shutdown function and OSInit verifies
// it afterwards. 0x81 in the boot-info bytes (0x8000315C/D) is the FAILURE
// marker: writing it (first attempt) triggered the OSPanic("OS.c:1153",
// "Failed to register network shutdown function") directly. Success leaves
// the bytes untouched (0), so the stub writes nothing.
extern "C" void HLE___OSInitNet_801b26b0()
{
    RT_LOG(RT_TAG_OS) << "HLE___OSInitNet_801b26b0 called: network subsystem init skipped "
                      << "(success: failure markers left at 0)." << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B26B0, HLE___OSInitNet_801b26b0, (), ());

// VIGetScanMode (R2VE01 0x801BC4A0): the Feb-2009 SDK body reads the VI
// TV-mode MMIO register (0xCC00206C) directly; the engine calls it right
// after VIGetTvFormat (fn_8003DC58+0xA8, compared against VI_PROGRESSIVE=2).
// Retail NTSC boots 480i: report VI_INTERLACE (0).
extern "C" uint32_t HLE_VIGetScanMode_801bc4a0()
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        RT_LOG(RT_TAG_OS) << "HLE_VIGetScanMode_801bc4a0 called: returning VI_INTERLACE (0)" << std::endl;
    }
    return 0;
}
PPC_NATIVE_OVERRIDE(801BC4A0, HLE_VIGetScanMode_801bc4a0, uint32_t, (), ());

REGISTER_NATIVE_FUNCTION(0x801B7AF0, SIInit_801b2de0);
PPC_NATIVE_OVERRIDE_VOID(801B8830, HLE_SISetSamplingRate_801b8830, (uint32_t msec), (msec));

// OS____InitMemoryProtection (0x801A7DFC): real version touches MMU/MMIO we don't emulate;
// no-op and return success so boot doesn't stall.
extern "C" uint32_t OS____InitMemoryProtection_801a7dfc(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8)
{
    RT_LOG(RT_TAG_OS) << "OS____InitMemoryProtection_801a7dfc called (stubbed): r3=0x" << std::hex << r3 << std::dec << std::endl;

    return 0;
}

PPC_NATIVE_OVERRIDE(801ADC90, OS____InitMemoryProtection_801a7dfc, uint32_t, (uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8), (r3, r4, r5, r6, r7, r8));

// OSGetConsoleType (R2VE01 0x801A58E0): retail Wii console type = 1. The
// donor's "retail 0x12" was MKWii-specific; Sin's OSInit string table maps
// 0x12 to "Pre-production board 2-1" (a dev unit), and dev-mode also enables
// the firmware-version diagnostics block whose assert OSFatals the boot.
extern "C" uint32_t OS__GetConsoleType_8019f33c(uint32_t /*r4*/, uint32_t /*r5*/, uint32_t /*r6*/,
                                                uint32_t /*r7*/, uint32_t /*r8*/, uint32_t /*r31*/)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        RT_LOG(RT_TAG_OS) << "OSGetConsoleType: returning retail 0x1" << std::endl;
    }
    return 0x00000001u;
}

// Register the function
PPC_NATIVE_OVERRIDE(801A58E0, OS__GetConsoleType_8019f33c, uint32_t, (uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r31), (r4, r5, r6, r7, r8, r31));

// OSGetResetCode (0x801a8a50): real version reads MMIO 0xCC003024; we always report
// Cold Boot (0).
extern "C" uint32_t OSGetResetCode_801a8a50()
{
    // Boot-path flags BS2 would have set before the game runs: a disc boot
    // happens from IPL, so __OSInIPL = 1. The guest crt's own __init_data
    // zeroes .sbss AFTER every host-side seeding pass, so this native (called
    // inside OSInit, before the __OSInIPL gate at 0x801A6514) is the earliest
    // reliable place to plant them. Without this, OSInit takes the NAND-boot
    // diagnostics path and OSFatals ("Failed to register network shutdown
    // function") at OSInit source lines 1153/1171.
    Memory::Write32(0x804B21D8u, 0x00000001u);  // __OSInIPL: disc boot
    Memory::Write32(0x804B21DCu, 0x00000000u);  // __OSInNandBoot

    // Log occasionally just to track boot flow
    static bool logged = false;
    if (!logged) {
        RT_LOG(RT_TAG_OS) << "OSGetResetCode_801a8a50 called: returning 0 (Cold Boot)"
                          << " (__OSInIPL seeded)" << std::endl;
        logged = true;
    }
    return 0;
}

// Register the function
PPC_NATIVE_OVERRIDE(801AEA10, OSGetResetCode_801a8a50, uint32_t, (), ());

// __OSInitSTM (0x801AB848): real version opens /dev/stm/* handles. We stub it by writing
// fake handles and the success flag into the SDA (r13) block so OSResetSystem's checks pass.
extern "C" uint32_t __OSInitSTM_HLE_801ab848(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) return 0;

    RT_LOG(RT_TAG_OS) << "__OSInitSTM_HLE_801ab848 called: initializing STM state" << std::endl;

    // STM_Initialized, /dev/stm/immediate and /dev/stm/eventhook.
    const uint32_t kStmInitializedAddr = MKW_GADDR(80386934);
    const uint32_t kStmImmediateHandleAddr = MKW_GADDR(80386938);
    const uint32_t kStmEventHookHandleAddr = MKW_GADDR(8038693C);

    try {
        // Mark STM as initialized
        ::Memory::Write32(kStmInitializedAddr, 1);

        // Fake non-zero handles so callers' zero-checks pass.
        ::Memory::Write32(kStmImmediateHandleAddr, 0x00535401); // "ST\x01"
        ::Memory::Write32(kStmEventHookHandleAddr, 0x00535402); // "ST\x02"

        // Default Power/Reset callback pointers are left unset; safe since we never fire
        // the STM hardware interrupt that would invoke them.
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOG(RT_TAG_OS) << "__OSInitSTM: Failed to write STM state to SDA @ 0x"
                  << std::hex << e.address() << std::dec << " (" << e.reason() << ")" << std::endl;
        return 0; // Return failure
    }

    // Return 1 (success)
    return 1;
}

// Register the function
PPC_NATIVE_OVERRIDE(801B1800, __OSInitSTM_HLE_801ab848, uint32_t, (CpuContext* ctx), (ctx));


extern "C" void OS____PSInit_801a04a0()
{
    RT_LOG(RT_TAG_OS) << "OS____PSInit_801a04a0 called (stubbed)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(801A6B50, OS____PSInit_801a04a0, (), ());



extern "C" void __init_hardware_80006348()
{
    RT_LOG(RT_TAG_OS) << "__init_hardware_80006348 called (stubbed)" << std::endl;
}

SIN_DISABLED_NATIVE_OVERRIDE_VOID(80006348,__init_hardware_80006348, (), ());


// PPC SPR (Special Purpose Register) access stubs; these registers don't exist on x86 so
// each one just logs and no-ops.

// Each PPC_NATIVE_OVERRIDE_VOID line stays written out per stub (not looped) because the
// translator text-scans them to decide which addresses to skip translating.
#define PPC_SPR_STUB_BODY(name, message) \
    extern "C" void name() { RT_LOG(RT_TAG_OS) << message << std::endl; }

PPC_SPR_STUB_BODY(PPCMfhid0_8012e574, "PPCMfhid0 called (stubbed) - Move From HID0")
PPC_NATIVE_OVERRIDE_VOID(8015E790, PPCMfhid0_8012e574, (), ());

PPC_SPR_STUB_BODY(PPCMthid0_8012e57c, "PPCMthid0 called (stubbed) - Move To HID0")
PPC_NATIVE_OVERRIDE_VOID(8015E7A0, PPCMthid0_8012e57c, (), ());

extern "C" void PPCMtdec_8012e594()
{
    static std::atomic<int> logCount{0};
    if (logCount.fetch_add(1) < 4) {
        RT_LOG(RT_TAG_OS) << "PPCMtdec called (stubbed) - Move To Decrementer" << std::endl;
    }
    // We don't simulate decrementer exceptions, so pump due alarms here instead. Capped at
    // 32, not 1: a single alarm let unrelated periodic alarms backlog and delay
    // AsyncDisplay's pacing alarm by multiple retraces.
    OS_HLE_ProcessAlarms(32);
    // The real decrementer exception runs the SDK's
    // DecrementerExceptionCallback (R2VE01 0x801A7160): system-time
    // bookkeeping, time-slice preemption and a conditional OSLoadContext
    // (0x801A8880) switch. Raw decrementer waits (mtdec; spin) rely on that
    // state machine to observe their wake condition, so the set-point invokes
    // the callback with the current thread context. The callback's own path
    // re-reaches mtdec (its timer bookkeeping), so an in-flight guard keeps
    // the simulation from recursing: nested set-points only pump alarms.
    static thread_local int callbackDepth = 0;
    if (callbackDepth == 0) {
        CpuContext* cpu = &GetPersistentCpuContext();
        const uint32_t currentContext = ::Memory::Read32(0x800000E4u);  // __OSCurrentContext
        // Only preempt when a real thread context exists: at boot the global
        // can hold a non-thread value (observed 0x80000000, the boot-info
        // region), and the callback's OSLoadContext then jumps to a NULL
        // SRR0. Arena-plausible addresses only.
        if (currentContext >= 0x80300000u && currentContext < 0x81800000u) {
            cpu->gpr[3] = currentContext;
            ++callbackDepth;
            try {
                MKW_GUEST_FUNC(801A7160)(cpu);
            } catch (...) {
                --callbackDepth;
                throw;
            }
            --callbackDepth;
            SelectThread_801a9c08(cpu);
        }
    }
}
PPC_NATIVE_OVERRIDE_VOID(8015E7D0, PPCMtdec_8012e594, (), ());

extern "C" void PPCSync_8012e59c()
{
    // PowerPC `sync`. Translated guest code runs on one host thread at a time
    // and the runtime's own cross-thread state uses C++ atomics, so there is no
    // guest-visible reordering for this barrier to prevent.
}
PPC_NATIVE_OVERRIDE_VOID(8015E7E0, PPCSync_8012e59c, (), ());

PPC_SPR_STUB_BODY(PPCMtmmcr0_8012e5b8, "PPCMtmmcr0 called (stubbed) - Move To MMCR0")
PPC_NATIVE_OVERRIDE_VOID(8015E810, PPCMtmmcr0_8012e5b8, (), ());

PPC_SPR_STUB_BODY(PPCMtmmcr1_8012e5c0, "PPCMtmmcr1 called (stubbed) - Move To MMCR1")
PPC_NATIVE_OVERRIDE_VOID(8015E820, PPCMtmmcr1_8012e5c0, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc1_8012e5c8, "PPCMtpmc1 called (stubbed) - Move To PMC1")
PPC_NATIVE_OVERRIDE_VOID(8015E830, PPCMtpmc1_8012e5c8, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc2_8012e5d0, "PPCMtpmc2 called (stubbed) - Move To PMC2")
PPC_NATIVE_OVERRIDE_VOID(8015E840, PPCMtpmc2_8012e5d0, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc3_8012e5d8, "PPCMtpmc3 called (stubbed) - Move To PMC3")
PPC_NATIVE_OVERRIDE_VOID(8015E850, PPCMtpmc3_8012e5d8, (), ());

PPC_SPR_STUB_BODY(PPCMtpmc4_8012e5e0, "PPCMtpmc4 called (stubbed) - Move To PMC4")
PPC_NATIVE_OVERRIDE_VOID(8015E860, PPCMtpmc4_8012e5e0, (), ());

extern "C" uint32_t PPCMfhid2_8012e630_impl()
{
    if (CpuContext* cpu = TryGetCpuContext()) {
        if (cpu->hid2 == 0) {
            cpu->hid2 = 0x10000000u;
        }
        return cpu->hid2;
    }
    return 0x10000000u;
}
// Register the function using a return-value stub
extern "C" void PPCMfhid2_HLE_8012e630(CpuContext* ctx)
{
    ctx->gpr[3] = PPCMfhid2_8012e630_impl();
}
REGISTER_TRANSLATED_FUNCTION(0x8012e630, PPCMfhid2_HLE_8012e630);

extern "C" void PPCMthid2_8012e638(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    cpu->hid2 = cpu->gpr[3];

    static std::atomic<int> logCount{0};
    if (logCount.fetch_add(1) < 4) {
        RT_LOG(RT_TAG_OS) << "PPCMthid2 set HID2=0x" << std::hex << cpu->hid2 << std::dec << std::endl;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8015E8D0, PPCMthid2_8012e638, (CpuContext* ctx), (ctx));

PPC_SPR_STUB_BODY(PPCMfwpar_8012e640,
                  "PPCMfwpar called (stubbed) - Move From Write Pipe Address Register")
SIN_DISABLED_NATIVE_OVERRIDE_VOID(8012e640, PPCMfwpar_8012e640, (), ());

PPC_SPR_STUB_BODY(PPCMtwpar_8012e64c,
                  "PPCMtwpar called (stubbed) - Move To Write Pipe Address Register")
PPC_NATIVE_OVERRIDE_VOID(8015E8E0, PPCMtwpar_8012e64c, (), ());

PPC_SPR_STUB_BODY(PPCDisableSpeculation_8012e654, "PPCDisableSpeculation called (stubbed)")
PPC_NATIVE_OVERRIDE_VOID(8015E8F0, PPCDisableSpeculation_8012e654, (), ());

PPC_SPR_STUB_BODY(PPCMthid4_8012e684, "PPCMthid4 called (stubbed) - Move To HID4")
PPC_NATIVE_OVERRIDE_VOID(8015E930, PPCMthid4_8012e684, (), ());

// Quit / reset / fatal entry points of the R2VE01 OS (Sin & Punishment: Star Successor).
//
// The game's main (0x8003D9C8) falls out of its frame loop into OSRestart / OSShutdownSystem /
// OSReturnToDataManager, and the frame function (0x8003EC1C) calls OSReturnToMenu / OSRestart for
// the HOME menu items. The SDK bodies end in IOS (/dev/es ESP_LaunchTitle via the *synchronous*
// IOS_Ioctlv, which has no HLE), the DVD drive shutdown handshake, hardware reset registers
// (0xCC003024) and an endless wait for the Wii System Menu. None of that exists here, so the call
// would hang or fault. The host owns process lifetime: each entry point performs the same clean
// shutdown the OSResetSystem native does (stop the GX/AX workers, drop the guest mappings, exit 0).
//
// OSFatal (0x801AAF80) would otherwise draw its message on the XFB through the IPL font and then
// spin; report it through the crash-artifact path instead.

#include "memory.h"
#include "hle/audio/ax_dsp.h"
#include "gx_worker.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "system_bridge.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string ReadGuestText(uint32_t addr)
{
    if (addr == 0) {
        return std::string();
    }
    std::string out;
    out.reserve(128);
    try {
        for (uint32_t i = 0; i < 1024; ++i) {
            const uint8_t b = Memory::Read8(addr + i);
            if (b == 0) {
                break;
            }
            out.push_back(static_cast<char>(b));
        }
    } catch (const Memory::AccessViolation&) {
        out += "<invalid_guest_ptr>";
    }
    return out;
}

[[noreturn]] void CleanGuestExit(const char* why)
{
    RT_LOGF(RT_TAG_OS, "%s: guest requested exit, shutting down cleanly\n", why);
    std::fflush(stderr);
    // The AX mix worker holds resolved host pointers into the guest regions; stop it (and the GX
    // worker) before those mappings are torn down.
    GxWorker::Stop();
    AxDspHle::ShutdownMixWorker();
    Memory::Reset();
    SetRuntimeExitCode(0);
    std::exit(EXIT_SUCCESS);
}

} // namespace

// OSShutdownSystem(void) - power-off request.
extern "C" void OSShutdownSystem_HLE_Sin()
{
    CleanGuestExit("OSShutdownSystem");
}
PPC_NATIVE_OVERRIDE_VOID(801AE3F0, OSShutdownSystem_HLE_Sin, (), ());

// OSRestart(u32 resetCode) - HOME menu "Reset": the console relaunches the title. A PC build exits;
// the user relaunches the executable.
extern "C" void OSRestart_HLE_Sin(uint32_t resetCode)
{
    RT_LOGF(RT_TAG_OS, "OSRestart(resetCode=0x%08X)\n", resetCode);
    CleanGuestExit("OSRestart");
}
PPC_NATIVE_OVERRIDE_VOID(801AE510, OSRestart_HLE_Sin, (uint32_t resetCode), (resetCode));

// OSReturnToMenu(void) - HOME menu "Wii Menu".
extern "C" void OSReturnToMenu_HLE_Sin()
{
    CleanGuestExit("OSReturnToMenu");
}
PPC_NATIVE_OVERRIDE_VOID(801AE880, OSReturnToMenu_HLE_Sin, (), ());

// OSReturnToDataManager(void) - Data Management screen request.
extern "C" void OSReturnToDataManager_HLE_Sin()
{
    CleanGuestExit("OSReturnToDataManager");
}
PPC_NATIVE_OVERRIDE_VOID(801AE8C0, OSReturnToDataManager_HLE_Sin, (), ());

// OSFatal(GXColor fg, GXColor bg, const char* msg).
extern "C" void OSFatal_HLE_Sin(CpuContext* ctx)
{
    const uint32_t fg = ctx ? ctx->gpr[3] : 0;
    const uint32_t bg = ctx ? ctx->gpr[4] : 0;
    const uint32_t msgPtr = ctx ? ctx->gpr[5] : 0;
    const std::string message = ReadGuestText(msgPtr);
    RT_LOGF(RT_TAG_OS, "OSFatal fg=0x%08X bg=0x%08X msg='%s'\n", fg, bg, message.c_str());
    if (ctx) {
        SystemBridge::DumpCpuState(ctx);
    }
    const std::string details = message.empty() ? std::string("OSFatal was called without a message.") : message;
    RuntimeCrash::WriteCrashArtifacts("osfatal", details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("the guest operating system reported a fatal error", details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}
PPC_NATIVE_OVERRIDE_VOID(801AAF80, OSFatal_HLE_Sin, (CpuContext* ctx), (ctx));

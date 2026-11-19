// Sin & Punishment: guest WPAD boundary.
//
// The game's WPADInit (R2VE01 0x801C0C10, Feb-2009-Jun-2009 WPAD) drives the
// full Bluetooth handshake with IOS. The runtime's BT HLE targets the donor's
// older MKWii WPAD: the versioned exchange leaves response buffers
// uninitialized and the periodic-sampling alarm registration ends up with a
// garbage callback (first symptom: guest jump to 0x801AADE0, interior of
// fn_801AAB30, straight out of WPADInit's call chain).
//
// The runtime owns controller input end-to-end: real Wii Remotes pair through
// the host Bluetooth stack, and native pads are synthesized at the PAD/KPAD
// layer (where the dual-analog aiming path also lives). The guest's handshake
// is therefore skipped; the rest of the guest WPAD layer sees an initialized
// stack and reads whatever the input natives feed it.

#include "hle_stubs.h"
#include "runtime_log.h"

extern "C" void WPADInit_HLE_801c0c10(CpuContext* ctx)
{
    (void)ctx;
    static bool logged = false;
    if (!logged) {
        RT_LOG(RT_TAG_HLE) << "WPADInit_HLE_801c0c10 called: guest Bluetooth handshake skipped; "
                           << "input is host-owned (virtual remotes / native pads)." << std::endl;
        logged = true;
    }
}
SIN_DISABLED_NATIVE_OVERRIDE_VOID(801C0C10, WPADInit_HLE_801c0c10, (CpuContext* ctx), (ctx));

#pragma once

// Gamepad-backed virtual Wii Remote + Nunchuk (the dual-analog feature).
//
// An Xbox-style controller is presented to the emulated Bluetooth controller
// as a real Wii Remote with a Nunchuk attached. The right stick moves a
// synthetic sensor-bar view (IR dots), the left stick is the Nunchuk stick, so
// the guest's own WPAD/KPAD stack does all pointer math exactly as on hardware.

#include "bluetooth/bt_controller.h"
#include "input_map.h"

namespace VirtualRemote {

// Live extension hot-swap: Classic Controller <-> Nunchuk (pointer scheme). Thread-safe, lock-free and cheap; a
// no-op when `scheme` is already the requested one. The remote unplugs the current extension (status report with
// the extension flag cleared), waits for the guest to notice, then plugs the other one (status report with the
// flag set) so the guest's WPAD re-runs its extension init and KPAD reports the new type. Applied from the
// remote's poll, so it needs no other call. Ignored when SIN_VREMOTE_SCRIPT contains scheme= tokens (the script
// owns the scheme in that run).
void SetScheme(InputMap::Scheme scheme);

// The scheme most recently requested (or the settings default before the first request).
InputMap::Scheme GetScheme();

// Host used by the emulated Bluetooth controller: physical Wii Remotes keep
// their slots; slot 0 falls back to the virtual remote when a gamepad is
// present (or SIN_VIRTUAL_REMOTE=1 forces it for testing).
Bluetooth::RemoteHost& Host();

// Presses the game's B / cancel button on the virtual remote for `millis` ms (used to close the stock Options page
// once the recomp settings overlay that replaced it has been dismissed). Thread-safe.
void InjectCancel(int millis);

} // namespace VirtualRemote

#pragma once

// Gamepad-backed virtual Wii Remote + Nunchuk (the dual-analog feature).
//
// An Xbox-style controller is presented to the emulated Bluetooth controller
// as a real Wii Remote with a Nunchuk attached. The right stick moves a
// synthetic sensor-bar view (IR dots), the left stick is the Nunchuk stick, so
// the guest's own WPAD/KPAD stack does all pointer math exactly as on hardware.

#include "bluetooth/bt_controller.h"

namespace VirtualRemote {

// Host used by the emulated Bluetooth controller: physical Wii Remotes keep
// their slots; slot 0 falls back to the virtual remote when a gamepad is
// present (or SIN_VIRTUAL_REMOTE=1 forces it for testing).
Bluetooth::RemoteHost& Host();

} // namespace VirtualRemote

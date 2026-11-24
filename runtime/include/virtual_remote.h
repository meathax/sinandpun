#pragma once
#include <cstddef>

// Gamepad-backed virtual Wii Remote + Nunchuk (the dual-analog feature).
//
// An Xbox-style controller is presented to the emulated Bluetooth controller
// as a real Wii Remote with a Nunchuk attached. The right stick moves a
// synthetic sensor-bar view (IR dots), the left stick is the Nunchuk stick, so
// the guest's own WPAD/KPAD stack does all pointer math exactly as on hardware.

#include "bluetooth/bt_controller.h"
#include "input_map.h"

namespace VirtualRemote {

void SetMenuNavigation(bool enabled);
void SetPartnerControllable(bool controllable);

// The modern action map owns host input unless a real Wii Remote is attached
// or SIN_VIRTUAL_REMOTE=0 explicitly requests the legacy input path.
bool UsesVirtualInput();
bool UsesVirtualSlot(std::size_t slot);

// True while a Wii Remote (physical or virtual) represents this player. The legacy GameCube pad path must then stay
// silent for that port: a host gamepad also showing up as a GameCube controller would make the game switch its
// control method away from the Wii Remote (Control Priority), so a real Wii Remote would stop behaving like one.
bool RemoteOwnsPort(std::size_t slot);

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
// their slots and are passed through byte for byte (the game's own default Wii
// Remote + Nunchuk controls apply, no host remapping); slot 0 otherwise uses the
// virtual remote (SIN_VIRTUAL_REMOTE=0 disables it). When the backend of a
// connected slot changes (a real remote connects or leaves), the link is dropped
// once so the game reconnects and initialises the new device from scratch, as a
// Wii does when a remote is swapped.
Bluetooth::RemoteHost& Host();

// Latest values the virtual remote put into a data report (aim telemetry / tests, runtime/src/hle/aim_telemetry.cpp).
// Sticks are SDL convention (+x right, +y down) as the remote sees them after bindings; cam* is the synthetic IR
// pointer in 1024x768 camera pixels (pointer scheme); classic*/q* are what the Classic report carried.
struct AimDebug {
    uint32_t serial = 0;            // data reports built so far
    int scheme = 1;                 // InputMap::Scheme value attached right now
    bool scripted = false;          // SIN_VREMOTE_SCRIPT drove this report (gamepad bypassed)
    bool extPresent = false;
    float rx = 0, ry = 0;           // aim stick, raw (before deadzone/curve)
    float lx = 0, ly = 0;           // move stick after the left deadzone
    float camX = 0, camY = 0;
    float classicX = 0, classicY = 0;   // shaped right stick sent as Classic RX / RY (+y down)
    uint8_t rxQ = 0, ryQ = 0, lxQ = 0, lyQ = 0;  // Classic raw 5/6-bit values
    uint8_t nunX = 0x80, nunY = 0x80;
    bool nunC = false, nunZ = false;
    uint16_t buttons = 0;           // Wii Remote core buttons
    uint8_t cl4 = 0, cl5 = 0;       // Classic pressed masks (1 = pressed)
    float lt = 0, rt = 0;
};
AimDebug GetAimDebug();

// Presses the game's B / cancel button on the virtual remote for `millis` ms (used to close the stock Options page
// once the recomp settings overlay that replaced it has been dismissed). Thread-safe.
void InjectCancel(int millis);

} // namespace VirtualRemote

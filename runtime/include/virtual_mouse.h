#pragma once

// Synthetic mouse + keyboard harness (test tool, inert unless SIN_VMOUSE_SCRIPT is set).
//
// It pushes real SDL events (SDL_EVENT_MOUSE_MOTION / BUTTON / KEY / WINDOW_*) with the game window's id into the SDL
// queue, so aurora's event pump, settings_overlay::HandleEvents and AimInput::HandleSdlEvent run exactly as with a
// physical mouse and keyboard.
//
// SIN_VMOUSE_SCRIPT = segments separated by '|' or ';'. A segment is `seconds[:field]...` (seconds = duration):
//   at=x/y      cursor target in window fractions (0..1 over the client area; outside 0..1 = outside the window)
//   ramp        move linearly from the previous position to `at` over the segment (otherwise jump at the start)
//   jit         alternate +-1 px each tick while holding (a real mouse is never perfectly still)
//   v=vx/vy     relative motion in px/s (xrel/yrel events; use with relative mode)
//   b=L+R+M+X1+X2   mouse buttons held during the segment
//   k=W+A+Space+Left Shift   keys held during the segment (SDL scancode names, '+' separated)
//   rep         send key-repeat events at 30 Hz for held keys
//   wheel=1|-1  one wheel notch at the segment start
//   focus=lost|gain, leave   window focus / mouse-leave events at the segment start
//   mark=name   label printed in the log
// Env: SIN_VMOUSE_WARP=1 also moves the real OS cursor with SDL_WarpMouseInWindow (the motion event then comes from
// the OS); SIN_VMOUSE_SCRIPT_FILE=path re-reads the script text from a file. SIN_VMOUSE_LOG=<ms> readout period
// (default 100, 0 = off). Log lines: "[vmouse] ..." (see virtual_mouse.cpp).

namespace VirtualMouse {

bool Active();
// Main thread, once per frame (AimInput::UpdateWindowState).
void Tick();

} // namespace VirtualMouse

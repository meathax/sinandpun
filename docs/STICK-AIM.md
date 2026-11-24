# Controls: Wii Remote, stick aim and gyro

## Wii Remote + Nunchuk

A physical Wii Remote is passed to the game byte for byte. The game's own WPAD/KPAD code reads it exactly as on a
Wii, so the original default controls always apply and no gamepad binding, aiming style or other setting changes them:

| Wii input | Action (manual pp. 5-7, 15-18) |
| --- | --- |
| Pointer | Move the reticle; menus |
| B | Tap: slash (up to 3 hits). Hold: shoot |
| A | Tap: lock on. Hold, then release: charge shot. Menus: confirm |
| Nunchuk stick | Move; push up to hover; menus |
| Nunchuk C | Jump; menus: confirm |
| Nunchuk Z | Evade (in the direction you are moving) |
| + | Pause / skip cut scene |
| - | Switch character (once unlocked) |
| HOME | HOME menu |
| Control Pad | Menus only |
| 1, 2 | Not used by the game |

The game's own **Options > Controller Settings** can still reassign its actions, as on a Wii.

**Use Wii Remotes** is now on by default. While it was off, SDL opened a connected Wii Remote as an ordinary gamepad.
Its input then went through the virtual remote and the gamepad bindings, so there was no real pointer, the Nunchuk
acted as a left stick and the buttons followed SDL's layout. The original Wii Remote (057e:0306) is now always hidden
from SDL. The Wii Remote Plus (057e:0330) shares its ID with the Wii U Pro Controller, so turn the option off only to
use a Pro Controller as a gamepad.

Two more fixes:

- **Swapping devices reconnects cleanly.** If a real remote connects while the virtual remote holds player 1 (or the
  real remote leaves), the Bluetooth link is dropped once. The game then reconnects and initialises the new device from
  scratch. Before this fix the link kept running on the old device's setup and the real remote stayed dead.
- **No second controller behind the remote.** While a Wii Remote owns a player, a host gamepad is no longer also shown
  to the game as a GameCube controller. If it were, touching it would make the game switch its control method away from
  the Wii Remote.

Searching for remotes automatically is now off by default, because continuous Bluetooth inquiry disturbs headphones.
Paired remotes reconnect with any button, and **Pair** searches on request.

## Right-stick aim

The right stick drives the same Wii pointer as a Wii Remote (`runtime/include/dual_analog.h`).

1. **Radial deadzone with smooth rescale.** Travel below 0.10 is ignored. Travel from there to 0.92 maps to 0..1, so
   there is no jump at the edge and worn sticks still reach full speed. Direction is always kept: no snapping to axes.
2. **Response curve:** `f(m) = 0.15*m + 0.85*m^2.2`.
   - The linear share keeps small deflections responsive straight out of the deadzone.
   - The power term gives fine control near the centre and full speed at full tilt.
   - Unlike the old smoothstep, the curve is still accelerating at the rim.
3. **Velocity pointer.** The curve output sets reticle speed directly.
   - There is no ramp-up, smoothing, speed cap or edge boost.
   - Releasing the stick holds the reticle where it is.
   - Speed is integrated over real time on the virtual remote's 10 ms report clock. It does not depend on the
     presentation frame rate; the game logic stays at 60 Hz.
4. **Speed.** At full tilt the reticle moves 1,300 camera px/s, which crosses the visible playfield in about 0.35 s.
   Vertical speed is scaled by 810/456 so equal deflection moves the same number of screen pixels on both axes.

Measured in game (stage 5, scripted stick, 50 Hz telemetry):

| Stick | Reticle speed |
| --- | --- |
| Full tilt | 1,299 px/s; 0.22 s covered 60 % of the screen width |
| 30 % tilt | 97 px/s; slow tracking |
| 45° diagonal | 920 px/s horizontal (0.707 x 1,300), direction kept |

Player settings are **Aim sensitivity** (multiplies the speed) and **Invert vertical aim**. **Show advanced tuning**
reveals Aim deadzone, Aim acceleration (curve exponent), Vertical aim speed, Full-tilt point, Move deadzone and
Trigger press point.

The Classic Controller aiming style applies only the deadzone and sensitivity, because the game applies its own
response to the Classic right stick.

## Gyro

The gyro moves the same pointer. Defaults:

- **Sensitivity:** 12 px per degree. The full screen width takes about 37° of rotation.
- **Axes:** player space.
- **Axis matching:** vertical motion is scaled so one degree moves the same number of screen pixels on both axes.
- **Tightening:** rotation slower than 1.2°/s is scaled down in proportion, so hand tremor does not shake the reticle.
  Deliberate motion is unaffected.

The Gyro page has these settings:

- Gyro aiming on/off and a status line.
- One sensitivity slider.
- **Gyro button**: Not used, Hold to aim, or Hold to pause (ratchet, R3 by default).
- Invert vertical gyro.
- Calibrate.
- Advanced: axes and invert horizontal.

## Menu

The Controls tab pages were General / Gamepad / Keyboard & Mouse / Aiming / Devices, with about 40 tuning rows. They
are now:

- **Gamepad**: aiming style, sensitivity, invert, live preview, advanced tuning, button icons, recommended layout.
- **Gyro**
- **Buttons**: a device selector, then the bindings.
- **Mouse**
- **Wii Remote**: the Wii controls summary, connection, sensor bar, rumble and the mapping wizard.

## Config.toml compatibility

`[input] version = 6`. Older files load without errors.

- Keys of removed settings are read and ignored: `curve`, `deadzone_mode`, `dual_zone_*`, `tracking_speed`,
  `sensitivity_x/y`, `max_speed`, `speed_cap`, `ramp_ms`, `edge_boost`, `edge_accel_ms`, `recenter_*`,
  `smoothing_ms` and `pointer_mode`.
- When upgrading from version 5 or older:
  - The stick tuning restarts from the new defaults.
  - These settings are kept: invert Y, left deadzone, left saturation and invert move Y.
  - A right deadzone or trigger point that differs from the old defaults is also kept.
  - The old file is backed up to `Config.toml.input-v<N>.bak`.
- Gyro, mouse and binding keys are unchanged.

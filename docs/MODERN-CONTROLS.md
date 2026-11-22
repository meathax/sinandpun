# Modern controls

The default layout keeps both thumbs on the sticks during combat:

| Action | Xbox One | Keyboard / mouse |
| --- | --- | --- |
| Shoot (hold), slash (tap) | RT | Left mouse / J |
| Lock on (tap), charge (hold/release) | LT | Right mouse / K |
| Jump | LB | Space / mouse side 1 |
| Evade | RB | Left Shift / mouse side 2 |
| Move | Left stick | WASD |
| Aim | Right stick | Mouse / arrow keys |
| Precision aim | Right-stick click | Left Ctrl |
| Recenter pointer | Left-stick click | R / middle mouse |
| Pause / skip | Menu | Escape / P |
| Switch character, when unlocked | View | Tab |
| Confirm / back in game menus | A / B | Enter / Backspace |

The guest button meanings come from the [Nintendo instruction booklet](https://impzone.club/pdf/sinpunishmentss.pdf),
pages 5â€“7; the host layout is a port design choice. Shooting and slashing share
one original game action, as do lock-on and charged shots.

Options > Controls > Gamepad or Keyboard & Mouse exposes all 25 actions,
including movement/aim directions, menu buttons and auxiliary Wii buttons.
Choose any of three slots and press a button, pull a trigger, move a stick,
press a key, click a mouse button or scroll. Conflicts swap bindings; clearing
a slot genuinely unbinds it. Settings and button prompts update live and save.
Escape, Delete and F1 can be captured. F10 remains a fixed recovery key.

The options UI deliberately keeps A/Enter to select, B/Escape to go back,
X to apply, Y to reset the page, LB/RB to change tabs, and left-stick click,
Delete or right-click to clear a binding. These remain reachable even after
all gameplay bindings are cleared. Capture dialogs have Clear/Cancel buttons
and a timeout; the captured press is suppressed before returning to the list.

Controls > Overview > Modern Xbox layout restores the recommended gamepad
profile and Automatic input scheme without changing keyboard/mouse bindings.
Automatic drives the Wii Remote pointer from either the gamepad or the mouse.
Legacy Classic Controller remains available under Overview. In pointer mode,
Remote A serves both confirm and lock, and Remote B serves both cancel and fire.

### Right-stick aiming

Start with **Controls > Gamepad > Balanced**. Make small stick movements to
track a target, push toward the rim to cross the screen, and release to park
the reticle. Holding right-stick click reduces speed across the whole range.

| Preset | Tracking gain (camera px/s) | Sweep begins at usable travel | Full-stick speed (camera px/s) |
| --- | ---: | ---: | ---: |
| Balanced | 420 | 52% | 3,200 |
| Sharpshooter | 260 | 65% | 2,400 |
| Arcade rush | 600 | 42% | 5,200 |

**Tracking speed** sets the gain inside the sweep threshold. At 10% usable
travel, Balanced moves 42 camera pixels per second. Above the threshold a
quadratic term adds speed progressively. **Full-stick speed** adjusts the peak
independently while it exceeds the tracking gain. Each preset responds immediately
to direction changes. Speed cap, timed boost and smoothing remain available for
custom tuning. At full horizontal tilt the three presets traverse the calibrated
playfield in roughly 140, 187 and 86 milliseconds respectively (before guest
sampling and display latency). The stick stops at the visible edge. Recenter
accounts for the Wii sensor-bar position, and diagonal travel accounts for the
game's 810 by 456 reticle layer. Version 4 upgrades the original Automatic aim defaults to Balanced.

The radial calibration follows [Hypersect’s analog-stick analysis](https://blog.hypersect.com/interpreting-analog-sticks/).
Independent deadzone and full-input controls also appear in [Halo Infinite’s controller settings](https://support.halowaypoint.com/hc/en-us/articles/4407649252116-Guide-to-Halo-Infinite-Game-Settings).
The preset values are tuned for this game's pointer.

Version 3 upgrades only completely unchanged legacy profiles. A customized
profile is retained as a unit to avoid introducing conflicts. An untouched
old gamepad profile pinned to Pointer moves to Automatic. The original config
is backed up as `Config.toml.input-v2.bak` before the upgraded config is saved.

The virtual input path now works without a gamepad connected. While it owns
player 1, the same SDL pad is not also exposed through the independent legacy
GameCube PADRead path. A real connected Wii Remote takes precedence;
`SIN_VIRTUAL_REMOTE=0` explicitly restores the legacy path.

Regression coverage: the production remote-button resolver reproduces the
old A-to-fire alias and verifies the new RT/LT/LB/RB mapping in both schemes,
keyboard/mouse events, rebinding and release of the former binding. Config
tests cover migration, preservation of custom profiles and three-slot / empty
binding round-trips. SDL capture tests cover Escape/Delete/F1 and wheel input.

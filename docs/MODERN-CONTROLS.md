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
pages 5–7; the host layout is a port design choice. Shooting and slashing share
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
Automatic uses Classic for a gamepad and Remote+Nunchuk for mouse/keyboard.
The legacy Pointer mode necessarily shares Remote A between confirm/lock and
Remote B between cancel/fire; Classic separates menu and combat buttons.

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

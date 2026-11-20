# Classic Controller emulation: root cause, fix, live hot-swap

Scope: `runtime/src/wiimote/virtual_remote.cpp`, `runtime/include/virtual_remote.h`. Nothing here was run in game
(no builds were allowed); every claim is from the guest binary, the existing `[bt]` log and standalone tests.

## 1. Root cause: the guest turns on extension encryption and our remote sent plain data

Guest log of a Nunchuk run (`build/boot-err.log`), after the extension appeared:

```
[bt] out id=0x16 ... 04 A4 00 F0 01 55      write 0x55 -> 0xA400F0   (encryption off)
[bt] out id=0x16 ... 04 A4 00 FB 01 00      write 0x00 -> 0xA400FB
[bt] out id=0x17 ... 04 A4 00 FA 00 06      read ID (6 bytes)  -> 00 00 A4 20 00/01 00/01
[bt] out id=0x16 ... 04 A4 00 F0 01 AA      write 0xAA -> 0xA400F0   (ENCRYPTION ON)
[bt] out id=0x16 ... 04 A4 00 40 06 62      key bytes 0x40..0x45
[bt] out id=0x16 ... 04 A4 00 46 06 56      key bytes 0x46..0x4B
[bt] out id=0x16 ... 04 A4 00 4C 04 39      key bytes 0x4C..0x4F  (16-byte key complete)
[bt] out id=0x17 ... 04 A4 00 20 00 20      read 32 bytes of calibration at 0xA40020
```

Guest code (main.dol, `python tools/dis_dol.py <addr>`):

* `0x801C76D0` (state `0x04A400FA`, called from the read-reply dispatcher at `0x801C8348`): reads the ID, `lbz r0,5(r29)`
  = last ID byte; `cmpwi r0,1` -> `li r0,2; stb r0,0(r29)` stores type **2 = Classic** (0 -> 1 = Nunchuk). The format
  byte (ID byte 4) must be 1..3 for Classic (`0x801C78C8..0x801C78DC`, else type 0xFC "unknown"). Our ID
  `00 00 A4 20 01 01` passes this. The type is handed to the game's extension callback (`0x801C79A4`) before calibration.
* `0x801C7A0C..0x801C7AD0`: after the type is accepted the guest writes **0xAA to 0xA400F0**, writes the 16-byte key to
  `0xA40040/46/4C` and reads 0x20 bytes at `0xA40020`. `0x801CA810` (types 1,2,4,17,18) and `0x801CAF00` generate that key.
* The guest contains the first-party key tables: `main.dol` file offsets `0x31D7A0` (answers), `0x31D7D0` (keygen S-box),
  `0x31D8D0` (8 S-boxes, guest addr `0x803216C0..`) are **byte-identical** to `kAnswers/kKeygenSbox/kSboxes` in
  `wiimote_ext_crypto.cpp` (verified by script). Code references: `0x801CA834`, `0x801CAF24`. So the guest decrypts all
  extension register reads and extension bytes in data reports with the Dolphin/Hector Martin key schedule.
* Calibration check at `0x801C7190..`: 14-byte sum + 0x55 / + 0xAA checksums over the (decrypted) block.

`VirtualWiimote` ignored all of that: it served `m_ext` calibration and the 6-byte extension data **in plain text**. The
guest decrypted plain bytes into garbage: the calibration checksum failed (stick calibration invalid) and every Classic
button/stick bit decoded as noise. That matches the scripted observation (title "Press A" worked because it is the
**Wii Remote core** A bit, which is not encrypted; Classic A on file-select was ignored because it lives only in the
encrypted extension bytes). The same defect affected the Nunchuk stick/C/Z/accel path (never verified in game).

Not the cause (checked and correct): ID bytes `00 00 A4 20 01 01`, status flag `0x02`, 6-byte standard Classic layout and bit
order (matches wiibrew and Dolphin `Classic.cpp`: byte4 R .02 + .04 home .08 - .10 L .20 down .40 right .80; byte5 up .01
left .02 ZR .04 X .08 A .10 Y .20 B .40 ZL .80), calibration block shape and checksum (8-bit max/min/centre for LX LY RX RY,
two trigger neutrals, sum+0x55 / sum+0xAA, same as Dolphin), report mode `0x37` placement (ext at byte 16).

## 2. Fix (virtual_remote.cpp)

* `ObserveExtensionWrite()` tracks 0x55/0xAA at `A400F0` and the key at `A40040..4F`; `Wiimote::ExtCrypto::FromKey()`
  (existing, ported from Dolphin, unit-checked: 350/350 random first-party keys accepted and round-trip) builds the tables.
* `ReadMemory()` encrypts register replies of the extension block (table offset = register low byte) while encryption is on.
* `BuildDataReport()` encrypts the 6 extension bytes with offset 0 while encryption is on. If a key is rejected the remote
  logs `ext crypto: encryption enabled but the guest key was not accepted; sending plain data`.
* Nothing is encrypted before the guest writes 0xAA, and 0x55 clears key/tables, exactly like the real extension.
* Classic scheme: menu keys (Confirm=A, Cancel=B, Pause=+, Minus, Home, D-pad) are also pressed on the core buttons so the
  game still sees them if its method ends up "Wii Remote (and Nunchuk)" with a Classic attached. `SIN_VREMOTE_CLASSIC_CORE=0`
  disables the mirroring. Fire/Lock/Jump/Evade stay Classic-only (R, ZR, L, ZL).

## 3. The game's own "control method" (for the lead to verify, not changed)

Strings (main.dol): `Control Priority - Select the default controller for player 1/2` (msg 351), notices
`Your control method has been set to ...` (msg 279 Wii Remote, 280 + Nunchuk, 281 **Classic Controller**, 282 GCN, 283 Zapper)
and msg 286 `Please connect a Nunchuk or Classic Controller...`. The notice builder is `0x80090E34`
(method byte at obj+0x177: 0 remote, 1 nunchuk, 2 classic, 3 GCN, 4 zapper; message selection `0x80090E9C..`), queued by
`0x80091738` (no static callers in main.dol, so it is called from a stage module). Consequences:

* After plugging a Classic the game may show a "control method has been set to the Classic Controller" notice and take
  the Control Priority option into account. A hot-swap can therefore pop a notice; closing it needs A/B (core mirror helps).
* If the game still ignores Classic after this fix, check Options -> Control Priority and the notice text first.

## 4. Live hot-swap design

API: `VirtualRemote::SetScheme(InputMap::Scheme)` / `GetScheme()` (`virtual_remote.h`). `SetScheme` is one relaxed atomic
store (no lock, no-op when unchanged); `aim_input.cpp` already calls it. Work happens in `Pop()` (the BT poll, mutex held):

1. Idle and requested != attached: log `scheme -> <x> (unplug <y>)`, ext present=false, encryption/key cleared, extension
   registers zeroed, **status 0x20 with the extension flag (0x02) cleared** is queued.
2. While unplugged: reads/writes of `0xA4xxxx` fail (read error 8 / write error 7), data reports carry zero extension bytes.
3. After the status was handed to the guest (`m_pending` empty) **and 500 ms** (`kUnplugHold`): attach the latest requested
   scheme, rebuild ID + calibration, reset the Classic stick filter, log `scheme -> <x> (plug)`, queue **status 0x20 with the
   flag set**. The guest then re-runs its extension init (the 0x55/0xFB/ID/0xAA/key/calibration sequence above) and KPAD
   reports type 1 (Nunchuk) or 2 (Classic).
4. Pointer state, IR, LEDs, report mode and the pending queue are left untouched (mode is NOT reset: if the real remote
   resets it, the guest re-sends 0x12 anyway; keeping it cannot strand the pointer). `ResetState()` (link ready) starts with
   the requested scheme, else `AimSettings::Get().scheme`.

## 5. Test hook (no human needed)

`SIN_VREMOTE_SCRIPT` segments (`seconds:buttons:rx/ry:lx/ly`, `;` or `|` separated) accept, in any field after the first:

* `scheme=classic` / `scheme=pointer`: hot-swap when the segment starts.
* `core=0`: while Classic is attached the script's buttons are NOT also pressed on the core buttons (Classic-only test).
* a bare segment `scheme=classic` applies to the next timed segment.

When the script contains `scheme=` it owns the scheme: external `SetScheme` calls are ignored for the run (the input arbiter
cannot fight it). Script buttons keep their old behaviour (A = core A + Classic A) unless `core=0`.

Example: Classic from the start, confirm at the title, then swap to pointer and back:
`SIN_VREMOTE_SCRIPT="3:::|2:A:scheme=classic|1.5::|2:A:core=0|3:|3:A:scheme=pointer|4:A:scheme=classic:core=0"`
(`scheme=` and `core=` are accepted in the buttons field position only when written as separate `:` fields, so a segment
`5:A:scheme=classic` is "5 s, A, then classic".)

## 6. Log lines that prove each step

| Event | Log line (`[bt]`) |
| --- | --- |
| script swap | `script t=..s segment N: scheme=classic` |
| unplug | `scheme -> classic (unplug pointer)` then `status 0x20 -> guest: extension=0 (none)` |
| guest reaction | `ext read reg=0x..` (ID: `reg=0xFA len=6`), `ext write reg=0xF0 len=1 first=55` |
| plug | `scheme -> classic (plug)` then `status 0x20 -> guest: extension=1 (classic)` |
| encryption | `ext write reg=0xF0 first=AA`, `ext write reg=0x40/0x46/0x4C`, `ext crypto: key accepted, encryption ON`, `ext read reg=0x20 len=16 err=0 enc=1` (twice) |
| failure | `ext crypto: encryption enabled but the guest key was not accepted` |

Expected guest handshake after plug: status request is NOT needed; the guest should start with the `0xF0=55`, `0xFB=00`, ID
read; ID read must return `00 00 A4 20 01 01` for classic. If no `ext write` line follows the plug status, the guest ignored the
unsolicited status report (then step 3 needs the guest's own status request; see risks).

## 7. Risks

* Encryption was inferred from the guest handshake, key tables and Dolphin behaviour, not yet seen end-to-end in game: if the
  guest key schedule differs from first-party (FromKey fails) the remote sends plain data and logs it.
* Unsolicited 0x20 handling in the guest SDK is assumed to start extension init like on hardware; not run.
* The game may show its own "control method" notice on each swap, and its Control Priority option can select which
  controller type it prefers; this is game-side behaviour I did not change.
* A swap takes ~0.5 s plus guest init (several hundred ms): input is dead for about one second.
* `aim_settings.h:345` currently fails to compile on its own (`kSchemeModeKeys` unqualified; belongs to the input agent).
